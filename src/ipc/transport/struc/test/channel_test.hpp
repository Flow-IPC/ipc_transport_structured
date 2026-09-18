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
 * Each test body below is a function template over certain <knobs>; each sibling .cpp invokes ..._TESTS()
 * (see bottom of file) to expand the battery given its <knobs>; together they cover the full matrix.
 * This bounds compiler RAM use per .cpp (translation unit) versus simply placing everything into one .cpp.
 * In particular each per-<knobs> ipc::session + struc::Channel stack costs GBs of compiler RAM, for debug-info
 * builds, at least with some Linux gcc.  2+ such translation units compiling concurrently can exhaust a
 * smaller build machine (including GitHub CI runners in 2026). */

#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/transport/struc/test/test_schema.capnp.h"
#include "ipc/transport/error.hpp"
#include "ipc/transport/bipc_mq_handle.hpp"
#include "ipc/transport/posix_mq_handle.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util.hpp>
#include <flow/test/test_common_util.hpp>
#include <boost/thread/future.hpp>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ipc::transport::struc::test
{

namespace
{
  using flow::async::Single_thread_task_loop;
  using flow::Error_code;
  using flow::async::Synchronicity;
  using flow::util::ostream_op_string;
  using session::schema::MqType;
  using std::atomic;
  using std::optional;

  // Number of concurrent requester threads.
  constexpr int N_THREADS = 4;
  // Number of sync_request() calls per thread.
  constexpr int N_REQUESTS_PER_THREAD = 1000;

  // --- Test bodies, each parameterized on <MqType, transmit-native-handles>. ---

  /* Exercises concurrent send()/sync_request() on the same struc::Channel.
   * Side A (cli): multiple threads hammer sync_request() (which don't require explicit synchronization) concurrently.
   * Side B (srv): echoes responses via expect_msgs(). */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_sync_request_concurrency()
  {
    atomic<bool> cli_err{false};
    atomic<bool> srv_err{false};
    auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                  ([&](auto&&) { cli_err = true; },
                   [&](auto&&) { srv_err = true; });

    /* Server side: echo back every CoolReq as a CoolRsp with the same value.
     * When handles are enabled: odd-val requests carry a handle; echo one back in the response. */
    atomic<uint64_t> srv_handle_checks{0};
    pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&& req)
    {
      /* We are in unspecified struc::Channel background thread.  Formally we are allowed to do things, even like
       * .send(), in here -- and we do.  It is informally suggested to instead post()-or-equivalent such work
       * onto our own worker thread, so here we explicitly go against that; and hey... perhaps that's a nice thing
       * opportunistically exercise.  Just remember (here and elsewhere in this file) that one must be very careful
       * when operating within an "unspecified" thread of an async-I/O-pattern Flow-IPC object. */

      const auto val = req->body_root().getCoolReq().getCoolVal();
      auto rsp = pair.m_srv->create_msg();
      if constexpr(TRANSMIT_NATIVE_HANDLES)
      {
        auto hndl = req->emit_native_handle_or_null();
        EXPECT_EQ(val % 2 == 0, hndl.null()) << "Val [" << val << "]: odd <=> expecting handle.";
        if (!hndl.null())
        {
          hndl.close();
          rsp.store_native_handle_or_null(util::Native_handle{::dup(STDOUT_FILENO)});
        }
        ++srv_handle_checks;
      }
      rsp.body_root()->initCoolRsp().setCoolVal(val);
      pair.m_srv->send(&rsp, req.get());
    });

    // Client side: N_THREADS task loops, each doing N_REQUESTS_PER_THREAD sync_request() calls.
    atomic<uint64_t> total_successes{0};
    std::vector<std::unique_ptr<Single_thread_task_loop>> loops;
    loops.reserve(N_THREADS);

    for (int t = 0; t < N_THREADS; ++t)
    {
      auto loop = std::make_unique<Single_thread_task_loop>(nullptr, ostream_op_string("cli_worker_", t));
      loop->start();
      loop->post([&, t]()
      {
        for (int i = 0; i < N_REQUESTS_PER_THREAD; ++i)
        {
          const uint64_t val = uint64_t(t) * N_REQUESTS_PER_THREAD + i;

          auto req = pair.m_cli->create_msg();
          req.body_root()->initCoolReq().setCoolVal(val);
          if constexpr(TRANSMIT_NATIVE_HANDLES)
          {
            if ((val % 2) != 0) // Odd: attach a dup'd STDOUT handle.
            {
              req.store_native_handle_or_null(util::Native_handle{::dup(STDOUT_FILENO)});
            }
          }

          Error_code err;
          auto rsp = pair.m_cli->sync_request(&req, nullptr, &err);

          ASSERT_FALSE(err) << "sync_request() error: [" << err << "] [" << err.message() << "].";
          ASSERT_TRUE(rsp) << "sync_request() returned null response.";
          EXPECT_EQ(rsp->body_root().getCoolRsp().getCoolVal(), val);

          if constexpr(TRANSMIT_NATIVE_HANDLES)
          {
            auto rsp_hndl = rsp->emit_native_handle_or_null();
            EXPECT_EQ(val % 2 == 0, rsp_hndl.null()) << "Val [" << val << "]: odd <=> expecting handle.";
            rsp_hndl.close();
          }

          ++total_successes;
        }
      }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START);
      loops.push_back(std::move(loop));
    }

    // Destroying the loops joins each thread (after its posted task completes).
    loops.clear();

    EXPECT_FALSE(cli_err) << "Client channel error handler fired.";
    EXPECT_FALSE(srv_err) << "Server channel error handler fired.";
    EXPECT_EQ(total_successes.load(), uint64_t(N_THREADS * N_REQUESTS_PER_THREAD));
    if constexpr(TRANSMIT_NATIVE_HANDLES)
    {
      EXPECT_EQ(srv_handle_checks.load(), uint64_t(N_THREADS * N_REQUESTS_PER_THREAD));
    }
  }

  /* Verifies that concurrent sync_request() calls serialize: total wall time should be the sum
   * of individual server-side delays, not the max. */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_sync_request_serialization()
  {
    using boost::chrono::milliseconds;
    using boost::chrono::steady_clock;

    atomic<bool> cli_err{false};
    atomic<bool> srv_err{false};
    auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                  ([&](const Error_code&) { cli_err = true; },
                   [&](const Error_code&) { srv_err = true; });

    // Server-side timer loop for scheduling delayed responses.
    Single_thread_task_loop srv_loop{nullptr, "srv_loop"};
    srv_loop.start();

    pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&& req)
    {
      // Delay each response by 200ms.
      srv_loop.schedule_from_now(milliseconds{200},
                                 [&, req = std::move(req)](auto)
      {
        const auto val = req->body_root().getCoolReq().getCoolVal();
        auto rsp = pair.m_srv->create_msg();
        rsp.body_root()->initCoolRsp().setCoolVal(val);
        pair.m_srv->send(&rsp, req.get());
      });
    });

    // Launch 2 concurrent sync_request()s.  Due to serialization the total time should be ~400ms.
    const auto t0 = steady_clock::now();

    Single_thread_task_loop loop_a{nullptr, "cli_a"};
    Single_thread_task_loop loop_b{nullptr, "cli_b"};
    loop_a.start();
    loop_b.start();

    atomic<int> successes{0};

    loop_a.post([&]()
    {
      auto req = pair.m_cli->create_msg();
      req.body_root()->initCoolReq().setCoolVal(0);
      Error_code err;
      auto rsp = pair.m_cli->sync_request(&req, nullptr, &err);
      EXPECT_FALSE(err) << "[" << err << "] [" << err.message() << "].";
      EXPECT_TRUE(rsp);
      ++successes;
    }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START);
    loop_b.post([&]()
    {
      auto req = pair.m_cli->create_msg();
      req.body_root()->initCoolReq().setCoolVal(1);
      Error_code err;
      auto rsp = pair.m_cli->sync_request(&req, nullptr, &err);
      EXPECT_FALSE(err) << "[" << err << "] [" << err.message() << "].";
      EXPECT_TRUE(rsp);
      ++successes;
    }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START);

    // Join both threads. We ensured via Synchronicity:: that each post()ed body is currently running.
    loop_a.stop(); // Returns once that sync_request() finishes (or instantly if already finished).
    loop_b.stop(); // Ditto.

    const auto elapsed = steady_clock::now() - t0;

    EXPECT_FALSE(cli_err);
    EXPECT_FALSE(srv_err);
    EXPECT_EQ(successes.load(), 2);
    /* If truly serialized, total elapsed should be >= 2 * 200ms = 400ms.
     * If they ran in parallel it would be ~200ms.  Use 350ms as threshold. */
    EXPECT_GE(elapsed, milliseconds{350}) << "sync_request() calls appear to have run in parallel, not serialized.";
  }

  /* Verifies that sync_request() returns S_TIMEOUT when the server responds too late,
   * and that the channel remains usable afterward -- a subsequent request gets its own
   * correct response (not the late one from the timed-out request). */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_sync_request_timeout()
  {
    using boost::chrono::milliseconds;

    atomic<bool> cli_err{false};
    atomic<bool> srv_err{false};
    auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                  ([&](const Error_code&) { cli_err = true; },
                   [&](const Error_code&) { srv_err = true; });

    // Server-side timer loop for scheduling the delayed (too-late) response.
    Single_thread_task_loop srv_loop{nullptr, "srv_loop"};
    srv_loop.start();

    /* Server: respond to val==0 after 500ms (too late for the 100ms client timeout);
     * respond to all others immediately. */
    pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&& req)
    {
      // If val == 0 the delay it.  Respond too late: 500ms delay vs. the client's 100ms timeout.
      const auto val = req->body_root().getCoolReq().getCoolVal();
      srv_loop.schedule_from_now(milliseconds{(val == 0) ? 500 : 0},
                                 [&, val, req = std::move(req)](auto)
      {
        auto rsp = pair.m_srv->create_msg();
        rsp.body_root()->initCoolRsp().setCoolVal(val);
        pair.m_srv->send(&rsp, req.get());
      });
    });

    // First request (val=0): should time out -- server will respond, but too late.
    {
      auto req = pair.m_cli->create_msg();
      req.body_root()->initCoolReq().setCoolVal(0);
      Error_code err;
      auto rsp = pair.m_cli->sync_request(&req, nullptr, milliseconds{100}, &err);

      ASSERT_TRUE(err) << "Expected timeout error.";
      EXPECT_EQ(err, transport::error::Code::S_TIMEOUT);
      EXPECT_FALSE(rsp);
    }

    /* Channel should still be usable.  Second request (val=1): should succeed and get its own
     * response (val=1), not the late response to the first request (val=0). */
    ASSERT_FALSE(cli_err) << "Channel should not be hosed after timeout.";
    {
      auto req = pair.m_cli->create_msg();
      req.body_root()->initCoolReq().setCoolVal(1);
      Error_code err;
      auto rsp = pair.m_cli->sync_request(&req, nullptr, milliseconds{100}, &err);

      EXPECT_FALSE(err) << "Post-timeout sync_request() error: [" << err
                        << "] [" << err.message() << "].";
      EXPECT_TRUE(rsp);
      if (rsp)
      {
        EXPECT_EQ(rsp->body_root().getCoolRsp().getCoolVal(), 1u);
      }
    }
    EXPECT_FALSE(cli_err);
    EXPECT_FALSE(srv_err);
  }

  // Verifies that sync_request() returns an error when the server gracefully closes via async_end_sending().
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_sync_request_graceful_close()
  {
    using boost::chrono::milliseconds;

    const auto do_it = [&](milliseconds pause)
    {
      std::cout << "Sub-case: Will pause [" << pause << "] "
                   "before gracefully-closing server-side send-pipe.\n" << std::flush;

      atomic<bool> cli_err{false};
      atomic<bool> srv_err{false};
      auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                    ([&](const Error_code&) { cli_err = true; },
                     [&](const Error_code&) { srv_err = true; });

      // Server: upon receiving a request, gracefully end sending instead of responding.
      Single_thread_task_loop srv_loop{nullptr, "srv_requestee"};
      srv_loop.start();

      pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&&)
      {
        srv_loop.schedule_from_now(pause, [&](auto)
        {
          pair.m_srv->async_end_sending([](const Error_code&) {});
        });
      });

      // Client: sync_request on a worker thread.
      Error_code client_err;
      bool got_rsp = false;

      Single_thread_task_loop cli_loop{nullptr, "cli_requester"};
      cli_loop.start();
      cli_loop.post([&]()
      {
        auto req = pair.m_cli->create_msg();
        req.body_root()->initCoolReq().setCoolVal(42);
        auto rsp = pair.m_cli->sync_request(&req, nullptr, &client_err);
        got_rsp = bool(rsp);
      }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START);

      // Joining the worker thread -- the sync_request should have completed (with error).
      cli_loop.stop();

      EXPECT_EQ(client_err, transport::error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE);
      EXPECT_FALSE(got_rsp);
    }; // const auto do_it =

    do_it(milliseconds{0});
    do_it(milliseconds{1500});
  }

  /* Verifies that sync_request() returns an error when the server channel is destroyed (hard close).
   * Skipped for MQ-only (no socket pipe): closing one MQ end doesn't signal the other (no EOF/RST);
   * see https://github.com/Flow-IPC/ipc_core/issues/23. */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_sync_request_hard_close()
  {
    using boost::chrono::milliseconds;

    if constexpr((MQ_TYPE_OR_NONE != MqType::NONE) && (!TRANSMIT_NATIVE_HANDLES))
    {
      GTEST_SKIP() << "MQ-only channel: hard close is undetectable (no socket pipe); "
                      "see https://github.com/Flow-IPC/ipc_core/issues/23.";
    }

    const auto do_it = [&](milliseconds pause)
    {
      std::cout << "Sub-case: Will pause [" << pause << "] "
                   "before hard-closing server-side channel peer.\n" << std::flush;

      atomic<bool> cli_err{false};
      atomic<bool> srv_err{false};
      auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                    ([&](const Error_code&) { cli_err = true; },
                     [&](const Error_code&) { srv_err = true; });

      // Server: upon receiving a request, signal readiness but don't respond.
      Single_thread_task_loop srv_loop{nullptr, "srv_requestee"};
      srv_loop.start();

      boost::promise<void> req_received;
      pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&&)
      {
        srv_loop.schedule_from_now(pause, [&](auto)
        {
          req_received.set_value(); // Don't respond.
        });
      });

      // Client: sync_request on a worker thread.
      Error_code client_err;
      bool got_rsp = false;

      Single_thread_task_loop cli_loop{nullptr, "cli_requester"};
      cli_loop.start();
      cli_loop.post([&]()
      {
        auto req = pair.m_cli->create_msg();
        req.body_root()->initCoolReq().setCoolVal(42);
        auto rsp = pair.m_cli->sync_request(&req, nullptr, &client_err);
        got_rsp = bool(rsp);
      }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START);

      // Wait until the server has received the request (so the client is blocked in sync_request).
      req_received.get_future().wait();

      // Destroy the server channel.
      pair.m_srv.reset();

      // Join the client worker -- sync_request should have unblocked with an error.
      cli_loop.stop();

      EXPECT_TRUE(client_err) << "Expected pipe-hosed error from hard close.";
      EXPECT_FALSE(got_rsp);
    }; // const auto do_it =

    do_it(milliseconds{0});
    do_it(milliseconds{1500});
  }

  /* @todo Test S_SYNC_OP_INTERRUPTED_BY_CONCURRENT_NB_ERROR: a concurrent non-blocking call (send(), etc.)
   * triggers a pipe-hosing error while sync_request() is blocked.  sync_request() should get
   * error::Code::S_SYNC_OP_INTERRUPTED_BY_CONCURRENT_NB_ERROR while the true error is emitted by the concurrent
   * call.  Hard to trigger without instrumentation (need to force a send error mid-wait); consider a test fixture. */

  /* Verifies that during the sync_request() blocking wait, the rest of the channel continues operating:
   *   - Unsolicited in-messages are dispatched to expect_msgs() handlers (in thread W, during the wait).
   *   - expect_msgs() can be registered concurrently, and queued messages are emitted immediately.
   *   - send() succeeds concurrently.
   *   - async_request() succeeds concurrently and its response handler fires. */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_sync_request_concurrent_ops()
  {
    using boost::chrono::milliseconds;

    atomic<bool> cli_err{false};
    atomic<bool> srv_err{false};
    auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                  ([&](const Error_code&) { cli_err = true; },
                   [&](const Error_code&) { srv_err = true; });

    // Server-side timer loop for scheduling the delayed sync_request response.
    Single_thread_task_loop srv_loop{nullptr, "srv_loop"};
    srv_loop.start();

    /* Signal when the server has received the sync_request's CoolReq
     * (meaning the client is now blocked in sync_request()). */
    boost::promise<void> sync_req_arrived;

    // Server: CoolReq handling -- delay response to val==1000 by 500ms; respond to others immediately.
    pair.m_srv->expect_msgs(Body::COOL_REQ, [&](auto&& req)
    {
      const auto val = req->body_root().getCoolReq().getCoolVal();
      const bool delay = val == 1000;

      if (delay) { sync_req_arrived.set_value(); }

      srv_loop.schedule_from_now(milliseconds{delay ? 500 : 0},
                                 [&, val, req = std::move(req)](auto)
      {
        auto rsp = pair.m_srv->create_msg();
        rsp.body_root()->initCoolRsp().setCoolVal(val);
        pair.m_srv->send(&rsp, req.get());
      });
    });

    /* Server: handler for unsolicited CoolRsp from the client (verifies client's send() worked).
     * When handles are enabled, also verify the handle arrived. */
    atomic<bool> srv_got_concurrent_send{false};
    atomic<bool> srv_got_concurrent_handle{false};
    pair.m_srv->expect_msgs(Body::COOL_RSP, [&](auto&& rsp)
    {
      srv_got_concurrent_send = true;
      if constexpr(TRANSMIT_NATIVE_HANDLES)
      {
        auto hndl = rsp->emit_native_handle_or_null();
        srv_got_concurrent_handle = !hndl.null();
        hndl.close();
      }
    });

    /* sync_request() will block for 500ms (the server delays its response that long).  All concurrent
     * operations should complete well within that window.  Each handler checks inline that it fired
     * promptly; we use 400ms as a generous threshold (well under the 500ms sync_request() delay). */
    constexpr auto PROMPTNESS_LIMIT = milliseconds{400};
    using boost::chrono::steady_clock;

    // Launch sync_request (val=1000) on a worker thread.
    Error_code sync_err;
    bool sync_rsp_ok = false;
    uint64_t sync_rsp_val = 0;

    Single_thread_task_loop cli_sync_loop{nullptr, "cli_sync"};
    cli_sync_loop.start();
    cli_sync_loop.post([&]()
    {
      auto req = pair.m_cli->create_msg();
      req.body_root()->initCoolReq().setCoolVal(1000);
      auto rsp = pair.m_cli->sync_request(&req, nullptr, &sync_err);
      sync_rsp_ok = bool(rsp);
      if (rsp)
      {
        sync_rsp_val = rsp->body_root().getCoolRsp().getCoolVal();
      }
    }, Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START);

    // Wait for the server to have received the sync_request()'s CoolReq (client is now blocked).
    sync_req_arrived.get_future().wait();

    /* --- Concurrent operations while sync_request() is blocked ---
     * t0 marks the start of concurrent ops; each handler verifies it fired within PROMPTNESS_LIMIT. */
    const auto t0 = steady_clock::now();

    atomic<int> successes{0};

    // (A) Register expect_msgs for unsolicited COOL_RSP on the client, then have the server send one.
    pair.m_cli->expect_msgs(Body::COOL_RSP, [&](auto&&)
    {
      ++successes;
      EXPECT_LT(steady_clock::now() - t0, PROMPTNESS_LIMIT)
        << "Unsolicited-message handler should fire promptly.";
    });
    {
      auto msg = pair.m_srv->create_msg();
      msg.body_root()->initCoolRsp().setCoolVal(777);
      pair.m_srv->send(&msg); // Unsolicited -- no originating message.
    }

    /* (B) Client sends an unsolicited CoolRsp to the server via send().
     *     When handles are enabled, attach one to exercise the handle path concurrently. */
    {
      auto msg = pair.m_cli->create_msg();
      msg.body_root()->initCoolRsp().setCoolVal(888);
      if constexpr(TRANSMIT_NATIVE_HANDLES)
      {
        msg.store_native_handle_or_null(util::Native_handle{::dup(STDOUT_FILENO)});
      }
      pair.m_cli->send(&msg);
    }

    // (C) Client issues an async_request() (val=2000) -- server responds immediately; handler should fire.
    {
      auto req = pair.m_cli->create_msg();
      req.body_root()->initCoolReq().setCoolVal(2000);
      pair.m_cli->async_request(&req, nullptr, nullptr,
                                [&](auto&&)
      {
        ++successes;
        EXPECT_LT(steady_clock::now() - t0, PROMPTNESS_LIMIT)
          << "async_request() response handler should fire promptly.";
      });
    }

    /* (D) Test late expect_msgs registration: undo part (A)'s handler so subsequent COOL_RSP
     *     messages have no handler and get queued internally.  Then have the server send one;
     *     then re-register a new handler -- the queued message should be emitted immediately.
     *     (Small delay first to let (A)'s message be fully dispatched before we undo its handler.) */
    boost::this_thread::sleep_for(milliseconds{100});
    pair.m_cli->undo_expect_msgs(Body::COOL_RSP);
    // Now send a COOL_RSP that will arrive with no handler registered -- it gets queued.
    {
      auto msg = pair.m_srv->create_msg();
      msg.body_root()->initCoolRsp().setCoolVal(666);
      pair.m_srv->send(&msg);
    }
    // Wait for the message to arrive and be queued on the client side.
    boost::this_thread::sleep_for(milliseconds{100});
    // Re-register: queued message should emit immediately to the new handler.
    const auto t_late = steady_clock::now();
    pair.m_cli->expect_msgs(Body::COOL_RSP, [&](auto&&)
    {
      ++successes;
      /* This handler should fire ~immediately upon registration (message already queued).
       * Use a tight threshold relative to re-registration time, not t0. */
      EXPECT_LT(steady_clock::now() - t_late, milliseconds{100})
        << "Late-registered expect_msgs() handler should fire promptly from queue.";
    });

    // --- Wait for sync_request() to complete ---
    cli_sync_loop.stop();

    // Verify sync_request() itself succeeded.
    ASSERT_FALSE(sync_err) << "sync_request() error: [" << sync_err << "] [" << sync_err.message() << "].";
    ASSERT_TRUE(sync_rsp_ok);
    EXPECT_EQ(sync_rsp_val, 1000u);

    // Verify the server received the concurrent send().
    EXPECT_TRUE(srv_got_concurrent_send) << "Server should have received client's concurrent send().";
    if constexpr(TRANSMIT_NATIVE_HANDLES)
    {
      EXPECT_TRUE(srv_got_concurrent_handle) << "Server should have received handle with concurrent send().";
    }

    EXPECT_EQ(successes, 3) << "We are supposed to ensure 3 things happened at appropriate times, but "
                                 "apparently not all of them occurred in the first place (before sync_request() "
                                 "finished).";

    EXPECT_FALSE(cli_err);
    EXPECT_FALSE(srv_err);
  }

  /* Big-payload traffic: a request whose serialization spans many segments -- the heap builder's
   * segments are sized to the transport's max-blob-size, so a couple hundred KiB of list data forces
   * multi-segment emission in every config, most acutely over the small-max-message-size MQ pipes --
   * content-verified via a sum echoed in the response; then the same in the opposite direction.
   * (The sibling tests here all use near-empty payloads, while transport_test pushes big payloads but
   * always in one fixed channel config per mode.  So this test is what exercises multi-segment
   * serialization in each of the 6 configs.) */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_multi_segment_payloads()
  {
    using boost::chrono::seconds;

    atomic<bool> cli_err{false};
    atomic<bool> srv_err{false};
    auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                  ([&](const Error_code&) { cli_err = true; },
                   [&](const Error_code&) { srv_err = true; });

    constexpr size_t N = 32 * 1024; // x8 bytes each = 256KiB of list payload: many segments in any config.

    // Arm each side to respond to a request by echoing the sum of its payload list.
    const auto arm_responder = [](auto& chan)
    {
      chan->expect_msgs(Body::COOL_REQ, [&chan](auto&& req)
      {
        const auto payload = req->body_root().getCoolReq().getPayload();
        uint64_t sum = 0;
        for (size_t idx = 0; idx != payload.size(); ++idx)
        {
          sum += payload[idx];
        }
        auto rsp = chan->create_msg();
        rsp.body_root()->initCoolRsp().setCoolVal(sum);
        chan->send(&rsp, req.get());
      });
    };
    arm_responder(pair.m_cli);
    arm_responder(pair.m_srv);

    // One big request each direction; the echoed sum proves the many segments arrived intact.
    const auto send_big_and_check = [&](auto& chan, uint64_t salt)
    {
      auto req = chan->create_msg();
      auto payload = req.body_root()->initCoolReq().initPayload(N);
      uint64_t expected_sum = 0;
      for (size_t idx = 0; idx != N; ++idx)
      {
        const uint64_t val = salt + idx;
        payload.set(idx, val);
        expected_sum += val;
      }
      Error_code err;
      const auto rsp = chan->sync_request(&req, nullptr, seconds(10), &err);
      EXPECT_FALSE(err) << "[" << err << "] [" << err.message() << "].";
      ASSERT_TRUE(rsp);
      EXPECT_EQ(rsp->body_root().getCoolRsp().getCoolVal(), expected_sum);
    };
    send_big_and_check(pair.m_cli, 1212);
    send_big_and_check(pair.m_srv, 6767);

    EXPECT_FALSE(cli_err);
    EXPECT_FALSE(srv_err);
  } // test_multi_segment_payloads()

  /* The unexpected-response machinery, end to end.  A response arrives at the client for which no expectation is
   * registered: because the one-off request was already satisfied by an earlier response; or because the
   * open-ended request's expectation was undone via undo_expect_responses().  Then the client fires its
   * set_unexpected_response_handler() handler with the offending message, and informs the server via an internal
   * message; the server fires its set_remote_unexpected_response_handler() handler with the offending out-message's
   * ID.  Stats count all of it on both sides; and the informing happens whether or not any handler is registered.
   * Also the set/unset/undo return-value contracts along the way. */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_unexpected_response()
  {
    using boost::promise;
    using std::string;

    atomic<bool> cli_err{false};
    atomic<bool> srv_err{false};
    auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                  ([&](const Error_code&) { cli_err = true; },
                   [&](const Error_code&) { srv_err = true; });
    using Struc_channel_t = typename decltype(pair)::Struc_channel_t;
    using Msg_in_ptr = typename Struc_channel_t::Msg_in_ptr;
    using msg_id_out_t = typename Struc_channel_t::msg_id_out_t;
    auto& cli = *pair.m_cli;
    auto& srv = *pair.m_srv;

    const auto make_req = [](Struc_channel_t& chan, uint64_t val)
    {
      auto msg = chan.create_msg();
      msg.body_root()->initCoolReq().setCoolVal(val);
      return msg;
    };
    const auto make_rsp = [](Struc_channel_t& chan, uint64_t val)
    {
      auto msg = chan.create_msg();
      msg.body_root()->initCoolRsp().setCoolVal(val);
      return msg;
    };
    const auto never = [](auto&&) { ADD_FAILURE() << "A response was expected never to arrive here."; };

    /* Sync point: one round trip in each direction.  Each side handles its in-pipe in order, so once the
     * client has the response to its request, it has processed anything the server sent earlier (e.g., an
     * offending response); and once the server has the response to *its* request, it has processed anything
     * the client sent earlier, including the internal message about that offending response (the client sends
     * it while processing the offense, hence before it sends its response to the server's request). */
    const auto arm_echo = [&](Struc_channel_t& chan)
    {
      chan.expect_msgs(Body::COOL_REQ, [&chan, make_rsp](auto&& req)
      {
        const auto val = req->body_root().getCoolReq().getCoolVal();
        auto rsp = make_rsp(chan, val);
        chan.send(&rsp, req.get());
      });
    };
    const auto settle = [&]()
    {
      for (auto* const chan : { &cli, &srv })
      {
        auto req = make_req(*chan, 0);
        Error_code err;
        const auto rsp = chan->sync_request(&req, nullptr, &err);
        EXPECT_FALSE(err) << err.message();
        EXPECT_TRUE(rsp);
      }
    };

    /* Server: echo (for settle()); but for a request with a non-zero value, respond *twice* -- a proper response
     * via send(), then a duplicate via async_request() (a response that itself expects a response), so that its
     * out-message ID is known to us, for checking against what the remote-unexpected-response handler reports.
     * Except for the value 2 (the undo scenario), hand the request over to the test thread instead, which shall
     * respond at its own pace. */
    constexpr uint64_t VAL_UNDO_SCENARIO = 2;
    atomic<msg_id_out_t> dupe_rsp_id{0};
    optional<promise<Msg_in_ptr>> srv_got_req;
    srv.expect_msgs(Body::COOL_REQ, [&](auto&& req)
    {
      const auto val = req->body_root().getCoolReq().getCoolVal();
      if (val == VAL_UNDO_SCENARIO)
      {
        srv_got_req->set_value(std::move(req));
        return;
      }
      // else
      auto rsp = make_rsp(srv, val);
      srv.send(&rsp, req.get());
      if (val != 0)
      {
        auto dupe_rsp = make_rsp(srv, val + 1);
        msg_id_out_t id;
        srv.async_request(&dupe_rsp, req.get(), &id, never);
        dupe_rsp_id = id;
      }
    });
    arm_echo(cli);

    // The handlers under test; each scenario re-arms its promise before triggering.
    optional<promise<uint64_t>> cli_unexpected; // Value from the offending response's body.
    optional<promise<msg_id_out_t>> srv_remote_unexpected; // Offending out-message ID as reported.
    EXPECT_FALSE(cli.unset_unexpected_response_handler()); // Nothing to unset yet.
    EXPECT_FALSE(srv.unset_remote_unexpected_response_handler());
    EXPECT_TRUE(cli.set_unexpected_response_handler([&](Msg_in_ptr&& msg)
    {
      cli_unexpected->set_value(msg->body_root().getCoolRsp().getCoolVal());
    }));
    EXPECT_FALSE(cli.set_unexpected_response_handler([](Msg_in_ptr&&) {})); // Already set.
    EXPECT_TRUE(srv.set_remote_unexpected_response_handler([&](msg_id_out_t msg_id_out, string&& mdt_text)
    {
      EXPECT_FALSE(mdt_text.empty());
      srv_remote_unexpected->set_value(msg_id_out);
    }));
    EXPECT_FALSE(srv.set_remote_unexpected_response_handler([](msg_id_out_t, string&&) {}));

    // Scenario 1: one-off request, satisfied by the 1st response; the duplicate is unexpected.
    {
      FLOW_TEST_TRACE_CTX("Satisfied one-off request, then a duplicate response.");
      cli_unexpected.emplace();
      srv_remote_unexpected.emplace();
      promise<uint64_t> cli_got_rsp;
      auto req = make_req(cli, 10);
      EXPECT_TRUE(cli.async_request(&req, nullptr, nullptr, [&](Msg_in_ptr&& rsp)
      {
        cli_got_rsp.set_value(rsp->body_root().getCoolRsp().getCoolVal());
      }));
      EXPECT_EQ(cli_got_rsp.get_future().get(), 10u);
      EXPECT_EQ(cli_unexpected->get_future().get(), 11u);
      EXPECT_EQ(srv_remote_unexpected->get_future().get(), dupe_rsp_id.load());
      EXPECT_TRUE(srv.undo_expect_responses(dupe_rsp_id)); // (Tidy up the duplicate's own expectation.)
    }

    // Scenario 2: open-ended request; its expectation undone; then the (single) response is unexpected.
    {
      FLOW_TEST_TRACE_CTX("Open-ended request undone, then its response.");
      cli_unexpected.emplace();
      srv_remote_unexpected.emplace();
      srv_got_req.emplace();
      auto req = make_req(cli, VAL_UNDO_SCENARIO);
      msg_id_out_t req_id;
      EXPECT_TRUE(cli.async_request(&req, nullptr, &req_id, never));
      const auto srv_req = srv_got_req->get_future().get();
      EXPECT_TRUE(cli.undo_expect_responses(req_id));
      EXPECT_FALSE(cli.undo_expect_responses(req_id)); // Already undone.

      auto rsp = make_rsp(srv, 22);
      msg_id_out_t rsp_id;
      EXPECT_TRUE(srv.async_request(&rsp, srv_req.get(), &rsp_id, never));
      EXPECT_EQ(cli_unexpected->get_future().get(), 22u);
      EXPECT_EQ(srv_remote_unexpected->get_future().get(), rsp_id);
      EXPECT_TRUE(srv.undo_expect_responses(rsp_id));
    }

    // Scenario 3: no handlers registered anywhere; the same thing happens, minus the handler invocations.
    {
      FLOW_TEST_TRACE_CTX("Satisfied one-off request, then a duplicate response; no handlers.");
      EXPECT_TRUE(cli.unset_unexpected_response_handler());
      EXPECT_FALSE(cli.unset_unexpected_response_handler());
      EXPECT_TRUE(srv.unset_remote_unexpected_response_handler());
      EXPECT_FALSE(srv.unset_remote_unexpected_response_handler());
      cli_unexpected.reset(); // A handler firing now would be a null deref: loud enough.
      srv_remote_unexpected.reset();

      promise<uint64_t> cli_got_rsp;
      auto req = make_req(cli, 30);
      EXPECT_TRUE(cli.async_request(&req, nullptr, nullptr, [&](Msg_in_ptr&& rsp)
      {
        cli_got_rsp.set_value(rsp->body_root().getCoolRsp().getCoolVal());
      }));
      EXPECT_EQ(cli_got_rsp.get_future().get(), 30u);
      settle(); // The duplicate response and the internal message about it have been processed by now.
      EXPECT_TRUE(srv.undo_expect_responses(dupe_rsp_id));
    }

    // Stats: 3 offenses in total, each = 1 unexpected response at the client + 1 internal message client -> server.
    settle();
    const auto cs = cli.stats();
    const auto ss = srv.stats();
    EXPECT_EQ(cs.m_core.m_rcv.m_unexpected_responses, 3u);
    EXPECT_EQ(cs.m_core.m_snd.m_msg.m_internal_msgs, 3u);
    EXPECT_EQ(ss.m_core.m_rcv.m_msg.m_internal_msgs, 3u);
    EXPECT_EQ(ss.m_core.m_rcv.m_unexpected_responses, 0u);
    EXPECT_EQ(ss.m_core.m_snd.m_msg.m_internal_msgs, 0u);
    EXPECT_EQ(cs.m_core.m_rcv.m_msg.m_internal_msgs, 0u);
    // Canary (see its doc header): an internal message failed to serialize = a Flow-IPC bug.
    EXPECT_EQ(cs.m_core.m_snd.m_internal_msgs_unserializable, 0u);
    EXPECT_EQ(ss.m_core.m_snd.m_internal_msgs_unserializable, 0u);

    EXPECT_FALSE(cli_err);
    EXPECT_FALSE(srv_err);
  } // test_unexpected_response()

  /* async_end_sending() at the struc level, and what a hosed channel looks like.  The client ends sending: its
   * completion handler fires; every send-type call is then refused with no error emitted and without leaving a
   * response expectation behind; receiving still works.  The server, seeing graceful-close, hoses: its on-error
   * handler reports it; every expectation and handler it had registered is discarded (gauges back to 0) and every
   * registration/undo call is refused from then on; async_end_sending() still works there, as documented. */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_end_sending_and_hosing()
  {
    using boost::promise;

    promise<Error_code> cli_err;
    promise<Error_code> srv_err;
    auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                  ([&](const Error_code& err_code) { cli_err.set_value(err_code); },
                   [&](const Error_code& err_code) { srv_err.set_value(err_code); });
    using Struc_channel_t = typename decltype(pair)::Struc_channel_t;
    using Msg_in_ptr = typename Struc_channel_t::Msg_in_ptr;
    using msg_id_out_t = typename Struc_channel_t::msg_id_out_t;
    auto& cli = *pair.m_cli;
    auto& srv = *pair.m_srv;
    const auto never = [](auto&&) { ADD_FAILURE() << "This handler was expected never to fire."; };
    const auto make_req = [](Struc_channel_t& chan, uint64_t val)
    {
      auto msg = chan.create_msg();
      msg.body_root()->initCoolReq().setCoolVal(val);
      return msg;
    };
    const auto make_rsp = [](Struc_channel_t& chan, uint64_t val)
    {
      auto msg = chan.create_msg();
      msg.body_root()->initCoolRsp().setCoolVal(val);
      return msg;
    };

    // Server: load it up with every kind of registration, so that we can watch the hosing discard them all.
    EXPECT_TRUE(srv.expect_msgs(Body::COOL_REQ, never));
    EXPECT_TRUE(srv.expect_msg(Body::COOL_RSP, never));
    msg_id_out_t srv_req_id;
    {
      auto req = make_req(srv, 1);
      EXPECT_TRUE(srv.async_request(&req, nullptr, &srv_req_id, never)); // (Client shall never respond.)
    }
    EXPECT_TRUE(srv.set_unexpected_response_handler(never));
    EXPECT_TRUE(srv.set_remote_unexpected_response_handler([](msg_id_out_t, std::string&&) { ADD_FAILURE(); }));
    {
      const auto stats = srv.stats().m_core.m_rcv;
      EXPECT_EQ(stats.m_expect_msgs_active, 1u);
      EXPECT_EQ(stats.m_expect_msg_active, 1u);
      EXPECT_EQ(stats.m_expect_response_sticky_active, 1u);
    }
    // Server also sends the client a notification, which the client shall pick up only after it ends sending.
    {
      auto msg = make_rsp(srv, 2);
      EXPECT_TRUE(srv.send(&msg));
    }

    // Client ends sending.
    promise<Error_code> cli_end_sending_done;
    EXPECT_TRUE(cli.async_end_sending([&](const Error_code& err_code) { cli_end_sending_done.set_value(err_code); }));
    EXPECT_FALSE(cli.async_end_sending(never)); // Dupe while pending.
    EXPECT_FALSE(cli_end_sending_done.get_future().get()); // Success.
    EXPECT_FALSE(cli.async_end_sending(never)); // Dupe after completion.
    {
      FLOW_TEST_TRACE_CTX("Client, after ending sending.");
      Error_code err_code;
      auto req = make_req(cli, 3);
      EXPECT_FALSE(cli.send(&req, nullptr, &err_code));
      EXPECT_FALSE(err_code);
      EXPECT_FALSE(cli.async_request(&req, nullptr, nullptr, never, &err_code));
      EXPECT_FALSE(err_code);
      msg_id_out_t id;
      EXPECT_FALSE(cli.async_request(&req, nullptr, &id, never, &err_code));
      EXPECT_FALSE(err_code);
      EXPECT_FALSE(cli.sync_request(&req, nullptr, &err_code));
      EXPECT_FALSE(err_code);
      // The refused requests did not leave expectations behind.
      const auto stats = cli.stats().m_core.m_rcv;
      EXPECT_EQ(stats.m_expect_response_one_off_active, 0u);
      EXPECT_EQ(stats.m_expect_response_sticky_active, 0u);
      // Receiving is unaffected: the server's earlier notification is delivered.
      promise<uint64_t> got_val;
      EXPECT_TRUE(cli.expect_msg(Body::COOL_RSP, [&](Msg_in_ptr&& msg)
                                                   { got_val.set_value(msg->body_root().getCoolRsp().getCoolVal()); }));
      EXPECT_EQ(got_val.get_future().get(), 2u);
    }

    // Server is hosed by the graceful-close.
    EXPECT_EQ(srv_err.get_future().get(), transport::error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE);
    {
      FLOW_TEST_TRACE_CTX("Server, hosed.");
      const auto stats = srv.stats().m_core.m_rcv;
      EXPECT_EQ(stats.m_expect_msgs_active, 0u);
      EXPECT_EQ(stats.m_expect_msg_active, 0u);
      EXPECT_EQ(stats.m_expect_response_sticky_active, 0u);
      EXPECT_EQ(stats.m_expect_response_one_off_active, 0u);

      EXPECT_FALSE(srv.expect_msgs(Body::COOL_REQ, never));
      EXPECT_FALSE(srv.expect_msg(Body::COOL_RSP, never));
      EXPECT_FALSE(srv.undo_expect_msgs(Body::COOL_REQ));
      EXPECT_FALSE(srv.undo_expect_responses(srv_req_id));
      EXPECT_FALSE(srv.set_unexpected_response_handler(never));
      EXPECT_FALSE(srv.unset_unexpected_response_handler()); // Discarded by the hosing already.
      EXPECT_FALSE(srv.set_remote_unexpected_response_handler([](msg_id_out_t, std::string&&) {}));
      EXPECT_FALSE(srv.unset_remote_unexpected_response_handler());
      Error_code err_code;
      auto req = make_req(srv, 4);
      EXPECT_FALSE(srv.send(&req, nullptr, &err_code));
      EXPECT_FALSE(err_code);
      EXPECT_FALSE(srv.async_request(&req, nullptr, nullptr, never, &err_code));
      EXPECT_FALSE(err_code);
      EXPECT_FALSE(srv.sync_request(&req, nullptr, &err_code));
      EXPECT_FALSE(err_code);
      /* async_end_sending() operates at the lower layer and is the recommended last step even after hosing.
       * It completes; with what code depends on the transport (its doc header explains), so we don't check.
       * Until now the client's in-pipe was fine (see the receive above); this graceful-close now hoses it too. */
      promise<void> srv_end_sending_done;
      EXPECT_TRUE(srv.async_end_sending([&](const Error_code&) { srv_end_sending_done.set_value(); }));
      srv_end_sending_done.get_future().wait();
    }
    EXPECT_EQ(cli_err.get_future().get(), transport::error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE);
  } // test_end_sending_and_hosing()

  /* undo_expect_msgs() and undo_expect_responses() refuse one-off expectations (expect_msg(); one-off
   * async_request()), which remain in force and fire; they undo the sticky kinds. */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_undo_refuses_one_offs()
  {
    using boost::promise;

    atomic<bool> cli_err{false};
    atomic<bool> srv_err{false};
    auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                  ([&](const Error_code&) { cli_err = true; },
                   [&](const Error_code&) { srv_err = true; });
    using Struc_channel_t = typename decltype(pair)::Struc_channel_t;
    using Msg_in_ptr = typename Struc_channel_t::Msg_in_ptr;
    using msg_id_out_t = typename Struc_channel_t::msg_id_out_t;
    auto& cli = *pair.m_cli;
    auto& srv = *pair.m_srv;
    const auto never = [](auto&&) { ADD_FAILURE() << "This handler was expected never to fire."; };
    const auto make_req = [](Struc_channel_t& chan, uint64_t val)
    {
      auto msg = chan.create_msg();
      msg.body_root()->initCoolReq().setCoolVal(val);
      return msg;
    };
    const auto make_rsp = [](Struc_channel_t& chan, uint64_t val)
    {
      auto msg = chan.create_msg();
      msg.body_root()->initCoolRsp().setCoolVal(val);
      return msg;
    };

    // Server hands each request over to the test thread, which responds (or not) at its own pace.
    std::optional<promise<Msg_in_ptr>> srv_got_req;
    EXPECT_TRUE(srv.expect_msgs(Body::COOL_REQ, [&](Msg_in_ptr&& req) { srv_got_req->set_value(std::move(req)); }));

    {
      FLOW_TEST_TRACE_CTX("One-off message expectation.");
      promise<uint64_t> got_val;
      EXPECT_TRUE(cli.expect_msg(Body::COOL_RSP, [&](Msg_in_ptr&& msg)
                                                   { got_val.set_value(msg->body_root().getCoolRsp().getCoolVal()); }));
      EXPECT_FALSE(cli.undo_expect_msgs(Body::COOL_RSP)); // Refused: it is one-off.
      EXPECT_EQ(cli.stats().m_core.m_rcv.m_expect_msg_active, 1u); // Still in force...
      auto msg = make_rsp(srv, 5);
      EXPECT_TRUE(srv.send(&msg));
      EXPECT_EQ(got_val.get_future().get(), 5u); // ...and fires.
      // Whereas the sticky kind is undone (once).
      EXPECT_TRUE(cli.expect_msgs(Body::COOL_RSP, never));
      EXPECT_TRUE(cli.undo_expect_msgs(Body::COOL_RSP));
      EXPECT_FALSE(cli.undo_expect_msgs(Body::COOL_RSP));
      EXPECT_EQ(cli.stats().m_core.m_rcv.m_expect_msgs_active, 0u);
    }

    {
      FLOW_TEST_TRACE_CTX("One-off response expectation.");
      /* An open-ended request (its ID reported to us); then a one-off one (its ID is not reported, but out-message
       * IDs are sequential -- they are the seq#s of the structured protocol -- and nothing else is sent in
       * between, so it is the next ID). */
      srv_got_req.emplace();
      msg_id_out_t sticky_id;
      auto sticky_req = make_req(cli, 6);
      EXPECT_TRUE(cli.async_request(&sticky_req, nullptr, &sticky_id, never));
      srv_got_req->get_future().wait(); // (The server shall never respond to this one.)
      srv_got_req.emplace();
      promise<uint64_t> got_val;
      auto one_off_req = make_req(cli, 7);
      EXPECT_TRUE(cli.async_request(&one_off_req, nullptr, nullptr, [&](Msg_in_ptr&& rsp)
                                                                      { got_val.set_value(rsp->body_root().getCoolRsp()
                                                                                             .getCoolVal()); }));
      const auto srv_one_off_req = srv_got_req->get_future().get();
      const msg_id_out_t one_off_id = sticky_id + 1;

      EXPECT_FALSE(cli.undo_expect_responses(one_off_id)); // Refused: it is one-off.
      EXPECT_EQ(cli.stats().m_core.m_rcv.m_expect_response_one_off_active, 1u); // Still in force...
      auto rsp = make_rsp(srv, 8);
      EXPECT_TRUE(srv.send(&rsp, srv_one_off_req.get()));
      EXPECT_EQ(got_val.get_future().get(), 8u); // ...and fires.
      // Whereas the sticky kind is undone (once).
      EXPECT_TRUE(cli.undo_expect_responses(sticky_id));
      EXPECT_FALSE(cli.undo_expect_responses(sticky_id));
      EXPECT_EQ(cli.stats().m_core.m_rcv.m_expect_response_sticky_active, 0u);
    }

    EXPECT_FALSE(cli_err);
    EXPECT_FALSE(srv_err);
  } // test_undo_refuses_one_offs()

  /* Like make_session_struc_pair() but the server-side struc::Channel is left un-start()ed: nothing reads its
   * in-pipes, so the client's sends can fill the low-level transports up (would-block).  Caller start()s it. */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  auto make_session_struc_pair_srv_unstarted(std::function<void(const Error_code&)> on_cli_err)
  {
    using Result = Struc_session_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>;
    using Struc_channel_t = typename Result::Struc_channel_t;

    auto session_channel_pair = make_session_channel_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>();
    const auto builder_config
      = Struc_channel_t::heap_fixed_builder_config(session_channel_pair.m_cli_channels.front());
    const auto reader_config = Struc_channel_t::heap_reader_config(session_channel_pair.m_cli_channels.front());

    Result result;
    result.m_sessions = std::move(session_channel_pair.m_sessions);
    result.m_cli
      = std::make_unique<Struc_channel_t>(nullptr, std::move(session_channel_pair.m_cli_channels.front()),
                                          builder_config, NULL_SESSION, reader_config,
                                          result.m_sessions->m_cli_session.session_token());
    result.m_srv
      = std::make_unique<Struc_channel_t>(nullptr, std::move(session_channel_pair.m_srv_channels.front()),
                                          builder_config, NULL_SESSION, reader_config,
                                          result.m_sessions->m_srv_session.session_token());
    result.m_cli->start(std::move(on_cli_err));
    return result;
  }

  /* async_end_sending() at the struc level with both pipes of a 2-pipe channel in would-block (the receiver is
   * not reading): the graceful-close has to queue behind the messages on each pipe; the completion handler fires,
   * with success, only once the receiver drains them all; and it does receive them all, then the graceful-close.
   * (transport::Channel-level tests cover the combining logic in detail; this is the indirect call site.) */
  template<MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
  void test_end_sending_would_block()
  {
    if constexpr((MQ_TYPE_OR_NONE == MqType::NONE) || (!TRANSMIT_NATIVE_HANDLES))
    {
      GTEST_SKIP() << "Needs a 2-pipe channel (MQs for blobs, socket for handles).";
    }
    else
    {
      using boost::promise;
      using util::Native_handle;

      /* Sub-cases: which pipe(s) to fill.  A filled pipe holds many messages ahead of its graceful-close; the
       * other holds 1.  So the graceful-close of the unfilled pipe tends to be processed by the receiver first,
       * while the filled pipe still has messages in flight: the situation in which those must not be lost. */
      for (const auto& [fill_blob_pipe, fill_hndl_pipe] : { std::make_pair(true, false), std::make_pair(false, true),
                                                            std::make_pair(true, true) })
      {
        FLOW_TEST_TRACE_CTX("Fill blob pipe? = [", fill_blob_pipe, "]; fill handles pipe? = [", fill_hndl_pipe, "].");

        atomic<bool> cli_err{false};
        auto pair = make_session_struc_pair_srv_unstarted<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                      ([&](const Error_code&) { cli_err = true; });
        using Struc_channel_t = typename decltype(pair)::Struc_channel_t;
        using Msg_in_ptr = typename Struc_channel_t::Msg_in_ptr;
        auto& cli = *pair.m_cli;
        auto& srv = *pair.m_srv;

        /* Messages without a handle go over the blobs pipe (the MQs); with one, over the handles pipe (the socket).
         * A big payload, in the latter case, so that each message takes several low-level blobs: fills the socket
         * buffer sooner.  To fill a pipe: send until the transport reports would-block.
         *
         * The arithmetic assumes the session's default MQ message size (Session_server::mq_msg_size_limit() left
         * at 0 by the harness; SHM-none session => 8 KiB), which caps each low-level blob on either pipe: a 32 KiB
         * payload is ~5 blobs, so the socket fills in well under 10 messages; the MQs hold 10.  Make that explicit: */
        EXPECT_EQ(cli.owned_channel()->send_blob_max_size(), 8u * 1024);
        constexpr size_t PAYLOAD_N = 4 * 1024; // x8 bytes.
        size_t n_sent = 0;
        const auto send_msgs = [&](bool with_hndl, bool fill)
        {
          const auto would_block_count = [&]() -> uint64_t
          {
            return with_hndl ? cli.owned_channel()->native_handle_send_stats().m_would_block_count
                             : cli.owned_channel()->blob_send_stats().m_would_block_count;
          };
          do
          {
            auto req = cli.create_msg();
            auto payload = req.body_root()->initCoolReq().initPayload(with_hndl ? PAYLOAD_N : 1);
            payload.set(0, n_sent);
            if (with_hndl)
            {
              req.store_native_handle_or_null(Native_handle{::dup(STDOUT_FILENO)});
            }
            Error_code err_code;
            ASSERT_TRUE(cli.send(&req, nullptr, &err_code));
            ASSERT_FALSE(err_code) << err_code.message();
            ++n_sent;
            ASSERT_LT(n_sent, size_t(10000)) << "Low-level send buffers never filled up?  Test premise is off.";
          }
          while (fill && (would_block_count() == 0));
        };
        send_msgs(false, fill_blob_pipe);
        send_msgs(true, fill_hndl_pipe);

        promise<Error_code> end_sending_done;
        EXPECT_TRUE(cli.async_end_sending([&](const Error_code& err_code)
                                            { end_sending_done.set_value(err_code); }));

        // Now the server reads: everything the client sent, in order per pipe; then the graceful-close hoses it.
        promise<Error_code> srv_err;
        atomic<size_t> n_received{0};
        EXPECT_TRUE(srv.expect_msgs(Body::COOL_REQ, [&](Msg_in_ptr&& req)
        {
          req->emit_native_handle_or_null().close(); // (Null or not.)
          ++n_received;
        }));
        EXPECT_TRUE(srv.start([&](const Error_code& err_code) { srv_err.set_value(err_code); }));

        EXPECT_FALSE(end_sending_done.get_future().get()); // Success, once flushed.
        EXPECT_EQ(srv_err.get_future().get(), transport::error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE);
        EXPECT_EQ(n_received.load(), n_sent);
        EXPECT_FALSE(cli_err);
      }
    }
  } // test_end_sending_would_block()
} // namespace (anon)

/* Instantiation macros: CHANNEL_TYPE_TESTS() expands the whole battery of test-body functions above into
 * TEST() cases covering the given MqType x {handles=false, handles=true}.  Invoked by each sibling TU for
 * its one MqType (see top of this file). */
#define CHANNEL_TYPE_TEST(test_name, mq_val, mq_moniker) \
  TEST(Struc_channel_test, test_name##_##mq_moniker##_NoHandles) { test_##test_name<MqType::mq_val, false>(); } \
  TEST(Struc_channel_test, test_name##_##mq_moniker##_Handles)   { test_##test_name<MqType::mq_val, true>();  }
#define CHANNEL_TYPE_TESTS(mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(sync_request_concurrency, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(sync_request_serialization, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(sync_request_timeout, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(sync_request_graceful_close, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(sync_request_hard_close, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(sync_request_concurrent_ops, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(multi_segment_payloads, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(unexpected_response, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(end_sending_and_hosing, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(undo_refuses_one_offs, mq_val, mq_moniker) \
  CHANNEL_TYPE_TEST(end_sending_would_block, mq_val, mq_moniker)

} // namespace ipc::transport::struc::test
