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
#include "ipc/transport/error.hpp"
#include "ipc/transport/bipc_mq_handle.hpp"
#include "ipc/transport/posix_mq_handle.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util.hpp>
#include <boost/thread/future.hpp>
#include <atomic>
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
    std::atomic<bool> cli_err{false};
    std::atomic<bool> srv_err{false};
    auto pair = make_session_struc_pair<Body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>
                  ([&](auto&&) { cli_err = true; },
                   [&](auto&&) { srv_err = true; });

    /* Server side: echo back every CoolReq as a CoolRsp with the same value.
     * When handles are enabled: odd-val requests carry a handle; echo one back in the response. */
    std::atomic<uint64_t> srv_handle_checks{0};
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
          hndl.release();
          rsp.store_native_handle_or_null(util::Native_handle{::dup(STDOUT_FILENO)});
        }
        ++srv_handle_checks;
      }
      rsp.body_root()->initCoolRsp().setCoolVal(val);
      pair.m_srv->send(&rsp, req.get());
    });

    // Client side: N_THREADS task loops, each doing N_REQUESTS_PER_THREAD sync_request() calls.
    std::atomic<uint64_t> total_successes{0};
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
            rsp_hndl.release();
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

    std::atomic<bool> cli_err{false};
    std::atomic<bool> srv_err{false};
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

    std::atomic<int> successes{0};

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

    std::atomic<bool> cli_err{false};
    std::atomic<bool> srv_err{false};
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

      std::atomic<bool> cli_err{false};
      std::atomic<bool> srv_err{false};
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

      std::atomic<bool> cli_err{false};
      std::atomic<bool> srv_err{false};
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

    std::atomic<bool> cli_err{false};
    std::atomic<bool> srv_err{false};
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
                                 [&, val, delay, req = std::move(req)](auto)
      {
        auto rsp = pair.m_srv->create_msg();
        rsp.body_root()->initCoolRsp().setCoolVal(val);
        pair.m_srv->send(&rsp, req.get());
      });
    });

    /* Server: handler for unsolicited CoolRsp from the client (verifies client's send() worked).
     * When handles are enabled, also verify the handle arrived. */
    std::atomic<bool> srv_got_concurrent_send{false};
    std::atomic<bool> srv_got_concurrent_handle{false};
    pair.m_srv->expect_msgs(Body::COOL_RSP, [&](auto&& rsp)
    {
      srv_got_concurrent_send = true;
      if constexpr(TRANSMIT_NATIVE_HANDLES)
      {
        auto hndl = rsp->emit_native_handle_or_null();
        srv_got_concurrent_handle = !hndl.null();
        hndl.release();
      }
    });

    /* sync_request() will block for 500ms (the server delays its response that long).  All concurrent
     * operations should complete well within that window.  Each handler checks inline that it fired
     * promptly; we use 400ms as a generous threshold (well under the 500ms sync_request delay). */
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

    // Wait for the server to have received the sync_request's CoolReq (client is now blocked).
    sync_req_arrived.get_future().wait();

    /* --- Concurrent operations while sync_request() is blocked ---
     * t0 marks the start of concurrent ops; each handler verifies it fired within PROMPTNESS_LIMIT. */
    const auto t0 = steady_clock::now();

    std::atomic<int> successes{0};

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

    // (C) Client issues an async_request (val=2000) -- server responds immediately; handler should fire.
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

    // --- Wait for sync_request to complete ---
    cli_sync_loop.stop();

    // Verify sync_request itself succeeded.
    ASSERT_FALSE(sync_err) << "sync_request error: [" << sync_err << "] [" << sync_err.message() << "].";
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

    std::atomic<bool> cli_err{false};
    std::atomic<bool> srv_err{false};
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

  /* Instantiation macro: expands one test-body function into 6 TEST() cases covering the full matrix.
   * Each combo: {MqType::NONE, BIPC, POSIX} x {handles=false, handles=true}. */
#define CHANNEL_TYPE_TEST(test_name) \
  TEST(Struc_channel_test, test_name##_None_NoHandles)  { test_##test_name<MqType::NONE,  false>(); } \
  TEST(Struc_channel_test, test_name##_None_Handles)    { test_##test_name<MqType::NONE,  true>();  } \
  TEST(Struc_channel_test, test_name##_Bipc_NoHandles)  { test_##test_name<MqType::BIPC,  false>(); } \
  TEST(Struc_channel_test, test_name##_Bipc_Handles)    { test_##test_name<MqType::BIPC,  true>();  } \
  TEST(Struc_channel_test, test_name##_Posix_NoHandles) { test_##test_name<MqType::POSIX, false>(); } \
  TEST(Struc_channel_test, test_name##_Posix_Handles)   { test_##test_name<MqType::POSIX, true>();  }
} // namespace (anon)

CHANNEL_TYPE_TEST(sync_request_concurrency)
CHANNEL_TYPE_TEST(sync_request_serialization)
CHANNEL_TYPE_TEST(sync_request_timeout)
CHANNEL_TYPE_TEST(sync_request_graceful_close)
CHANNEL_TYPE_TEST(sync_request_hard_close)
CHANNEL_TYPE_TEST(sync_request_concurrent_ops)
CHANNEL_TYPE_TEST(multi_segment_payloads)

#undef CHANNEL_TYPE_TEST

} // namespace ipc::transport::struc::test
