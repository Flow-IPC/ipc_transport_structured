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
 * Each test body below is a function template over a certain <knob> (MqType or ShmType, per body); the
 * sibling .cpp s spread the TEST()s expanding them across translation units.  This bounds compiler RAM
 * use per .cpp (translation unit) versus simply placing everything into one .cpp.  In particular each
 * per-<knob> ipc::session + struc::Channel stack costs GBs of compiler RAM, for debug-info builds, at
 * least with some Linux gcc.  2+ such translation units compiling concurrently can exhaust a smaller
 * build machine (including GitHub CI runners in 2026). */

#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/transport/struc/test/test_schema.capnp.h"
#include "ipc/transport/struc/channel_stats.hpp"
#include "ipc/transport/struc/sync_io/channel_stats.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/transport/bipc_mq_handle.hpp"
#include "ipc/transport/posix_mq_handle.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/log/log.hpp>
#include <flow/test/test_common_util.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <boost/thread/future.hpp>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ipc::transport::struc::test
{

namespace
{
  using flow::async::Single_thread_task_loop;
  using flow::log::Logger;
  using flow::Error_code;
  using flow::util::stat::print;
  using flow::util::this_thread::sleep_for;
  using session::schema::MqType;
  using session::schema::ShmType;
  using boost::chrono::milliseconds;
  using boost::chrono::seconds;

  /* Always-on console logger for test-progress output (FLOW_LOG_INFO etc.).
   * Survives across all TESTs in this TU; object internals use `logger` (toggleable) instead. */
  ipc::test::Test_logger g_logger_obj;
  Logger* const g_logger_console = &g_logger_obj;

  /* Sums sample counts across all buckets of a `Histogram_counter`; used to verify that a histogram did or did
   * not receive samples after various actions (and/or to verify cleared vs. not). */
  // (`inline` on the plain helpers here and below: a sibling TU may use only a subset; -Wunused-function otherwise.)
  inline uint64_t histo_total(const flow::util::stat::Histogram_counter& h, size_t n_buckets)
  {
    uint64_t total = 0;
    for (size_t idx = 0; idx != n_buckets; ++idx)
    {
      total += h.count_for_bucket(idx);
    }
    return total;
  }

  /* A few little helpers for preparing outgoing messages.  Nothing fancy; just clearer at call sites. */

  template<typename Pair>
  auto make_req(Pair& pair, uint64_t val, bool from_cli = true)
  {
    auto& ch = from_cli ? *pair.m_cli : *pair.m_srv;
    auto msg = ch.create_msg();
    msg.body_root()->initCoolReq().setCoolVal(val);
    return msg;
  }

  template<typename Pair>
  auto make_rsp(Pair& pair, uint64_t val, bool from_cli = false)
  {
    auto& ch = from_cli ? *pair.m_cli : *pair.m_srv;
    auto msg = ch.create_msg();
    msg.body_root()->initCoolRsp().setCoolVal(val);
    return msg;
  }

  /* End-to-end stats test.  Exercises ~every category of stat in both struc::stat::Channel_stats layers
   * (m_core = sync_io::stat::Channel_stats: Snd/Rcv Msg + Rcv expect/cache/routing/RTT) and the struc-level
   * m_sync_req extension.  Phases run sequentially on thread U1; where needed, U1 awaits the
   * other side (thread W or V1) via a per-phase promise before checking stats and proceeding.
   *
   * Thread structure:
   *   U (test main) - renamed; just waits on a final promise.
   *   U1 (Single_thread_task_loop "cli_u1") -- runs the phase sequence.  `sync_request()` blocks U1;
   *                                            server's thread W continues to service in the meantime.
   *   V1 (Single_thread_task_loop "srv_v1") -- used only for Phase F (schedule_from_now() for delayed send).
   *   Thread W (internal, owned by each struc::Channel) -- where expect_msgs()/expect_msg()/async_request()
   *                                                        handlers fire.  Post-to-V1 done only when
   *                                                        we need to defer work (delayed send). */
  template<MqType MQ_TYPE>
  void test_send_receive_stats()
  {
#if 1
    Logger* const logger = nullptr; // Normal test run: object internals silent.
#else
    Logger* const logger = g_logger_console; // Flip for debugging.
#endif
    Logger::this_thread_set_logged_nickname("U", g_logger_console);

    std::atomic<bool> cli_err_fired{false};
    std::atomic<bool> srv_err_fired{false};
    auto pair = make_session_struc_pair<Body, MQ_TYPE, true>
                  ([&](auto&&...) { cli_err_fired = true; },
                   [&](auto&&...) { srv_err_fired = true; },
                   logger);

    Single_thread_task_loop loop_u1{logger, "cli_u1"};
    Single_thread_task_loop loop_v1{logger, "srv_v1"};
    loop_u1.start();
    loop_v1.start();

    boost::promise<void> test_done;

    loop_u1.post([&]()
    {
      FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

      /* Session Info_collector handles: used by Phase 0 below, and referenced again in Phase A for the
       * cross-phase independence check (app-channel ops in Phase A must not perturb SMC stats). */
      auto* const cli_sc = pair.m_sessions->m_cli_session.info_collector();
      auto* const srv_sc = pair.m_sessions->m_srv_session.info_collector();
      EXPECT_NE(cli_sc, nullptr) << "Client Info_collector should be engaged post-sync_connect().";
      EXPECT_NE(srv_sc, nullptr) << "Server Info_collector should be engaged post-async_accept().";

      /* ---------- Phase 0: SMC stats snapshot (for Phase A independence check) ----------
       * Deeper SMC-stats smoke (nonzero asserts, per-side reset semantics, SHM-provider variants) lives in
       * test_smc_info_collector_smoke() below.  Here we just snapshot the SMC stats so Phase A can confirm
       * that subsequent struc::Channel-under-test traffic does not perturb the SMC (stats are per-channel;
       * the SMC is not special). */
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase 0: SMC stats snapshot.");
        FLOW_LOG_INFO("Phase 0 cli SMC struc stats:\n" << print(cli_sc->master_channel_stats()));
        FLOW_LOG_INFO("Phase 0 srv SMC struc stats:\n" << print(srv_sc->master_channel_stats()));
      }
      const auto smc_snap_cli = cli_sc->master_channel_stats();
      const auto smc_snap_srv = srv_sc->master_channel_stats();
      const auto smc_snap_cli_snd = cli_sc->master_channel_transport_snd_stats();
      const auto smc_snap_srv_snd = srv_sc->master_channel_transport_snd_stats();
      const auto smc_snap_cli_rcv = cli_sc->master_channel_transport_rcv_stats();
      const auto smc_snap_srv_rcv = srv_sc->master_channel_transport_rcv_stats();

      // ---------- Phase A: initial state ----------
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase A: initial state.");
        const auto s = pair.m_cli->stats();
        EXPECT_EQ(s.m_core.m_snd.m_msg.m_user_msgs, 0u);
        EXPECT_EQ(s.m_core.m_snd.m_msg.m_notifications, 0u);
        EXPECT_EQ(s.m_core.m_snd.m_msg.m_requests, 0u);
        EXPECT_EQ(s.m_core.m_rcv.m_msg.m_user_msgs, 0u);
        EXPECT_EQ(s.m_core.m_rcv.m_unsolicited_msgs_routed, 0u);
        EXPECT_EQ(s.m_core.m_rcv.m_expect_msgs_active, 0u);
        EXPECT_EQ(s.m_core.m_rcv.m_expect_response_one_off_active, 0u);
        EXPECT_EQ(s.m_sync_req.m_count, 0u);
        EXPECT_EQ(s.m_sync_req.m_timeouts, 0u);
        EXPECT_EQ(s.m_sync_req.m_late_responses, 0u);

        /* SMC independence: reading pair.m_cli's channel stats above did not touch the SMC.
         * Struc-level stats: exact match (nothing between snapshot and now touches the SMC at
         * the struc layer).  Transport-level stats: use GE, because an auto-ping sent/received
         * in the background window is possible and harmless (increments m_total_msgs by 1 on
         * each side without any struc-level effect). */
        EXPECT_EQ(cli_sc->master_channel_stats().m_core.m_snd.m_msg.m_user_msgs,
                  smc_snap_cli.m_core.m_snd.m_msg.m_user_msgs);
        EXPECT_EQ(cli_sc->master_channel_stats().m_core.m_rcv.m_msg.m_user_msgs,
                  smc_snap_cli.m_core.m_rcv.m_msg.m_user_msgs);
        EXPECT_EQ(srv_sc->master_channel_stats().m_core.m_snd.m_msg.m_user_msgs,
                  smc_snap_srv.m_core.m_snd.m_msg.m_user_msgs);
        EXPECT_EQ(srv_sc->master_channel_stats().m_core.m_rcv.m_msg.m_user_msgs,
                  smc_snap_srv.m_core.m_rcv.m_msg.m_user_msgs);
        EXPECT_GE(cli_sc->master_channel_transport_snd_stats().m_total_msgs,
                  smc_snap_cli_snd.m_total_msgs);
        EXPECT_GE(srv_sc->master_channel_transport_snd_stats().m_total_msgs,
                  smc_snap_srv_snd.m_total_msgs);
        EXPECT_GE(cli_sc->master_channel_transport_rcv_stats().m_total_msgs,
                  smc_snap_cli_rcv.m_total_msgs);
        EXPECT_GE(srv_sc->master_channel_transport_rcv_stats().m_total_msgs,
                  smc_snap_srv_rcv.m_total_msgs);
      }

      // ---------- Phase B: notifications (send-side + receive-side routing, expect_msgs) ----------
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase B: notifications.");

        std::atomic<int> srv_rcv_count{0};
        boost::promise<void> srv_got_2;
        pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&&)
        {
          // Thread W (srv).  Plain recording; no response.
          if (++srv_rcv_count == 2)
          {
            srv_got_2.set_value();
          }
        });

        // Plain notification.
        {
          auto msg = make_req(pair, 1);
          pair.m_cli->send(&msg);
        }
        // Notification with native handle.
        {
          auto msg = make_req(pair, 2);
          msg.store_native_handle_or_null(util::Native_handle{::dup(STDOUT_FILENO)});
          pair.m_cli->send(&msg);
        }

        ASSERT_EQ(srv_got_2.get_future().wait_for(seconds{3}), boost::future_status::ready)
          << "Server did not receive both notifications.";

        const auto cs = pair.m_cli->stats();
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_user_msgs, 2u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_notifications, 2u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_requests, 0u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_handle_bearing_msgs, 1u);
        // Small msg => single-segment => 1 blob per msg (post-optimization: header in seg0 blob).
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_single_segment_msgs, 2u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_multi_segment_msgs, 0u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_total_segments, 2u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_total_low_lvl_blobs, 2u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_msgs_with_split_segments, 0u);

        const auto ss = pair.m_srv->stats();
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_user_msgs, 2u);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_notifications, 2u);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_handle_bearing_msgs, 1u);
        EXPECT_EQ(ss.m_core.m_rcv.m_unsolicited_msgs_routed, 2u);
        EXPECT_EQ(ss.m_core.m_rcv.m_unsolicited_msgs_cached, 0u);
        EXPECT_EQ(ss.m_core.m_rcv.m_expect_msgs_count, 1u); // Sticky registered once so far.
        EXPECT_EQ(ss.m_core.m_rcv.m_expect_msgs_active, 1u);
        EXPECT_EQ(ss.m_core.m_rcv.m_expect_msg_count, 0u);
        EXPECT_EQ(ss.m_core.m_rcv.m_expect_q_immediate_pops, 0u);

        /* The rcv-side structural counters are maintained by the parse path -- code independent of the
         * snd-side's -- yet after a completed exchange they must mirror the snd side exactly: */
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_requests, 0u);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_single_segment_msgs, 2u);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_multi_segment_msgs, 0u);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_total_segments, 2u);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_total_low_lvl_blobs, 2u);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_msgs_with_split_segments, 0u);

        FLOW_LOG_INFO("Phase B cli stats:\n" << print(cs));
        FLOW_LOG_INFO("Phase B srv stats:\n" << print(ss));
      }

      // ---------- Phase C: async_request() + response ----------
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase C: async_request() + response.");

        // Re-register srv's sticky handler to respond (instead of just counting).
        ASSERT_TRUE(pair.m_srv->undo_expect_msgs(Body::COOL_REQ));
        pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&& req_in)
        {
          auto rsp = make_rsp(pair, req_in->body_root().getCoolReq().getCoolVal());
          pair.m_srv->send(&rsp, req_in.get());
        });

        boost::promise<void> got_rsp;
        auto req = make_req(pair, 100);
        Error_code err;
        const bool ok = pair.m_cli->async_request(&req, nullptr, nullptr,
                                                  [&](auto&&) { got_rsp.set_value(); }, &err);
        ASSERT_TRUE(ok);
        ASSERT_FALSE(err);

        ASSERT_EQ(got_rsp.get_future().wait_for(seconds{3}), boost::future_status::ready)
          << "Response did not arrive.";

        const auto cs = pair.m_cli->stats();
        const auto ss = pair.m_srv->stats();

        // Sender (client): +1 request, specifically one-off.
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_user_msgs, 3u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_requests, 1u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_requests_one_off, 1u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_request_responses, 0u);

        // Server: it sent the response: +1 notification, specifically a notification-response.
        EXPECT_EQ(ss.m_core.m_snd.m_msg.m_user_msgs, 1u);
        EXPECT_EQ(ss.m_core.m_snd.m_msg.m_notifications, 1u);
        EXPECT_EQ(ss.m_core.m_snd.m_msg.m_notification_responses, 1u);
        EXPECT_EQ(ss.m_core.m_snd.m_msg.m_requests, 0u);

        // Client received the response: on rcv-side, responses count as notifications (doc).
        EXPECT_EQ(cs.m_core.m_rcv.m_msg.m_user_msgs, 1u);
        EXPECT_EQ(cs.m_core.m_rcv.m_msg.m_notifications, 1u);
        EXPECT_EQ(cs.m_core.m_rcv.m_msg.m_notification_responses, 1u);

        /* Server received the request -- but request-ness is not transmitted on the wire (see the
         * m_notifications doc header): on the rcv side every user message counts as a notification, and the
         * requests family is structurally zero.  Assert exactly those documented invariants: */
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_notifications, ss.m_core.m_rcv.m_msg.m_user_msgs);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_requests, 0u);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_requests_one_off, 0u);
        EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_request_responses, 0u);
        EXPECT_EQ(cs.m_core.m_rcv.m_msg.m_request_responses, 0u); // (Responses arrive as notifications; see above.)

        // One-off expectation satisfied (gauge back to 0).
        EXPECT_EQ(cs.m_core.m_rcv.m_expect_response_one_off_active, 0u);
        EXPECT_EQ(cs.m_core.m_rcv.m_unexpected_responses, 0u);

        // One RTT sample was recorded.
        EXPECT_EQ(histo_total(cs.m_core.m_rcv.m_histo_one_off_request_rtt_usec,
                              sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                  1u);

        FLOW_LOG_INFO("Phase C cli stats:\n" << print(cs));
        FLOW_LOG_INFO("Phase C srv stats:\n" << print(ss));
      }

      // ---------- Phase D: expect_msg() (one-off) + cached messages ----------
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase D: expect_msg() (one-off) + cached messages.");

        // Remove server-side sticky so an incoming COOL_REQ will get cached, not routed.
        ASSERT_TRUE(pair.m_srv->undo_expect_msgs(Body::COOL_REQ));

        const auto cached_before = pair.m_srv->stats().m_core.m_rcv.m_unsolicited_msgs_cached;

        // Client: send unsolicited COOL_REQ.  Server will have no handler for it -> cache.
        {
          auto msg = make_req(pair, 200);
          pair.m_cli->send(&msg);
        }

        // Wait for srv's thread W to enqueue it.  (No IPC-level event to await directly; poll the stat.)
        bool cached_ok = false;
        for (unsigned int idx = 0; idx != 60; ++idx)
        {
          if (pair.m_srv->stats().m_core.m_rcv.m_unsolicited_msgs_cached > cached_before)
          {
            cached_ok = true;
            break;
          }
          sleep_for(milliseconds{50});
        }
        ASSERT_TRUE(cached_ok) << "Expected unsolicited msg to be cached on server.";

        // Now register one-off: should immediately pop the queued msg.
        boost::promise<void> popped;
        ASSERT_TRUE(pair.m_srv->expect_msg(Body::COOL_REQ, [&](auto&&) { popped.set_value(); }));

        ASSERT_EQ(popped.get_future().wait_for(seconds{3}), boost::future_status::ready)
          << "Immediate-pop handler did not fire.";

        const auto ss = pair.m_srv->stats();
        EXPECT_GE(ss.m_core.m_rcv.m_unsolicited_msgs_cached, cached_before + 1);
        EXPECT_EQ(ss.m_core.m_rcv.m_expect_msg_count, 1u);
        EXPECT_EQ(ss.m_core.m_rcv.m_expect_q_immediate_pops, 1u);
        EXPECT_EQ(ss.m_core.m_rcv.m_expect_msg_active, 0u); // One-off consumed.

        FLOW_LOG_INFO("Phase D srv stats:\n" << print(ss));
      }

      // ---------- Phase E: sync_request() success ----------
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase E: sync_request() success.");

        const auto cs0 = pair.m_cli->stats();
        const auto sr_count_before = cs0.m_sync_req.m_count;
        const auto sr_timeouts_before = cs0.m_sync_req.m_timeouts;
        const auto sr_late_before = cs0.m_sync_req.m_late_responses;
        const auto snd_req_oneoff_before = cs0.m_core.m_snd.m_msg.m_requests_one_off;
        const auto latency_samples_before
          = histo_total(cs0.m_sync_req.m_histo_latency_usec,
                        sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS);
        const auto rtt_samples_before
          = histo_total(cs0.m_core.m_rcv.m_histo_one_off_request_rtt_usec,
                        sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS);

        // Server handler: echo the value back as CoolRsp.
        pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&& req_in)
        {
          auto rsp = make_rsp(pair, req_in->body_root().getCoolReq().getCoolVal());
          pair.m_srv->send(&rsp, req_in.get());
        });

        // Call blocks U1 briefly; thread W processes rsp during the wait.
        auto req = make_req(pair, 500);
        Error_code err;
        auto rsp = pair.m_cli->sync_request(&req, nullptr, &err);
        ASSERT_FALSE(err) << "[" << err << "] [" << err.message() << "].";
        ASSERT_TRUE(rsp);
        EXPECT_EQ(rsp->body_root().getCoolRsp().getCoolVal(), 500u);

        const auto cs = pair.m_cli->stats();
        EXPECT_EQ(cs.m_sync_req.m_count, sr_count_before + 1);
        EXPECT_EQ(cs.m_sync_req.m_timeouts, sr_timeouts_before);
        EXPECT_EQ(cs.m_sync_req.m_late_responses, sr_late_before);
        // +1 sync_req latency sample.
        EXPECT_EQ(histo_total(cs.m_sync_req.m_histo_latency_usec,
                              sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                  latency_samples_before + 1);

        // Double-counting: sync_request() internally calls async_request() one-off, so core snd and
        // core rcv-RTT histogram also move.
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_requests_one_off, snd_req_oneoff_before + 1);
        EXPECT_EQ(histo_total(cs.m_core.m_rcv.m_histo_one_off_request_rtt_usec,
                              sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                  rtt_samples_before + 1);

        FLOW_LOG_INFO("Phase E cli stats:\n" << print(cs));
      }

      // ---------- Phase F: sync_request() timeout + late response ----------
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase F: sync_request() timeout + late response.");

        const auto cs0 = pair.m_cli->stats();
        const auto sr_count_before   = cs0.m_sync_req.m_count;
        const auto sr_timeouts_before = cs0.m_sync_req.m_timeouts;
        const auto sr_late_before    = cs0.m_sync_req.m_late_responses;
        const auto latency_samples_before
          = histo_total(cs0.m_sync_req.m_histo_latency_usec,
                        sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS);

        ASSERT_TRUE(pair.m_srv->undo_expect_msgs(Body::COOL_REQ));

        // Srv: on receiving the req, schedule a delayed send on V1.  Delay > client timeout -> timeout.
        boost::promise<void> delayed_send_fired;
        pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&& req_in)
        {
          loop_v1.schedule_from_now(milliseconds{200},
                                    [&, req = std::move(req_in)](auto)
          {
            auto rsp = make_rsp(pair, 999);
            pair.m_srv->send(&rsp, req.get());
            delayed_send_fired.set_value();
          });
        });

        auto req = make_req(pair, 999);
        Error_code err;
        auto rsp = pair.m_cli->sync_request(&req, nullptr, milliseconds{50}, &err);
        EXPECT_EQ(err, transport::error::Code::S_TIMEOUT);
        EXPECT_FALSE(rsp);

        // Wait for V1 to actually dispatch the (too-late) send...
        ASSERT_EQ(delayed_send_fired.get_future().wait_for(seconds{3}), boost::future_status::ready);
        // ...and give client's thread W a small margin to process it and bump m_late_responses.
        sleep_for(milliseconds{200});

        const auto cs = pair.m_cli->stats();
        EXPECT_EQ(cs.m_sync_req.m_count, sr_count_before + 1);
        EXPECT_EQ(cs.m_sync_req.m_timeouts, sr_timeouts_before + 1);
        EXPECT_EQ(cs.m_sync_req.m_late_responses, sr_late_before + 1);
        // m_histo_latency_usec records timeout (not late-arrival) duration -- i.e., it *is* recorded.
        EXPECT_EQ(histo_total(cs.m_sync_req.m_histo_latency_usec,
                              sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                  latency_samples_before + 1);

        FLOW_LOG_INFO("Phase F cli stats:\n" << print(cs));
      }

      // ---------- Phase G: print()/declare_stats() smoke test ----------
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase G: print()/declare_stats() smoke test.");
        std::ostringstream oss;
        oss << print(pair.m_cli->stats());
        EXPECT_FALSE(oss.str().empty()) << "print(stats()) produced empty output.";
        /* Spot-check: m_sync_req items appear with the "sync_req." sub-prefix added by the composing
         * declare_stats() for stat::Channel_stats; m_core items keep `name_prefix` unchanged (no "core."). */
        EXPECT_NE(oss.str().find("sync_req.late_responses="), std::string::npos);
        EXPECT_NE(oss.str().find("snd.msg.user_msgs="), std::string::npos);
      }

      // ---------- Phase H: reset ----------
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase H: reset.");

        const auto pre = pair.m_cli->stats();
        // Capture gauges (must survive reset).
        const auto expect_msgs_active_pre = pre.m_core.m_rcv.m_expect_msgs_active;
        const auto expect_msg_active_pre = pre.m_core.m_rcv.m_expect_msg_active;
        const auto expect_resp_oneoff_active_pre = pre.m_core.m_rcv.m_expect_response_one_off_active;
        const auto expect_resp_sticky_active_pre = pre.m_core.m_rcv.m_expect_response_sticky_active;
        const auto reassembly_q_depth_pre = pre.m_core.m_rcv.m_reassembly_q_depth;
        const auto pending_msgs_depth_pre = pre.m_core.m_rcv.m_pending_msgs_depth;
        // Any accumulator pre-reset should be nonzero, otherwise this test doesn't prove much.
        ASSERT_GT(pre.m_core.m_snd.m_msg.m_user_msgs, 0u);
        ASSERT_GT(pre.m_sync_req.m_count, 0u);

        pair.m_cli->stats_reset();

        const auto post = pair.m_cli->stats();

        // Accumulators: all zero.
        EXPECT_EQ(post.m_core.m_snd.m_msg.m_user_msgs, 0u);
        EXPECT_EQ(post.m_core.m_snd.m_msg.m_notifications, 0u);
        EXPECT_EQ(post.m_core.m_snd.m_msg.m_requests, 0u);
        EXPECT_EQ(post.m_core.m_snd.m_msg.m_requests_one_off, 0u);
        EXPECT_EQ(post.m_core.m_snd.m_msg.m_total_segments, 0u);
        EXPECT_EQ(post.m_core.m_snd.m_msg.m_total_low_lvl_blobs, 0u);
        EXPECT_EQ(post.m_core.m_rcv.m_msg.m_user_msgs, 0u);
        EXPECT_EQ(post.m_core.m_rcv.m_msg.m_notifications, 0u);
        EXPECT_EQ(post.m_core.m_rcv.m_unsolicited_msgs_routed, 0u);
        EXPECT_EQ(post.m_core.m_rcv.m_unsolicited_msgs_cached, 0u);
        EXPECT_EQ(post.m_core.m_rcv.m_expect_msgs_count, 0u);
        EXPECT_EQ(post.m_core.m_rcv.m_expect_msg_count, 0u);
        EXPECT_EQ(post.m_core.m_rcv.m_expect_q_immediate_pops, 0u);
        EXPECT_EQ(post.m_sync_req.m_count, 0u);
        EXPECT_EQ(post.m_sync_req.m_timeouts, 0u);
        EXPECT_EQ(post.m_sync_req.m_late_responses, 0u);

        // Gauges: preserved.
        EXPECT_EQ(post.m_core.m_rcv.m_expect_msgs_active, expect_msgs_active_pre);
        EXPECT_EQ(post.m_core.m_rcv.m_expect_msg_active, expect_msg_active_pre);
        EXPECT_EQ(post.m_core.m_rcv.m_expect_response_one_off_active, expect_resp_oneoff_active_pre);
        EXPECT_EQ(post.m_core.m_rcv.m_expect_response_sticky_active, expect_resp_sticky_active_pre);
        EXPECT_EQ(post.m_core.m_rcv.m_reassembly_q_depth, reassembly_q_depth_pre);
        EXPECT_EQ(post.m_core.m_rcv.m_pending_msgs_depth, pending_msgs_depth_pre);

        // High-water marks: reset to current gauge value (new observation window).
        EXPECT_EQ(post.m_core.m_rcv.m_reassembly_q_hi_wmark, reassembly_q_depth_pre);
        EXPECT_EQ(post.m_core.m_rcv.m_pending_msgs_hi_wmark, pending_msgs_depth_pre);

        // Histograms: samples cleared, bucket config preserved.
        EXPECT_EQ(histo_total(post.m_core.m_snd.m_msg.m_histo_msg_sz,
                              sync_io::stat::Channel_stats::Msg::S_HISTO_MSG_SZ_N_BUCKETS),
                  0u);
        EXPECT_EQ(histo_total(post.m_core.m_rcv.m_msg.m_histo_msg_sz,
                              sync_io::stat::Channel_stats::Msg::S_HISTO_MSG_SZ_N_BUCKETS),
                  0u);
        EXPECT_EQ(histo_total(post.m_core.m_rcv.m_histo_one_off_request_rtt_usec,
                              sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                  0u);
        EXPECT_EQ(histo_total(post.m_sync_req.m_histo_latency_usec,
                              sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                  0u);

        FLOW_LOG_INFO("Phase H post-reset cli stats:\n" << print(post));
      }

      // ---------- Phase I: post-reset ----------
      {
        FLOW_TEST_TRACE();
        FLOW_LOG_INFO("Phase I: post-reset.");

        ASSERT_TRUE(pair.m_srv->undo_expect_msgs(Body::COOL_REQ));

        boost::promise<void> got;
        ASSERT_TRUE(pair.m_srv->expect_msg(Body::COOL_REQ, [&](auto&&) { got.set_value(); }));

        {
          auto msg = make_req(pair, 1);
          pair.m_cli->send(&msg);
        }

        ASSERT_EQ(got.get_future().wait_for(seconds{3}), boost::future_status::ready);

        const auto cs = pair.m_cli->stats();
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_user_msgs, 1u);
        EXPECT_EQ(cs.m_core.m_snd.m_msg.m_notifications, 1u);

        FLOW_LOG_INFO("Phase I cli stats:\n" << print(cs));
      }

      FLOW_LOG_INFO("All phases complete; signaling U.");
      test_done.set_value();
    }); // loop_u1.post() -- end of U1 test body.

    auto fut = test_done.get_future();
    ASSERT_EQ(fut.wait_for(seconds{30}), boost::future_status::ready)
      << "Test timed out waiting for U1 to complete.";

    loop_u1.stop();
    loop_v1.stop();

    EXPECT_FALSE(cli_err_fired) << "Client channel on-error fired.";
    EXPECT_FALSE(srv_err_fired) << "Server channel on-error fired.";
  } // test_send_receive_stats()

  /* Struct-level reset-semantics test.  No IPC; just poke the stat structs by hand, run
   * `flow::util::stat::stats_reset()`, and verify: accumulators zeroed, gauges preserved,
   * high-water marks reset to current gauge value, histograms cleared (sample counts 0) but bucket
   * config preserved.  Covers `sync_io::stat::Channel_stats`, `struc::stat::Channel_sync_req_stats`, and the
   * composed `struc::stat::Channel_stats` (which delegates to both of those via nested
   * `declare_stats()`). */
  inline void test_stats_reset_semantics()
  {
    using flow::util::stat::stats_reset;

    // ---- sync_io::stat::Channel_stats ----
    {
      FLOW_TEST_TRACE();

      sync_io::stat::Channel_stats s;

      // Set some accumulators.
      s.m_snd.m_msg.m_user_msgs = 42;
      s.m_snd.m_msg.m_notifications = 10;
      s.m_snd.m_msg.m_requests = 32;
      s.m_snd.m_msg.m_total_low_lvl_blobs = 100;
      s.m_rcv.m_msg.m_user_msgs = 50;
      s.m_rcv.m_unsolicited_msgs_routed = 40;
      s.m_rcv.m_unsolicited_msgs_cached = 10;
      s.m_rcv.m_expect_msg_count = 7;
      s.m_rcv.m_expect_msgs_count = 3;
      s.m_rcv.m_expect_q_immediate_pops = 2;
      s.m_rcv.m_reassembly_q_insertions = 1;
      s.m_rcv.m_unexpected_responses = 0;
      s.m_rcv.m_liveness_checks = 5;

      // Set gauges + high-water marks (HWM != current, so we can verify HWM snaps to current).
      s.m_rcv.m_expect_msgs_active = 2;
      s.m_rcv.m_expect_msg_active = 1;
      s.m_rcv.m_expect_response_one_off_active = 3;
      s.m_rcv.m_expect_response_sticky_active = 4;
      s.m_rcv.m_reassembly_q_depth = 2;
      s.m_rcv.m_reassembly_q_hi_wmark = 9;
      s.m_rcv.m_pending_msgs_depth = 5;
      s.m_rcv.m_pending_msgs_hi_wmark = 12;

      // Record a few histogram samples (tests bucket config is preserved after reset).
      s.m_rcv.m_histo_one_off_request_rtt_usec.record_value(123);
      s.m_rcv.m_histo_one_off_request_rtt_usec.record_value(4567);
      ASSERT_EQ(histo_total(s.m_rcv.m_histo_one_off_request_rtt_usec,
                            sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                2u);

      stats_reset(&s, sync_io::stat::Channel_stats{});

      // Accumulators: zero.
      EXPECT_EQ(s.m_snd.m_msg.m_user_msgs, 0u);
      EXPECT_EQ(s.m_snd.m_msg.m_notifications, 0u);
      EXPECT_EQ(s.m_snd.m_msg.m_requests, 0u);
      EXPECT_EQ(s.m_snd.m_msg.m_total_low_lvl_blobs, 0u);
      EXPECT_EQ(s.m_rcv.m_msg.m_user_msgs, 0u);
      EXPECT_EQ(s.m_rcv.m_unsolicited_msgs_routed, 0u);
      EXPECT_EQ(s.m_rcv.m_unsolicited_msgs_cached, 0u);
      EXPECT_EQ(s.m_rcv.m_expect_msg_count, 0u);
      EXPECT_EQ(s.m_rcv.m_expect_msgs_count, 0u);
      EXPECT_EQ(s.m_rcv.m_expect_q_immediate_pops, 0u);
      EXPECT_EQ(s.m_rcv.m_reassembly_q_insertions, 0u);
      EXPECT_EQ(s.m_rcv.m_liveness_checks, 0u);

      // Gauges: preserved.
      EXPECT_EQ(s.m_rcv.m_expect_msgs_active, 2u);
      EXPECT_EQ(s.m_rcv.m_expect_msg_active, 1u);
      EXPECT_EQ(s.m_rcv.m_expect_response_one_off_active, 3u);
      EXPECT_EQ(s.m_rcv.m_expect_response_sticky_active, 4u);
      EXPECT_EQ(s.m_rcv.m_reassembly_q_depth, 2u);
      EXPECT_EQ(s.m_rcv.m_pending_msgs_depth, 5u);

      // High-water marks: reset to current gauge (new observation window).
      EXPECT_EQ(s.m_rcv.m_reassembly_q_hi_wmark, 2u);
      EXPECT_EQ(s.m_rcv.m_pending_msgs_hi_wmark, 5u);

      // Histograms: cleared; bucket config still works (record another sample and re-check).
      EXPECT_EQ(histo_total(s.m_rcv.m_histo_one_off_request_rtt_usec,
                            sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                0u);
      s.m_rcv.m_histo_one_off_request_rtt_usec.record_value(50);
      EXPECT_EQ(histo_total(s.m_rcv.m_histo_one_off_request_rtt_usec,
                            sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                1u);
    }

    // ---- struc::stat::Channel_sync_req_stats ----
    {
      FLOW_TEST_TRACE();

      stat::Channel_sync_req_stats s;
      s.m_count = 10;
      s.m_timeouts = 2;
      s.m_late_responses = 1;
      s.m_histo_latency_usec.record_value(42);
      s.m_histo_latency_usec.record_value(123);
      ASSERT_EQ(histo_total(s.m_histo_latency_usec,
                            sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                2u);

      stats_reset(&s, stat::Channel_sync_req_stats{});

      EXPECT_EQ(s.m_count, 0u);
      EXPECT_EQ(s.m_timeouts, 0u);
      EXPECT_EQ(s.m_late_responses, 0u);
      EXPECT_EQ(histo_total(s.m_histo_latency_usec,
                            sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                0u);
      // Bucket config preserved.
      s.m_histo_latency_usec.record_value(42);
      EXPECT_EQ(histo_total(s.m_histo_latency_usec,
                            sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                1u);
    }

    // ---- struc::stat::Channel_stats (composition) ----
    {
      FLOW_TEST_TRACE();

      stat::Channel_stats s;
      s.m_core.m_snd.m_msg.m_user_msgs = 5;
      s.m_core.m_rcv.m_expect_msgs_active = 2; // gauge
      s.m_core.m_rcv.m_pending_msgs_hi_wmark = 99;
      s.m_core.m_rcv.m_pending_msgs_depth = 4;
      s.m_sync_req.m_count = 7;
      s.m_sync_req.m_timeouts = 1;
      s.m_sync_req.m_late_responses = 1;
      s.m_sync_req.m_histo_latency_usec.record_value(10);

      stats_reset(&s, stat::Channel_stats{});

      // Core accumulators + sync_req accumulators: all zero.
      EXPECT_EQ(s.m_core.m_snd.m_msg.m_user_msgs, 0u);
      EXPECT_EQ(s.m_sync_req.m_count, 0u);
      EXPECT_EQ(s.m_sync_req.m_timeouts, 0u);
      EXPECT_EQ(s.m_sync_req.m_late_responses, 0u);

      // Core gauge preserved; HWM reset to current gauge.
      EXPECT_EQ(s.m_core.m_rcv.m_expect_msgs_active, 2u);
      EXPECT_EQ(s.m_core.m_rcv.m_pending_msgs_depth, 4u);
      EXPECT_EQ(s.m_core.m_rcv.m_pending_msgs_hi_wmark, 4u);

      // sync_req histogram cleared; bucket config preserved (record + recount).
      EXPECT_EQ(histo_total(s.m_sync_req.m_histo_latency_usec,
                            sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                0u);
      s.m_sync_req.m_histo_latency_usec.record_value(42);
      EXPECT_EQ(histo_total(s.m_sync_req.m_histo_latency_usec,
                            sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_N_BUCKETS),
                1u);
    }
  } // test_stats_reset_semantics()

  /* Session Info_collector smoke test.  Parameterized on ShmType so we cover vanilla sessions plus
   * both SHM providers (SHM-classic, SHM-jemalloc); SMC stats are MqType-agnostic (the SMC is
   * always `Native_socket_stream`-backed regardless of MqType), so one MqType is sufficient here.
   *
   * Checks:
   *   - info_collector() returns non-null after session establishment.
   *   - All 6 Info_collector methods work end-to-end (log via print(); assert > 0 where expected).
   *     Log-in exchange always involves user-level SMC traffic on both sides, both directions, so
   *     all 6 stat sets should have moved.  SHM-enabled variants add extra SMC exchange for arena
   *     setup; the same > 0 asserts cover both cases.
   *   - reset() semantics are per-side: resetting client-side stats leaves server-side untouched. */
  template<ShmType SHM_TYPE>
  void test_smc_info_collector_smoke()
  {
#if 1
    Logger* const logger = nullptr;
#else
    Logger* const logger = g_logger_console;
#endif
    Logger::this_thread_set_logged_nickname("U", g_logger_console);

    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("SMC Info_collector smoke [ShmType=" << int(SHM_TYPE) << "]: setting up session.");

    auto pair = make_session_channel_pair<MqType::BIPC, true, SHM_TYPE>(logger);

    auto* const cli_sc = pair.m_sessions->m_cli_session.info_collector();
    auto* const srv_sc = pair.m_sessions->m_srv_session.info_collector();
    ASSERT_NE(cli_sc, nullptr) << "Client Info_collector should be engaged post-sync_connect().";
    ASSERT_NE(srv_sc, nullptr) << "Server Info_collector should be engaged post-async_accept().";

    const auto cli_smc = cli_sc->master_channel_stats();
    const auto srv_smc = srv_sc->master_channel_stats();
    const auto cli_smc_snd = cli_sc->master_channel_transport_snd_stats();
    const auto srv_smc_snd = srv_sc->master_channel_transport_snd_stats();
    const auto cli_smc_rcv = cli_sc->master_channel_transport_rcv_stats();
    const auto srv_smc_rcv = srv_sc->master_channel_transport_rcv_stats();

    FLOW_LOG_INFO("cli SMC struc stats:\n" << print(cli_smc));
    FLOW_LOG_INFO("srv SMC struc stats:\n" << print(srv_smc));
    FLOW_LOG_INFO("cli SMC transport snd stats:\n" << print(cli_smc_snd));
    FLOW_LOG_INFO("srv SMC transport snd stats:\n" << print(srv_smc_snd));
    FLOW_LOG_INFO("cli SMC transport rcv stats:\n" << print(cli_smc_rcv));
    FLOW_LOG_INFO("srv SMC transport rcv stats:\n" << print(srv_smc_rcv));

    // Log-in exchange: each side sent and received at least one user-level msg over the SMC.
    EXPECT_GT(cli_smc.m_core.m_snd.m_msg.m_user_msgs, 0u);
    EXPECT_GT(srv_smc.m_core.m_snd.m_msg.m_user_msgs, 0u);
    EXPECT_GT(cli_smc.m_core.m_rcv.m_msg.m_user_msgs, 0u);
    EXPECT_GT(srv_smc.m_core.m_rcv.m_msg.m_user_msgs, 0u);
    // ...and accordingly the underlying transport moved bytes in both directions on both sides.
    EXPECT_GT(cli_smc_snd.m_total_msgs, 0u);
    EXPECT_GT(srv_smc_snd.m_total_msgs, 0u);
    EXPECT_GT(cli_smc_rcv.m_total_msgs, 0u);
    EXPECT_GT(srv_smc_rcv.m_total_msgs, 0u);

    // Reset client side only; verify client zeroes out and server is unaffected (reset is per-side).
    cli_sc->master_channel_stats_reset();
    cli_sc->master_channel_transport_snd_stats_reset();
    cli_sc->master_channel_transport_rcv_stats_reset();

    EXPECT_EQ(cli_sc->master_channel_stats().m_core.m_snd.m_msg.m_user_msgs, 0u);
    EXPECT_EQ(cli_sc->master_channel_stats().m_core.m_rcv.m_msg.m_user_msgs, 0u);
    EXPECT_EQ(cli_sc->master_channel_transport_snd_stats().m_total_msgs, 0u);
    EXPECT_EQ(cli_sc->master_channel_transport_rcv_stats().m_total_msgs, 0u);

    EXPECT_GT(srv_sc->master_channel_stats().m_core.m_snd.m_msg.m_user_msgs, 0u);
    EXPECT_GT(srv_sc->master_channel_stats().m_core.m_rcv.m_msg.m_user_msgs, 0u);
    EXPECT_GT(srv_sc->master_channel_transport_snd_stats().m_total_msgs, 0u);
    EXPECT_GT(srv_sc->master_channel_transport_rcv_stats().m_total_msgs, 0u);
  } // test_smc_info_collector_smoke()

  /* Corner-case coverage for Session_mv::info_collector() forwarder in pre-PEER states:
   *   - Default-cted NULL-state Client/Server_session: impl exists but `m_info_collector` unengaged;
   *     accessor returns null.
   *   - Moved-from Client/Server_session: `impl()` null; forwarder short-circuits to null.
   *   - almost-PEER Server_session (async_accept_log_in() succeeded, pre-init_handlers()):
   *     engaged and populated with log-in traffic; accessor returns non-null.
   * Templated on ShmType so we cover vanilla sessions plus both SHM providers: while as
   * of this writing the SHM variants inherit the forwarder verbatim, that's a white-box observation
   * and could change.
   * (PEER state for both sides is covered by test_smc_info_collector_smoke().) */
  template<ShmType SHM_TYPE>
  void test_info_collector_pre_peer_states()
  {
#if 1
    Logger* const logger = nullptr;
#else
    Logger* const logger = g_logger_console;
#endif
    Logger::this_thread_set_logged_nickname("U", g_logger_console);

    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("info_collector() pre-PEER states [ShmType=" << int(SHM_TYPE) << "]: starting.");

    using Session_types_t = Session_types_select<MqType::NONE, true, SHM_TYPE>;
    using Client_session_t = typename Session_types_t::Client_session_t;
    using Session_server_t = typename Session_types_t::Session_server_t;
    using Server_session_t = typename Session_server_t::Server_session_obj;

    // Default-cted: impl exists but Info_collector is unengaged; accessor returns null.
    {
      FLOW_LOG_INFO("Pre-PEER states: default-cted sessions.");
      Client_session_t cli;
      EXPECT_FALSE(cli.info_collector())
        << "Default-cted Client_session should yield null Info_collector (impl exists but unengaged).";

      Server_session_t srv;
      EXPECT_FALSE(srv.info_collector())
        << "Default-cted Server_session should yield null Info_collector (impl exists but unengaged).";
    }

    // Moved-from: `impl()` is null after the move; forwarder's `impl() ? ... : nullptr` returns null.
    {
      FLOW_LOG_INFO("Pre-PEER states: moved-from sessions.");
      Client_session_t cli;
      Client_session_t cli_other{std::move(cli)};
      EXPECT_FALSE(cli.info_collector()) << "Moved-from Client_session should yield null (no impl).";

      Server_session_t srv;
      Server_session_t srv_other{std::move(srv)};
      EXPECT_FALSE(srv.info_collector()) << "Moved-from Server_session should yield null (no impl).";
    }

    /* Almost-PEER server: after async_accept_log_in() but before init_handlers().  Engaged; log-in
     * traffic (both directions) has populated the SMC stats. */
    {
      FLOW_LOG_INFO("Pre-PEER states: almost-PEER Server_session.");
      auto pair = make_session_channel_pair<MqType::NONE, true, SHM_TYPE>(logger, true); // <-- leave_srv_almost_peer
      auto* const srv_sc = pair.m_sessions->m_srv_session.info_collector();
      ASSERT_TRUE(srv_sc) << "Almost-PEER Server_session should have engaged Info_collector.";

      const auto srv_smc = srv_sc->master_channel_stats();
      FLOW_LOG_INFO("Almost-PEER srv SMC struc stats:\n" << print(srv_smc));
      EXPECT_GT(srv_smc.m_core.m_snd.m_msg.m_user_msgs, 0u)
        << "Log-in exchange should have sent messages on the SMC.";
      EXPECT_GT(srv_smc.m_core.m_rcv.m_msg.m_user_msgs, 0u)
        << "Log-in exchange should have received messages on the SMC.";

      /* Finalize: transitions almost-PEER -> PEER.  Not strictly required for destruction, but
       * matches the factory's default behavior and lets the session be used normally (otherwise
       * channel-opening etc. is intentionally restricted). */
      pair.m_sessions->m_srv_session.init_handlers([](const Error_code&) {});
    }
  } // test_info_collector_pre_peer_states()

  /* The Info_collector API trio untouched by the preceding two helpers:
   *   - master_channel_live(): true while the source Session lives;
   *   - `os << collector` one-shot printing, including the m_fmt knobs (m_multiline honored; m_verbose
   *     documented-ignored);
   *   - the copy-outliving-Session afterlife contract (the class's one subtle behavior -- the weak_ptr
   *     scheme): once the Session dies, a surviving copy answers live()=false; its stat-getters return
   *     all-zero values; its resetters no-op; and printing it yields the documented unavailable-note.
   * Not templated on ShmType, unlike the siblings: everything here (print mechanics, weak_ptr afterlife)
   * is ShmType- and MqType-agnostic. */
  inline void test_info_collector_print_and_afterlife()
  {
#if 1
    Logger* const logger = nullptr;
#else
    Logger* const logger = g_logger_console;
#endif
    Logger::this_thread_set_logged_nickname("U", g_logger_console);

    FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);
    FLOW_LOG_INFO("Info_collector print + afterlife: setting up session.");

    auto pair = make_session_channel_pair<MqType::NONE, true, ShmType::NONE>(logger);
    auto* const cli_sc = pair.m_sessions->m_cli_session.info_collector();
    auto* const srv_sc = pair.m_sessions->m_srv_session.info_collector();
    ASSERT_TRUE(cli_sc);
    ASSERT_TRUE(srv_sc);

    // Alive-ness is observable (both sides).
    EXPECT_TRUE(cli_sc->master_channel_live());
    EXPECT_TRUE(srv_sc->master_channel_live());

    const auto print_str = [](const auto& sc) -> std::string
    {
      std::ostringstream os;
      os << sc;
      return os.str();
    };

    // The one-shot print, multi-line form: all 3 sections, in the documented "- "/newline shape.
    cli_sc->m_fmt.m_multiline = true;
    const auto multi = print_str(*cli_sc);
    FLOW_LOG_INFO("cli Info_collector printed, multi-line form (for the eyeball):\n" << multi);
    EXPECT_EQ(multi.rfind("- smc-struc: [", 0), 0u); // I.e., output starts with that.
    EXPECT_NE(multi.find("\n- smc-transport-snd: ["), std::string::npos);
    EXPECT_NE(multi.find("\n- smc-transport-rcv: ["), std::string::npos);

    // Single-line form: same 3 sections, " | "-separated, no line breaks.
    cli_sc->m_fmt.m_multiline = false;
    const auto single = print_str(*cli_sc);
    FLOW_LOG_INFO("cli Info_collector printed, single-line form (for the eyeball): " << single);
    EXPECT_EQ(single.rfind("smc-struc: [", 0), 0u);
    EXPECT_NE(single.find(" | smc-transport-snd: ["), std::string::npos);
    EXPECT_NE(single.find(" | smc-transport-rcv: ["), std::string::npos);
    EXPECT_EQ(single.find('\n'), std::string::npos);

    /* m_verbose is documented to have no effect (no verbose-only content to gate): flipping it changes
     * nothing.  (Back-to-back equality rests on the SMC being quiescent between the two grabs -- the
     * same assumption the rest of this suite makes for its exact-count asserts.) */
    cli_sc->m_fmt.m_verbose = true;
    EXPECT_EQ(print_str(*cli_sc), single);
    cli_sc->m_fmt.m_verbose = false;

    /* The afterlife.  Copy the collectors (copyable by design); the copies observe the same SMC while
     * it lives... */
    auto cli_sc_copy = *cli_sc;
    auto srv_sc_copy = *srv_sc;
    EXPECT_TRUE(cli_sc_copy.master_channel_live());
    EXPECT_GT(cli_sc_copy.master_channel_stats().m_core.m_snd.m_msg.m_user_msgs, 0u); // (Log-in traffic.)

    /* ...then the source Sessions die.  (NB: cli_sc/srv_sc now dangle -- they point into the dead
     * Sessions; only the copies may be touched from here on.) */
    pair.m_sessions->destroy_sessions();

    // live() flips; the stat-getters return all-zero values...
    EXPECT_FALSE(cli_sc_copy.master_channel_live());
    EXPECT_FALSE(srv_sc_copy.master_channel_live());
    EXPECT_EQ(cli_sc_copy.master_channel_stats().m_core.m_snd.m_msg.m_user_msgs, 0u);
    EXPECT_EQ(cli_sc_copy.master_channel_stats().m_core.m_rcv.m_msg.m_user_msgs, 0u);
    EXPECT_EQ(cli_sc_copy.master_channel_transport_snd_stats().m_total_msgs, 0u);
    EXPECT_EQ(cli_sc_copy.master_channel_transport_rcv_stats().m_total_msgs, 0u);

    // ...the resetters no-op (the assert is completing at all, plus values staying zero)...
    cli_sc_copy.master_channel_stats_reset();
    cli_sc_copy.master_channel_transport_snd_stats_reset();
    cli_sc_copy.master_channel_transport_rcv_stats_reset();
    EXPECT_EQ(cli_sc_copy.master_channel_stats().m_core.m_snd.m_msg.m_user_msgs, 0u);

    // ...and printing yields the documented note (deterministic => exact-equality assert).
    EXPECT_EQ(print_str(cli_sc_copy), "[session master channel gone; no stats avail]");
    EXPECT_EQ(print_str(srv_sc_copy), "[session master channel gone; no stats avail]");
  } // test_info_collector_print_and_afterlife()
} // namespace (anon)


/* Instantiation macro: one TEST per MqType (all with TRANSMIT_NATIVE_HANDLES=true -- full matrix
 * isn't needed for stats, as handle-bearing msgs exercise the extra snd-stat path and the other
 * knobs are orthogonal). */
#define STATS_TEST(test_name) \
  TEST(Struc_channel_stats_test, test_name##_Nss) { test_##test_name<MqType::NONE>(); } \
  TEST(Struc_channel_stats_test, test_name##_Bipc) { test_##test_name<MqType::BIPC>(); } \
  TEST(Struc_channel_stats_test, test_name##_Posix) { test_##test_name<MqType::POSIX>();}

} // namespace ipc::transport::struc::test
