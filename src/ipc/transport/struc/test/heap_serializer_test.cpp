/* Flow-IPC: Structured Transport
 * Copyright (c) 2023 Akamai Technologies, Inc.; and other contributors.
 * Each commit is copyright by its respective author or author's employer.
 *
 * Licensed under the MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE. */

/* Direct Heap_fixed_builder -> Heap_reader pair tests: no Channel, no transport.  In particular this
 * lets us exercise the deserialization() error paths (corrupt split-records, misalignment, no segments)
 * that a well-behaved channel cannot trigger; msg_test.cpp covers the happy split paths end-to-end. */

#include "ipc/transport/struc/heap_serializer.hpp"
#include "ipc/transport/struc/test/test_schema.capnp.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace ipc::transport::struc::test
{

namespace
{

using flow::Error_code;

// Small cap, so splitting is easy to trigger.  Word-multiples throughout (the builder rounds down anyway).
constexpr size_t CAP = 8 * 1024;
constexpr size_t FRAME_SZ = 128;
constexpr size_t SEG_SZ_INIT = 4 * 1024;
constexpr size_t WORD_SZ = sizeof(::capnp::word);

Heap_fixed_builder make_builder()
{
  return Heap_fixed_builder{{ nullptr, CAP, SEG_SZ_INIT, FRAME_SZ, nullptr }};
}

// Fill builder's root with a CoolReq: coolVal + a payload of n_uint64s known values.
void fill_msg(Heap_fixed_builder* builder, uint64_t cool_val, size_t n_uint64s)
{
  auto root = builder->payload_msg_builder()->initRoot<Body>().initCoolReq();
  root.setCoolVal(cool_val);
  auto payload = root.initPayload(n_uint64s);
  for (size_t i = 0; i < n_uint64s; ++i)
  {
    payload.set(i, i);
  }
}

/* Copy builder-emitted segments into a Heap_reader, as a transport would have (each (sub-)segment's contents
 * land in a fresh word-aligned heap buffer of exactly its size). */
void feed_segments(Heap_reader* reader, const Segment_bufs& target_bufs)
{
  for (const auto& buf : target_bufs)
  {
    Segment_blob_in blob;
    blob.resize(buf.size());
    std::memcpy(blob.data(), buf.data(), buf.size());
    reader->add_serialization_segment(std::move(blob));
  }
}

// Check the deserialized CoolReq against what fill_msg() would have set.
void check_msg(const Body::Reader& root, uint64_t cool_val, size_t n_uint64s)
{
  ASSERT_TRUE(root.hasCoolReq());
  const auto req = root.getCoolReq();
  EXPECT_EQ(req.getCoolVal(), cool_val);
  const auto payload = req.getPayload();
  ASSERT_EQ(payload.size(), n_uint64s);
  for (size_t i = 0; i < n_uint64s; ++i)
  {
    if (payload[i] != i)
    {
      ADD_FAILURE() << "payload[" << i << "] = [" << payload[i] << "]; expected [" << i << "].";
      break;
    }
  }
}

} // namespace (anon)

// Round trip without splitting; the header-area contract (location, size, zeroed-on-1st-emit); no-error semantics.
TEST(Heap_serializer_test, round_trip_no_split)
{
  auto builder = make_builder();
  fill_msg(&builder, 42, 100); // Fits in seg 1 comfortably.

  Segment_bufs target_bufs;
  util::Blob_mutable hdr_blob;
  Split_segments split_segs;
  Error_code err_code = error::Code::S_INVALID_ARGUMENT; // Ensure success *clears* it.
  builder.emit_serialization(&target_bufs, &hdr_blob, &split_segs, NULL_SESSION, &err_code);
  EXPECT_FALSE(err_code);
  ASSERT_EQ(target_bufs.size(), 1u);
  EXPECT_TRUE(split_segs.empty());
  EXPECT_EQ(builder.n_serialization_segments(), 1u);

  // Header: FRAME_SZ bytes immediately preceding seg 1; zeroed as-of the first emit_serialization().
  ASSERT_EQ(hdr_blob.size(), FRAME_SZ);
  EXPECT_EQ(static_cast<const uint8_t*>(hdr_blob.data()) + FRAME_SZ, target_bufs.front().data());
  const auto hdr_data = static_cast<const uint8_t*>(hdr_blob.data());
  EXPECT_TRUE(std::all_of(hdr_data, hdr_data + FRAME_SZ, [](uint8_t b) { return b == 0; }));

  Heap_reader reader{{ nullptr, target_bufs.size(), nullptr }};
  feed_segments(&reader, target_bufs);
  err_code = error::Code::S_INVALID_ARGUMENT; // Ditto re. clearing.
  const auto root = reader.deserialization<Body>(nullptr, &err_code);
  EXPECT_FALSE(err_code);
  check_msg(root, 42, 100);
}

// Round trip with splitting: a payload far over the cap => 2+ sub-segments + split-records; reassembly rejoins all.
TEST(Heap_serializer_test, round_trip_split)
{
  // ~2.5x-cap worth of list data => its capnp segment must be split into 3 sub-segments (2 full + partial or similar).
  constexpr size_t N = (5 * CAP / 2) / sizeof(uint64_t);

  auto builder = make_builder();
  fill_msg(&builder, 43, N);

  Segment_bufs target_bufs;
  util::Blob_mutable hdr_blob;
  Split_segments split_segs;
  Error_code err_code;
  builder.emit_serialization(&target_bufs, &hdr_blob, &split_segs, NULL_SESSION, &err_code);
  ASSERT_FALSE(err_code);
  ASSERT_FALSE(split_segs.empty());
  EXPECT_GT(target_bufs.size(), builder.n_serialization_segments()); // Splitting inflated the blob count.
  for (const auto& buf : target_bufs)
  {
    EXPECT_LE(buf.size(), CAP);
    EXPECT_EQ(buf.size() % WORD_SZ, 0u);
  }
  for (const auto& split_seg : split_segs)
  {
    EXPECT_GE(split_seg.m_n_cont_subsegs, 1u);
    EXPECT_LT(split_seg.m_start_idx + split_seg.m_n_cont_subsegs, target_bufs.size());
  }

  Heap_reader reader{{ nullptr, target_bufs.size(), nullptr }};
  feed_segments(&reader, target_bufs);
  const auto root = reader.deserialization<Body>(&split_segs, &err_code);
  EXPECT_FALSE(err_code);
  check_msg(root, 43, N);
}

// split_segs = null with an over-cap segment => S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG.
TEST(Heap_serializer_test, leaf_too_big)
{
  auto builder = make_builder();
  fill_msg(&builder, 44, (2 * CAP) / sizeof(uint64_t));

  Segment_bufs target_bufs;
  util::Blob_mutable hdr_blob;
  Error_code err_code;
  builder.emit_serialization(&target_bufs, &hdr_blob, nullptr, NULL_SESSION, &err_code);
  EXPECT_EQ(err_code, error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG);
}

// deserialization() without any add_serialization_segment() => S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS.
TEST(Heap_serializer_test, insufficient_segments)
{
  Heap_reader reader{{ nullptr, 0, nullptr }};
  Error_code err_code;
  reader.deserialization<Body>(nullptr, &err_code);
  EXPECT_EQ(err_code, error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS);
}

/* Corrupt split-records (as if from a hostile/buggy peer) => S_DESERIALIZE_FAILED_REASSEMBLY_FAILED -- and
 * never memory-unsafety.  Includes the wraparound shape (huge start_idx) that a naive additive bounds check
 * would wave through. */
TEST(Heap_serializer_test, reassembly_failures)
{
  // A legit split serialization to feed each scenario's fresh reader (3 sub-segments or so).
  auto builder = make_builder();
  fill_msg(&builder, 45, (5 * CAP / 2) / sizeof(uint64_t));
  Segment_bufs target_bufs;
  util::Blob_mutable hdr_blob;
  Split_segments split_segs;
  Error_code err_code;
  builder.emit_serialization(&target_bufs, &hdr_blob, &split_segs, NULL_SESSION, &err_code);
  ASSERT_FALSE(err_code);
  ASSERT_GE(target_bufs.size(), 3u);

  const auto expect_reassembly_failure = [&](const Split_segments& bad_split_segs)
  {
    Heap_reader reader{{ nullptr, target_bufs.size(), nullptr }};
    feed_segments(&reader, target_bufs);
    Error_code our_err_code;
    reader.deserialization<Body>(&bad_split_segs, &our_err_code);
    EXPECT_EQ(our_err_code, error::Code::S_DESERIALIZE_FAILED_REASSEMBLY_FAILED);
  };

  expect_reassembly_failure({}); // Non-null but empty: against internal protocol.
  expect_reassembly_failure({ Split_segment{ 0, 0 } }); // Zero continuation sub-segs: ditto.
  expect_reassembly_failure({ Split_segment{ 0, target_bufs.size() } }); // Runs past the last segment.
  expect_reassembly_failure({ Split_segment{ target_bufs.size(), 1 } }); // Starts past the last segment.
  // The would-be-wraparound shapes: start_idx + n_cont + 1 overflows to a small "valid-looking" value.
  expect_reassembly_failure({ Split_segment{ std::numeric_limits<size_t>::max(), 1 } });
  expect_reassembly_failure({ Split_segment{ 1, std::numeric_limits<size_t>::max() } });
}

// Segment with misaligned start address, or word-fractional size => S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED.
TEST(Heap_serializer_test, misaligned_segment)
{
  // A legit small serialization to corrupt in delivery.
  auto builder = make_builder();
  fill_msg(&builder, 46, 100);
  Segment_bufs target_bufs;
  util::Blob_mutable hdr_blob;
  Split_segments split_segs;
  Error_code err_code;
  builder.emit_serialization(&target_bufs, &hdr_blob, &split_segs, NULL_SESSION, &err_code);
  ASSERT_FALSE(err_code);
  const auto& buf = target_bufs.front();

  { // Misaligned start: blob whose begin() sits at offset 1 within its (aligned) buffer.
    Heap_reader reader{{ nullptr, 1, nullptr }};
    Segment_blob_in blob;
    blob.resize(buf.size(), 1); // start() = 1 => begin() = buffer + 1 = misaligned.
    std::memcpy(blob.data(), buf.data(), buf.size());
    reader.add_serialization_segment(std::move(blob));
    reader.deserialization<Body>(nullptr, &err_code);
    EXPECT_EQ(err_code, error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED);
  }

  { // Word-fractional size: aligned start but size % sizeof(word) != 0.
    Heap_reader reader{{ nullptr, 1, nullptr }};
    Segment_blob_in blob;
    blob.resize(buf.size() - 1);
    std::memcpy(blob.data(), buf.data(), buf.size() - 1);
    reader.add_serialization_segment(std::move(blob));
    reader.deserialization<Body>(nullptr, &err_code);
    EXPECT_EQ(err_code, error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED);
  }
}

} // namespace ipc::transport::struc::test
