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

#pragma once

/* Test battery: the shared substance of the similarly named *.cpp files (siblings) -- not an API header.
 * Most test bodies below are function templates over a certain <knob> (ShmType, mostly); the sibling
 * .cpp s spread the TEST()s expanding them across translation units.  This bounds compiler RAM use per
 * .cpp (translation unit) versus simply placing everything into one .cpp.  In particular each per-<knob>
 * ipc::session + struc::Channel stack costs GBs of compiler RAM, for debug-info builds, at least with
 * some Linux gcc.  2+ such translation units compiling concurrently can exhaust a smaller build machine
 * (including GitHub CI runners in 2026). */

#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/transport/struc/test/test_schema.capnp.h"
#include "ipc/transport/struc/serializer_stats.hpp"
#include "ipc/transport/struc/heap_serializer.hpp"
#include "ipc/transport/struc/shm/serializer_stats.hpp"
#include "ipc/shm/classic/classic_fwd.hpp"
#include "ipc/transport/struc/struc_fwd.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util.hpp>
#include <flow/log/log.hpp>
#include <flow/test/test_common_util.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <flow/util/stat/stat_set_list.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/future.hpp>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <sstream>

/* Some terminology in comments/logs of this test:
 *   - PH = Pure-Heap: Send struc::Channel messages/serializing struc::Msg or direct Struct_builder objects (etc.)
 *     backed by the heap only.  So Struct_builder = Heap_fixed_builder, Struct_reader = Heap_reader, and internally
 *     there is only the one layer of serialization, directly in heap, of the user's capnp payload.  Therefore there's
 *     one Serializer_stats struct.
 *   - SH = SHM/Heap: ...ditto... backed by SHM (with heap -- as always -- used as the IPC-transmitted envelope).
 *     So Struct_builder = shm::Builder, Struct_reader = shm::Reader, and internally there are the following
 *     (therefore a separate Serializer_stats struct for each; and both are separate from PH's, although built
 *     internally by the same code).
 *     - SH-Core or Core: The actual user's capnp payload, in SHM.
 *     - SH-Outer or Outer: As maintained by an internal Heap_fixed_builder, this is the envelope/outer serialization;
 *       each message contains exactly 1 thing: a tiny capnp-message with a SHM-handle pointing at the
 *       Core serialization in SHM.  As a result the messages traversing IPC happen to be very simple and small:
 *       always 1 segment of exactly the same small size (but struc::Channel just transmits them without
 *       knowing/caring what's in side).  (With PH they can be bigger and highly variable in general, as the actual
 *       user's capnp payload is inside.) */

namespace ipc::transport::struc::test
{

namespace
{
  using flow::async::Single_thread_task_loop;
  using flow::log::Logger;
  using flow::Error_code;
  using flow::util::stat::print;
  using flow::util::stat::stats_assign;
  using flow::util::stat::stats_reset;
  using session::schema::MqType;
  using std::string;
  using boost::chrono::seconds;

  /* Always-on console logger for test-progress output (FLOW_LOG_INFO etc.).
   * Survives across all TESTs in this TU; object internals use `logger` (toggleable) instead. */
  ipc::test::Test_logger g_logger_obj;
  Logger* const g_logger_console = &g_logger_obj;

  // Sums sample counts across all buckets of a Histogram_counter.
  // (`inline` on the plain helpers here and below: a sibling TU may use only a subset; -Wunused-function otherwise.)
  inline uint64_t histo_total(const flow::util::stat::Histogram_counter& h)
  {
    uint64_t total = 0;
    for (size_t idx = 0; idx != h.size(); ++idx)
    {
      total += h.count_for_bucket(idx);
    }
    return total;
  }

  /* Universal "create / mutate-no-alloc / dtor-or-move" phase asserts for a single-Msg_out
   * lifecycle observed on a single bottom-engine stats axis.  Used by:
   *   - PH (`stat::Heap_serializer_stats`): the only stats axis.
   *   - SH (`Core_serializer_stats`): the bottom (SHM) engine axis.  (NOT for the Outer/top
   *     axis -- that one is flat-until-send and observed inline at the call site.)
   *
   * `expect_segs_outcome`: 1 for single-seg msg, 2+ for multi-seg.  Drives which bucket of
   *                       m_histo_segs_per_msg should have incremented.
   * `expect_no_big_leaf`: if true, asserts big-leaf gauges flat throughout.  Big-leaf logic is
   *                       identical on PH and SH Core (both fire when min_sz > current seg sz);
   *                       so the right value here is purely a function of payload size, not the
   *                       PH-vs-SH axis.  Phase A and similar small-payload phases pass true;
   *                       phases that deliberately force a big leaf pass false.
   * `expect_no_cap_lock`: if true, asserts m_seg_grow_cap_lock_count flat throughout.  PH may
   *                       or may not (depends on payload size); SH Core *always* passes true
   *                       (no IPC cap, so cap-lock can never fire). */
  template<typename Stats>
  void check_phase_single_msg_lifecycle(const Stats& pre,
                                        const Stats& after_create,
                                        const Stats& mid,
                                        const Stats& post,
                                        uint32_t expect_segs_outcome,
                                        bool expect_no_big_leaf,
                                        bool expect_no_cap_lock)
  {
    // After create: msgs/gauges advance; histograms still flat (sampled at builder dtor/move-from).
    EXPECT_EQ(after_create.m_snd.m_msgs - pre.m_snd.m_msgs, 1u);
    EXPECT_EQ(after_create.m_snd.m_msgs_outstanding - pre.m_snd.m_msgs_outstanding, 1u);
    EXPECT_GE(after_create.m_snd.m_msgs_outstanding_hi_wmark, after_create.m_snd.m_msgs_outstanding);
    EXPECT_GT(after_create.m_snd.m_alloc_lifetime_sz, pre.m_snd.m_alloc_lifetime_sz);
    EXPECT_GT(after_create.m_snd.m_alloc_outstanding_sz, pre.m_snd.m_alloc_outstanding_sz);
    EXPECT_EQ(histo_total(after_create.m_snd.m_histo_msg_alloc_sz),
              histo_total(pre.m_snd.m_histo_msg_alloc_sz));
    EXPECT_EQ(histo_total(after_create.m_snd.m_histo_msg_used_sz),
              histo_total(pre.m_snd.m_histo_msg_used_sz));
    EXPECT_EQ(histo_total(after_create.m_snd.m_histo_segs_per_msg),
              histo_total(pre.m_snd.m_histo_segs_per_msg));

    // After no-alloc mutate: alloc tallies flat (caller's mutators must not have allocated).
    EXPECT_EQ(mid.m_snd.m_alloc_lifetime_sz, after_create.m_snd.m_alloc_lifetime_sz);
    EXPECT_EQ(mid.m_snd.m_alloc_outstanding_sz, after_create.m_snd.m_alloc_outstanding_sz);

    // After dtor/move-from: gauges back to baseline; high-water marks monotone; histograms +1 each.
    EXPECT_EQ(post.m_snd.m_msgs_outstanding, pre.m_snd.m_msgs_outstanding);
    EXPECT_EQ(post.m_snd.m_alloc_outstanding_sz, pre.m_snd.m_alloc_outstanding_sz);
    EXPECT_GE(post.m_snd.m_msgs_outstanding_hi_wmark, after_create.m_snd.m_msgs_outstanding);
    EXPECT_GE(post.m_snd.m_alloc_outstanding_sz_hi_wmark, after_create.m_snd.m_alloc_outstanding_sz);
    EXPECT_EQ(histo_total(post.m_snd.m_histo_msg_alloc_sz)
              - histo_total(pre.m_snd.m_histo_msg_alloc_sz), 1u);
    EXPECT_EQ(histo_total(post.m_snd.m_histo_msg_used_sz)
              - histo_total(pre.m_snd.m_histo_msg_used_sz), 1u);
    EXPECT_EQ(histo_total(post.m_snd.m_histo_segs_per_msg)
              - histo_total(pre.m_snd.m_histo_segs_per_msg), 1u);
    EXPECT_EQ(post.m_snd.m_histo_segs_per_msg.count_for_bucket_containing_outcome(expect_segs_outcome)
              - pre.m_snd.m_histo_segs_per_msg.count_for_bucket_containing_outcome(expect_segs_outcome), 1u);

    if (expect_no_big_leaf)
    {
      EXPECT_EQ(post.m_snd.m_big_leaf_alloc_count, pre.m_snd.m_big_leaf_alloc_count);
      EXPECT_EQ(post.m_snd.m_msgs_with_big_leaf, pre.m_snd.m_msgs_with_big_leaf);
    }
    if (expect_no_cap_lock)
    {
      EXPECT_EQ(post.m_snd.m_seg_grow_cap_lock_count, pre.m_snd.m_seg_grow_cap_lock_count);
    }
  } // check_phase_single_msg_lifecycle()

  /* "This stats axis didn't move" assertion.  Used for Outer (heap envelope) on the SH side
   * during Phases A-D, where no send() fires and the top engine never allocates.  Covers all the
   * counters/gauges/histograms one might reasonably expect to twitch during builder activity. */
  template<typename Stats>
  void expect_axis_fully_flat(const Stats& post, const Stats& pre)
  {
    EXPECT_EQ(post.m_snd.m_msgs, pre.m_snd.m_msgs);
    EXPECT_EQ(post.m_snd.m_msgs_outstanding, pre.m_snd.m_msgs_outstanding);
    EXPECT_EQ(post.m_snd.m_alloc_lifetime_sz, pre.m_snd.m_alloc_lifetime_sz);
    EXPECT_EQ(post.m_snd.m_alloc_outstanding_sz, pre.m_snd.m_alloc_outstanding_sz);
    EXPECT_EQ(post.m_snd.m_big_leaf_alloc_count, pre.m_snd.m_big_leaf_alloc_count);
    EXPECT_EQ(post.m_snd.m_msgs_with_big_leaf, pre.m_snd.m_msgs_with_big_leaf);
    EXPECT_EQ(post.m_snd.m_seg_grow_cap_lock_count, pre.m_snd.m_seg_grow_cap_lock_count);
    EXPECT_EQ(histo_total(post.m_snd.m_histo_msg_alloc_sz), histo_total(pre.m_snd.m_histo_msg_alloc_sz));
    EXPECT_EQ(histo_total(post.m_snd.m_histo_msg_used_sz), histo_total(pre.m_snd.m_histo_msg_used_sz));
    EXPECT_EQ(histo_total(post.m_snd.m_histo_segs_per_msg), histo_total(pre.m_snd.m_histo_segs_per_msg));
    EXPECT_EQ(histo_total(post.m_snd.m_histo_big_leaf_sz), histo_total(pre.m_snd.m_histo_big_leaf_sz));
    EXPECT_EQ(post.m_rcv.m_msgs_outstanding, pre.m_rcv.m_msgs_outstanding);
    EXPECT_EQ(post.m_rcv.m_alloc_lifetime_sz, pre.m_rcv.m_alloc_lifetime_sz);
    EXPECT_EQ(post.m_rcv.m_alloc_outstanding_sz, pre.m_rcv.m_alloc_outstanding_sz);
    EXPECT_EQ(post.m_rcv.m_used_outstanding_sz, pre.m_rcv.m_used_outstanding_sz);
    EXPECT_EQ(histo_total(post.m_rcv.m_histo_msg_alloc_sz), histo_total(pre.m_rcv.m_histo_msg_alloc_sz));
    EXPECT_EQ(histo_total(post.m_rcv.m_histo_msg_used_sz), histo_total(pre.m_rcv.m_histo_msg_used_sz));
  } // expect_axis_fully_flat()

  /* "This axis's rcv side never sampled" assertion.  Used for Core (SHM-msg-inner) on the SH side
   * during Phases E and G: the SHM payload is zero-copy on receive, so Core rcv stats are
   * structurally unsampled.  Snd side may freely advance; only rcv is asserted flat. */
  template<typename Stats>
  void expect_rcv_flat(const Stats& post, const Stats& pre)
  {
    EXPECT_EQ(post.m_rcv.m_msgs_outstanding, pre.m_rcv.m_msgs_outstanding);
    EXPECT_EQ(post.m_rcv.m_alloc_lifetime_sz, pre.m_rcv.m_alloc_lifetime_sz);
    EXPECT_EQ(post.m_rcv.m_alloc_outstanding_sz, pre.m_rcv.m_alloc_outstanding_sz);
    EXPECT_EQ(post.m_rcv.m_used_outstanding_sz, pre.m_rcv.m_used_outstanding_sz);
    EXPECT_EQ(histo_total(post.m_rcv.m_histo_msg_alloc_sz), histo_total(pre.m_rcv.m_histo_msg_alloc_sz));
    EXPECT_EQ(histo_total(post.m_rcv.m_histo_msg_used_sz), histo_total(pre.m_rcv.m_histo_msg_used_sz));
  } // expect_rcv_flat()

  /* Phase A driver: single-segment Msg_out lifecycle, no send().  Validates one snd-side axis
   * (= Heap on PH; = Core on SH).  Cap-lock cannot fire on either (small msg / no cap), so the
   * `expect_no_cap_lock=true` path of the inner check applies uniformly. */
  template<typename Stats, typename Channel>
  void test_phase_a(const Stats& stats_global, Channel& cli, size_t seg_init_sz)
  {
    Stats pre, after_create, mid, post;
    stats_assign(&pre, stats_global);

    /* Msg_out ctor calls payload_msg_builder()->getRoot<Body>() -> first allocateSegment() fires.
     * On PH that's the heap engine; on SH that's the bottom (SHM) engine.  Top (Outer) engine on
     * SH stays untouched until emit_serialization() (= Channel::send()), which is not called here. */
    auto msg = cli.create_msg(seg_init_sz);
    stats_assign(&after_create, stats_global);

    msg.body_root()->initCoolReq().setCoolVal(1);
    msg.body_root()->getCoolReq().setCoolVal(42);
    stats_assign(&mid, stats_global);

    msg = {};
    stats_assign(&post, stats_global);

    check_phase_single_msg_lifecycle(pre, after_create, mid, post,
                                     /* expect_segs_outcome */ 1,
                                     /* expect_no_big_leaf */ true,
                                     /* expect_no_cap_lock */ true);
  } // test_phase_a()

  /* Phase B driver: incremental fill -> multi-segment Msg_out, no big-leaf.  Validates one
   * snd-side axis.  Payload sizing chosen to work for both PH and SH-Core:
   *   - 1500 x UInt64 = 12 KiB list payload, plus list-overhead.
   *   - With seg_init_sz=8Ki and capnp's exponential growth, m_segment_sz becomes 16Ki after seg 0
   *     (on both PH and SH-Core, since SH-Core has no cap and PH's 64Ki cap doesn't bind here).
   *   - 12Ki list cannot fit in seg 0 (root struct + scalars consume some space), forcing seg 1.
   *   - 12Ki < 16Ki preferred next-seg size on both -> no big-leaf event. */
  template<typename Stats, typename Channel>
  void test_phase_b(const Stats& stats_global, Channel& cli, size_t seg_init_sz)
  {
    Stats pre;
    stats_assign(&pre, stats_global);

    auto msg = cli.create_msg(seg_init_sz);
    msg.body_root()->initCoolReq().setCoolVal(0);

    Stats after_init;
    stats_assign(&after_init, stats_global);

    {
      auto payload = msg.body_root()->getCoolReq().initPayload(1500);
      for (uint32_t idx = 0; idx != payload.size(); ++idx) { payload.set(idx, idx); }
    }
    Stats after_payload;
    stats_assign(&after_payload, stats_global);
    EXPECT_GT(after_payload.m_snd.m_alloc_lifetime_sz, after_init.m_snd.m_alloc_lifetime_sz);
    EXPECT_GT(after_payload.m_snd.m_alloc_outstanding_sz, after_init.m_snd.m_alloc_outstanding_sz);
    EXPECT_EQ(after_payload.m_snd.m_big_leaf_alloc_count, pre.m_snd.m_big_leaf_alloc_count);

    msg = {};
    Stats post;
    stats_assign(&post, stats_global);
    EXPECT_EQ(post.m_snd.m_msgs - pre.m_snd.m_msgs, 1u);
    EXPECT_EQ(post.m_snd.m_msgs_outstanding, pre.m_snd.m_msgs_outstanding);
    EXPECT_EQ(post.m_snd.m_alloc_outstanding_sz, pre.m_snd.m_alloc_outstanding_sz);
    EXPECT_EQ(histo_total(post.m_snd.m_histo_segs_per_msg)
              - histo_total(pre.m_snd.m_histo_segs_per_msg), 1u);
    // Multi-segment msg lands in the bucket covering outcome=2 (last bucket; all >=2 clamp here).
    EXPECT_EQ(post.m_snd.m_histo_segs_per_msg.count_for_bucket_containing_outcome(2)
              - pre.m_snd.m_histo_segs_per_msg.count_for_bucket_containing_outcome(2), 1u);
    EXPECT_EQ(post.m_snd.m_big_leaf_alloc_count, pre.m_snd.m_big_leaf_alloc_count);
    EXPECT_EQ(post.m_snd.m_msgs_with_big_leaf, pre.m_snd.m_msgs_with_big_leaf);
  } // test_phase_b()

  /* Phase C driver: big-leaf forcing.  Validates one snd-side axis.
   *   - 12000 x UInt64 = 96 KiB single-list-leaf.
   *   - PH: exceeds 64Ki seg-and-frame cap -> big-leaf bypass + seg-grow-cap-lock both fire.
   *   - SH-Core: no cap.  After seg 0 (8Ki), m_segment_sz = 16Ki; 96Ki > 16Ki -> big-leaf fires.
   *     Cap-lock cannot fire (SHM has no transport cap -> no cap-lock event possible).
   * `expect_cap_lock`: true on PH, false on SH-Core. */
  template<typename Stats, typename Channel>
  void test_phase_c(const Stats& stats_global, Channel& cli, size_t seg_init_sz,
                    bool expect_cap_lock)
  {
    Stats pre;
    stats_assign(&pre, stats_global);

    auto msg = cli.create_msg(seg_init_sz);
    msg.body_root()->initCoolReq().setCoolVal(0);

    {
      auto payload = msg.body_root()->getCoolReq().initPayload(12'000);
      for (uint32_t idx = 0; idx != payload.size(); ++idx) { payload.set(idx, idx); }
    }
    Stats after_big;
    stats_assign(&after_big, stats_global);
    EXPECT_EQ(after_big.m_snd.m_big_leaf_alloc_count - pre.m_snd.m_big_leaf_alloc_count, 1u);
    EXPECT_EQ(histo_total(after_big.m_snd.m_histo_big_leaf_sz)
              - histo_total(pre.m_snd.m_histo_big_leaf_sz), 1u);
    if (expect_cap_lock)
    {
      EXPECT_EQ(after_big.m_snd.m_seg_grow_cap_lock_count
                - pre.m_snd.m_seg_grow_cap_lock_count, 1u);
    }
    else
    {
      EXPECT_EQ(after_big.m_snd.m_seg_grow_cap_lock_count, pre.m_snd.m_seg_grow_cap_lock_count);
    }

    msg = {};
    Stats post;
    stats_assign(&post, stats_global);
    // Exactly one big-leaf-bearing builder destroyed -> +1 to msgs_with_big_leaf.
    EXPECT_EQ(post.m_snd.m_msgs_with_big_leaf - pre.m_snd.m_msgs_with_big_leaf, 1u);
    EXPECT_EQ(post.m_snd.m_msgs - pre.m_snd.m_msgs, 1u);
    if (!expect_cap_lock)
    {
      EXPECT_EQ(post.m_snd.m_seg_grow_cap_lock_count, pre.m_snd.m_seg_grow_cap_lock_count);
    }
  } // test_phase_c()

  /* Phase D driver: high-water-mark check via N concurrent Msg_outs.  Validates one snd-side axis.
   * Generic stat-keeping property of m_msgs_outstanding_hi_wmark; identical mechanics on PH and SH.  */
  template<typename Stats, typename Channel>
  void test_phase_d(const Stats& stats_global, Channel& cli, size_t seg_init_sz)
  {
    Stats pre;
    stats_assign(&pre, stats_global);
    constexpr size_t N = 3;

    std::vector<decltype(cli.create_msg(seg_init_sz))> msgs;
    msgs.reserve(N);
    for (size_t idx = 0; idx != N; ++idx)
    {
      auto m = cli.create_msg(seg_init_sz);
      m.body_root()->initCoolReq().setCoolVal(idx);
      msgs.push_back(std::move(m));
    }

    Stats peak;
    stats_assign(&peak, stats_global);
    EXPECT_EQ(peak.m_snd.m_msgs_outstanding - pre.m_snd.m_msgs_outstanding, N);
    EXPECT_GE(peak.m_snd.m_msgs_outstanding_hi_wmark, peak.m_snd.m_msgs_outstanding);

    msgs.clear();
    Stats post;
    stats_assign(&post, stats_global);
    EXPECT_EQ(post.m_snd.m_msgs_outstanding, pre.m_snd.m_msgs_outstanding);
    EXPECT_GE(post.m_snd.m_msgs_outstanding_hi_wmark, peak.m_snd.m_msgs_outstanding);
    EXPECT_EQ(histo_total(post.m_snd.m_histo_msg_alloc_sz)
              - histo_total(pre.m_snd.m_histo_msg_alloc_sz), N);
    EXPECT_EQ(histo_total(post.m_snd.m_histo_segs_per_msg)
              - histo_total(pre.m_snd.m_histo_segs_per_msg), N);
  } // test_phase_d()

  /* Phase E driver: send() -> Msg_in materializes -> snd advances + rcv-side observable.  Drives
   * one "heap-like" stats axis: PH = Heap (only axis); SH = Outer (top heap envelope).
   * `held_hook` runs after the helper has snapshotted `held` and run its own heap-like assertions:
   * SH uses it to snapshot Core and assert Core snd advances + Core rcv flat (zero-copy).  PH
   * passes a no-op. */
  template<typename Stats, typename Channel, typename Server, typename HeldHook>
  void test_phase_e(const Stats& heap_like_global, Channel& cli, Server& srv,
                    size_t seg_init_sz, HeldHook&& held_hook)
  {
    Stats pre;
    stats_assign(&pre, heap_like_global);

    /* Server handler pins req_in into a type-erased holder so its dtor runs synchronously on the
     * caller thread (after we've made our checks), not on the channel's internal thread.  This
     * avoids a race window where the caller would observe rcv gauges before the dtor runs. */
    boost::shared_ptr<void> rcv_msg_keeper;
    boost::promise<void> got_msg;
    srv.expect_msg(Body::COOL_REQ, [&](auto req_in)
    {
      rcv_msg_keeper = req_in;
      got_msg.set_value();
    });

    {
      auto msg = cli.create_msg(seg_init_sz);
      msg.body_root()->initCoolReq().setCoolVal(7);
      cli.send(&msg);
      // msg dtor at scope exit: builder lifetime ends -> snd histos sampled.
    }

    ASSERT_EQ(got_msg.get_future().wait_for(seconds{3}), boost::future_status::ready)
      << "Server did not receive the message.";

    Stats held;
    stats_assign(&held, heap_like_global);

    // Snd: 1 builder lifetime; m_msgs +1; m_msgs_outstanding back to pre (already dtor'd); histo +1.
    EXPECT_EQ(held.m_snd.m_msgs - pre.m_snd.m_msgs, 1u);
    EXPECT_EQ(held.m_snd.m_msgs_outstanding, pre.m_snd.m_msgs_outstanding);
    EXPECT_GT(held.m_snd.m_alloc_lifetime_sz, pre.m_snd.m_alloc_lifetime_sz);
    EXPECT_EQ(histo_total(held.m_snd.m_histo_segs_per_msg)
              - histo_total(pre.m_snd.m_histo_segs_per_msg), 1u);

    // Rcv: Msg_in pinned -> gauges elevated; histos sampled at deserialization-start (already +1).
    EXPECT_EQ(held.m_rcv.m_msgs_outstanding - pre.m_rcv.m_msgs_outstanding, 1u);
    EXPECT_GE(held.m_rcv.m_msgs_outstanding_hi_wmark, held.m_rcv.m_msgs_outstanding);
    EXPECT_GT(held.m_rcv.m_alloc_lifetime_sz, pre.m_rcv.m_alloc_lifetime_sz);
    EXPECT_GT(held.m_rcv.m_alloc_outstanding_sz, pre.m_rcv.m_alloc_outstanding_sz);
    EXPECT_GT(held.m_rcv.m_used_outstanding_sz, pre.m_rcv.m_used_outstanding_sz);
    EXPECT_EQ(histo_total(held.m_rcv.m_histo_msg_alloc_sz)
              - histo_total(pre.m_rcv.m_histo_msg_alloc_sz), 1u);
    EXPECT_EQ(histo_total(held.m_rcv.m_histo_msg_used_sz)
              - histo_total(pre.m_rcv.m_histo_msg_used_sz), 1u);

    held_hook();

    // Release the Msg_in -- reader dtor runs here on the caller thread, synchronously.
    rcv_msg_keeper.reset();

    Stats post;
    stats_assign(&post, heap_like_global);
    EXPECT_EQ(post.m_rcv.m_msgs_outstanding, pre.m_rcv.m_msgs_outstanding);
    EXPECT_EQ(post.m_rcv.m_alloc_outstanding_sz, pre.m_rcv.m_alloc_outstanding_sz);
    EXPECT_EQ(post.m_rcv.m_used_outstanding_sz, pre.m_rcv.m_used_outstanding_sz);
    // High-water marks preserved (monotonic).
    EXPECT_GE(post.m_rcv.m_msgs_outstanding_hi_wmark, held.m_rcv.m_msgs_outstanding);
    EXPECT_GE(post.m_rcv.m_alloc_outstanding_sz_hi_wmark, held.m_rcv.m_alloc_outstanding_sz);
    EXPECT_GE(post.m_rcv.m_used_outstanding_sz_hi_wmark, held.m_rcv.m_used_outstanding_sz);
  } // test_phase_e()

  /* Phase G driver: resend semantics.  emit_serialization() may be called multiple times; for the
   * heap-like axis (PH = Heap, SH = Outer) the cached single-segment serialization is re-emitted
   * with no new alloc / no histogram sample (sampling deferred to builder dtor).  Rcv side gets a
   * fresh Msg_in per send, so its outstanding gauge advances per send.
   *
   * Hooks for the SH caller (PH passes no-ops):
   *   `after_first_hook`: snapshot Core post-1st-send.
   *   `after_second_hook`: snapshot Core post-2nd-send -- and assert Core snd flat between sends,
   *                        Core rcv flat (zero-copy).
   *   `after_post_hook`: snapshot Core post-msg-dtor + post-rcv-release -- and assert Core snd
   *                      histos +1 (one builder lifetime). */
  template<typename Stats, typename Channel, typename Server,
           typename AfterFirstHook, typename AfterSecondHook, typename AfterPostHook>
  void test_phase_g(const Stats& heap_like_global, Channel& cli, Server& srv,
                    size_t seg_init_sz,
                    AfterFirstHook&& after_first_hook,
                    AfterSecondHook&& after_second_hook,
                    AfterPostHook&& after_post_hook)
  {
    /* expect_msg() (singular, auto-unregister on receipt): Re-register before each send so
     * each registration matches exactly one message and is gone before the helper returns. */
    std::vector<boost::shared_ptr<void>> rcv_msg_keepers;
    boost::promise<void> got_msg_1;
    boost::promise<void> got_msg_2;
    srv.expect_msg(Body::COOL_REQ, [&](auto req_in)
    {
      rcv_msg_keepers.push_back(req_in);
      got_msg_1.set_value();
    });

    Stats pre;
    stats_assign(&pre, heap_like_global);

    auto msg = cli.create_msg(seg_init_sz);
    msg.body_root()->initCoolReq().setCoolVal(11);

    // 1st send.
    cli.send(&msg);
    ASSERT_EQ(got_msg_1.get_future().wait_for(seconds{3}), boost::future_status::ready)
      << "Server did not receive the 1st message.";

    Stats after_first_send;
    stats_assign(&after_first_send, heap_like_global);
    after_first_hook();

    // Re-register for the 2nd message before issuing the 2nd send.
    srv.expect_msg(Body::COOL_REQ, [&](auto req_in)
    {
      rcv_msg_keepers.push_back(req_in);
      got_msg_2.set_value();
    });

    // 2nd send -- same Msg_out, no mutation between sends.
    cli.send(&msg);
    ASSERT_EQ(got_msg_2.get_future().wait_for(seconds{3}), boost::future_status::ready)
      << "Server did not receive the 2nd message.";

    Stats after_second_send;
    stats_assign(&after_second_send, heap_like_global);
    after_second_hook();

    /* Snd-side delta between sends: nothing on the heap-like axis.  Cached serialization
     * re-emitted; no new alloc, no histo sample (sampling at builder dtor, hasn't happened). */
    EXPECT_EQ(after_second_send.m_snd.m_msgs, after_first_send.m_snd.m_msgs);
    EXPECT_EQ(after_second_send.m_snd.m_msgs_outstanding, after_first_send.m_snd.m_msgs_outstanding);
    EXPECT_EQ(after_second_send.m_snd.m_alloc_lifetime_sz, after_first_send.m_snd.m_alloc_lifetime_sz);
    EXPECT_EQ(after_second_send.m_snd.m_alloc_outstanding_sz, after_first_send.m_snd.m_alloc_outstanding_sz);
    EXPECT_EQ(after_second_send.m_snd.m_big_leaf_alloc_count, after_first_send.m_snd.m_big_leaf_alloc_count);
    EXPECT_EQ(after_second_send.m_snd.m_msgs_with_big_leaf, after_first_send.m_snd.m_msgs_with_big_leaf);
    EXPECT_EQ(after_second_send.m_snd.m_seg_grow_cap_lock_count,
              after_first_send.m_snd.m_seg_grow_cap_lock_count);
    EXPECT_EQ(histo_total(after_second_send.m_snd.m_histo_msg_alloc_sz),
              histo_total(after_first_send.m_snd.m_histo_msg_alloc_sz));
    EXPECT_EQ(histo_total(after_second_send.m_snd.m_histo_msg_used_sz),
              histo_total(after_first_send.m_snd.m_histo_msg_used_sz));
    EXPECT_EQ(histo_total(after_second_send.m_snd.m_histo_segs_per_msg),
              histo_total(after_first_send.m_snd.m_histo_segs_per_msg));
    EXPECT_EQ(histo_total(after_second_send.m_snd.m_histo_big_leaf_sz),
              histo_total(after_first_send.m_snd.m_histo_big_leaf_sz));

    // Rcv side: each send produced a fresh Msg_in.  Both pinned -> +2 from pre.
    EXPECT_EQ(after_second_send.m_rcv.m_msgs_outstanding - pre.m_rcv.m_msgs_outstanding, 2u);

    // Drop the Msg_out and the two pinned Msg_ins.
    msg = {};
    rcv_msg_keepers.clear();

    /* Subtlety: ours may not have been the last ref to a given Msg_in: the channel's delivery machinery can
     * hold a transient one, releasing it on a worker thread an instant after the handler returns; and the
     * rcv-side stats update only then (reader dtor).  So (bounded-)wait for the drain before asserting.
     * (Seen to lose that race in practice: ~once, SHM-jemalloc variant, MinSizeRel, in CI.) */
    Stats post;
    for (auto deadline = flow::Fine_clock::now() + boost::chrono::seconds(5);;)
    {
      stats_assign(&post, heap_like_global);
      if ((post.m_rcv.m_msgs_outstanding == pre.m_rcv.m_msgs_outstanding)
          || (flow::Fine_clock::now() >= deadline))
      {
        break;
      }
      flow::util::this_thread::sleep_for(boost::chrono::milliseconds(1));
    }
    after_post_hook();

    // Snd-side dtor sampled the histograms exactly once (one builder lifetime, regardless of #sends).
    EXPECT_EQ(histo_total(post.m_snd.m_histo_segs_per_msg)
              - histo_total(after_second_send.m_snd.m_histo_segs_per_msg), 1u);
    // Rcv-side: both Msg_ins released -> outstanding back to pre baseline.
    EXPECT_EQ(post.m_rcv.m_msgs_outstanding, pre.m_rcv.m_msgs_outstanding);
  } // test_phase_g()

  /* End-to-end-ish test of pure-heap stat::Heap_serializer_global_stats.
   *
   * Strategy: Native_socket_stream-only transport, as it is the most mainstream as of this writing.  Per-msg
   * seg-init is pinned via create_msg(SEG_INIT_SZ, ...) for deterministic phase math; the
   * histogram bucketings in stat::Heap_serializer_stats_cfg are tuned around 8Ki seg-init / 64Ki
   * seg-cap, so they cooperate.
   *
   * Globals leak across TESTs in the unit_test executable, so all assertions are delta-based
   * against a snapshot grabbed at the start of each phase via stats_assign().
   *
   * Phases A-D exercise Msg_out lifecycle without Channel::send() (snd-side stats are about
   * builder-instance lifetimes; transmission is irrelevant).  Phase E performs a send() solely
   * so a Msg_in materializes and rcv-side stats can be observed.
   *
   * Threads:
   *   U  (test main) -- waits on a final promise.
   *   U1 -- runs the phase sequence.  As of this writing could've kept it in U, but this way the
   *         setup is more realistic/convenient (Channel/etc. handlers running in their own
   *         background threads can .post() back onto U1 as recommended for Flow-IPC async-I/O
   *         pattern) for test maintenance/extensions.
   *   Thread W (internal, owned by srv struc::Channel) -- where expect_msg() handler fires.
   *
   * @todo Add an MQ-enabled variant (POSIX and/or Bipc).  Purpose: inspect how histograms /
   * big-leaf / seg-growth-capacity-lock counts shift under MQ's tighter 8Ki IPC-message-size
   * cap.  Basic non-crash coverage of MQ paths is already provided by other test suites.
   * Important warning before extending this test: with default MQ config, the per-IPC-message
   * size cap is 8Ki; before reusing the multi-segment / big-leaf assertions below for an
   * MQ-backed transport, lift that cap via Session_server::mq_msg_size_limit() (or equivalent)
   * -- otherwise the phases below will not behave as written.  However, this will be forbidden by
   * typical Linux, at least, config -- 8Ki being the typical limit on the limit.  So in that case
   * we should think of something if only to see how some of these histograms look. */
  inline void test_send_receive_serializer_stats_heap()
  {
#if 1
    Logger* const logger = nullptr; // Normal test run: object internals silent.
#else
    Logger* const logger = g_logger_console; // Flip for debugging.
#endif

    // Pin per-message seg-init explicitly so phase math is deterministic.
    constexpr size_t SEG_INIT_SZ = 8 * 1024;

    Logger::this_thread_set_logged_nickname("U", g_logger_console);
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

    std::atomic<bool> cli_err_fired{false};
    std::atomic<bool> srv_err_fired{false};
    auto pair = make_session_struc_pair<Body, MqType::NONE, false>
                  ([&](auto&&...) { cli_err_fired = true; },
                   [&](auto&&...) { srv_err_fired = true; },
                   logger);

    Single_thread_task_loop loop_u1{logger, "cli_u1"};
    loop_u1.start();

    boost::promise<void> test_done;

    loop_u1.post([&]()
    {
      const auto& stats_global = stat::Heap_serializer_global_stats::get().stats_default();

      // ---------- Phase A: Msg_out single-segment incremental lifecycle (no send) ----------
      {
        FLOW_TEST_TRACE_CTX("PH Phase A");
        FLOW_LOG_INFO("Phase A: Msg_out single-segment lifecycle (no send).");
        test_phase_a(stats_global, *pair.m_cli, SEG_INIT_SZ);
      }

      // ---------- Phase B: Msg_out incremental fill -> multi-segment (no send) ----------
      {
        FLOW_TEST_TRACE_CTX("PH Phase B");
        FLOW_LOG_INFO("Phase B: Msg_out incremental fill -> multi-segment (no send).");
        test_phase_b(stats_global, *pair.m_cli, SEG_INIT_SZ);
      }

      // ---------- Phase C: Msg_out big-leaf forcing + cap-lock (no send) ----------
      {
        FLOW_TEST_TRACE_CTX("PH Phase C");
        FLOW_LOG_INFO("Phase C: Msg_out big-leaf forcing + cap-lock (no send).");
        test_phase_c(stats_global, *pair.m_cli, SEG_INIT_SZ,
                     /* expect_cap_lock */ true);
      }

      // ---------- Phase D: high-water mark -- multiple Msg_outs alive simultaneously ----------
      {
        FLOW_TEST_TRACE_CTX("PH Phase D");
        FLOW_LOG_INFO("Phase D: high-water mark check (3 Msg_outs alive concurrently).");
        test_phase_d(stats_global, *pair.m_cli, SEG_INIT_SZ);
      }

      // ---------- Phase E: send() -> Msg_in materializes; rcv-side checks ----------
      {
        FLOW_TEST_TRACE_CTX("PH Phase E");
        FLOW_LOG_INFO("Phase E: send() -> Msg_in materializes; rcv-side checks.");
        test_phase_e(stats_global, *pair.m_cli, *pair.m_srv, SEG_INIT_SZ, []{});
      }

      // ---------- Phase G: resend -- 2nd send() of same Msg_out is snd-side no-op ----------
      {
        FLOW_TEST_TRACE_CTX("PH Phase G");
        FLOW_LOG_INFO("Phase G: resend semantics; 2nd send() reuses cached serialization.");
        test_phase_g(stats_global, *pair.m_cli, *pair.m_srv, SEG_INIT_SZ,
                     /* after_first_hook  */ []{},
                     /* after_second_hook */ []{},
                     /* after_post_hook   */ []{});
      }

      // ---------- Phase F: print() smoke test ----------
      {
        FLOW_TEST_TRACE_CTX("PH Phase F");
        FLOW_LOG_INFO("Phase F: print()/declare_stats() smoke test.");
        std::ostringstream oss;
        oss << print(stats_global);
        EXPECT_FALSE(oss.str().empty()) << "print(stats()) produced empty output.";
        // Spot-check: m_snd / m_rcv items appear with their respective sub-prefix.
        EXPECT_NE(oss.str().find("snd.msgs="), string::npos);
        EXPECT_NE(oss.str().find("rcv.msgs_outstanding="), string::npos);
      }

      test_done.set_value();
    });

    test_done.get_future().wait();
    loop_u1.stop();

    EXPECT_FALSE(cli_err_fired);
    EXPECT_FALSE(srv_err_fired);
  } // test_send_receive_serializer_stats_heap()

  /* End-to-end-ish test of SHM-backed serializer global stats.  Templated on the SHM provider
   * (SHM-classic or SHM-jemalloc) so the body is shared verbatim; two TEST()s instantiate it.
   *
   * Phase set mirrors the PH test (A-G): for Phases A-D the Core (SHM-msg-inner) axis is
   * exercised via the same helpers used by the PH test (Core ~ PH for stat-keeping mechanics,
   * modulo cap-lock which cannot fire under SHM since there is no IPC-message-size cap), and
   * around each call the Outer (heap-envelope) axis is asserted fully flat: no send() means the
   * top engine never allocates -> its dtor early-outs.  Phases E and G perform send() and observe
   * both axes diverge: Outer behaves heap-like (advances per send / per pin); Core advances
   * snd-side but its rcv side is structurally never sampled (zero-copy SHM rcv).
   *
   * Outer (top/heap) axis: each builder's outer envelope holds a single tiny segment containing
   * just a SHM-handle.  Top-engine getRoot is *lazy* -- it fires only at emit_serialization()
   * (= Channel::send()).  Therefore until the first send() the top engine has zero allocated
   * segments and its dtor early-outs without sampling histograms or moving any gauge.
   *
   * Core (bottom/SHM) axis: snd side behaves exactly like the PH heap engine (same big-leaf
   * logic, same dtor sampling), except the cap-lock event cannot fire (SHM has no IPC-message-
   * size cap).  Hence the shared Phase helpers apply directly with `expect_cap_lock=false`. */
  template<session::schema::ShmType SHM_TYPE>
  void test_send_receive_serializer_stats_shm()
  {
    using session::schema::MqType;
    using shm::stat::Outer_serializer_stats;
    using shm::stat::Core_serializer_stats;
    using shm::stat::Outer_serializer_global_stats;
    using shm::stat::Core_serializer_global_stats;

#if 1
    Logger* const logger = nullptr;
#else
    Logger* const logger = g_logger_console;
#endif

    constexpr size_t SEG_INIT_SZ = 8 * 1024;

    Logger::this_thread_set_logged_nickname("U", g_logger_console);
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

    using Cli_session_t = typename Session_pair<MqType::NONE, false, SHM_TYPE>::Client_session_t;
    using Arena = typename Cli_session_t::Arena;
    using Struc_channel_t = typename Cli_session_t::template Structured_channel<Body>;

    std::atomic<bool> cli_err_fired{false};
    std::atomic<bool> srv_err_fired{false};

    /* The PH test uses make_session_struc_pair(), but as of this writing that one makes
     * a heap-backed struc::Channel pair (which is not wrong) but no SHM-backed struc::Channel pair.
     * Do exactly what its doc header suggests in this situation.
     * @todo If make_session_struc_pair() is extended accordingly, simplify here. */
    auto raw_pair = make_session_channel_pair<MqType::NONE, false, SHM_TYPE>(logger);
    auto& sessions = *raw_pair.m_sessions;

    auto cli = std::make_unique<Struc_channel_t>(logger, std::move(raw_pair.m_cli_channels.front()),
                                                 Channel_base::S_SERIALIZE_VIA_SESSION_SHM,
                                                 &sessions.m_cli_session);
    auto srv = std::make_unique<Struc_channel_t>(logger, std::move(raw_pair.m_srv_channels.front()),
                                                 Channel_base::S_SERIALIZE_VIA_SESSION_SHM,
                                                 &sessions.m_srv_session);
    cli->start([&](auto&&...) { cli_err_fired = true; });
    srv->start([&](auto&&...) { srv_err_fired = true; });

    Single_thread_task_loop loop_u1{logger, "cli_u1"};
    loop_u1.start();

    boost::promise<void> test_done;

    loop_u1.post([&]()
    {
      const auto& outer_global = Outer_serializer_global_stats<Arena>::get().stats_default();
      const auto& core_global = Core_serializer_global_stats<Arena>::get().stats_default();

      /* For Phases A-D: Outer must stay completely flat (no send -> top engine never allocates ->
       * dtor early-outs).  Snapshot Outer pre/post around each helper invocation and assert. */

      // ---------- Phase A: Msg_out single-segment lifecycle (no send) ----------
      {
        FLOW_TEST_TRACE_CTX("SH Phase A");
        FLOW_LOG_INFO("SH Phase A: Msg_out single-segment lifecycle (no send).");
        Outer_serializer_stats outer_pre, outer_post;
        stats_assign(&outer_pre, outer_global);
        test_phase_a(core_global, *cli, SEG_INIT_SZ);
        stats_assign(&outer_post, outer_global);
        expect_axis_fully_flat(outer_post, outer_pre);
      }

      // ---------- Phase B: Msg_out incremental fill -> multi-segment (no send) ----------
      {
        FLOW_TEST_TRACE_CTX("SH Phase B");
        FLOW_LOG_INFO("SH Phase B: Msg_out incremental fill -> multi-segment (no send).");
        Outer_serializer_stats outer_pre, outer_post;
        stats_assign(&outer_pre, outer_global);
        test_phase_b(core_global, *cli, SEG_INIT_SZ);
        stats_assign(&outer_post, outer_global);
        expect_axis_fully_flat(outer_post, outer_pre);
      }

      // ---------- Phase C: Msg_out big-leaf forcing (no send; cap-lock cannot fire on Core) ----------
      {
        FLOW_TEST_TRACE_CTX("SH Phase C");
        FLOW_LOG_INFO("SH Phase C: Msg_out big-leaf forcing (no send).");
        Outer_serializer_stats outer_pre, outer_post;
        stats_assign(&outer_pre, outer_global);
        test_phase_c(core_global, *cli, SEG_INIT_SZ,
                     /* expect_cap_lock */ false);
        stats_assign(&outer_post, outer_global);
        expect_axis_fully_flat(outer_post, outer_pre);
      }

      // ---------- Phase D: high-water mark -- multiple Msg_outs alive simultaneously ----------
      {
        FLOW_TEST_TRACE_CTX("SH Phase D");
        FLOW_LOG_INFO("SH Phase D: high-water mark check (3 Msg_outs alive concurrently).");
        Outer_serializer_stats outer_pre, outer_post;
        stats_assign(&outer_pre, outer_global);
        test_phase_d(core_global, *cli, SEG_INIT_SZ);
        stats_assign(&outer_post, outer_global);
        expect_axis_fully_flat(outer_post, outer_pre);
      }

      // ---------- Phase E: send() -> Msg_in materializes; rcv-side checks ----------
      {
        FLOW_TEST_TRACE_CTX("SH Phase E");
        FLOW_LOG_INFO("SH Phase E: send() -> Msg_in materializes; rcv-side checks.");

        Core_serializer_stats core_pre;
        stats_assign(&core_pre, core_global);

        test_phase_e(outer_global, *cli, *srv, SEG_INIT_SZ, [&]
        {
          // Core snd: 1 builder lifetime; histograms +1.  Core rcv: zero-copy -> never sampled.
          Core_serializer_stats core_held;
          stats_assign(&core_held, core_global);
          EXPECT_EQ(core_held.m_snd.m_msgs - core_pre.m_snd.m_msgs, 1u);
          EXPECT_EQ(core_held.m_snd.m_msgs_outstanding, core_pre.m_snd.m_msgs_outstanding);
          EXPECT_GT(core_held.m_snd.m_alloc_lifetime_sz, core_pre.m_snd.m_alloc_lifetime_sz);
          EXPECT_EQ(histo_total(core_held.m_snd.m_histo_segs_per_msg)
                    - histo_total(core_pre.m_snd.m_histo_segs_per_msg), 1u);
          expect_rcv_flat(core_held, core_pre);
        });
      }

      // ---------- Phase G: resend -- 2nd send() reuses cached Outer; Core just lend()-bumps ----------
      {
        FLOW_TEST_TRACE_CTX("SH Phase G");
        FLOW_LOG_INFO("SH Phase G: resend; 2nd send() reuses cached Outer + bumps SHM ref-count for Core.");

        Core_serializer_stats core_pre;
        Core_serializer_stats core_after_first;
        stats_assign(&core_pre, core_global);

        test_phase_g(outer_global, *cli, *srv, SEG_INIT_SZ,
          [&]
          {
            stats_assign(&core_after_first, core_global);
          },
          [&]
          {
            /* Core snd flat between sends: lend() bumps the SHM-handle process-owner ref-count
             * internally but allocates no new SHM.  Core rcv: still zero (zero-copy SHM rcv). */
            Core_serializer_stats core_after_second;
            stats_assign(&core_after_second, core_global);
            EXPECT_EQ(core_after_second.m_snd.m_msgs, core_after_first.m_snd.m_msgs);
            EXPECT_EQ(core_after_second.m_snd.m_msgs_outstanding, core_after_first.m_snd.m_msgs_outstanding);
            EXPECT_EQ(core_after_second.m_snd.m_alloc_lifetime_sz, core_after_first.m_snd.m_alloc_lifetime_sz);
            EXPECT_EQ(core_after_second.m_snd.m_alloc_outstanding_sz,
                      core_after_first.m_snd.m_alloc_outstanding_sz);
            EXPECT_EQ(histo_total(core_after_second.m_snd.m_histo_segs_per_msg),
                      histo_total(core_after_first.m_snd.m_histo_segs_per_msg));
            expect_rcv_flat(core_after_second, core_pre);
          },
          [&]
          {
            // Core snd dtor sampled histograms exactly once (one builder lifetime).
            Core_serializer_stats core_post;
            stats_assign(&core_post, core_global);
            EXPECT_EQ(histo_total(core_post.m_snd.m_histo_segs_per_msg)
                      - histo_total(core_after_first.m_snd.m_histo_segs_per_msg), 1u);
          });
      }

      // ---------- Phase F: print() smoke test ----------
      {
        FLOW_TEST_TRACE_CTX("SH Phase F");
        FLOW_LOG_INFO("SH Phase F: print()/declare_stats() smoke test.");
        std::ostringstream oss_outer, oss_core;
        oss_outer << print(outer_global);
        oss_core << print(core_global);
        EXPECT_FALSE(oss_outer.str().empty()) << "print(outer_stats()) produced empty output.";
        EXPECT_FALSE(oss_core.str().empty()) << "print(core_stats()) produced empty output.";
        EXPECT_NE(oss_outer.str().find("snd.msgs="), string::npos);
        EXPECT_NE(oss_core.str().find("snd.msgs="), string::npos);
      }

      test_done.set_value();
    });

    test_done.get_future().wait();
    loop_u1.stop();

    EXPECT_FALSE(cli_err_fired);
    EXPECT_FALSE(srv_err_fired);
  } // test_send_receive_serializer_stats_shm()

  /* App-scope SHM end-to-end smoke test.  Verifies that the app-scope serialization path correctly
   * accumulates into the same `Outer_serializer_global_stats<Arena>` + `Core_serializer_global_stats<Arena>`
   * singletons as the session-scope path; both scopes feed one global per Arena (by default).
   *
   * Why a *separate* test (vs. splicing into test_send_receive_serializer_stats_shm()): the
   * app-scope channel ctor uses `session->app_shm()` as the lender arena, which is asymmetric for
   * SHM-jemalloc -- only Server_session has `app_shm()`; Client_session does not (won't compile
   * with the Serialize_via_app_shm tag-form ctor).  Workable but pushes the existing test into
   * messy territory; cleaner as its own thing.
   *
   * Two init-channels per session: [0] session-scope, [1] app-scope.  Server sends one COOL_REQ on
   * each; client receives.  Around each send/receive, snapshot Outer + Core globals; assert
   * snd-side tell-tale deltas (m_msgs, m_alloc_lifetime_sz, histo_total(m_histo_segs_per_msg)).
   * No need to disambiguate scope contributions to the global -- the user-facing contract is
   * "both scopes feed the same global, by default," and that is what we verify. */
  template<session::schema::ShmType SHM_TYPE>
  void test_send_receive_app_scope_serializer_stats_shm()
  {
    using shm::stat::Outer_serializer_stats;
    using shm::stat::Core_serializer_stats;
    using shm::stat::Outer_serializer_global_stats;
    using shm::stat::Core_serializer_global_stats;

#if 1
    Logger* const logger = nullptr;
#else
    Logger* const logger = g_logger_console;
#endif

    constexpr size_t SEG_INIT_SZ = 8 * 1024;

    Logger::this_thread_set_logged_nickname("U", g_logger_console);
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

    using Cli_session_t = typename Session_pair<MqType::NONE, false, SHM_TYPE>::Client_session_t;
    using Arena = typename Cli_session_t::Arena;
    using Struc_channel_t = typename Cli_session_t::template Structured_channel<Body>;

    std::atomic<bool> err_fired{false};
    auto err_handler = [&](auto&&...) { err_fired = true; };

    // Two init-channels per side: [0] session-scope, [1] app-scope.
    auto raw_pair = make_session_channel_pair<MqType::NONE, false, SHM_TYPE>(logger, false, 0, 2);
    auto& sessions = *raw_pair.m_sessions;

    /* Server side: tag-form ctor for both scopes (works under both SHM-providers:
     * Server_session has both session_shm() and app_shm() either way). */
    auto srv_session_chan
      = std::make_unique<Struc_channel_t>(logger, std::move(raw_pair.m_srv_channels[0]),
                                          Channel_base::S_SERIALIZE_VIA_SESSION_SHM,
                                          &sessions.m_srv_session);
    auto srv_app_chan
      = std::make_unique<Struc_channel_t>(logger, std::move(raw_pair.m_srv_channels[1]),
                                          Channel_base::S_SERIALIZE_VIA_APP_SHM,
                                          &sessions.m_srv_session);

    /* Client side: tag-form ctor for both channels.  cli_session_chan is straightforward.
     * For cli_app_chan, however, the right tag depends on the SHM provider's nature:
     *   - SHM-classic (arena-*sharing*): per-pool semantics; the client's per-app pool is what
     *     resolves app-scope messages -- so use `S_SERIALIZE_VIA_APP_SHM`, which is fine because
     *     SHM-classic Client_session does have `app_shm()`.
     *   - SHM-jemalloc (arena-*lending*): there is no per-app client-side arena (Client_session
     *     has no `app_shm()`, as each side maintains its own arenas, and the lifetime here is that of Session_server);
     *     the session-scope SHM-jemalloc handle is a process-to-process
     *     resolver that handles any incoming SHM-jemalloc-borne messages, including ones the
     *     server sent from its app-scope arena.  So use `S_SERIALIZE_VIA_SESSION_SHM`. */
    auto cli_session_chan
      = std::make_unique<Struc_channel_t>(logger, std::move(raw_pair.m_cli_channels[0]),
                                          Channel_base::S_SERIALIZE_VIA_SESSION_SHM,
                                          &sessions.m_cli_session);
    std::unique_ptr<Struc_channel_t> cli_app_chan;
    if constexpr(SHM_TYPE == session::schema::ShmType::CLASSIC)
    {
      cli_app_chan
        = std::make_unique<Struc_channel_t>(logger, std::move(raw_pair.m_cli_channels[1]),
                                            Channel_base::S_SERIALIZE_VIA_APP_SHM,
                                            &sessions.m_cli_session);
    }
    else
    {
      static_assert(SHM_TYPE == session::schema::ShmType::JEMALLOC);
      cli_app_chan
        = std::make_unique<Struc_channel_t>(logger, std::move(raw_pair.m_cli_channels[1]),
                                            Channel_base::S_SERIALIZE_VIA_SESSION_SHM,
                                            &sessions.m_cli_session);
    }

    srv_session_chan->start(err_handler);
    srv_app_chan->start(err_handler);
    cli_session_chan->start(err_handler);
    cli_app_chan->start(err_handler);

    Single_thread_task_loop loop_u1{logger, "app_scope_u1"};
    loop_u1.start();
    boost::promise<void> test_done;

    loop_u1.post([&]()
    {
      const auto& outer_global = Outer_serializer_global_stats<Arena>::get().stats_default();
      const auto& core_global = Core_serializer_global_stats<Arena>::get().stats_default();

      /* Helper: send one COOL_REQ from server-side `sender` and wait for it on client-side
       * `receiver`; snapshot pre/post Outer + Core globals; assert tell-tale snd-side deltas. */
      auto do_one_round = [&](Struc_channel_t& sender, Struc_channel_t& receiver, const char* tag)
      {
        FLOW_LOG_INFO("App-scope SHM round [" << tag << "]: sending one msg.");

        Outer_serializer_stats outer_pre, outer_post;
        Core_serializer_stats core_pre, core_post;
        stats_assign(&outer_pre, outer_global);
        stats_assign(&core_pre, core_global);

        boost::shared_ptr<void> rcv_keeper;
        boost::promise<void> got_msg;
        receiver.expect_msg(Body::COOL_REQ, [&](auto req_in)
        {
          rcv_keeper = req_in;
          got_msg.set_value();
        });

        auto msg = sender.create_msg(SEG_INIT_SZ);
        msg.body_root()->initCoolReq().setCoolVal(77);
        sender.send(&msg);
        ASSERT_EQ(got_msg.get_future().wait_for(seconds{3}), boost::future_status::ready)
          << "Receiver did not receive the [" << tag << "] message.";

        // Drop the server-side Msg_out and the client-side Msg_in: dtor sampling fires on snd.
        msg = {};
        rcv_keeper.reset();

        stats_assign(&outer_post, outer_global);
        stats_assign(&core_post, core_global);

        // Snd-side tell-tales on Outer (heap envelope) and Core (in-SHM payload).
        EXPECT_EQ(outer_post.m_snd.m_msgs - outer_pre.m_snd.m_msgs, 1u) << tag;
        EXPECT_GT(outer_post.m_snd.m_alloc_lifetime_sz, outer_pre.m_snd.m_alloc_lifetime_sz) << tag;
        EXPECT_EQ(histo_total(outer_post.m_snd.m_histo_segs_per_msg)
                  - histo_total(outer_pre.m_snd.m_histo_segs_per_msg), 1u) << tag;
        EXPECT_EQ(core_post.m_snd.m_msgs - core_pre.m_snd.m_msgs, 1u) << tag;
        EXPECT_GT(core_post.m_snd.m_alloc_lifetime_sz, core_pre.m_snd.m_alloc_lifetime_sz) << tag;
        EXPECT_EQ(histo_total(core_post.m_snd.m_histo_segs_per_msg)
                  - histo_total(core_pre.m_snd.m_histo_segs_per_msg), 1u) << tag;
      };

      do_one_round(*srv_session_chan, *cli_session_chan, "session-scope");
      do_one_round(*srv_app_chan, *cli_app_chan, "app-scope");

      test_done.set_value();
    });

    test_done.get_future().wait();
    loop_u1.stop();

    EXPECT_FALSE(err_fired);
  } // test_send_receive_app_scope_serializer_stats_shm()

  /* Reset semantics for stat::Heap_serializer_stats.  Uses a *local* Heap_serializer_stats_p instance,
   * not the global, so this case is hermetic w/r/t sibling TESTs in the same executable.
   * (Global-reset semantics belong in flow/'s own stat-set test, not here.) */
  inline void test_serializer_stats_reset_semantics()
  {
    Logger::this_thread_set_logged_nickname("U", g_logger_console);
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Serializer_stats reset semantics: starting.");

    // Local instance: default-cted via stat::Heap_serializer_stats_cfg.
    stat::Heap_serializer_stats s;

    // Drive a sample across the major categories.
    s.m_snd.m_msgs.store(10);
    s.m_snd.m_msgs_outstanding.store(2);
    s.m_snd.m_msgs_outstanding_hi_wmark.store(7);
    s.m_snd.m_alloc_lifetime_sz.store(123456);
    s.m_snd.m_alloc_outstanding_sz.store(8192);
    s.m_snd.m_alloc_outstanding_sz_hi_wmark.store(40000);
    s.m_snd.m_histo_msg_alloc_sz.record_value(8 * 1024);
    s.m_snd.m_histo_msg_used_sz.record_value(2 * 1024);
    s.m_snd.m_histo_segs_per_msg.record_value(1);
    s.m_snd.m_histo_segs_per_msg.record_value(2);
    s.m_snd.m_big_leaf_alloc_count.store(3);
    s.m_snd.m_histo_big_leaf_sz.record_value(20 * 1024);
    s.m_snd.m_msgs_with_big_leaf.store(1);
    s.m_snd.m_seg_grow_cap_lock_count.store(0);
    s.m_snd.m_frame_lifetime_sz.store(64);

    s.m_rcv.m_msgs_outstanding.store(1);
    s.m_rcv.m_msgs_outstanding_hi_wmark.store(5);
    s.m_rcv.m_alloc_lifetime_sz.store(999);
    s.m_rcv.m_alloc_outstanding_sz.store(2048);
    s.m_rcv.m_alloc_outstanding_sz_hi_wmark.store(10000);
    s.m_rcv.m_used_outstanding_sz.store(1024);
    s.m_rcv.m_used_outstanding_sz_hi_wmark.store(5000);
    s.m_rcv.m_histo_msg_alloc_sz.record_value(64 * 1024);
    s.m_rcv.m_histo_msg_used_sz.record_value(2 * 1024);

    // Sanity: at least one histogram non-empty before reset (otherwise the test proves little).
    ASSERT_EQ(histo_total(s.m_snd.m_histo_segs_per_msg), 2u);

    stats_reset(&s, stat::Heap_serializer_stats{});

    // Accumulators: zeroed.
    EXPECT_EQ(s.m_snd.m_msgs, 0u);
    EXPECT_EQ(s.m_snd.m_alloc_lifetime_sz, 0u);
    EXPECT_EQ(s.m_snd.m_big_leaf_alloc_count, 0u);
    EXPECT_EQ(s.m_snd.m_msgs_with_big_leaf, 0u);
    EXPECT_EQ(s.m_snd.m_seg_grow_cap_lock_count, 0u);
    EXPECT_EQ(s.m_snd.m_frame_lifetime_sz, 0u);
    EXPECT_EQ(s.m_rcv.m_alloc_lifetime_sz, 0u);

    // Gauges: preserved.
    EXPECT_EQ(s.m_snd.m_msgs_outstanding, 2u);
    EXPECT_EQ(s.m_snd.m_alloc_outstanding_sz, 8192u);
    EXPECT_EQ(s.m_rcv.m_msgs_outstanding, 1u);
    EXPECT_EQ(s.m_rcv.m_alloc_outstanding_sz, 2048u);
    EXPECT_EQ(s.m_rcv.m_used_outstanding_sz, 1024u);

    // High-water marks: snapped to current gauge value (new observation window).
    EXPECT_EQ(s.m_snd.m_msgs_outstanding_hi_wmark, 2u);
    EXPECT_EQ(s.m_snd.m_alloc_outstanding_sz_hi_wmark, 8192u);
    EXPECT_EQ(s.m_rcv.m_msgs_outstanding_hi_wmark, 1u);
    EXPECT_EQ(s.m_rcv.m_alloc_outstanding_sz_hi_wmark, 2048u);
    EXPECT_EQ(s.m_rcv.m_used_outstanding_sz_hi_wmark, 1024u);

    // Histograms: cleared but bucket config still valid (record + recount).
    EXPECT_EQ(histo_total(s.m_snd.m_histo_msg_alloc_sz), 0u);
    EXPECT_EQ(histo_total(s.m_snd.m_histo_msg_used_sz), 0u);
    EXPECT_EQ(histo_total(s.m_snd.m_histo_segs_per_msg), 0u);
    EXPECT_EQ(histo_total(s.m_snd.m_histo_big_leaf_sz), 0u);
    EXPECT_EQ(histo_total(s.m_rcv.m_histo_msg_alloc_sz), 0u);
    EXPECT_EQ(histo_total(s.m_rcv.m_histo_msg_used_sz), 0u);

    s.m_snd.m_histo_segs_per_msg.record_value(1);
    EXPECT_EQ(histo_total(s.m_snd.m_histo_segs_per_msg), 1u);
  } // test_serializer_stats_reset_semantics()

  /* Tier-2 sanity smoke test: drive Struct_builder + Struct_reader directly with stack-local
   * Serializer_stats objects, bypassing struc::Channel / Msg_out / Msg_in entirely.  Confirms
   * that m_stats hooks fire when wired through Config without any of the Channel boilerplate.
   * Not exhaustive -- end-to-end coverage above already proves the hooks fire under realistic
   * conditions; this is just a typecheck that direct use also works.
   *
   * Three modes via `SHM_TYPE`:
   *   - PH (NONE): Heap_fixed_builder + Heap_reader; one stats axis (heap).
   *   - SH-classic, SH-jemalloc: shm::Builder + shm::Reader; two axes -- Outer (heap-resident
   *     SHM-handle envelope, lifecycle identical to PH-heap) + Core (in-SHM payload, snd-side
   *     identical lifecycle to PH-heap, rcv-side a structural no-op due to zero-copy).
   *
   * Mirrored under SH: every PH check on `local.{m_snd|m_rcv}` becomes a check on the SH-Outer
   * axis (`primary_local.{m_snd|m_rcv}`).  The SH-Core axis gets one spot-check per group, just
   * to confirm wiring is live; rcv-side core is asserted entirely flat (zero-copy).
   *
   * Builder side: build a tiny Body, then emit_serialization() and copy each emitted blob
   * (simulating IPC transmission).
   * Reader side: feed the copied segments into a reader, deserialize, sanity-check the
   * round-tripped value.
   *
   * Also sanity-checks that wiring m_stats to *local* Stat_sets leaves the process-global
   * singletons untouched (spot-check on a few representative fields). */
  template<session::schema::ShmType SHM_TYPE = session::schema::ShmType::NONE>
  void test_direct_builder_smoke()
  {
    using flow::util::Blob_sans_log_context;
    using shm::stat::Outer_serializer_stats;
    using shm::stat::Core_serializer_stats;
    using shm::stat::Outer_serializer_global_stats;
    using shm::stat::Core_serializer_global_stats;
    using std::conditional_t;

    constexpr bool SHM = (SHM_TYPE != session::schema::ShmType::NONE);

#if 1
    [[maybe_unused]] Logger* const logger = nullptr; // Normal test run: object internals silent.
#else
    [[maybe_unused]] Logger* const logger = g_logger_console; // Flip for debugging.
#endif

    Logger::this_thread_set_logged_nickname("U", g_logger_console);
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Direct " << (SHM ? "shm::" : "Heap_") << "{builder|reader} smoke test: starting "
                  "(SHM_TYPE=[" << int(SHM_TYPE) << "]).");

    /* Primary stats axis: PH=stat::Heap_serializer_stats; SH=Outer_serializer_stats.  Both have
     * identical struct shape (m_snd + m_rcv with the same field names), so all post-build /
     * post-deserialize / post-dtor assertions below funnel through `primary_local` uniformly. */
    using Primary_stats = conditional_t<SHM, Outer_serializer_stats, stat::Heap_serializer_stats>;
    Primary_stats primary_local;

    /* SH-only: SHM-resident Core axis.  Snd-side has same lifecycle as PH heap; rcv-side is
     * structurally a no-op due to zero-copy (nothing to track on receive). */
    [[maybe_unused]] Core_serializer_stats core_local;

    // Pre-snapshots of corresponding globals; we assert these stay flat at the end.
    Primary_stats primary_pre;
    [[maybe_unused]] Core_serializer_stats core_pre;
    if constexpr(SHM)
    {
      using Arena = typename Session_pair<MqType::NONE, false, SHM_TYPE>::Client_session_t::Arena;
      stats_assign(&primary_pre, Outer_serializer_global_stats<Arena>::get().stats_default());
      stats_assign(&core_pre, Core_serializer_global_stats<Arena>::get().stats_default());
    }
    else
    {
      stats_assign(&primary_pre, stat::Heap_serializer_global_stats::get().stats_default());
    }

    // Snapshot of the serialization, copied byte-for-byte to simulate IPC transmission.
    std::vector<Blob_sans_log_context> rcv_segs;

    /* Generic lambdas absorbing the parts of the build-emit-copy / feed-deserialize-check ritual
     * common to PH and SH (only the builder/reader/session types differ; those are fixed at the
     * call site).  Primary axis = PH-heap (PH) or SH-Outer (SH); identical struct shape so the
     * checks are uniform.  Lifecycle-coupled checks live inside these lambdas so they fire while
     * the builder/reader is alive. */
    auto do_build_emit_copy = [&](auto& b, auto&& session_arg)
    {
      auto* mb = b.payload_msg_builder();
      auto root = mb->template initRoot<Body>();
      root.initCoolReq().setCoolVal(99);

      auto check_snd_bumps = [&]()
      {
        EXPECT_GE(primary_local.m_snd.m_msgs, 1u);
        EXPECT_GE(primary_local.m_snd.m_msgs_outstanding, 1u);
        EXPECT_GT(primary_local.m_snd.m_alloc_lifetime_sz, 0u);
      };

      /* Primary-axis snd-side bump timing depends on the engine:
       *   - PH: heap engine accumulates at build (init/alloc).
       *   - SH-Outer: flat-until-emit; the Outer envelope is only serialized inside
       *     emit_serialization() below.
       * So check pre-emit for PH and post-emit for SH. */
      if constexpr(!SHM)
      {
        check_snd_bumps();
      }

      // Emit and copy.  Tiny payload => no splitting expected.
      Segment_bufs target_blobs;
      util::Blob_mutable hdr_blob;
      EXPECT_NO_THROW(b.emit_serialization(&target_blobs, &hdr_blob, nullptr, session_arg));
      ASSERT_FALSE(target_blobs.empty());

      if constexpr(SHM)
      {
        check_snd_bumps();
      }

      rcv_segs.reserve(target_blobs.size());
      for (const auto& src : target_blobs)
      {
        rcv_segs.emplace_back();
        rcv_segs.back().assign_copy(src);
      }
    };

    auto do_read_check = [&](auto& r)
    {
      for (auto& seg : rcv_segs)
      {
        r.add_serialization_segment(std::move(seg));
      }
      auto rcv_root = r.template deserialization<Body>(nullptr);
      ASSERT_TRUE(rcv_root.isCoolReq());
      EXPECT_EQ(rcv_root.getCoolReq().getCoolVal(), 99u);

      // Reader alive: rcv gauges + histograms advanced (sampled at deserialization()).
      EXPECT_EQ(primary_local.m_rcv.m_msgs_outstanding, 1u);
      EXPECT_GT(primary_local.m_rcv.m_alloc_lifetime_sz, 0u);
      EXPECT_GT(primary_local.m_rcv.m_alloc_outstanding_sz, 0u);
      EXPECT_GT(primary_local.m_rcv.m_used_outstanding_sz, 0u);
      EXPECT_EQ(histo_total(primary_local.m_rcv.m_histo_msg_alloc_sz), 1u);
      EXPECT_EQ(histo_total(primary_local.m_rcv.m_histo_msg_used_sz), 1u);
    };

    if constexpr(SHM)
    {
      /* SH: requires a live session pair.  Configs come from the session itself; replace the
       * wired-in global m_stats pointer(s) with our locals so this test stays hermetic.
       * Sessions must outlive both Builder and Reader.  Note `n_init_channels = 0`: this site
       * uses sessions only -- no channels needed. */
      auto raw_pair = make_session_channel_pair<MqType::NONE, false, SHM_TYPE>
                        (logger, false, 0, 0);
      auto& sessions = *raw_pair.m_sessions;

      auto snd_cfg = sessions.m_cli_session.session_shm_builder_config();
      snd_cfg.m_stats = &primary_local.m_snd;
      snd_cfg.m_core_stats = &core_local.m_snd;

      auto rcv_cfg = sessions.m_srv_session.session_shm_reader_config();
      rcv_cfg.m_stats = &primary_local.m_rcv;

      auto lender_session = sessions.m_cli_session.session_shm_lender_session();

      using Builder_t = typename decltype(snd_cfg)::Builder;
      using Reader_t = typename decltype(rcv_cfg)::Reader;

      {
        Builder_t b{snd_cfg};
        do_build_emit_copy(b, lender_session);
        // Core axis: spot-check (during-builder-life group; SH-only).
        EXPECT_GE(core_local.m_snd.m_msgs_outstanding, 1u);
      }
      // Core axis: spot-check (post-builder-dtor group; SH-only).
      EXPECT_EQ(core_local.m_snd.m_msgs_outstanding, 0u);

      {
        Reader_t r{rcv_cfg};
        do_read_check(r);
      }

      /* Mirrored-side instantiation coverage: the same session-scope config-vending APIs off the *other*
       * side of each (above: cli builder/lender, srv reader; now the reverse).  These are template members:
       * never even compiled unless called -- the asymmetric-API bug class -- so sanity-check only; the
       * round-trip above already proves the machinery, and a 2nd one would perturb this test's stats math. */
      {
        auto mirror_snd_cfg = sessions.m_srv_session.session_shm_builder_config();
        EXPECT_TRUE(mirror_snd_cfg.m_arena);
        EXPECT_TRUE(sessions.m_srv_session.session_shm_lender_session());
        auto mirror_rcv_cfg = sessions.m_cli_session.session_shm_reader_config();
        EXPECT_TRUE(mirror_rcv_cfg.m_session);
      }
    } // if constexpr(SHM)
    else
    {
      Heap_fixed_builder::Config snd_cfg
      {
        .m_logger_ptr = nullptr,
        .m_seg_and_frame_sz_cap = 64 * 1024,
        .m_segment_sz_init = 8 * 1024,
        .m_frame_prefix_sz = 0,
        .m_stats = &primary_local.m_snd
      };
      Heap_reader::Config rcv_cfg
      {
        .m_logger_ptr = nullptr,
        .m_n_segments_est = 0,
        .m_stats = &primary_local.m_rcv
      };

      {
        Heap_fixed_builder b{snd_cfg};
        do_build_emit_copy(b, NULL_SESSION);
      }
      {
        Heap_reader r{rcv_cfg};
        do_read_check(r);
      }
    } // else if (!SHM)

    /* Builder + reader both destroyed.  Primary axis: snd gauges back to 0, snd histos sampled +1;
     * rcv gauges back to 0 (rcv lifetime + histos were sampled at deserialization() and verified
     * during reader's life, above). */
    EXPECT_EQ(primary_local.m_snd.m_msgs_outstanding, 0u);
    EXPECT_EQ(primary_local.m_snd.m_alloc_outstanding_sz, 0u);
    EXPECT_EQ(histo_total(primary_local.m_snd.m_histo_segs_per_msg), 1u);
    EXPECT_EQ(primary_local.m_rcv.m_msgs_outstanding, 0u);
    EXPECT_EQ(primary_local.m_rcv.m_alloc_outstanding_sz, 0u);
    EXPECT_EQ(primary_local.m_rcv.m_used_outstanding_sz, 0u);

    // Globals untouched (snd + rcv): spot-check on the most representative fields.
    Primary_stats primary_post;
    if constexpr(SHM)
    {
      using Arena = typename Session_pair<MqType::NONE, false, SHM_TYPE>::Client_session_t::Arena;
      stats_assign(&primary_post, Outer_serializer_global_stats<Arena>::get().stats_default());

      /* Core globals also untouched.  Plus: SH-Core rcv axis is structurally always flat (zero-copy),
       * verified here via the global which equals rcv-side post == pre. */
      Core_serializer_stats core_post;
      stats_assign(&core_post, Core_serializer_global_stats<Arena>::get().stats_default());
      EXPECT_EQ(core_post.m_snd.m_alloc_lifetime_sz, core_pre.m_snd.m_alloc_lifetime_sz);
      EXPECT_EQ(core_post.m_rcv.m_alloc_lifetime_sz, core_pre.m_rcv.m_alloc_lifetime_sz);
      EXPECT_EQ(core_local.m_rcv.m_alloc_lifetime_sz, 0u);
    }
    else
    {
      stats_assign(&primary_post, stat::Heap_serializer_global_stats::get().stats_default());
    }
    EXPECT_EQ(primary_post.m_snd.m_msgs, primary_pre.m_snd.m_msgs);
    EXPECT_EQ(primary_post.m_snd.m_alloc_lifetime_sz, primary_pre.m_snd.m_alloc_lifetime_sz);
    EXPECT_EQ(histo_total(primary_post.m_snd.m_histo_segs_per_msg),
              histo_total(primary_pre.m_snd.m_histo_segs_per_msg));
    EXPECT_EQ(primary_post.m_rcv.m_alloc_lifetime_sz, primary_pre.m_rcv.m_alloc_lifetime_sz);
    EXPECT_EQ(histo_total(primary_post.m_rcv.m_histo_msg_alloc_sz),
              histo_total(primary_pre.m_rcv.m_histo_msg_alloc_sz));
    EXPECT_EQ(histo_total(primary_post.m_rcv.m_histo_msg_used_sz),
              histo_total(primary_pre.m_rcv.m_histo_msg_used_sz));
  } // test_direct_builder_smoke()

  /* Session_server-level app-scope config APIs: Session_server::app_shm_builder_config(Client_app) -- plus,
   * SHM-classic only, `::app_shm_lender_session(Client_app)` and `::app_shm_reader_config(Client_app)` --
   * exist for setting up app-scope-serializing channels/messages *without* a live session in hand (the
   * app-scope arena is Session_server-owned and outlives sessions).  Code (including transport_test functional-test
   * suite as of this writing) typically uses the handier Session-level
   * counterparts instead (e.g., via the `S_SERIALIZE_VIA_APP_SHM` tag ctor); being template
   * members, these were not even getting compiled absent this test.  So: instantiate them all; and assert
   * the documented redundancy -- for a given Client_app they return the same (field-for-field) configs as
   * the Session-level counterparts off a live session of that same app.
   *
   * SHM-jemalloc, being an arena-lending SHM-provider, lacks some of these APIs, as they would make no sense:
   * The shm::Reader needs the per-session `Shm_session` -- for `borrow_object()` of the SHM-stored segment
   * list -- which a session-less Session_server lacks.  SHM-classic, being arena-sharing, has those APIs,
   * as in its case Shm_session=Arena again: classic::Pool_arena.  There's no separate Shm_session type. */
  template<session::schema::ShmType SHM_TYPE>
  void test_app_shm_configs_via_session_server()
  {
#if 1
    Logger* const logger = nullptr; // Normal test run: object internals silent.
#else
    Logger* const logger = g_logger_console; // Flip for debugging.
#endif
    Logger::this_thread_set_logged_nickname("U", g_logger_console);
    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Session_server-level app-shm-config test: starting (SHM_TYPE=[" << int(SHM_TYPE) << "]).");

    // Sessions only -- no channels needed (n_init_channels = 0).
    auto raw_pair = make_session_channel_pair<MqType::NONE, false, SHM_TYPE>(logger, false, 0, 0);
    auto& sessions = *raw_pair.m_sessions;
    auto& srv = *sessions.m_srv;
    auto& srv_session = sessions.m_srv_session;

    const auto app_ptr = srv_session.client_app();
    ASSERT_TRUE(app_ptr);
    const auto& app = *app_ptr;

    constexpr size_t SEG1_SZ = 8 * 1024;

    { // Builder configs (both providers): server-level (by Client_app) = session-level, field-for-field.
      const auto srv_cfg = srv.app_shm_builder_config(app, SEG1_SZ);
      const auto ses_cfg = srv_session.app_shm_builder_config(SEG1_SZ);
      EXPECT_EQ(srv_cfg.m_logger_ptr, ses_cfg.m_logger_ptr);
      EXPECT_EQ(srv_cfg.m_segment_sz_init, SEG1_SZ);
      EXPECT_EQ(ses_cfg.m_segment_sz_init, SEG1_SZ);
      EXPECT_EQ(srv_cfg.m_frame_prefix_sz, ses_cfg.m_frame_prefix_sz);
      EXPECT_TRUE(srv_cfg.m_arena);
      EXPECT_EQ(srv_cfg.m_arena, ses_cfg.m_arena); // The app-scope arena itself: the key equality.
      EXPECT_EQ(srv_cfg.m_core_stats, ses_cfg.m_core_stats); // Both wire the same per-Arena global defaults...
      EXPECT_EQ(srv_cfg.m_stats, ses_cfg.m_stats); // ...on both stats axes.

      // Default-segment1_sz overloads: same equality (also compile-coverage for the default args).
      EXPECT_EQ(srv.app_shm_builder_config(app).m_segment_sz_init,
                srv_session.app_shm_builder_config().m_segment_sz_init);
    }

    if constexpr(SHM_TYPE == session::schema::ShmType::CLASSIC)
    { // Lender-session + reader config: SHM-classic only (see doc comment atop this function).
      EXPECT_TRUE(srv.app_shm_lender_session(app));
      EXPECT_EQ(srv.app_shm_lender_session(app), srv_session.app_shm_lender_session());

      const auto srv_cfg = srv.app_shm_reader_config(app);
      const auto ses_cfg = srv_session.app_shm_reader_config();
      EXPECT_EQ(srv_cfg.m_logger_ptr, ses_cfg.m_logger_ptr);
      EXPECT_TRUE(srv_cfg.m_session);
      EXPECT_EQ(srv_cfg.m_session, ses_cfg.m_session);
      EXPECT_EQ(srv_cfg.m_stats, ses_cfg.m_stats);
    }

    /* Session-level app-scope APIs off the sides not yet exercised above -- instantiation coverage
     * (same rationale as the rest of this function: template members compile only if called) plus the
     * asymmetry-specific semantics per SHM-provider sort. */
    auto& cli_session = sessions.m_cli_session;
    if constexpr(SHM_TYPE == session::schema::ShmType::CLASSIC)
    {
      /* Arena-sharing: the client side has the full app-scope API set too, off its *own* handle to the
       * same underlying pool -- hence non-null but *distinct* from the server-side arena pointer. */
      const auto cli_cfg = cli_session.app_shm_builder_config(SEG1_SZ);
      EXPECT_TRUE(cli_cfg.m_arena);
      EXPECT_NE(cli_cfg.m_arena, srv_session.app_shm_builder_config(SEG1_SZ).m_arena);
      EXPECT_TRUE(cli_session.app_shm_lender_session());
      EXPECT_TRUE(cli_session.app_shm_reader_config().m_session);
    }
    else
    {
      /* Arena-lending: no client-side app-scope builder/lender (no arena of one's own to allocate in or
       * lend from) -- but reading/borrowing is scope-agnostic, so the reader configs -- both scopes, both
       * sides -- and the lender-sessions all converge on the per-session Shm_session. */
      EXPECT_EQ(srv_session.app_shm_lender_session(), srv_session.session_shm_lender_session());
      EXPECT_EQ(cli_session.app_shm_reader_config().m_session, cli_session.shm_session());
      EXPECT_EQ(srv_session.app_shm_reader_config().m_session, srv_session.shm_session());
      EXPECT_EQ(cli_session.shm_reader_config().m_session, cli_session.shm_session());
      EXPECT_EQ(srv_session.shm_reader_config().m_session, srv_session.shm_session());
    }
  } // test_app_shm_configs_via_session_server()

} // namespace (anon)

} // namespace ipc::transport::struc::test
