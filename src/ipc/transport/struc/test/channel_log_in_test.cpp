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

#include "ipc/transport/struc/channel.hpp"
#include "ipc/transport/struc/test/test_schema.capnp.h"
#include "ipc/transport/channel.hpp"
#include "ipc/transport/sync_io/native_socket_stream.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/test/test_common_util.hpp>
#include <flow/util/util.hpp>
#include <gtest/gtest.h>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/thread/future.hpp>
#include <boost/uuid/random_generator.hpp>
#include <optional>
#include <utility>

/* The struc::Channel log-in phase: the rigid little protocol by which a session master channel starts, wherein
 * the server side expects exactly 1 log-in request and answers it, the client side sends exactly 1 log-in request
 * and awaits the response, and only then does the general API open up.  In production only ipc::session drives
 * it (privately, inside its master channel a/k/a SMC), always in the one proper order; so here we drive both
 * roles by hand over a plain socket pair: the happy path in both orderings, every gate on the general API while
 * logging in (each refused call followed by the proper call succeeding), and the Channel-hosing corners reachable
 * with a valid opposing peer.
 *
 * Reminder: Flow-IPC assumes trust in the validity of the opposing peer's software: it's not malicious or blatantly
 * buggy.  There are nevertheless various checks about proper behavior from the opposing peer (in the actual code,
 * that is; not speaking here of tests), but this is for safety rather than security: to catch our own bugs, if any,
 * in civilized fashion. */

namespace ipc::transport::struc::test
{

namespace
{

using Sio_channel = Socket_stream_channel<true>;
using Struc_channel = Channel<Sio_channel, Body, Heap_fixed_builder::Config, Heap_reader::Config>;
using Msg_out = Struc_channel::Msg_out;
using Msg_in_ptr = Struc_channel::Msg_in_ptr;
using boost::promise;
using boost::unique_future;
using boost::chrono::milliseconds;
using flow::util::this_thread::sleep_for;
using std::pair;
using std::make_pair;
using std::optional;
using std::nullopt;

// Logger for the objects under test: null (chatty).  Flip to console when a failing test needs their logs.
flow::log::Logger* obj_logger()
{
#if 1
  return nullptr;
#else
  static ipc::test::Test_logger s_logger;
  return &s_logger;
#endif
}

// Two sync_io transport::Channels (handles pipe over a socket pair), in PEER state.
pair<Sio_channel, Sio_channel> make_sio_channel_pair()
{
  using Peer_socket = Native_socket_stream_cfg::Protocol::socket;
  using Sio_nss = transport::sync_io::Native_socket_stream;

  flow::util::Task_engine io;
  Peer_socket sock_a{io};
  Peer_socket sock_b{io};
  boost::asio::local::connect_pair(sock_a, sock_b);

  return { Sio_channel{obj_logger(), "cli", Sio_nss{obj_logger(), "cliSock", Native_handle{sock_a.release()}}},
           Sio_channel{obj_logger(), "srv", Sio_nss{obj_logger(), "srvSock", Native_handle{sock_b.release()}}} };
}

/* A struc::Channel in log-in phase in the given role; or, if `is_server` is null, one that is already logged-in
 * (with an arbitrary session token), which is how we get a well-formed peer to misbehave protocol-wise
 * at the other end. */
Struc_channel make_struc_channel(Sio_channel&& sio_channel, optional<bool> is_server)
{
  if (is_server)
  {
    return Struc_channel{obj_logger(), std::move(sio_channel), Channel_base::S_SERIALIZE_VIA_HEAP, *is_server};
  }
  // else
  return Struc_channel{obj_logger(), std::move(sio_channel), Channel_base::S_SERIALIZE_VIA_HEAP,
                       boost::uuids::random_generator()()};
}

// One struc::Channel peer, started, plus what its on-error handler reported if anything.
struct Peer
{
  explicit Peer(Sio_channel&& sio_channel, optional<bool> is_server) :
    m_err_future(m_err_promise.get_future()),
    m_channel(make_struc_channel(std::move(sio_channel), is_server))
  {
    EXPECT_TRUE(m_channel.start([this](const Error_code& err_code) { m_err_promise.set_value(err_code); }));
  }

  // Waits for the channel to have hosed; returns the reported error.
  Error_code hosed_with()
  {
    return m_err_future.get();
  }

  // For checks that the on-error handler did *not* fire (the channel is fine).
  bool hosed() const
  {
    return m_err_future.is_ready();
  }

  Msg_out make_req(uint64_t val)
  {
    auto msg = m_channel.create_msg();
    msg.body_root()->initCoolReq().setCoolVal(val);
    return msg;
  }

  Msg_out make_rsp(uint64_t val)
  {
    auto msg = m_channel.create_msg();
    msg.body_root()->initCoolRsp().setCoolVal(val);
    return msg;
  }

  // Waits until an unsolicited in-message has been cached: the log-in request arrived before expect_log_in_request().
  void await_cached_log_in_request() const
  {
    for (unsigned int n = 0; m_channel.stats().m_core.m_rcv.m_unsolicited_msgs_cached == 0; ++n)
    {
      ASSERT_LT(n, 500u) << "Log-in request never arrived (5 seconds).";
      sleep_for(milliseconds{10});
    }
  }

  // (Declared ahead of m_channel: the on-error handler may fire, on the channel's thread, up to its destruction.)
  promise<Error_code> m_err_promise;
  unique_future<Error_code> m_err_future;
  Struc_channel m_channel;
};

// Client and server peers in log-in phase (or a logged-in stand-in on either side; see make_struc_channel()).
struct Peers
{
  explicit Peers(optional<bool> cli_is_server = false, optional<bool> srv_is_server = true) :
    m_sio_channels(make_sio_channel_pair()),
    m_cli(std::move(m_sio_channels.first), cli_is_server),
    m_srv(std::move(m_sio_channels.second), srv_is_server)
  {
    // A session token is visible only once logged-in; hence for now only in a logged-in stand-in.
    EXPECT_EQ(m_cli.m_channel.session_token().is_nil(), cli_is_server.has_value());
    EXPECT_EQ(m_srv.m_channel.session_token().is_nil(), srv_is_server.has_value());
  }

  // The proper client step: send the log-in request; returns the future for the response.
  unique_future<Msg_in_ptr> cli_log_in()
  {
    m_cli_rsp.emplace();
    auto req = m_cli.make_req(1);
    EXPECT_TRUE(m_cli.m_channel.async_request(&req, nullptr, nullptr,
                                              [this](Msg_in_ptr&& rsp) { m_cli_rsp->set_value(std::move(rsp)); }));
    return m_cli_rsp->get_future();
  }

  // The proper server step: expect the log-in request; returns the future for it.
  unique_future<Msg_in_ptr> srv_expect_log_in(Struc_channel::Msg_which_in which = Body::COOL_REQ)
  {
    m_srv_req.emplace();
    EXPECT_TRUE(m_srv.m_channel.expect_log_in_request(which, [this](Msg_in_ptr&& req)
                                                                { m_srv_req->set_value(std::move(req)); }));
    return m_srv_req->get_future();
  }

  // Once both sides are logged in: general traffic works in both directions.
  void expect_logged_in_traffic()
  {
    for (const auto& [requester, responder] : { make_pair(&m_cli, &m_srv), make_pair(&m_srv, &m_cli) })
    {
      EXPECT_TRUE(responder->m_channel.expect_msg(Body::COOL_REQ, [responder](Msg_in_ptr&& req)
      {
        auto rsp = responder->make_rsp(req->body_root().getCoolReq().getCoolVal() + 1);
        responder->m_channel.send(&rsp, req.get());
      }));
      auto req = requester->make_req(100);
      Error_code err_code;
      const auto rsp = requester->m_channel.sync_request(&req, nullptr, &err_code);
      EXPECT_FALSE(err_code) << err_code.message();
      ASSERT_TRUE(rsp);
      EXPECT_EQ(rsp->body_root().getCoolRsp().getCoolVal(), 101u);
    }
    EXPECT_FALSE(m_cli.hosed());
    EXPECT_FALSE(m_srv.hosed());
  }

  pair<Sio_channel, Sio_channel> m_sio_channels; // (Moved-from once m_cli/m_srv are constructed.)
  Peer m_cli;
  Peer m_srv;
  optional<promise<Msg_in_ptr>> m_cli_rsp;
  optional<promise<Msg_in_ptr>> m_srv_req;
}; // struct Peers

} // namespace (anon)

// The proper sequence, server expecting first: tokens null during log-in, equal and non-nil after; then traffic.
TEST(Struc_channel_log_in_test, happy_path_expect_first)
{
  Peers peers;
  auto srv_req = peers.srv_expect_log_in();
  EXPECT_FALSE(peers.m_srv.m_channel.expect_log_in_request(Body::COOL_REQ, [](Msg_in_ptr&&) {})); // Once only.
  auto cli_rsp = peers.cli_log_in();

  const auto req = srv_req.get();
  EXPECT_EQ(req->body_root().getCoolReq().getCoolVal(), 1u);
  EXPECT_TRUE(peers.m_srv.m_channel.session_token().is_nil()); // Not until the response is sent.
  auto rsp = peers.m_srv.make_rsp(2);
  EXPECT_TRUE(peers.m_srv.m_channel.send(&rsp, req.get()));
  const auto srv_token = peers.m_srv.m_channel.session_token();
  EXPECT_FALSE(srv_token.is_nil()); // Logged in: the token the server generated is now visible.

  EXPECT_EQ(cli_rsp.get()->body_root().getCoolRsp().getCoolVal(), 2u);
  EXPECT_EQ(peers.m_cli.m_channel.session_token(), srv_token); // And the client learned it from the response.

  peers.expect_logged_in_traffic();
}

// Ditto but the request arrives before the server expects it: the cached-request path.
TEST(Struc_channel_log_in_test, happy_path_request_first)
{
  Peers peers;
  auto cli_rsp = peers.cli_log_in();
  peers.m_srv.await_cached_log_in_request();

  auto srv_req = peers.srv_expect_log_in();
  const auto req = srv_req.get();
  auto rsp = peers.m_srv.make_rsp(2);
  EXPECT_TRUE(peers.m_srv.m_channel.send(&rsp, req.get()));
  EXPECT_EQ(cli_rsp.get()->body_root().getCoolRsp().getCoolVal(), 2u);
  EXPECT_EQ(peers.m_cli.m_channel.session_token(), peers.m_srv.m_channel.session_token());

  peers.expect_logged_in_traffic();
}

/* Every general-API call refused (no-op, `false`) while logging in, on each side; and after each refused misuse of
 * the one permitted operation, the proper form of it still succeeds. */
TEST(Struc_channel_log_in_test, gates_while_logging_in)
{
  Peers peers;
  auto& cli = peers.m_cli.m_channel;
  auto& srv = peers.m_srv.m_channel;
  const auto no_op = [](Msg_in_ptr&&) {};
  Error_code err_code;

  {
    FLOW_TEST_TRACE_CTX("Server side.");
    EXPECT_FALSE(srv.expect_msg(Body::COOL_REQ, no_op));
    EXPECT_FALSE(srv.expect_msgs(Body::COOL_REQ, no_op));
    EXPECT_FALSE(srv.undo_expect_msgs(Body::COOL_REQ));
    EXPECT_FALSE(srv.undo_expect_responses(1));
    EXPECT_TRUE(srv.session_token().is_nil());
    // The log-in response must be a response: an unsolicited message is refused, without hosing anything.
    auto rsp = peers.m_srv.make_rsp(2);
    EXPECT_FALSE(srv.send(&rsp, nullptr, &err_code));
    EXPECT_FALSE(err_code);
    EXPECT_FALSE(srv.async_request(&rsp, nullptr, nullptr, no_op, &err_code));
    EXPECT_FALSE(err_code);
    EXPECT_FALSE(peers.m_srv.hosed());
  }
  {
    FLOW_TEST_TRACE_CTX("Client side.");
    EXPECT_FALSE(cli.expect_log_in_request(Body::COOL_REQ, no_op)); // Wrong role.
    EXPECT_FALSE(cli.expect_msg(Body::COOL_RSP, no_op));
    EXPECT_FALSE(cli.expect_msgs(Body::COOL_RSP, no_op));
    EXPECT_FALSE(cli.undo_expect_msgs(Body::COOL_RSP));
    EXPECT_FALSE(cli.undo_expect_responses(1));
    EXPECT_TRUE(cli.session_token().is_nil());
    // The log-in request must be a request: a plain send() (no response handler) is refused.
    auto req = peers.m_cli.make_req(1);
    EXPECT_FALSE(cli.send(&req, nullptr, &err_code));
    EXPECT_FALSE(err_code);
    EXPECT_FALSE(peers.m_cli.hosed());
  }

  // The proper calls still work after all that.  (Server first, so that the request is expected on arrival.)
  auto srv_req = peers.srv_expect_log_in();
  auto cli_rsp = peers.cli_log_in();
  // At most one log-in request: a 2nd one (even properly formed) is refused while the 1st is pending.
  {
    auto req = peers.m_cli.make_req(3);
    EXPECT_FALSE(cli.async_request(&req, nullptr, nullptr, no_op, &err_code));
    EXPECT_FALSE(err_code);
  }
  const auto req = srv_req.get();
  auto rsp = peers.m_srv.make_rsp(2);
  EXPECT_TRUE(srv.send(&rsp, req.get()));
  EXPECT_EQ(cli_rsp.get()->body_root().getCoolRsp().getCoolVal(), 2u);
  peers.expect_logged_in_traffic();
} // TEST(Struc_channel_log_in_test, gates_while_logging_in)

/* The two sides disagree on the log-in request type (server expects CoolReq; client's request is a CoolRsp):
 * the server hoses with S_STRUCT_CHANNEL_GOT_UNEXPECTED_LOG_IN_REQUEST, whether the expectation was registered
 * before the request arrived or after (then expect_log_in_request() returns `true` and hoses asynchronously). */
TEST(Struc_channel_log_in_test, wrong_log_in_request_type)
{
  for (const bool expect_first : {true, false})
  {
    FLOW_TEST_TRACE_CTX("Server expects before the request arrives? = [", expect_first, "].");
    Peers peers;
    unique_future<Msg_in_ptr> srv_req;
    if (expect_first)
    {
      srv_req = peers.srv_expect_log_in(Body::COOL_REQ);
    }
    auto req = peers.m_cli.make_rsp(1); // The "wrong" type of log-in request, from the server's point of view.
    EXPECT_TRUE(peers.m_cli.m_channel.async_request(&req, nullptr, nullptr, [](Msg_in_ptr&&)
    {
      ADD_FAILURE() << "The log-in response must never arrive: the server hoses instead.";
    }));
    if (!expect_first)
    {
      peers.m_srv.await_cached_log_in_request();
      srv_req = peers.srv_expect_log_in(Body::COOL_REQ);
    }

    EXPECT_EQ(peers.m_srv.hosed_with(), error::Code::S_STRUCT_CHANNEL_GOT_UNEXPECTED_LOG_IN_REQUEST);
    EXPECT_FALSE(srv_req.is_ready()); // The request handler never fires.
    EXPECT_FALSE(peers.m_srv.m_channel.expect_log_in_request(Body::COOL_RSP, [](Msg_in_ptr&&) {})); // Hosed now.
    EXPECT_FALSE(peers.m_cli.hosed()); // Only the server detected anything; the client is merely never answered.
  }
}

/* A well-formed but protocol-misbehaving opposing peer: one that is already logged in (with some session token)
 * and simply sends a message.  To a logging-in client, which has not yet sent its log-in request, no in-message is
 * acceptable; to a server awaiting the log-in request, the message carries a non-nil session token.  Either way:
 * S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA. */
TEST(Struc_channel_log_in_test, misbehaving_peer)
{
  {
    FLOW_TEST_TRACE_CTX("Logging-in client versus logged-in peer.");
    Peers peers{false, nullopt};
    /* The client must not send its log-in request here: it would bear a nil session token, so the logged-in
     * stand-in would fail auth on it and hose itself first.  So the client has no request outstanding; any
     * in-message at all is then out of place. */
    auto msg = peers.m_srv.make_rsp(2);
    EXPECT_TRUE(peers.m_srv.m_channel.send(&msg));
    EXPECT_EQ(peers.m_cli.hosed_with(), error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA);
  }
  {
    FLOW_TEST_TRACE_CTX("Logging-in server versus logged-in peer.");
    Peers peers{nullopt, true};
    peers.srv_expect_log_in();
    auto msg = peers.m_cli.make_req(1);
    EXPECT_TRUE(peers.m_cli.m_channel.send(&msg)); // Looks like a log-in request, but bears a session token.
    EXPECT_EQ(peers.m_srv.hosed_with(), error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA);
  }
}

/* Once logged in, every in-message's session token is checked against the one established during log-in;
 * two logged-in peers with different tokens -- here, two stand-ins, each with its own random token -- each hose
 * on the other's first message with S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_BAD_AUTH.  And the check precedes
 * deserializing the message's body: the body never counts as received. */
TEST(Struc_channel_log_in_test, session_token_mismatch)
{
  Peers peers{nullopt, nullopt};
  EXPECT_NE(peers.m_cli.m_channel.session_token(), peers.m_srv.m_channel.session_token());

  auto msg = peers.m_cli.make_req(1);
  EXPECT_TRUE(peers.m_cli.m_channel.send(&msg));
  EXPECT_EQ(peers.m_srv.hosed_with(), error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_BAD_AUTH);
  EXPECT_EQ(peers.m_srv.m_channel.stats().m_core.m_rcv.m_msg.m_user_msgs, 0u);
  EXPECT_FALSE(peers.m_cli.hosed()); // Nothing came its way.
}

} // namespace ipc::transport::struc::test
