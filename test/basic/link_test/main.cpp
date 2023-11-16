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

#include <ipc/transport/struc/channel.hpp>
#include <flow/log/simple_ostream_logger.hpp>
#include <flow/log/async_file_logger.hpp>
#include "schema.capnp.h"

/* This little thing is *not* a unit-test; it is built to ensure the proper stuff links through our
 * build process.  We try to use a compiled thing or two; and a template (header-only) thing or two;
 * not so much for correctness testing but to see it build successfully and run without barfing. */
int main()
{
  using ipc::util::Shared_name;
  using ipc::util::Blob_const;
  using ipc::util::Blob_mutable;
  using ipc::transport::Native_socket_stream;
  using Socket_stream_channel = ipc::transport::Socket_stream_channel_of_blobs<true>;
  using ipc::transport::struc::Channel;
  using Structured_channel
    = Channel<Socket_stream_channel, link_test::FunBody,
              ipc::transport::struc::Heap_fixed_builder::Config,
              ipc::transport::struc::Heap_reader::Config>;
  namespace asio_local = ipc::transport::asio_local_stream_socket::local_ns;
  using asio_local::connect_pair;
  using ipc::transport::asio_local_stream_socket::Peer_socket;
  using ipc::util::Native_handle;

  using flow::async::Single_thread_task_loop;
  using flow::log::Simple_ostream_logger;
  using flow::log::Async_file_logger;
  using flow::log::Config;
  using flow::log::Sev;
  using flow::Error_code;
  using flow::Flow_log_component;

  using std::string;
  using std::exception;

  const string LOG_FILE = "ipc_transport_structured_link_test.log";
  const int BAD_EXIT = 1;
  const uint64_t TEST_VAL = 42;

  /* Set up logging within this function.  We could easily just use `cout` and `cerr` instead, but this
   * Flow stuff will give us time stamps and such for free, so why not?  Normally, one derives from
   * Log_context to do this very trivially, but we just have the one function, main(), so far so: */
  Config std_log_config;
  std_log_config.init_component_to_union_idx_mapping<Flow_log_component>(1000, 999);
  std_log_config.init_component_names<Flow_log_component>(flow::S_FLOW_LOG_COMPONENT_NAME_MAP, false, "link_test-");

  Simple_ostream_logger std_logger(&std_log_config);
  FLOW_LOG_SET_CONTEXT(&std_logger, Flow_log_component::S_UNCAT);

  // This is separate: the IPC/Flow logging will go into this file.
  FLOW_LOG_INFO("Opening log file [" << LOG_FILE << "] for IPC/Flow logs only.");
  Config log_config = std_log_config;
  log_config.configure_default_verbosity(Sev::S_INFO, true);
  /* First arg: could use &std_logger to log-about-logging to console; but it's a bit heavy for such a console-dependent
   * little program.  Just just send it to /dev/null metaphorically speaking. */
  Async_file_logger log_logger(nullptr, &log_config, LOG_FILE, false /* No rotation; we're no serious business. */);

  try
  {
    /* Use the template ipc::transport::struc::Channel and some other peripheral things.
     * As a reminder we're not trying to demo the library here; just to access certain things -- probably
     * most users would do something higher-level and more impressive than this.  We're ensuring stuff built OK
     * more or less.  In particular we don't have ipc::session available -- that's in ipc_session (which depends
     * on us in fact) -- so opening a channel is slightly painful by our standards.  (Nothing a sans-Flow-IPC
     * IPC coder would be surprised at having to do though.) */
    auto sock_name = Shared_name::ct("/cool_sock");
    sock_name.sanitize();

    // Make the 2 socket-stream peers from a nice little socketpair().
    flow::util::Task_engine dummy;
    Peer_socket snd_hndl_asio(dummy);
    Peer_socket rcv_hndl_asio(std::move(snd_hndl_asio));
    connect_pair(snd_hndl_asio, rcv_hndl_asio);
    Native_socket_stream snd_stream(&log_logger, "snd", Native_handle(snd_hndl_asio.release()));
    Native_socket_stream rcv_stream(&log_logger, "rcv", Native_handle(rcv_hndl_asio.release()));

    // Wrap each in transport::Channel.
    Socket_stream_channel snd_chan_raw(&log_logger, "snd", snd_stream.release());
    Socket_stream_channel rcv_chan_raw(&log_logger, "rcv", rcv_stream.release());

    // Then wrap each of those in transport::struc::Channel (capable of structured messaging).
    const auto builder_config = Structured_channel::heap_fixed_builder_config(snd_chan_raw);
    const auto reader_config = Structured_channel::heap_reader_config(snd_chan_raw);
    const auto token = boost::uuids::random_generator()();
    Structured_channel snd_chan(&log_logger, std::move(snd_chan_raw), builder_config,
                                ipc::transport::struc::NULL_SESSION, reader_config, token);
    Structured_channel rcv_chan(&log_logger, std::move(rcv_chan_raw), builder_config,
                                ipc::transport::struc::NULL_SESSION, reader_config, token);

    // We'll need a little thread/loop in which to do stuff.
    Single_thread_task_loop loop(&log_logger, "loop");
    loop.start();

    const auto on_err_func = [](const Error_code&) {};
    snd_chan.start(on_err_func);
    rcv_chan.start(on_err_func);

    // Channel 2 will await request and reply.
    rcv_chan.expect_msg(link_test::FunBody::COOL_REQ, [&](auto&& req)
    {
      loop.post([&, req]()
      {
        const auto test_val = req->body_root().getCoolReq().getCoolVal();
        FLOW_LOG_INFO("Channel 2 received structured request containing value [" << test_val << "]; "
                      "echoing it back to channel 1.");

        auto rsp = rcv_chan.create_msg();
        rsp.body_root()->initCoolRsp().setCoolVal(test_val);
        rcv_chan.send(rsp, req.get());
      });
    });

    FLOW_LOG_INFO("Channel 1 sending structured request containing value [" << TEST_VAL << "]; "
                  "will await response echoing it back at us.");

    auto req = snd_chan.create_msg();
    req.body_root()->initCoolReq().setCoolVal(TEST_VAL);
    auto rsp = snd_chan.sync_request(req);

    if (rsp->body_root().getCoolRsp().getCoolVal() != TEST_VAL)
    {
      FLOW_LOG_FATAL("WTF?!");
      std::abort();
    }

    FLOW_LOG_INFO("Looks good.  Exiting.");
  } // try
  catch (const exception& exc)
  {
    FLOW_LOG_WARNING("Caught exception: [" << exc.what() << "].");
    return BAD_EXIT;
  }

  return 0;
} // main()
