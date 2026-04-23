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

#include "ipc/transport/struc/channel.hpp"
#include "ipc/session/session_server.hpp"
#include "ipc/session/client_session.hpp"
#include "ipc/session/shm/classic/session_server.hpp"
#include "ipc/session/shm/classic/client_session.hpp"
#include "ipc/session/shm/arena_lend/jemalloc/session_server.hpp"
#include "ipc/session/shm/arena_lend/jemalloc/client_session.hpp"
#include "ipc/session/app.hpp"
#include "ipc/util/process_credentials.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util.hpp>
#include <boost/core/noncopyable.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/future.hpp>
#include <gtest/gtest.h>
#include <unistd.h>

namespace ipc::transport::struc::test
{

/* @todo Consider relocating this header into ipc/test (Flow-IPC-wide test tree); while it's been historically
 * a helper for making struc::Channels, it is not *necessarily* so limited, and with the addition of the
 * ShmType template parameter it now also knows about ipc::session::shm::{classic, arena_lend::jemalloc}. */

/* Traits: pick the Session types for a given (MqType, transmit-native-handles, ShmType) combination.
 * Three specializations below: no-SHM / SHM-classic / SHM-jemalloc.  The API of the chosen
 * Client_session_t / Session_server_t is identical across the three as far as this utility cares. */
template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES,
         session::schema::ShmType SHM_TYPE_OR_NONE>
struct Session_types_select;

template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
struct Session_types_select<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, session::schema::ShmType::NONE>
{
  using Client_session_t = session::Client_session<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>;
  using Session_server_t = session::Session_server<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>;
};

template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
struct Session_types_select<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, session::schema::ShmType::CLASSIC>
{
  using Client_session_t = session::shm::classic::Client_session<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>;
  using Session_server_t = session::shm::classic::Session_server<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>;
};

template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES>
struct Session_types_select<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, session::schema::ShmType::JEMALLOC>
{
  using Client_session_t
    = session::shm::arena_lend::jemalloc::Client_session<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>;
  using Session_server_t
    = session::shm::arena_lend::jemalloc::Session_server<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES>;
};

/* Utility: establish a session in-process and produce a channel pair.
 * Templated on session MqType, transmit-native-handles, and ShmType (defaulting to no-SHM) to cover the
 * full channel-type matrix x the three SHM-provider flavors (none, SHM-classic, SHM-jemalloc).
 * Outputs a pair of Channel_obj (sync_io-pattern channels).
 * The ipc::session objects, most notably the client and server `Session`s, are also available.
 * Destroy the struct to destroy all these things in the proper order.
 *
 * ### Concurrency and 2 sessions ###
 * In the intended use of ipc::session, one would have separate processes A and B respectively
 * hold (session-srv, srv-session, srv-side Channel_obj) and (cli-session, cli-side Channel_obj)
 * and then perform IPC between their `Channel_obj`s across process boundary.  We *could* model this more-proper
 * setup (maybe via fork() to begin; still not fully realistic due to using the same application but close(r)).
 *
 * While somewhat a risk, conceivably, in our case we aren't testing the end-to-end session aspects *necessarily*.
 * So that's why you see both cli_* things and srv_* things in one struct which is unusual and wouldn't be seen
 * in user code or functional-test code.
 * (Note that elsewhere functional tests exist that do the realistic things we're intentionally ignoring here.)
 *
 * That said, tests that need to be more realistic in that sense can
 * at least do cli work in thread A (and subthreads), srv work in thread B (and subthreads).
 * As of this writing some tests in fact do that, while others don't, depending on what is being tested.
 *
 * One more caution though: Even when doing sessions' work in 2 separate threads:
 *   - Client_session ctor + .sync_connect() and Server_session::sync_accept() will execute in the same thread
 *     inside make_session_*_pair().  (They offload internal work onto more background threads but never mind.)
 *   - Their dtors would execute in sequence in the same thread, when ~Session_pair() et al runs...
 *     - ...but in the case of SHM-jemalloc-based sessions <MqType jemalloc> that is explicitly disallowed (leads to
 *       deadlock: not a bug/by design due to internal impl factors).  Hence make_session_*_pair() avoids it by
 *       offloading one Session dtor to a short-lived thread in ~Session_pair().
 *
 * The point: As of this writing, for unit-testing purposes, this has not caused problems despite the lack of
 * realism.  That doesn't *guarantee* there isn't some problem we'll run into when testing something in the future,
 * so do be aware.  If needed, we can extend these utilities for realism.  This would add considerable usability
 * (in test code) complexity, so all else being equal it's best avoided in this context. */

/* The session objects common to Session_channel_pair and Struc_session_pair.
 *
 * ### Held exclusively via `shared_ptr` (Session_pair_ptr) ###
 * `Session_pair` is non-copyable/non-movable by design (see `private boost::noncopyable` and the custom dtor
 * which implicitly suppresses the move operators): the `Client_session`/`Session_server` inside store the
 * `Client_app`/`Server_app`/`Master_set` members *by address* (per the Flow-IPC App lifetime contract; see
 * `@warning` on those ctors).  Pinning the whole struct on the heap via a single `shared_ptr` gives every
 * member a stable address for the entire lifetime of `*this`, so the App members can be direct (no
 * per-member heap indirection needed) and the session members can be direct (no `optional`/`unique_ptr`
 * wrappers).  Accordingly the factories hand back `Session_pair_ptr` (inside `Session_channel_pair`
 * or `Struc_session_pair`). */
template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES,
         session::schema::ShmType SHM_TYPE_OR_NONE = session::schema::ShmType::NONE>
struct Session_pair :
  private boost::noncopyable
{
  using Synchronicity = flow::async::Synchronicity;
  using Session_types_t
    = Session_types_select<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>;
  using Client_session_t = typename Session_types_t::Client_session_t;
  using Session_server_t = typename Session_types_t::Session_server_t;
  using Server_session_t = typename Session_server_t::Server_session_obj;

  /* App registry -- stored directly (addresses stable since `*this` is heap-pinned via
   * `Session_pair_ptr`).  Declared *before* the session members so default member destruction runs the
   * sessions first, then these (apps outlive sessions, as contracted). */
  session::Client_app m_cli_app;
  session::Server_app m_srv_app;
  session::Client_app::Master_set m_cli_apps;
  session::Server_app::Master_set m_srv_apps;

  /* Sessions must outlive any channels they produced; `Session_pair` does not own channels directly but the
   * outer `Session_channel_pair` / `Struc_session_pair` does, and they always declare the `Session_pair`
   * member first -- so the sessions inside outlive those channels.
   *
   * Session_server has no default ctor (it starts listening on construction), hence `unique_ptr`.
   * Client_session and Server_session are default-constructible into NULL state, so they're direct members;
   * the factory move-assigns PEER-state sessions into place. */
  std::unique_ptr<Session_server_t> m_srv;
  Client_session_t m_cli_session;
  Server_session_t m_srv_session;

  /* Set true by the factory once everything is fully wired up (both sessions in PEER state).  When false,
   * `~Session_pair()` takes the cheap path and skips the STTL rendezvous below. */
  bool m_populated = false;

  /* ~Session -- for the SHM-jemalloc flavor only -- blocks during teardown waiting for the opposing side's
   * ~Session to have *started*: the two sides rendezvous at dtor entry.  In real deployments this is trivially
   * satisfied (each side lives in its own process, hence its own thread(s)).  In an in-process test, though,
   * both sessions are members of the same `Session_pair` and would destruct sequentially on one thread at
   * scope exit: srv dtor would block waiting for cli dtor to start, which cannot happen until srv dtor
   * returns: deadlock.  Fix: move one Session onto a short-lived thread and destroy the other here
   * concurrently.  A no-op-effect for the other SHM flavors, so we do it unconditionally (cheap; saves
   * an `if constexpr`).
   *
   * Re. the shared_ptr in the capture: `std::function` (STTL's task type) requires CopyConstructible for
   * its stored target regardless of whether a runtime copy occurs -- so a move-only `unique_ptr` capture
   * won't compile.  Solution: at the capture site, move `m_srv_session` into a new heap `Server_session_t`
   * owned by a `shared_ptr` (copyable), which is then what actually destructs inside the STTL task.
   *
   * Regarding concurrency and these sessions, please do read the comment block above Session_pair. */
  ~Session_pair()
  {
    if (!m_populated)
    {
      return; // Factory never completed full setup: just let members destruct normally.
    }
    // else

    flow::async::Single_thread_task_loop srv_dtor_thread{nullptr, "pair_dtor_srv"};
    srv_dtor_thread.start();
    srv_dtor_thread.post
      ([srv_sess = boost::make_shared<Server_session_t>(std::move(m_srv_session))]() mutable
       { srv_sess.reset(); },
       Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START); // Ensure task spinning before we proceed.
    // (SHM-jemalloc only) srv_sess.reset() => Server_session dtor waits for cli session dtor to begin.

    m_cli_session = {}; // Move-assign default-cted temp -> destroys PEER-state cli session now (in-thread).
    /* (SHM-jemalloc only) Client_session dtor waits for srv session dtor to begin.  It has.  They both
     * proceed.  Now srv_dtor_thread thread is joined. */
    srv_dtor_thread.stop();
  }
};

// Convenience alias: the factories always return a `Session_pair` inside one of these, never by value.
template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES,
         session::schema::ShmType SHM_TYPE_OR_NONE = session::schema::ShmType::NONE>
using Session_pair_ptr
  = boost::shared_ptr<Session_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>>;

template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES,
         session::schema::ShmType SHM_TYPE_OR_NONE = session::schema::ShmType::NONE>
struct Session_channel_pair
{
  using Session_pair_t = Session_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>;
  using Session_pair_ptr_t = Session_pair_ptr<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>;
  using Channel_obj = typename Session_pair_t::Client_session_t::Channel_obj;

  // Heap-pinned via shared_ptr so the App/session members inside have stable addresses.  See Session_pair.
  Session_pair_ptr_t m_sessions;

  Channel_obj m_cli_channel;
  Channel_obj m_srv_channel;
};

/* The `leave_srv_almost_peer` knob, when true, has the factory skip the
 * `sessions.m_srv_session.init_handlers(...)` call at the end, leaving the server session in
 * almost-PEER state (log-in succeeded, but init_handlers() has not yet run).  The client session is
 * still in PEER state.  Intended for tests that need to observe the almost-PEER state; such callers
 * are on the hook for calling `init_handlers()` themselves if they want the session to behave
 * normally (pre-`init_handlers()` the session is intentionally restricted -- e.g., cannot open
 * additional channels, `session_token()` may be invalid, etc.).
 *
 * The `shm_classic_pool_size_limit_mi_or_0` knob, when non-zero and SHM-classic is in use, caps
 * the per-session SHM-pool size (via Session_server::pool_size_limit_mi()) before async_accept()
 * runs -- useful when a test wants a small pool it can characterize (e.g. stuff an offset outside
 * the pool for a safety-check test).  0 means "leave default."  No-op for SHM-jemalloc / SHM-none. */
template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES,
         session::schema::ShmType SHM_TYPE_OR_NONE = session::schema::ShmType::NONE>
Session_channel_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>
  make_session_channel_pair(flow::log::Logger* logger = nullptr,
                            bool leave_srv_almost_peer = false,
                            [[maybe_unused]] size_t shm_classic_pool_size_limit_mi_or_0 = 0)
{
  /* @todo Since it's intended as a utility rather than just one thing inside one test, perhaps we should be
   * just a *bit* more careful about handling failed EXPECT()s below.  We can't just use ASSERT()s here, as we aren't
   * void, but we could wrap it in a void local lambda or something and then return null on any ASSERT() fail in that
   * lambda. Don't get us wrong; we don't expect test code to be perfectly resilient to failures; in practice
   * one fixes the first failure and re-runs -- what happens afterward hardly matters... but as a utility perhaps
   * we should be a *bit* more robust is all. / Same for make_session_struc_pair(). */

  using boost::chrono::seconds;
  namespace fs = boost::filesystem;
  using flow::async::Single_thread_task_loop;
  using flow::util::ostream_op_string;
  using flow::Error_code;

  using Result = Session_channel_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>;
  using Session_pair_t = typename Result::Session_pair_t;
  using Session_server_t = typename Session_pair_t::Session_server_t;
  using Client_session_t = typename Session_pair_t::Client_session_t;
  using Channels = typename Session_server_t::Channels;

  /* Discover our own argv[0] so that Client_app::m_exec_path matches
   * the session server's identity check (which reads /proc/<pid>/cmdline of the connecting process). */
  Error_code creds_err;
  const auto self_exe
    = util::Process_credentials::own_process_credentials().process_invoked_as(&creds_err);
  EXPECT_FALSE(creds_err) << "process_invoked_as() error: [" << creds_err << "] [" << creds_err.message() << "].";

  const fs::path work_dir = fs::canonical(fs::current_path().lexically_normal());

  /* Allocate Session_pair on the heap so the App members below have stable addresses for the whole lifetime
   * of the returned object.  (Session_server and Client_session store them by *address* per the Flow-IPC App
   * lifetime contract; function-locals would dangle the moment this function returns.) */
  Result result;
  result.m_sessions = boost::make_shared<Session_pair_t>();
  auto& sessions = *result.m_sessions;

  /* App descriptors -- both sides are the same process; we intentionally "cheat" the identity check.
   * Unique names per template instantiation, since each combo is a separate test.  If there's logging it
   * might be helpful; or maybe in debugger. */
  const auto cli_name = ostream_op_string("ipc_ut_cli_mq", int(MQ_TYPE_OR_NONE), "_h", int(TRANSMIT_NATIVE_HANDLES),
                                          "_shm", int(SHM_TYPE_OR_NONE));
  const auto srv_name = ostream_op_string("ipc_ut_srv_mq", int(MQ_TYPE_OR_NONE), "_h", int(TRANSMIT_NATIVE_HANDLES),
                                          "_shm", int(SHM_TYPE_OR_NONE));
  sessions.m_cli_app = session::Client_app{{ cli_name, self_exe, ::geteuid(), ::getegid() }};
  sessions.m_srv_app = session::Server_app{{ srv_name, self_exe, ::geteuid(), ::getegid() },
                                           { sessions.m_cli_app.m_name },
                                           work_dir,
                                           util::Permissions_level::S_USER_ACCESS};
  sessions.m_cli_apps
    = session::Client_app::Master_set{{ sessions.m_cli_app.m_name, sessions.m_cli_app }};
  sessions.m_srv_apps
    = session::Server_app::Master_set{{ sessions.m_srv_app.m_name, sessions.m_srv_app }};

  // Convenience refs into the just-populated, lifetime-stable (see above) members:
  const auto& cli_app = sessions.m_cli_app;
  const auto& srv_app = sessions.m_srv_app;
  const auto& cli_apps = sessions.m_cli_apps;
  const auto& srv_apps = sessions.m_srv_apps;

  // --- Server: start accepting (async_accept is non-blocking). ---
  sessions.m_srv
    = std::make_unique<Session_server_t>(logger, srv_apps.find(srv_app.m_name)->second, cli_apps);

  if constexpr (SHM_TYPE_OR_NONE == session::schema::ShmType::CLASSIC)
  {
    // Only meaningful for SHM-classic; see doc header.
    if (shm_classic_pool_size_limit_mi_or_0 != 0)
    {
      sessions.m_srv->pool_size_limit_mi(shm_classic_pool_size_limit_mi_or_0);
    }
  }

  // `m_srv_session` is a direct default-cted (NULL-state) member; async_accept() populates it in-place.

  Channels srv_channels;
  Error_code accept_err;
  boost::promise<void> accept_done;

  sessions.m_srv->async_accept
    (&sessions.m_srv_session,
     nullptr, // init_channels_by_srv_req (none)
     nullptr, // mdt_from_cli_or_null
     &srv_channels, // init_channels_by_cli_req
     [](auto&&...) { return 0; }, // n_init_channels_by_srv_req_func
     [](auto&&...) {},            // mdt_load_func
     [&](const Error_code& ec) { accept_err = ec; accept_done.set_value(); });

  // --- Client: connect on a worker thread (formally non-blocking). ---
  Channels cli_channels(1); // Pre-sized to 1: request 1 init-channel.
  Error_code cli_connect_err;

  Single_thread_task_loop cli_thread{logger, "cli_connector"};
  cli_thread.start();
  cli_thread.post([&]()
  {
    sessions.m_cli_session = Client_session_t{logger, cli_app, srv_app, [](const Error_code&) {}};
    sessions.m_cli_session.sync_connect
      (sessions.m_cli_session.mdt_builder(),
       &cli_channels, nullptr, nullptr, &cli_connect_err);
  });

  // Wait for async_accept to complete (with timeout to avoid hanging).
  auto accept_fut = accept_done.get_future();
  const auto status = accept_fut.wait_for(seconds{3});
  if (status == boost::future_status::timeout)
  {
    EXPECT_TRUE(false) << "async_accept timed out.";
  }
  else
  {
    accept_fut.get();
    EXPECT_FALSE(accept_err) << "async_accept error: [" << accept_err << "] [" << accept_err.message() << "].";
  }

  // Join client thread.
  cli_thread.stop();
  EXPECT_FALSE(cli_connect_err) << "sync_connect error: "
                                   "[" << cli_connect_err << "] [" << cli_connect_err.message() << "].";

  // Finalize server session (PEER state) unless caller asked to keep it in almost-PEER.
  if (!leave_srv_almost_peer)
  {
    sessions.m_srv_session.init_handlers([](const Error_code&) {});
  }

  // Verify we got channels on both sides.
  EXPECT_EQ(srv_channels.size(), 1) << "Expected 1 init-channel on server side.";
  EXPECT_EQ(cli_channels.size(), 1) << "Expected 1 init-channel on client side.";
  if (!srv_channels.empty())
  {
    result.m_cli_channel = std::move(cli_channels[0]);
    result.m_srv_channel = std::move(srv_channels[0]);
  }

  sessions.m_populated = true; // Full setup complete; enable the STTL rendezvous in ~Session_pair().
  return result;
}

/* Higher-level utility: produce a started struc::Channel pair backed by a full session.
 * Covers the full channel-type matrix: MqType x transmit-native-handles x ShmType (default: no-SHM).
 * However regardless of SHM_TYPE_OR_NONE the `Channel`s produced in this are (1) heap-backed and
 * are (2) async-I/O-pattern variety (as opposed to struc::sync_io::Channel).
 *
 * @todo The produced struc::Channels are always heap-backed (Heap_fixed_builder / Heap_reader), even when
 * SHM_TYPE_OR_NONE is CLASSIC or JEMALLOC.  If/when a test needs SHM-backed struc::Channel serialization,
 * this utility could grow an `if constexpr`-branched path that upgrades to `Session::Structured_channel`
 * via Channel_base::S_SERIALIZE_VIA_SESSION_SHM.  Note that heap-backed is not inherently wrong for
 * SHM-enabled sessions -- for small messages it is often as fast or faster; an adaptive path may eventually
 * exist elsewhere -- so tests that don't specifically care to exercise SHM-backed struc serialization can
 * keep using this utility as-is even with a SHM-enabled session type.
 *
 * @todo Similarly could have a knob to make m_{cli|srv}_channel struc::sync_io::Channel (sync_io-pattern core)
 * instead of struc::Channel (which comes with a background thread + event loop).
 *
 * Also, without the syntactic sugar of getting that with this utility, it is ~straightforward to:
 *   - make_session_channel_pair() to make {Session + unstructured sync_io transport::Channel x 2},
 *     then "manually" turn the latter x 2 into SHM-backed struc::Channel x 2.
 *   - For example:
 *     ~~~
 *     Session::Structured_channel[::Sync_io_obj]<Body>{logger, std::move(pair.m_..._channel),
 *                                                      struc::Channel_base::S_SERIALIZE_VIA_SESSION_SHM,
 *                                                      &pair.m_..._session}; // ... = {cli|srv}.
 *     // Can twiddle other knobs including at least changing to struc::Channel_base::S_SERIALIZE_VIA_APP_SHM.
 *     ~~~
 */
template<typename Message_body,
         session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES,
         session::schema::ShmType SHM_TYPE_OR_NONE = session::schema::ShmType::NONE>
struct Struc_session_pair
{
  using Session_pair_t = Session_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>;
  using Session_pair_ptr_t = Session_pair_ptr<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>;
  using Channel_obj = typename Session_pair_t::Client_session_t::Channel_obj;
  // The async struc::Channel takes (subsumes) a sync_io-pattern transport::Channel directly.
  using Struc_channel_t = Channel<Channel_obj, Message_body,
                                  Heap_fixed_builder::Config, Heap_reader::Config>;

  // Heap-pinned via shared_ptr so the sessions inside have stable addresses.  Must outlive the
  // struc::Channels that wrap the transport channels they produced.
  Session_pair_ptr_t m_sessions;

  std::unique_ptr<Struc_channel_t> m_cli;
  std::unique_ptr<Struc_channel_t> m_srv;
};

template<typename Message_body,
         session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES,
         session::schema::ShmType SHM_TYPE_OR_NONE = session::schema::ShmType::NONE>
Struc_session_pair<Message_body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>
  make_session_struc_pair(std::function<void(const flow::Error_code&)> on_cli_err,
                          std::function<void(const flow::Error_code&)> on_srv_err,
                          flow::log::Logger* logger = nullptr,
                          size_t shm_classic_pool_size_limit_mi_or_0 = 0)
{
  using Result
    = Struc_session_pair<Message_body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>;
  using Struc_channel_t = typename Result::Struc_channel_t;

  auto session_channel_pair
    = make_session_channel_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>
        (logger, false, shm_classic_pool_size_limit_mi_or_0);

  // Compute heap-serialization configs from the sync_io-pattern channel (before moving it).
  const auto builder_config
    = Struc_channel_t::heap_fixed_builder_config(session_channel_pair.m_cli_channel);
  const auto reader_config
    = Struc_channel_t::heap_reader_config(session_channel_pair.m_cli_channel);

  Result result;
  result.m_sessions = std::move(session_channel_pair.m_sessions); // shared_ptr move.

  // The async struc::Channel subsumes the sync_io transport::Channel directly.
  result.m_cli
    = std::make_unique<Struc_channel_t>(logger, std::move(session_channel_pair.m_cli_channel),
                                        builder_config, NULL_SESSION, reader_config,
                                        result.m_sessions->m_cli_session.session_token());
  result.m_srv
    = std::make_unique<Struc_channel_t>(logger, std::move(session_channel_pair.m_srv_channel),
                                        builder_config, NULL_SESSION, reader_config,
                                        result.m_sessions->m_srv_session.session_token());

  result.m_cli->start(std::move(on_cli_err));
  result.m_srv->start(std::move(on_srv_err));

  return result;
}

} // namespace ipc::transport::struc::test
