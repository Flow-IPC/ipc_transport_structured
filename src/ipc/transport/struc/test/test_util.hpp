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
#include "ipc/session/detail/server_session_dtl.hpp"
#include "ipc/session/app.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include "ipc/util/process_credentials.hpp"
#include "ipc/common.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util.hpp>
#include <boost/core/noncopyable.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>
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

/* Utility: establish a session in-process and produce N init-channels per side.
 * Templated on session MqType, transmit-native-handles, and ShmType (defaulting to no-SHM) to cover the
 * full channel-type matrix x the three SHM-provider types (none, SHM-classic, SHM-jemalloc).
 * Outputs vectors of Channel_obj (sync_io-pattern channels) sized per the `n_init_channels` factory arg
 * (default 1; 0 means: skip the init-channels feature, i.e., the resulting vectors are empty).
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
  using Channels = typename Session_server_t::Channels;

  // Mirrors of our template-parameter knobs, for generic code (e.g., typed-test name generators).
  static constexpr session::schema::MqType S_MQ_TYPE_OR_NONE = MQ_TYPE_OR_NONE;
  static constexpr bool S_TRANSMIT_NATIVE_HANDLES = TRANSMIT_NATIVE_HANDLES;
  static constexpr session::schema::ShmType S_SHM_TYPE_OR_NONE = SHM_TYPE_OR_NONE;

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

  /* ~Session -- for the SHM-jemalloc only -- blocks during teardown waiting for the opposing side's
   * ~Session to have *started*: the two sides rendezvous at dtor entry.  In real deployments this is trivially
   * satisfied (each side lives in its own process, hence its own thread(s)).  In an in-process test, though,
   * both sessions are members of the same `Session_pair` and would destruct sequentially on one thread at
   * scope exit: srv dtor would block waiting for cli dtor to start, which cannot happen until srv dtor
   * returns: deadlock.  Fix: move one Session onto a short-lived thread and destroy the other here
   * concurrently.  A no-op-effect for the other SHM-providers, so we do it unconditionally (cheap; saves
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
    destroy_sessions(); // No-op if not populated (factory never completed full setup).

    if (m_srv)
    {
      m_srv.reset(); // Destroy the Session_server; must precede the following.
      remove_server_persistent_bits();
    }
    // else { Factory never got as far as creating the server; the below items don't exist. }
  }

  /* Populates the m_*_app/m_*_apps members: the App universe describing the two sides -- which are the same
   * process; we intentionally "cheat" the identity check (Client_app::m_exec_path is set to our own binary
   * as read from the OS, so the session server's check of /proc/<pid>/cmdline of the connecting process --
   * us again -- passes).  The App names are unique per template instantiation, as each combo is a separate
   * test; and per-Server_app kernel-persistent items (e.g., the CNS (PID) file) have names derived from the
   * Server_app name, so this also keeps such items isolated.  `name_infix`, if given, adds further
   * uniqueness for tests that need several App universes (e.g., one per TEST, isolating their CNS files).
   * It must be alphanumeric: underscore is the logical path separator in Flow-IPC's Shared_name scheme, and
   * Session_server shall reject a name containing it with INVALID_ARGUMENT.
   *
   * Must be called before constructing Session_server/Client_session off these members.  The factories
   * below do so; tests wielding Session_pair internals directly call this themselves. */
  void populate_apps(const std::string& name_infix = std::string{})
  {
    namespace fs = boost::filesystem;
    using flow::util::ostream_op_string;

    /* Discover our own argv[0] so that Client_app::m_exec_path matches
     * the session server's identity check (which reads /proc/<pid>/cmdline of the connecting process). */
    Error_code creds_err;
    const auto self_exe
      = util::Process_credentials::own_process_credentials().process_invoked_as(&creds_err);
    EXPECT_FALSE(creds_err) << "process_invoked_as() error: [" << creds_err << "] ["
                            << creds_err.message() << "].";

    const fs::path work_dir = fs::canonical(fs::current_path().lexically_normal());

    const auto cli_name
      = ostream_op_string("ipcUtCli", name_infix, "Mq", int(MQ_TYPE_OR_NONE),
                          "H", int(TRANSMIT_NATIVE_HANDLES), "Shm", int(SHM_TYPE_OR_NONE));
    const auto srv_name
      = ostream_op_string("ipcUtSrv", name_infix, "Mq", int(MQ_TYPE_OR_NONE),
                          "H", int(TRANSMIT_NATIVE_HANDLES), "Shm", int(SHM_TYPE_OR_NONE));
    m_cli_app = session::Client_app{{ cli_name, self_exe, ::geteuid(), ::getegid() }};
    m_srv_app = session::Server_app{{ srv_name, self_exe, ::geteuid(), ::getegid() },
                                    { m_cli_app.m_name },
                                    work_dir,
                                    util::Permissions_level::S_USER_ACCESS};
    m_cli_apps = session::Client_app::Master_set{{ m_cli_app.m_name, m_cli_app }};
    m_srv_apps = session::Server_app::Master_set{{ m_srv_app.m_name, m_srv_app }};
  } // populate_apps()

  /* Computes the absolute path of the CNS (PID) file that a Session_server for m_srv_app creates (or
   * overwrites in place) when it runs.  The location/name is computed by the same code that computes it in
   * production -- via the short-lived-dummy-Session idiom that Session_server_impl itself uses for the same
   * purpose -- so this cannot go stale versus naming-scheme changes.  Pre-condition: populate_apps() ran. */
  boost::filesystem::path cns_path()
  {
    using Server_session_dtl = session::Server_session_dtl<Server_session_t>;

    auto empty_session_public
      = Server_session_dtl::ct_base(nullptr, m_srv_app, transport::sync_io::Native_socket_stream{});
    return Server_session_dtl{ empty_session_public }.base().cur_ns_store_absolute_path();
  }

  /// As cns_path() but yields the shared name of the CNS file's inter-process mutex.
  util::Shared_name cns_mutex_absolute_name()
  {
    using Server_session_dtl = session::Server_session_dtl<Server_session_t>;

    auto empty_session_public
      = Server_session_dtl::ct_base(nullptr, m_srv_app, transport::sync_io::Native_socket_stream{});
    return Server_session_dtl{ empty_session_public }.base().cur_ns_store_mutex_absolute_name();
  }

  /* Removes the two kernel-persistent items the Session_server has left behind by design: the CNS (PID) file
   * and its inter-process mutex.  In production these are left around deliberately: they are per-Server_app
   * (not per-server-instance), so there is exactly one of each, overwritten/reused in place by each successive
   * server instance; and deleting the mutex in particular would be racy against a concurrent client's open.
   * Neither consideration applies here-and-now (the server was just destroyed; any clients are long gone),
   * while the tests-leave-a-clean-slate ideal does; so out they go.
   *
   * With `expect_existence` (the default) both items must exist (the server ran, hence created them); test
   * failure otherwise.  Pass `false` to instead quietly remove-if-present: e.g., as pre-test cleanup of a
   * possible previous run's leavings. */
  void remove_server_persistent_bits(bool expect_existence = true)
  {
    namespace fs = boost::filesystem;

    const auto cns_file_path = cns_path();
    const auto mutex_name = cns_mutex_absolute_name();

    Error_code sink; // Distinguish did-not-exist (fs::remove => false) from I/O error (throw).
    if ((!fs::remove(cns_file_path, sink)) && expect_existence)
    {
      ADD_FAILURE() << "Expected CNS (PID) file [" << cns_file_path << "] to exist (Session_server ran and "
                       "created it) and be remove()able; it was not (error if any: [" << sink << "] ["
                    << sink.message() << "]).  Naming-scheme drift versus production?";
    }
    if ((!bipc::named_mutex::remove(mutex_name.native_str())) && expect_existence)
    {
      ADD_FAILURE() << "Expected CNS mutex [" << mutex_name << "] to exist (Session_server ran and created it) "
                       "and be remove()able; it was not.  Naming-scheme drift versus production?";
    }
  } // remove_server_persistent_bits()

  /* Destroys both sessions -- but not the Session_server (m_srv), the Apps, or `*this`: afterward the pair
   * is back in its pre-connect_sessions() state, and connect_sessions() may be called again.  That is how a
   * test exercises session-death-then-reconnect against one Session_server (which keeps listening
   * throughout).  No-op unless sessions are currently populated. */
  void destroy_sessions()
  {
    if (!m_populated)
    {
      return;
    }
    // else
    m_populated = false;
    destroy_sessions(&m_cli_session, &m_srv_session);
  }

  /* As destroy_sessions() but destroys a caller-owned session pair -- one made by the slot-taking
   * connect_sessions() overload; the member sessions and m_populated are not involved.  (The pair must be in
   * PEER state; both are left in NULL state.) */
  void destroy_sessions(Client_session_t* cli_session, Server_session_t* srv_session)
  {
    flow::async::Single_thread_task_loop srv_dtor_thread{nullptr, "pair_dtor_srv"};
    srv_dtor_thread.start();
    srv_dtor_thread.post
      ([srv_sess = boost::make_shared<Server_session_t>(std::move(*srv_session))]() mutable
       { srv_sess.reset(); },
       Synchronicity::S_ASYNC_AND_AWAIT_CONCURRENT_START); // Ensure task spinning before we proceed.
    // (SHM-jemalloc only) srv_sess.reset() => Server_session dtor waits for cli session dtor to begin.

    *cli_session = {}; // Move-assign default-cted temp -> destroys PEER-state cli session now (in-thread).
    /* (SHM-jemalloc only) Client_session dtor waits for srv session dtor to begin.  It has.  They both
     * proceed.  Now srv_dtor_thread thread is joined. */
    srv_dtor_thread.stop();
  }

  /* Establishes the session pair (both sides to PEER state) against m_srv -- which must exist and be
   * listening: the client constructs + sync_connect()s on a short-lived thread; the server async_accept()s
   * into m_srv_session, in-place.  Callable on a fresh pair (make_session_channel_pair() does exactly that)
   * or following destroy_sessions() -- the reconnect path: same Session_server, same Apps, new sessions.
   *
   * `n_init_channels != 0` requests that many init-channels per side; they land in the given out-vectors
   * (each then required non-null; with 0, nulls are fine).  `leave_srv_almost_peer` skips the server-side
   * init_handlers() call; see make_session_channel_pair() doc for that knob's use case. */
  void connect_sessions(flow::log::Logger* logger, size_t n_init_channels = 0,
                        Channels* cli_channels_or_null = nullptr, Channels* srv_channels_or_null = nullptr,
                        bool leave_srv_almost_peer = false)
  {
    assert(!m_populated && "connect_sessions() requires unpopulated sessions (fresh, or destroy_sessions()ed).");
    connect_sessions(logger, &m_cli_session, &m_srv_session,
                     n_init_channels, cli_channels_or_null, srv_channels_or_null, leave_srv_almost_peer);
    m_populated = true; // Full setup complete; enable the STTL rendezvous in destroy_sessions()/dtor.
  }

  /* As connect_sessions() but establishes the session pair into the given caller-owned NULL-state slots
   * instead of the member ones -- for a session pair *concurrent* with the member pair, against the same
   * Session_server (e.g., an arena borrowed via 2+ live sessions at once; session rotation with overlap).
   * Caller owns teardown: use the slot-taking destroy_sessions() overload (member m_populated is not
   * involved either way). */
  void connect_sessions(flow::log::Logger* logger, Client_session_t* cli_session, Server_session_t* srv_session,
                        size_t n_init_channels = 0,
                        Channels* cli_channels_or_null = nullptr, Channels* srv_channels_or_null = nullptr,
                        bool leave_srv_almost_peer = false)
  {
    using boost::chrono::seconds;
    using flow::async::Single_thread_task_loop;

    assert(m_srv && "connect_sessions() requires a constructed (listening) Session_server.");
    assert(((n_init_channels == 0) || (cli_channels_or_null && srv_channels_or_null))
           && "Requesting init-channels requires out-vectors for them.");

    /* `n_init_channels == 0` opts out of the init-channels feature entirely (pass null to both sides);
     * any non-zero value pre-sizes the client-side request vector and is verified post-handshake. */
    Error_code accept_err;
    boost::promise<void> accept_done;

    m_srv->async_accept
      (srv_session,
       nullptr, // init_channels_by_srv_req (none)
       nullptr, // mdt_from_cli_or_null
       (n_init_channels == 0) ? nullptr : srv_channels_or_null, // init_channels_by_cli_req
       [](auto&&...) { return 0; }, // n_init_channels_by_srv_req_func
       [](auto&&...) {},            // mdt_load_func
       [&](const Error_code& ec) { accept_err = ec; accept_done.set_value(); });

    // --- Client: connect on a worker thread (formally non-blocking). ---
    if (n_init_channels != 0)
    {
      cli_channels_or_null->resize(n_init_channels); // Pre-sized request vector.
    }
    Error_code cli_connect_err;

    Single_thread_task_loop cli_thread{logger, "cli_connector"};
    cli_thread.start();
    cli_thread.post([&]()
    {
      *cli_session = Client_session_t{logger, m_cli_app, m_srv_app, [](const Error_code&) {}};
      cli_session->sync_connect
        (cli_session->mdt_builder(),
         (n_init_channels == 0) ? nullptr : cli_channels_or_null, nullptr, nullptr, &cli_connect_err);
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
      srv_session->init_handlers([](const Error_code&) {});
    }

    // Verify we got the requested number of channels on both sides.
    if (n_init_channels != 0)
    {
      EXPECT_EQ(srv_channels_or_null->size(), n_init_channels) << "Server side init-channel count mismatch.";
      EXPECT_EQ(cli_channels_or_null->size(), n_init_channels) << "Client side init-channel count mismatch.";
    }
  } // connect_sessions()
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

  /* Init-channels established at session-open time; sized per the `n_init_channels` arg to
   * make_session_channel_pair() (default 1).  When `n_init_channels == 0`, both vectors are empty. */
  std::vector<Channel_obj> m_cli_channels;
  std::vector<Channel_obj> m_srv_channels;
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
 * the pool for a safety-check test).  0 means "leave default."  No-op for SHM-jemalloc / SHM-none.
 *
 * The `n_init_channels` knob controls how many init-channels are opened per side at session-open
 * time.  Default 1.  0 means: do not use the init-channels feature at all (pass null to
 * sync_connect()/async_accept()); the resulting `m_{cli|srv}_channels` are empty.  Use 0 only
 * if your test consumes the sessions and not the channels.  (@todo If/when needed, extend this
 * utility to also configure passive-open handlers, so a 0-init-channels test can use
 * Session::open_channel() to add channels on demand.) */
template<session::schema::MqType MQ_TYPE_OR_NONE, bool TRANSMIT_NATIVE_HANDLES,
         session::schema::ShmType SHM_TYPE_OR_NONE = session::schema::ShmType::NONE>
Session_channel_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>
  make_session_channel_pair(flow::log::Logger* logger = nullptr,
                            bool leave_srv_almost_peer = false,
                            [[maybe_unused]] size_t shm_classic_pool_size_limit_mi_or_0 = 0,
                            size_t n_init_channels = 1)
{
  /* @todo Since it's intended as a utility rather than just one thing inside one test, perhaps we should be
   * just a *bit* more careful about handling failed EXPECT()s below.  We can't just use ASSERT()s here, as we aren't
   * void, but we could wrap it in a void local lambda or something and then return null on any ASSERT() fail in that
   * lambda. Don't get us wrong; we don't expect test code to be perfectly resilient to failures; in practice
   * one fixes the first failure and re-runs -- what happens afterward hardly matters... but as a utility perhaps
   * we should be a *bit* more robust is all. / Same for make_session_struc_pair(). */

  using Result = Session_channel_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>;
  using Session_pair_t = typename Result::Session_pair_t;
  using Session_server_t = typename Session_pair_t::Session_server_t;
  using Channels = typename Session_server_t::Channels;

  /* Allocate Session_pair on the heap so the App members below have stable addresses for the whole lifetime
   * of the returned object.  (Session_server and Client_session store them by *address* per the Flow-IPC App
   * lifetime contract; function-locals would dangle the moment this function returns.) */
  Result result;
  result.m_sessions = boost::make_shared<Session_pair_t>();
  auto& sessions = *result.m_sessions;

  sessions.populate_apps(); // App descriptors; see its doc for the identity-check "cheat" and naming.

  // Convenience refs into the just-populated, lifetime-stable (see above) members:
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

  /* The connect/accept rendezvous itself lives on Session_pair, so that tests can re-run it (after
   * Session_pair::destroy_sessions()) against the same still-listening Session_server. */
  Channels srv_channels;
  Channels cli_channels;
  sessions.connect_sessions(logger, n_init_channels, &cli_channels, &srv_channels, leave_srv_almost_peer);
  result.m_cli_channels = std::move(cli_channels);
  result.m_srv_channels = std::move(srv_channels);
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
  make_session_struc_pair(std::function<void(const Error_code&)> on_cli_err,
                          std::function<void(const Error_code&)> on_srv_err,
                          flow::log::Logger* logger = nullptr,
                          size_t shm_classic_pool_size_limit_mi_or_0 = 0)
{
  using Result
    = Struc_session_pair<Message_body, MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>;
  using Struc_channel_t = typename Result::Struc_channel_t;

  // make_session_struc_pair() always uses exactly 1 init-channel per side; the vectors are unwrapped below.
  auto session_channel_pair
    = make_session_channel_pair<MQ_TYPE_OR_NONE, TRANSMIT_NATIVE_HANDLES, SHM_TYPE_OR_NONE>
        (logger, false, shm_classic_pool_size_limit_mi_or_0, 1);

  // Compute heap-serialization configs from the sync_io-pattern channel (before moving it).
  const auto builder_config
    = Struc_channel_t::heap_fixed_builder_config(session_channel_pair.m_cli_channels.front());
  const auto reader_config
    = Struc_channel_t::heap_reader_config(session_channel_pair.m_cli_channels.front());

  Result result;
  result.m_sessions = std::move(session_channel_pair.m_sessions); // shared_ptr move.

  // The async struc::Channel subsumes the sync_io transport::Channel directly.
  result.m_cli
    = std::make_unique<Struc_channel_t>(logger, std::move(session_channel_pair.m_cli_channels.front()),
                                        builder_config, NULL_SESSION, reader_config,
                                        result.m_sessions->m_cli_session.session_token());
  result.m_srv
    = std::make_unique<Struc_channel_t>(logger, std::move(session_channel_pair.m_srv_channels.front()),
                                        builder_config, NULL_SESSION, reader_config,
                                        result.m_sessions->m_srv_session.session_token());

  result.m_cli->start(std::move(on_cli_err));
  result.m_srv->start(std::move(on_srv_err));

  return result;
}

} // namespace ipc::transport::struc::test
