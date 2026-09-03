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

#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/transport/struc/test/test_schema.capnp.h"
#include "ipc/transport/struc/detail/msg_impl.hpp"
#include "ipc/transport/struc/detail/struc_fwd.hpp"
#include <flow/test/test_common_util.hpp>
#include <flow/util/util.hpp>
#include <boost/thread/future.hpp>
#include <gtest/gtest.h>
#include <array>
#include <memory>

#ifndef FLOW_OS_LINUX
static_assert(false, "See static_assert() near Native_handle_lifecycle test later in file.");
#endif
#include <fcntl.h>
#include <unistd.h>

namespace ipc::transport::struc::test
{

namespace
{
  using flow::Error_code;
  using flow::Fine_duration;
  using session::schema::MqType;
  using util::Native_handle;
  using util::Blob_mutable;
  using util::Blob_const;
  using handle_t = Native_handle::handle_t;

  // Generous timeout for any blocking wait in this test.
  constexpr Fine_duration TIMEOUT = boost::chrono::milliseconds{500};

  // Use the simplest channel type: Native_socket_stream only, no handles.
  // The transport max (~64Ki for UDS) is what determines when splitting kicks in.
  constexpr auto TEST_MQ_TYPE = MqType::NONE;
  constexpr bool TEST_HANDLES = false;

  using Test_struc_pair = Struc_session_pair<Body, TEST_MQ_TYPE, TEST_HANDLES>;
  using Struc_channel_t = Test_struc_pair::Struc_channel_t;

  // Helper: fill a CoolReq with coolVal and one or two List(UInt64) payloads of specified sizes.
  void fill_msg(typename Struc_channel_t::Msg_out& msg, uint64_t cool_val,
                size_t n_uint64s, size_t n_uint64s_2 = 0)
  {
    std::cout << "fill_msg: coolVal=[" << cool_val << "] payload=[" << n_uint64s
              << "] payload2=[" << n_uint64s_2 << "].\n" << std::flush;
    auto root = msg.body_root()->initCoolReq();
    root.setCoolVal(cool_val);
    if (n_uint64s > 0)
    {
      auto payload = root.initPayload(n_uint64s);
      for (size_t i = 0; i < n_uint64s; ++i)
      {
        payload.set(i, i);
      }
    }
    if (n_uint64s_2 > 0)
    {
      auto payload2 = root.initPayload2(n_uint64s_2);
      for (size_t i = 0; i < n_uint64s_2; ++i)
      {
        payload2.set(i, i + n_uint64s);
      }
    }
  }

  /* Helper: compare two capnp List(UInt64)s element-by-element; break on first mismatch.
   * Each of List1 and List2 can refer to List::Builder or List::Reader and will be treated conceptually as only the
   * latter. */
  template<typename List1, typename List2>
  void check_list(List1 got_list, List2 sent_list, const char* name)
  {
    ASSERT_EQ(got_list.size(), sent_list.size()) << name << " size mismatch.";
    for (size_t i = 0; i < sent_list.size(); ++i)
    {
      EXPECT_EQ(got_list[i], sent_list[i]) << name << "[" << i << "] mismatch.";
      if (got_list[i] != sent_list[i]) { break; } // Don't flood output.
    }
  }

  /* Helper: compare a received CoolReq against a sent CoolReq (coolVal + payload contents).
   * Each of `got` and `sent` can refer to Builder or Reader and will be treated conceptually as only the latter.
   * (Can also be more rigid and require e.g. 2 `Reader`s and require use of .asReader() by the caller in the case
   * of `sent`. That should be fine and not require any significant memory use or compute; I (ygoldfel) just chose to
   * keep entropy lower arguably. Or not. Either way's fine!) */
  template<typename ReaderOrBuilder1, typename ReaderOrBuilder2>
  void check_cool_req(ReaderOrBuilder1 got, ReaderOrBuilder2 sent)
  {
    EXPECT_EQ(got.getCoolVal(), sent.getCoolVal()) << "coolVal mismatch.";

    if (sent.hasPayload()) { check_list(got.getPayload(), sent.getPayload(), "payload"); }
    else { EXPECT_FALSE(got.hasPayload()) << "Unexpected payload."; }

    if (sent.hasPayload2()) { check_list(got.getPayload2(), sent.getPayload2(), "payload2"); }
    else { EXPECT_FALSE(got.hasPayload2()) << "Unexpected payload2."; }
  }

  // Helper: mutate only the scalar coolVal (no body re-init); prints similar to fill_msg().
  void set_cool_val(typename Struc_channel_t::Msg_out& msg, uint64_t cool_val)
  {
    std::cout << "set_cool_val: coolVal=[" << cool_val << "].\n" << std::flush;
    msg.body_root()->getCoolReq().setCoolVal(cool_val);
  }

  // White-box facade: expose private Msg_in internals (mdt_root()) for verification.
  using Msg_in_impl_t = Msg_in_impl<typename Struc_channel_t::Msg_in>;

  /* Helper: white-box check of the MDT (metadata) of a received Msg_in.
   * expected_n_split_segs: 0 = no splitting (splitSegmentsOrNull is null); N > 0 = N entries in the list. */
  void check_mdt(const typename Struc_channel_t::Msg_in& rcvd, size_t expected_n_split_segs)
  {
    Msg_in_impl_t rcvd_impl{const_cast<typename Struc_channel_t::Msg_in&>(rcvd)};

    const auto body_ser_info = rcvd_impl.mdt_root().getBodySerializationInfo();
    std::cout << "bodySerInfo = [" << kj::str(body_ser_info).cStr() << "].\n" << std::flush;

    if (expected_n_split_segs == 0)
    {
      EXPECT_FALSE(body_ser_info.hasSplitSegmentsOrNull()) << "Expected no split_segs; got some.";
    }
    else
    {
      ASSERT_TRUE(body_ser_info.hasSplitSegmentsOrNull()) << "Expected split_segs; got null.";
      EXPECT_EQ(body_ser_info.getSplitSegmentsOrNull().size(), expected_n_split_segs)
        << "split_segs list size mismatch.";
    }
  }

  /* Helper: send a Msg_out from cli, receive on srv, and verify the received body matches
   * the sent Msg_out (coolVal + payload contents) and the MDT split structure.
   * If originating_msg is non-null, send as a response to that message.
   * expected_n_split_segs: white-box MDT check (0 = no splitting, N = N split_segs entries). */
  void send_and_verify(Struc_channel_t& cli, Struc_channel_t& srv,
                       typename Struc_channel_t::Msg_out& msg, size_t expected_n_split_segs,
                       const typename Struc_channel_t::Msg_in* originating_msg = nullptr)
  {
    std::cout << "  Sending.\n" << std::flush;

    const auto sent = msg.body_root()->getCoolReq();

    boost::promise<void> received;
    typename Struc_channel_t::Msg_in_ptr rcvd;

    srv.expect_msg(Body::COOL_REQ, [&](auto&& in_msg)
    {
      rcvd = std::move(in_msg);
      received.set_value();
    });

    Error_code err;
    const auto ok = cli.send(&msg, originating_msg, &err);
    ASSERT_TRUE(ok) << "send() returned false (Channel not started or something?).";
    ASSERT_FALSE(err) << "send() error: [" << err << "] [" << err.message() << "].";

    auto fut = received.get_future();
    const auto status = fut.wait_for(TIMEOUT);
    ASSERT_EQ(status, boost::future_status::ready) << "Timed out waiting for message.";
    fut.get();

    std::cout << "  Verifying payload; + split-segs (white-box): [" << expected_n_split_segs << "] "
                 "split-segs; " << std::flush;
    check_cool_req(rcvd->body_root().getCoolReq(), sent);
    check_mdt(*rcvd, expected_n_split_segs);
  }
} // namespace (anon)

/* Tests reuse of a Msg_out across multiple send() calls with heap-backed serialization.
 *
 * Group 1 -- MDT content changes: same Msg_out sent as unsolicited, then as response (different
 * originatingMessageIdOrNone in the MDT header).
 *
 * Group 2 -- Split-structure transitions: exercises the MDT-builder-reuse and fallback paths in
 * Msg_out::emit_serialization() / load_mdt().  Each transition pair uses a fresh Msg_out; the
 * first send establishes the baseline split structure, and the second send exercises reuse (same
 * structure, only scalar coolVal changed) or fallback (different structure).
 *
 * Split layout legend (each letter = a distinct splitSegmentsOrNull layout):
 *   - no-split: message fits in seg 0; no split records.
 *   - A: payload exceeds per-segment cap; split_segs has 1 entry.
 *   - B: two segments each exceed cap; split_segs has 2 entries.
 *     (B arises naturally from A via body re-init: the orphaned old segment still exceeds the cap.)
 *
 * Note: transitions that would require *shrinking* the split structure on a reused Msg_out (e.g.,
 * A -> no-split) are not feasible to test cleanly, because capnp's MessageBuilder never reclaims
 * orphaned segment space.  The dedicated splitting-arithmetic test case(s) (planned as of this writing) covers
 * the splitting logic itself independently of Msg_out reuse.
 *
 * The test verifies that every send is received correctly, proving the reuse/fallback logic works. */
TEST(Msg_out, Reuse_heap_backed)
{
  std::atomic<bool> cli_err{false};
  std::atomic<bool> srv_err{false};
  auto pair = make_session_struc_pair<Body, TEST_MQ_TYPE, TEST_HANDLES>
                ([&](const Error_code&) { cli_err = true; },
                 [&](const Error_code&) { srv_err = true; });

  // Per-segment cap is essentially the transport's send_blob_max_size() (~64Ki for UDS).
  const size_t max_sz = pair.m_cli->owned_channel()->send_blob_max_size();
  ASSERT_GT(max_sz, 0u) << "Could not determine transport max blob size.";
  std::cout << "Per-segment cap (send_blob_max_size): [" << max_sz << "] bytes.\n" << std::flush;

  /* Compute payload sizes (in uint64_t count) to produce various split layouts.
   * Each uint64_t is 8 bytes in the capnp list.  To force a segment > max_sz,
   * the payload must exceed max_sz bytes.  capnp allocates seg 1 for the payload
   * when seg 0 can't hold it; the MessageBuilder must give at least what capnp asks for. */
  const size_t elems_per_max = max_sz / sizeof(uint64_t);

  // no-split: small payload that fits in seg 0 (~4Ki initial).
  constexpr size_t SZ_NO_SPLIT = 10;

  // A: payload ~1.25x max_sz => seg 1 > max_sz => split into 2 sub-segs.  split_segs: 1 entry.
  const size_t SZ_A = elems_per_max + (elems_per_max / 4);

  // B: two large payloads (payload + payload2), each forcing its own capnp segment to exceed
  // max_sz => 2 split records in split_segs.  Use fill_msg(msg, val, SZ_A, SZ_A).

  // Create the Msg_out we'll reuse throughout.
  auto msg = pair.m_cli->create_msg();

  // --- Group 1: MDT content changes (unsolicited vs response). ---
  std::cout << "Group 1: unsolicited vs. response reuse.\n" << std::flush;

  // Send 1: unsolicited, small payload.
  fill_msg(msg, 1, SZ_NO_SPLIT);
  { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, msg, 0); }

  /* Send 2: as a response.  The server sends a request via sync_request() which blocks until
   * the client's response arrives.  The client's expect_msg handler fires on its worker thread,
   * fills the reused msg, and sends the response -- all while sync_request blocks. */
  {
    typename Struc_channel_t::Msg_in_ptr originating;
    boost::promise<void> replied;

    // Client handler: on receiving the server's request, fill the reused msg and reply.
    pair.m_cli->expect_msg(Body::COOL_REQ, [&](auto&& in_msg)
    {
      originating = std::move(in_msg);
      fill_msg(msg, 2, SZ_NO_SPLIT);
      pair.m_cli->send(&msg, originating.get());
      replied.set_value();
    });

    // Server sends request and synchronously waits for the response.
    auto srv_req = pair.m_srv->create_msg();
    srv_req.body_root()->initCoolReq().setCoolVal(999);
    Error_code err;
    auto response = pair.m_srv->sync_request(&srv_req, nullptr, TIMEOUT, &err);
    ASSERT_FALSE(err) << "sync_request() error: [" << err << "] [" << err.message() << "].";
    ASSERT_TRUE(response) << "sync_request() returned null.";

    /* The handler's `msg` writes are in fact done by now (the response could not have arrived otherwise) --
     * but that ordering flows through kernel I/O which TSAN cannot see as a happens-before edge; so before
     * touching `msg` from this thread again, sync on the promise, which TSAN does see. */
    auto replied_fut = replied.get_future();
    const auto replied_status = replied_fut.wait_for(TIMEOUT);
    ASSERT_EQ(replied_status, boost::future_status::ready) << "Timed out waiting for client handler to finish.";
    replied_fut.get();

    // Verify response contents and MDT match what was sent.
    check_cool_req(response->body_root().getCoolReq(), msg.body_root()->getCoolReq());
    check_mdt(*response, 0);
  }

  // Send 3: unsolicited again (originatingMessageIdOrNone reverts to 0).
  fill_msg(msg, 3, SZ_NO_SPLIT);
  { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, msg, 0); }

  /* --- Group 2: Split-structure transitions. ---
   * Each transition pair uses a fresh Msg_out: the first send establishes the baseline split
   * structure (populating the MDT builder); the second send exercises reuse or fallback.
   * We use a fresh Msg_out per pair because capnp's MessageBuilder accumulates orphaned
   * segments on body re-init (initCoolReq()) -- orphaned space is permanently wasted, leading
   * to unpredictable builder state if reused many times with large payloads.  (To be clear, avoiding initCoolReq()
   * but trying to "change" the list size has the ~same effect; list size cannot be actually changed; replacing
   * an existing list with anything (even nothing) orphans the list as opposed to the space being reused.)
   *
   * Please understand that the white-box check of the split-structure (last arg to send_and_verify()) exists not
   * to verify the internals of that algorithm (not that that's bad per se) but rather to ensure that we are
   * *in fact* testing what we think we're testing, in terms of the first-send and second-send split-structure. */
  std::cout << "Group 2: split-structure transitions.\n" << std::flush;

  // no-split -> no-split (trivial reuse).  The 2nd send just merely changes the scalar coolVal.
  {
    auto m = pair.m_cli->create_msg();
    fill_msg(m, 10, SZ_NO_SPLIT); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 0); }
    set_cool_val(m, 11); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 0); }
  }

  // no-split -> A (split_segs 0 -> 1 entry; reuse fails, fresh builder).
  {
    auto m = pair.m_cli->create_msg();
    fill_msg(m, 12, SZ_NO_SPLIT); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 0); }
    // Note: This will orphan the small (SZ_NO_SPLIT) list.  A bit sloppy of us but doesn't affect accuracy of test.
    fill_msg(m, 13, SZ_A); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 1); }
  }

  /* A -> A (same split structure; reuse succeeds).
   * Only mutate the scalar coolVal -- avoid re-initializing the payload (which would orphan
   * the old segment and change the split structure). */
  {
    auto m = pair.m_cli->create_msg();
    fill_msg(m, 14, SZ_A); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 1); }
    set_cool_val(m, 15); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 1); }
  }

  /* A -> B (1 -> 2 split_segs entries; reuse fails, fresh builder).
   * The second fill_msg re-inits the body, orphaning the old payload segment; the orphaned
   * segment plus the new one both exceed the cap, producing 2 split records. */
  {
    auto m = pair.m_cli->create_msg();
    fill_msg(m, 16, SZ_A); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 1); }
    fill_msg(m, 17, SZ_A); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 2); }
  }

  /* B -> B (same split structure; reuse succeeds).
   * Same approach as A -> A: only mutate the scalar. */
  {
    auto m = pair.m_cli->create_msg();
    fill_msg(m, 18, SZ_A, SZ_A); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 2); }
    set_cool_val(m, 19); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 2); }
  }

  // no-split -> B (0 -> 2 entries; reuse fails, fresh builder).
  {
    auto m = pair.m_cli->create_msg();
    fill_msg(m, 38, SZ_NO_SPLIT); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 0); }
    fill_msg(m, 39, SZ_A, SZ_A); { FLOW_TEST_TRACE(); send_and_verify(*pair.m_cli, *pair.m_srv, m, 2); }
  }

  EXPECT_FALSE(cli_err) << "Client channel error handler fired.";
  EXPECT_FALSE(srv_err) << "Server channel error handler fired.";
}

/* Tests the splitting arithmetic in Heap_fixed_builder::emit_serialization() and the corresponding
 * reassembly in Heap_reader::deserialization().  Each case sends a message with a payload sized to
 * produce a specific split structure and verifies content + white-box split metadata.
 *
 * For a payload in seg 1 (no frame header), given seg_and_hdr_sz_cap = max_sz:
 *   - seg_sz <= max_sz: no split.
 *   - seg_sz > max_sz: split into sub-seg 1 (max_sz bytes) + n_full full continuation sub-segs
 *     (max_sz bytes each) + possibly 1 partial sub-seg.
 *   - n_cont_subsegs = n_full + (partial ? 1 : 0).
 *   - n_full = (seg_sz - max_sz) / max_sz.
 *   - partial = (seg_sz - max_sz) % max_sz.
 *
 * Since each List(UInt64) element is 8 bytes, we use element count to target specific n_full / partial
 * combinations.  There may be a small fixed overhead per capnp segment (a few words for far-pointer
 * landing pads), but it is negligible relative to max_sz and does not change which code paths execute. */
TEST(Msg_out, Split_edge_cases)
{
  std::atomic<bool> cli_err{false};
  std::atomic<bool> srv_err{false};
  auto pair = make_session_struc_pair<Body, TEST_MQ_TYPE, TEST_HANDLES>
                ([&](const Error_code&) { cli_err = true; },
                 [&](const Error_code&) { srv_err = true; });

  const size_t max_sz = pair.m_cli->owned_channel()->send_blob_max_size();
  ASSERT_GT(max_sz, 0u) << "Could not determine transport max blob size.";
  std::cout << "Per-segment cap (send_blob_max_size): [" << max_sz << "] bytes.\n" << std::flush;

  const size_t elems_per_max = max_sz / sizeof(uint64_t);

  /* White-box check of the full split structure, including per-record n_cont_subsegs values.
   * expected_n_cont_per_rec: empty => no splitting; each element => one split record's n_cont_subsegs. */
  const auto check_split_structure
    = [](const typename Struc_channel_t::Msg_in& rcvd,
         std::initializer_list<size_t> expected_n_cont_per_rec)
  {
    Msg_in_impl_t rcvd_impl{const_cast<typename Struc_channel_t::Msg_in&>(rcvd)};
    const auto body_ser_info = rcvd_impl.mdt_root().getBodySerializationInfo();
    if (expected_n_cont_per_rec.size() == 0)
    {
      EXPECT_FALSE(body_ser_info.hasSplitSegmentsOrNull()) << "Expected no split_segs; got some.";
      return;
    }
    // else

    ASSERT_TRUE(body_ser_info.hasSplitSegmentsOrNull()) << "Expected split_segs; got null.";
    const auto split_segs_list = body_ser_info.getSplitSegmentsOrNull();
    ASSERT_EQ(split_segs_list.size(), expected_n_cont_per_rec.size()) << "split_segs list size mismatch.";

    size_t idx = 0;
    for (const auto expected_n_cont : expected_n_cont_per_rec)
    {
      const auto subseg = split_segs_list[idx];
      static_assert(sizeof(subseg) == 2,
                    "Tweak this and next statements; apparently changing bit-width of SplitSegment in schema.");
      const auto n_cont = size_t(subseg >> 8);

      EXPECT_EQ(n_cont, expected_n_cont)
        << "split_segs[" << idx << "].nContSubsegs mismatch.";
      ++idx;
    }
  };

  /* Send a fresh message, receive it, verify contents and full split structure. */
  const auto send_and_check
    = [&](uint64_t cool_val, size_t n_uint64s, size_t n_uint64s_2,
          std::initializer_list<size_t> expected_n_cont_per_rec)
  {
    auto m = pair.m_cli->create_msg();
    fill_msg(m, cool_val, n_uint64s, n_uint64s_2);

    const auto sent = m.body_root()->getCoolReq();

    boost::promise<void> received;
    typename Struc_channel_t::Msg_in_ptr rcvd;
    pair.m_srv->expect_msg(Body::COOL_REQ, [&](auto&& in_msg)
    {
      rcvd = std::move(in_msg);
      received.set_value();
    });

    Error_code err;
    const auto ok = pair.m_cli->send(&m, nullptr, &err);
    ASSERT_TRUE(ok) << "send() returned false.";
    ASSERT_FALSE(err) << "send() error: [" << err << "] [" << err.message() << "].";

    auto fut = received.get_future();
    const auto status = fut.wait_for(TIMEOUT);
    ASSERT_EQ(status, boost::future_status::ready) << "Timed out waiting for message.";
    fut.get();

    std::cout << "  Verifying contents; and white-box split-structure: ["
              << expected_n_cont_per_rec.size() << "] split-segs.\n" << std::flush;
    check_cool_req(rcvd->body_root().getCoolReq(), sent);
    check_split_structure(*rcvd, expected_n_cont_per_rec);
  };

  // --- No split: small payload fits in seg 0. ---
  { FLOW_TEST_TRACE(); send_and_check(100, 10, 0, {}); }

  // --- Just over 1x cap: n_full=0, partial>0.  n_cont=1. ---
  { FLOW_TEST_TRACE(); send_and_check(101, elems_per_max + 1, 0, {1}); }

  // --- Just over 2x cap: n_full=1, partial>0.  n_cont=2.  Exercises for-loop body once. ---
  { FLOW_TEST_TRACE(); send_and_check(102, 2 * elems_per_max + 1, 0, {2}); }

  // --- Just over 3x cap: n_full=2, partial>0.  n_cont=3.  Exercises for-loop body twice. ---
  { FLOW_TEST_TRACE(); send_and_check(103, 3 * elems_per_max + 1, 0, {3}); }

  // --- Two independently split payloads: each just over 1x cap.  2 split records, each n_cont=1. ---
  // Exercises backward iteration over split_segs during reassembly.
  { FLOW_TEST_TRACE(); send_and_check(104, elems_per_max + 1, elems_per_max + 1, {1, 1}); }

  // --- Two split payloads of different sizes: just-over-1x + just-over-2x. ---
  // 2 split records with different n_cont values; reassembly must handle each independently.
  { FLOW_TEST_TRACE(); send_and_check(105, elems_per_max + 1, 2 * elems_per_max + 1, {1, 2}); }

  /* @todo *Maybe* try to force some edge condition(s), the main one coming to mind being variations with
   * partial=0 which is a small code-path we don't exercise above most likely.  It would be difficult:
   * there is some overhead on top of the actual List(UInt64) payload; both internal capnp overhead and
   * the additional scalar payloads.  It might be doable to experimentally find out the overhead(s) involved
   * and then size payload(s) based on that.  Even observing the results is non-trivial, as [sub-]segment sizes
   * are not in the MDT (which we access white-boxily already partially) but rather = IPC-datagram sizes.
   * We might have to scan logs or something.  Priority: not high... but it'd be nice.
   *
   * @todo There is also special logic for the leading (mandatory) segment, as that one must contend with the
   * MDT header which affects the arithmetic.  (To some extent we are exercising *some* of that arithmetic now.)
   * That is it'd be good to test having to split the leading segment... except in reality the initial segment
   * is *always*, due to (frankly helpful) capnp mechanics, of a certain known length, created before any
   * mutation occurs.  While that init-seg-sz *is* configurable, there might be guards against that thing exceeding
   * the max-sz... as there should be.  Priority: low... not only is it hard-to-impossible to trigger, but it
   * won't happen in reality either.  So it's academic in practice.  Though, we do want our algorithms to be robust. */

  EXPECT_FALSE(cli_err) << "Client channel error handler fired.";
  EXPECT_FALSE(srv_err) << "Server channel error handler fired.";
}

/* Probe the real split-segment-record capacity of the fixed-size mdt header
 * (BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL bytes).  The engine detects overflow adaptively -- the
 * capped mdt-header builder runs out of space => load_mdt() emits SERIALIZE_FAILED_SPLIT_SEGS_TOO_MANY -- so
 * there is no formula to verify per se.  Instead: guard against the capacity backsliding (e.g., schema growth
 * silently eating the space); and notice if it improves (then bump the expected constant below, happily). */
TEST(Load_mdt, Split_segs_capacity)
{
  using std::array;
  using Word = ::capnp::word;

  constexpr size_t HDR_SZ = BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL;
  /* Known-good as of this writing; see the EXPECTs below.  For the curious, the arithmetic behind the `20`
   * (wire format: https://capnproto.org/encoding.html; struct word-counts are also obtainable, constexpr,
   * via capnp::sizeInWords<T>()):
   *   - 128-byte header = 16 words (capnp word = 8 bytes).
   *   - Fixed part = 11 words: 1 = the root pointer; 5 = StructuredMessage (3 data words: id +
   *     originatingMessageIdOrNone + padded union discriminant; 2 pointer words: authHeader + the shared
   *     union-member slot); 1 = AuthHeader (1 pointer, to:); 2 = Uuid (2 data); 2 = BodySerializationInfo
   *     (1 data + 1 pointer).
   *   - Remaining 5 words = 40 bytes hold the split-segs list: a *primitive* list (no tag word, unlike a
   *     struct-list), elements packed at sizeof(SplitSegment = UInt16) = 2 bytes => floor(40 / 2) = 20.
   * If the schema grows, the fixed part grows, and the capacity shrinks -- the backslide the EXPECT...() below
   * would catch.  Meanwhile, as noted, calculations like this can be done programmatically and even at
   * compile time, sizeInWords<T>() being a nice tool.  (Reiterating, though: load_mdt() does not rely on any
   * such thing and just complains when out of space, whenever that happens.  This test merely ensures that point
   * does not change without our noticing.) */
  constexpr size_t EXPECTED_CAPACITY = 20;

  // Run a fresh load_mdt() (user-message form) with the given # of split-seg records; return emitted code.
  const auto err_from_n_recs = [&](size_t n_recs) -> Error_code
  {
    array<Word, HDR_SZ / sizeof(Word)> hdr_buf; // Word-aligned by element type, as capnp requires.
    Capped_sz_capnp_message_builder mdt_builder{Blob_mutable{hdr_buf.data(), sizeof(hdr_buf)},
                                                true}; // Zero it: array<> does not.
    const Split_segments split_segs(n_recs, Split_segment{ 1, 1 }); // Contents don't matter; just fit the wire type.
    Error_code err_code;
    EXPECT_TRUE(load_mdt(&mdt_builder, nullptr, &err_code,
                         Session_token{}, // Nil token.
                         0, // Not a response.
                         1, // User-message ID.
                         n_recs + 1, // Claim plausible seg-count; not checked against the list anyway.
                         split_segs.empty() ? nullptr : &split_segs))
      << "Builder is fresh each time; the reuse-impossible (false) return should not occur.";
    return err_code;
  };

  size_t capacity = 0;
  while (!err_from_n_recs(capacity + 1))
  {
    ++capacity;
    ASSERT_LT(capacity, 10 * EXPECTED_CAPACITY) << "Runaway probe; something is quite broken.";
  }
  std::cout << "Probed mdt-header split-seg record capacity: [" << capacity << "] "
               "(header size [" << HDR_SZ << "]).\n";

  EXPECT_EQ(capacity, EXPECTED_CAPACITY)
    << "If lower: schema/layout changes ate mdt-header space; investigate before blessing.  "
       "If higher: nice!  Update EXPECTED_CAPACITY to match.";
  EXPECT_EQ(err_from_n_recs(capacity + 1), error::Code::S_SERIALIZE_FAILED_SPLIT_SEGS_TOO_MANY);
  EXPECT_FALSE(err_from_n_recs(capacity));
}

/* Direct (if minimal) coverage of Capped_sz_capnp_message_builder/reader as such.  These two are exercised
 * constantly through every mdt header built/received, and the builder's throw-on-overflow behavior is pinned by
 * Split_segs_capacity above; so what this adds is (1) failure locality (a regression fails here, not as mystery
 * mdt corruption in some struc test); and (2) the one behavior nothing else exercises: a non-word-multiple
 * buffer size is rounded *down* to a whole word count (production callers only ever pass word multiples). */
TEST(Capped_sz, Round_trip_and_word_rounding)
{
  using flow::error::Runtime_error;
  using std::array;
  using Word = ::capnp::word;

  array<Word, 64> buf; // Reused throughout; comfortably exceeds all sizes probed below.

  // Round-trip at an exact-multiple size: build a small message; read it back through the reader.
  {
    Capped_sz_capnp_message_builder builder{Blob_mutable{buf.data(), sizeof(buf)}, true};
    auto root = builder.initRoot<Body>().initCoolReq();
    root.setCoolVal(77);
    auto payload = root.initPayload(3);
    for (size_t idx = 0; idx != 3; ++idx)
    {
      payload.set(idx, 100 + idx);
    }

    /* Reader may be given the whole buffer (trailing unused space is ignored by capnp) -- the same property
     * the mdt-header machinery relies on. */
    Capped_sz_capnp_message_reader reader{util::Blob_const{buf.data(), sizeof(buf)}};
    const auto rt_root = reader.getRoot<Body>().getCoolReq();
    EXPECT_EQ(rt_root.getCoolVal(), 77u);
    ASSERT_EQ(rt_root.getPayload().size(), 3u);
    EXPECT_EQ(rt_root.getPayload()[2], 102u);
  }

  /* Word-rounding: find the max List(UInt64) (1 word/element) buildable at a given byte-size cap.
   * A capacity of K words must yield the same max as K words + 7 bytes (round-down proof); and exactly
   * one more element at K+1 words (element granularity = word). */
  const auto max_payload_n = [&](size_t buf_sz) -> size_t
  {
    for (size_t n = 0; ; ++n)
    {
      Capped_sz_capnp_message_builder builder{Blob_mutable{buf.data(), buf_sz}, true};
      try
      {
        builder.initRoot<Body>().initCoolReq().initPayload(n);
      }
      catch (const Runtime_error& exc)
      {
        EXPECT_EQ(exc.code(), error::Code::S_INVALID_ARGUMENT);
        EXPECT_GT(n, 0u) << "Even an empty payload list did not fit; buffer too small for the fixed overhead.";
        return n - 1;
      }
      if (n >= buf.size())
      {
        EXPECT_TRUE(false) << "Runaway probe; something is quite broken.";
        return 0;
      }
    }
  };

  constexpr size_t K = 16; // Words.  (Arbitrary; small enough to keep the linear probes snappy.)
  const auto max_at_k = max_payload_n(K * sizeof(Word));
  std::cout << "Max payload elements at [" << K << "]-word cap: [" << max_at_k << "].\n";
  EXPECT_EQ(max_payload_n(K * sizeof(Word) + 7), max_at_k); // The 7 extra bytes must buy nothing.
  EXPECT_EQ(max_payload_n((K + 1) * sizeof(Word)), max_at_k + 1); // A full extra word buys exactly 1 element.
}

namespace // Helpers for the direct (channel-less) Msg_out/Msg_in object-semantics tests below.
{
  using Msg_out_t = Struc_channel_t::Msg_out;
  using Msg_in_t = Struc_channel_t::Msg_in;

#ifndef FLOW_OS_LINUX
  static_assert(false, "Native FD wrangling tested in Linux only in this context, "
                       "though anything POSIXy should be OK.  Windows is more of an open question generally.");
#endif

  // Is the FD open, as far as the OS is concerned?
  bool fd_is_open(handle_t fd)
  {
    return ::fcntl(fd, F_GETFD) != -1;
  }

  // Heap-backed Msg_out ctor arg.  Zero frame prefix: these messages are never sent, so no mdt header needed.
  Msg_out_t::Builder_config make_out_cfg()
  {
    return { nullptr, 8 * 1024, 8 * 1024, 0, nullptr };
  }
} // namespace

/* The stored-Native_handle ownership contract of Msg_out, as such (transmission of handles is separately
 * exercised in channel_test et al; here we pin the object-level semantics, largely of util::Own_native_handle
 * vintage): dtor returns the handle to the OS; replacement returns the replaced one; storing the same handle
 * value is the documented no-op (not returned to OS; source arg nulled). */
TEST(Msg_out, Native_handle_lifecycle)
{
  // Dtor returns the stored handle to the OS.
  const handle_t fd_a = ::dup(STDOUT_FILENO);
  ASSERT_TRUE(fd_is_open(fd_a));
  {
    Msg_out_t msg{make_out_cfg()};
    EXPECT_TRUE(msg.native_handle_or_null().null());
    msg.store_native_handle_or_null(Native_handle{fd_a});
    EXPECT_EQ(msg.native_handle_or_null().m_native_handle, fd_a);
    EXPECT_TRUE(fd_is_open(fd_a)); // Stored, not closed.
  }
  EXPECT_FALSE(fd_is_open(fd_a)); // Dtor returned it to OS.

  // Replacement returns the replaced handle; unloading via null returns the last one.
  const handle_t fd_b = ::dup(STDOUT_FILENO);
  const handle_t fd_c = ::dup(STDOUT_FILENO);
  {
    Msg_out_t msg{make_out_cfg()};
    msg.store_native_handle_or_null(Native_handle{fd_b});
    msg.store_native_handle_or_null(Native_handle{fd_c});
    EXPECT_FALSE(fd_is_open(fd_b)); // Replaced => returned to OS.
    EXPECT_TRUE(fd_is_open(fd_c));

    // Same-handle-value store: documented no-op -- stored handle survives; the (equal) source arg is nulled.
    Native_handle same{fd_c};
    msg.store_native_handle_or_null(std::move(same));
    EXPECT_TRUE(same.null());
    EXPECT_TRUE(fd_is_open(fd_c));
    EXPECT_EQ(msg.native_handle_or_null().m_native_handle, fd_c);

    msg.store_native_handle_or_null({}); // Unload.
    EXPECT_FALSE(fd_is_open(fd_c));
    EXPECT_TRUE(msg.native_handle_or_null().null());
  }
}

/* Ditto for Msg_in: an unemitted stored handle is returned to the OS by the dtor; an emitted one is disowned -- the
 * caller owns it thereafter. */
TEST(Msg_in, Native_handle_lifecycle)
{
  // Store; never emit: dtor must return it to the OS (avoid the potential leak).
  const handle_t fd_a = ::dup(STDOUT_FILENO);
  {
    // Msg_in ctor is private (Flow-IPC constructs these); go through the attorney a-la channel internals.
    const auto in_msg = Msg_in_impl_t::ct_base(Msg_in_t::Reader_config{ nullptr, 1, nullptr });
    Msg_in_impl_t{*in_msg}.store_native_handle_or_null(Native_handle{fd_a});
    EXPECT_TRUE(fd_is_open(fd_a));
  }
  EXPECT_FALSE(fd_is_open(fd_a));

  // Store; emit: we own it now; dtor must leave it alone.
  const handle_t fd_b = ::dup(STDOUT_FILENO);
  {
    const auto in_msg = Msg_in_impl_t::ct_base(Msg_in_t::Reader_config{ nullptr, 1, nullptr });
    Msg_in_impl_t{*in_msg}.store_native_handle_or_null(Native_handle{fd_b});
    const auto hndl = in_msg->emit_native_handle_or_null();
    EXPECT_EQ(hndl.m_native_handle, fd_b);
  }
  EXPECT_TRUE(fd_is_open(fd_b)); // Dtor did not touch the emitted (disowned) handle.
  Native_handle{fd_b}.close();
}

/* Msg_out move ops (`= default` as of this writing): the moved-to object owns the body and the stored handle;
 * the moved-from object's dtor disturbs neither; move-assignment returns the target's own prior handle to
 * the OS. */
TEST(Msg_out, Move_semantics)
{
  const handle_t fd_a = ::dup(STDOUT_FILENO);

  // Move-ction.
  auto src = std::make_unique<Msg_out_t>(make_out_cfg());
  src->body_root()->initCoolReq().setCoolVal(42);
  src->store_native_handle_or_null(Native_handle{fd_a});

  Msg_out_t dst{std::move(*src)};
  src.reset(); // Moved-from dtor runs: must close nothing, affect nothing.
  EXPECT_TRUE(fd_is_open(fd_a));
  EXPECT_EQ(dst.native_handle_or_null().m_native_handle, fd_a);
  EXPECT_EQ(dst.body_root()->getCoolReq().getCoolVal(), 42u);

  // Move-assignment: target's own handle is returned to the OS; source's contents transfer.
  const handle_t fd_b = ::dup(STDOUT_FILENO);
  {
    Msg_out_t dst_2{make_out_cfg()};
    dst_2.store_native_handle_or_null(Native_handle{fd_b});

    dst_2 = std::move(dst);
    EXPECT_FALSE(fd_is_open(fd_b)); // Its old handle: returned to OS by the assignment.
    EXPECT_TRUE(fd_is_open(fd_a));
    EXPECT_EQ(dst_2.native_handle_or_null().m_native_handle, fd_a);
    EXPECT_EQ(dst_2.body_root()->getCoolReq().getCoolVal(), 42u);
  }
  EXPECT_FALSE(fd_is_open(fd_a)); // And the transferred handle: returned by dst_2 dtor at scope end.
}

/* The advanced (Builder-subsuming) ctor form, both sub-modes: pre-root-initialized Builder (contents survive,
 * remain mutable) and un-root-initialized Builder (ctor initRoot<Body>()s; starts blank, mutable).
 *
 * transport_test as of this writing also exercises this feature. */
TEST(Msg_out, Advanced_ctor)
{
  // Sub-mode A: root already initialized (and partially filled) before Msg_out subsumes the Builder.
  {
    Msg_out_t::Builder builder{make_out_cfg()};
    builder.payload_msg_builder()->initRoot<Body>().initCoolReq().setCoolVal(7);

    Msg_out_t msg{std::move(builder)};
    EXPECT_EQ(msg.body_root()->getCoolReq().getCoolVal(), 7u);
    msg.body_root()->getCoolReq().setCoolVal(8); // Still mutable via body_root().
    EXPECT_EQ(msg.body_root()->getCoolReq().getCoolVal(), 8u);
  }

  // Sub-mode B: not root-initialized: ctor performs initRoot<Body>(); blank but usable as with vanilla ctor.
  {
    Msg_out_t::Builder builder{make_out_cfg()};
    Msg_out_t msg{std::move(builder)};
    msg.body_root()->initCoolReq().setCoolVal(9);
    EXPECT_EQ(msg.body_root()->getCoolReq().getCoolVal(), 9u);
  }
}

/* The orphanage() bottom-up technique: build a leaf detached, adopt it into the message tree, verify contents.
 * transport_test as of this writing also exercises this feature. */
TEST(Msg_out, Orphanage_build)
{
  Msg_out_t msg{make_out_cfg()};
  auto root = msg.body_root()->initCoolReq();

  auto orphan = msg.orphanage().newOrphan<::capnp::List<uint64_t>>(4);
  {
    auto orphan_list = orphan.get();
    for (size_t idx = 0; idx != 4; ++idx)
    {
      orphan_list.set(idx, 50 + idx);
    }
  }
  root.adoptPayload(std::move(orphan));

  ASSERT_TRUE(root.hasPayload());
  ASSERT_EQ(root.getPayload().size(), 4u);
  EXPECT_EQ(root.getPayload()[3], 53u);
}

} // namespace ipc::transport::struc::test
