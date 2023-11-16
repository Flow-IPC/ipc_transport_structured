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

/// @file

#include "ipc/transport/struc/heap_fixed_builder_capnp_msg_builder.hpp"

namespace ipc::transport::struc
{

// Implementations.

Heap_fixed_builder_capnp_message_builder::Heap_fixed_builder_capnp_message_builder
  (flow::log::Logger* logger_for_blobs, size_t segment_sz, size_t frame_prefix_sz, size_t frame_postfix_sz) :
  m_logger_for_blobs(logger_for_blobs),
  m_segment_sz(segment_sz), m_frame_prefix_sz(frame_prefix_sz), m_frame_postfix_sz(frame_postfix_sz)
{
  // That's all for now.
}

void Heap_fixed_builder_capnp_message_builder::emit_segment_blobs(Segment_ptrs* target_blob_ptrs_ptr)
{
  // We'll need to adjust it a bit below.
  auto& blobs = m_serialization_segments_plus_frame_space;
  auto& target_blob_ptrs = *target_blob_ptrs_ptr;

  assert((!blobs.empty()) && "Should not be possible for serialization to be empty with our use cases.  Investigate.");
  target_blob_ptrs.reserve(target_blob_ptrs.size() + blobs.size()); // Remember: we are appending; not replacing.

  const auto capnp_segs = getSegmentsForOutput();
  assert((capnp_segs.size() == n_segments())
         && "Somehow our MessageBuilder created fewer or more segments than allocateSegment() was called?!");

  for (size_t idx = 0; idx != capnp_segs.size(); ++idx)
  {
    // Pointer-to-word + size in words => pointer-to-byte + size in bytes.
    const auto capnp_seg = capnp_segs[idx].asBytes();
    const auto seg_sz = capnp_seg.size();

    auto& blob = *(blobs[idx]);

    assert((capnp_seg.begin() == blob.begin())
           && "Somehow capnp-returned segments are out of order to allocateSegment() calls; or something....");
    assert((seg_sz != 0)
           && "capnp shouldn't be generating zero-sized segments.");
    assert((seg_sz <= (blob.capacity() - m_frame_prefix_sz - m_frame_postfix_sz))
           && "capnp somehow overflowed the area we gave it.");

    blob.resize(seg_sz); // Just pull up end() to immediately follow the serialization.

    /* @todo Maybe output some stuff, like the segment dump, here, like
     * shm::capnp_message_builder::lend() does?  Though, in practice, this is always the
     * top serialization and hence is printed out in pretty good detail as the actual blob is transmitted over
     * ipc::transport core layer.  So maybe skip it to avoid unneeded verbosity. */

    /* Lastly output a *pointer*, as promised, to the Blob we hold.  They are free to do what they want
     * with the pointed-to `blob`, including sliding around its begin() and end() to make use of the
     * frame space.  They must not access the dereferenced ptr past *this lifetime, nor past the
     * next mutation (if any) (whichever happens first).  emit_segment_blobs() can be called agains after the
     * latter for an update serialization. */
    target_blob_ptrs.push_back(&blob);
  } // for (idx in [0, size()))
} // Heap_fixed_builder_capnp_message_builder::emit_segment_blobs()

kj::ArrayPtr<capnp::word> Heap_fixed_builder_capnp_message_builder::allocateSegment(unsigned int min_sz) // Virtual.
{
  using Word = capnp::word;
  using Capnp_word_buf = kj::ArrayPtr<Word>;
  using flow::util::Blob;
  using std::memset;
  constexpr size_t WORD_SZ = sizeof(Word);

  /* It'd be nice not to realloc m_serialization_segments_plus_frame_space's internal buffer, as that involves
   * moving `unique_ptr<Blob>`s around: constant-time/cheap though it is, if we can avoid it, good.
   * This value is pretty decent; 1 is really most typical, while 4 shallow `unique_ptr<Blob>`s is not a ton of RAM. */
  constexpr size_t N_SEGS_GUESS = 4;
  m_serialization_segments_plus_frame_space.reserve(N_SEGS_GUESS); // No-op after 1st time.

  /* Background from capnp: They're saying the need the allocated space for serialization to store at least min_sz:
   * probably they're going to store some object that needs at least this much space.  So typically it's some
   * scalar leaf thing, like 4 bytes or whatever; but it could be larger -- or even huge (e.g., a Data or List
   * of huge size, because the user mutated it so via a ::Builder).  Oh, and it has to be zeroed, as by calloc().
   *
   * So all we *have* to allocate is min_sz exactly in that sense.  But the idea is to try to allocate more, so that
   * capnp can efficiently shove more objects in there too without calling allocateSegment() for each one.
   * And a fixed-heap builder we promised to make that the fixed size m_segment_sz.
   * (MallocMessageBuilder has that mode too: FIXED_SIZE allocation-strategy.)  Of course, if min_sz exceeds that,
   * then we have no choice but to allocate the larger amount min_sz. */

  const size_t seg_sz
    = std::max(min_sz * WORD_SZ, // Don't forget: in their API min_sz is in `word`s.
               /* Seems prudent to give capnp an area that is a multiple of `word`s.  Maybe required.  Probably even.
                * However we must *not* exceed m_segment_sz, if we can help it.  A little less is okay. */
               (m_segment_sz / WORD_SZ) * WORD_SZ);

  // Now just follow the logic we expounded on in the various doc headers.  Keeping explanations light here.

  // By contract we can't let the prefix frame change the start of the area given to capnp to be non-aligned.
  assert(((m_frame_prefix_sz % sizeof(void*)) == 0)
         && "N=frame_prefix_sz passed to ctor, by contract, must be such that (aligned_ptr + N) is also aligned.");

  m_serialization_segments_plus_frame_space.emplace_back(new Blob(m_logger_for_blobs));
  auto& blob = *(m_serialization_segments_plus_frame_space.back());
  blob.reserve(m_frame_prefix_sz + seg_sz + m_frame_postfix_sz);
  blob.resize(seg_sz, m_frame_prefix_sz); // Set: size(), start(), respectively.
  assert(blob.size() != 0);
  // capnp requires: it must be zeroed.  (Blob intentionally doesn't zero.  @todo Would be a nice *optional* feature.)
  memset(blob.begin(), 0, blob.size());

  return Capnp_word_buf(reinterpret_cast<Word*>(blob.begin()),
                        reinterpret_cast<Word*>(blob.end()));
} // Heap_fixed_builder_capnp_message_builder::allocateSegment()

size_t Heap_fixed_builder_capnp_message_builder::n_segments() const
{
  return m_serialization_segments_plus_frame_space.size();
  // Note this equals getSegmentsForOutput().size().  emit_segment_blobs() as of this writing assert()s as much.
}

} // namespace ipc::transport::struc
