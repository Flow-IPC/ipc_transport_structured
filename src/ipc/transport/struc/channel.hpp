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
#pragma once

#include "ipc/transport/struc/sync_io/channel.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <boost/move/make_unique.hpp>

namespace ipc::transport::struc
{

// Types.

/**
 * Owning and wrapping a pre-connected transport::Channel peer (an endpoint of an established channel over which
 * *unstructured* messages and optionally native handles can be transported), this template is the central pillar of the
 * ipc::transport::struc (*structured layer*), capable of communicating structured capnp-schema-based messages (and
 * native handles).
 *
 * @see sync_io::Channel and util::sync_io doc headers.  The latter describes a general pattern which
 *      the former implements.  In general we recommend you use a `*this` rather than a sync_io::Channel --
 *      but you may have particular needs (summarized in util::sync_io doc header) that would make you decide
 *      otherwise.
 *
 * ### Context (short version) ###
 * struc::Channel (and its `sync_io` counterpart -- and, internally, its core) along with Msg_out and Msg_in
 * essentially *are* the ipc::transport structured layer.  We also
 * suspect that (and designed ipc::transport accordingly) most users of ipc::transport, or arguably even Flow-IPC
 * at large, will mostly care about the work they can do with struc::Channel and nothing else.  Users just
 * want to send structured messages (and sometimes native handles) in some forward-compatible protocol.  Everything
 * else -- sessions, unstructured-layer channels, the queues and/or socket streams comprised by those channels --
 * are the necessary but ideally-ignored-most-of-the-time *means* leading to the *ultimate end* of having a working
 * struc::Channel over which to exchange that structured stuff with the peer's struc::Channel counterpart.
 *
 * ### Context (more in-depth) ###
 * This has certain broad implications.  Suppose one first familiarizes themselves with the unstructured layer
 * (concepts Blob_sender, Blob_receiver, Native_handle_sender, Native_handle_receiver; bundling class template
 * transport::Channel; concept impls -- classes/class templates Native_socket_stream, Blob_stream_mq_sender,
 * Blob_stream_mq_receiver; low-level details like the MQ-related API and server class Native_socket_stream_acceptor).
 * If one indeed is familiar with that whole unstructured API, they'll note: struc::Channel API is quite
 * different.  Particularly it's very different in terms of how one handles *in-messages* (receiving).  To wit:
 *   - The unstructured APIs are roughly boost.asio-like (though in fact *not* requiring any integration with actual
 *     boost.asio).  That is:
 *     - Each async operation is invoked via an `X.async_F(H)` method, to which one passes the handler `H` to invoke;
 *       and `H()` is called *exactly* once: when the async op completes (with or without error); or `X` is
 *       destroyed first (in which case an `operation_aborted`-like error code is emitted to `H()`).
 *     - If more than one `X.async_F()` is invoked, for a given `F` (e.g., 3 Blob_receiver::async_receive_blob()
 *       outstanding calls on one Blob_receiver), then they are queued inside X as needed, with each arriving result
 *       firing 1 handler, dequeuing it in FIFO order.
 *     - If a pipe-hosing error occurs, it is fed to all queued handlers (i.e., the internal queue is cleared)
 *       and any future-invoked-async-API-passed handlers also.
 *   - By contrast: The structured (struc::Channel) API is not at all boost.asio-like.
 *     - There are no `async_F()` async ops as such.  There is no FIFO-queueing of handlers.  Instead:
 *     - One registers events handlers for each type of event in which one is interested.  While it's possible
 *       to auto-unregister an event handler after it fires once, the general case is that an event handler
 *       registration is permanent until explicitly undone.
 *       - As a sub-case of this paradigm, the act of sending an out-message can be optionally paired with
 *         registering an event handler for when (and if) the other peer responds to that specific out-message
 *         with a response in-message.  Thus request/response semantics (while optional) are built-in.
 *       - As a sub-case of this paradigm, a single error handler is registered; if a pipe-hosing error occurs,
 *         then that handler is invoked just once -- as opposed to invoking all the other stored handlers with
 *         that same #Error_code.
 *
 * As for the *out-message* (sending) API: It is actually extremely similar to the unstructured layer's
 * already admirably simple API.  To recap:
 *   - If you want to send a message, you invoke send() or async_request().  Each is non-blocking *and* synchronous
 *     *yet* it cannot fail due to a would-block condition.  There is no asynchronous send op!
 *     - Alternatively use sync_request() which is a blocking, optionally subject to timeout, version of
 *       async_request().  It will block until a one-off response is received to the sent message
 *       (or timeout, or error) and return that response synchronously (if indeed successful).
 *     - Note that, while sync_request() is blocking, it can be used for very quick request-response exchanges;
 *       indeed that is its main use case.  For example, a memory-allocation op might contact the memory-allocating
 *       process and return once that process has acknowledged completing the alloc request.
 *   - Graceful closing is forwarded to the unstructured layer.  The Blob_sender, Native_handle_sender,
 *     transport::Channel async_end_sending() API is mimicked by simply passing-through to the owned #Owned_channel.
 *     - This features an asynchronous component.  Discussion omitted here; just see async_end_sending().
 *
 * However the out-message (sending) API adds an important (optional but highly encouraged) capability:
 *   - Every out-message can be allocated in SHM (shared memory) instead of heap; send() (et al) then transparently
 *     sends (copies into transport, on other side copies out of transport) a tiny *handle* to the message.
 *     - This is enabled, or disabled, for the entire `*this` via compile-time parameters and ctor args.
 *       After that the API is completely identical regardless of whether this full *zero-copy* mode is in use
 *       or not.
 *   - An out-message can be reused and sent again.  It can be modified in-between.  It would be practical to use
 *     this technique to store complex data structures integral to your application as opposed to a mere messaging
 *     focus.  This does not *require* the SHM-backed (zero-copy) mode, but it greatly improves performance
 *     when sharing with another process.
 *     - This is an alternative to storing a C++ data structure, such as a `vector` of `map`s of ..., in SHM
 *       manually and transmitting a handle manually.  Discussion of details of this is outside the scope
 *       here, but you should be aware that it's the alternative approach for structured-data storage in SHM
 *       and its IPC.  They each have their pros/cons.
 *
 * Long story short:
 *   - Sending is simple as in the unstructured layer.
 *     - However zero-copy backing of the structured message's serialization in SHM allows for far improved
 *       perf.
 *   - Receiving follows an event-handler-registration model, instead of the unstructured layer's preferred
 *     async-op-and-handler model (as inspired by boost.asio).
 *
 * ### Overview: How to use it, why to use it ###
 * How to use this thing?  Broadly speaking:
 *   -# One first establishes a channel connection (obtains a transport::Channel in PEER state).  See
 *      transport::Channel.  Attention: You will need a `sync_io`-pattern-peer-bearing transport::Channel
 *      (which is enforced at compile-time); formally `Owned_channel::S_IS_SYNC_IO_OBJ == true` is required.
 *      ipc::session emits such `Channel`s.  If you are manually assembling a `Channel` (or alias or
 *      data-less sub-class), then attaining fresh PEER-state peer objects of the `sync_io`-pattern variety
 *      should be equally easy.  In the case of Native_socket_stream specifically, you can perform
 *      `.sync_connect()` on a `sync_io`-pattern transport::sync_io::Native_socket_stream; on success bundle
 *      it into `Channel`.
 *   -# One selects the capnp message schema they wish to use.  In other words, what kinds of messages does
 *      one want to send?  This determines the choice of the #Msg_body template parameter.
 *   -# One selects a technique as to where/how out-messages (and consequently in-messages) will be stored in RAM:
 *      SHM (for full zero-copy performance) or heap (for zero-copy on either side of the low-level transport)..
 *      (Details omitted here, but for the sake of this brief recap let's consider this a formality.)
 *   -# One then constructs the struc::Channel, passing the transport::Channel (and the config from previous bullet
 *      point) into ctor -- which is moved-into the resulting `*this`.
 *   -# One then exchanges messages (and, if desired, native handles) with the matching struc::Channel on the
 *      other side via the `*this` transmission API.
 *   -# When `*this` is destroyed, so is the owned transport::Channel (and no earlier).
 *
 * So struc::Channel "eats" (via `std::move()`) a presumably fresh (no traffic exchanged) transport::Channel and all
 * further work on the transport::Channel is through struc::Channel.
 *
 * What does struc::Channel add, broadly speaking, that transport::Channel does not already have?  Why even bother?
 * Answer:
 *   - Firstly, each message no longer consists of an (optional unstructured binary blob, optional #Native_handle) pair.
 *     Instead if consists of a (structured capnp-schema-based message, optional #Native_handle) pair.
 *     In other words this class adds the (mandatory) ability to send *structured* data instead of mere
 *     binary blobs.  The schema language of choice is Cap'n Proto (capnp), which allows for zero-copy perf
 *     *at least* until blobs enter the transport and after they exit the transport.
 *     - By choosing to use SHM-based serialization storage for out-messages, zero-copy perf can be extended
 *       end-to-end, meaning only a short handle is ever copied into the transport and out of it on the receiving
 *       side.
 *   - Secondly it establishes some convenient protocol notions on top of the basic message-boundary-respecting/reliable
 *     functionality of transport::Channel.
 *     - Message type (a/k/a #Msg_which): While #Msg_body can specify an essentially arbitrarily complex schema for
 *       what messages can be sent, with `*this` API being generally agnostic as to its details, it does establish
 *       one simple standard: The message schema #Msg_body *must* feature a top-level anonymous (capnp) *union*, with
 *       the "which" enumerator specifying the type of message that it is.  E.g., something approximating HTTP might
 *       have the top-level union specifying either a GET, POST, or CONNECT message type, with each one then featuring
 *       its own arbitrary capnp `struct` sub-schema, specifying what data come along with that type of message.
 *       - *Message type demuxing*: Based on this, one sets separate handlers for each individual value for
 *         an in-message's #Msg_which.
 *         - One-off demuxing: One can register the expectation for at most 1 message of a particular #Msg_which type.
 *           Then, when it is received, the expectation registration for that #Msg_which is auto-removed, as the
 *           user-supplied handler fires.
 *         - Open-ended demuxing: Alternatively one can set a permanent (unless explicitly unregistered later)
 *           expectation for a given #Msg_which.  Then the user-supplied handler will fire each time a matching
 *           in-message arrives.
 *         - (Queuing) Any messages that have no expect-this-`Msg_which` handler registered are queued inside `*this`.
 *           Registering a demux expectation later will immediately feed matching queued such in-messages, as if
 *           they'd arrived right then.
 *     - *Request/response demuxing*: Any message one sends can optionally be specified to be expecting response
 *       in-message(s).  This is the alternative to #Msg_which-based demuxing.  This establishes a basic
 *       request/response paradigm.
 *       - One-off response demuxing: One can specify that the given out-message expects up to exactly 1 response.
 *       - Open-ended response demuxing: One can instead specify that any number of responses may arrive
 *         (until one explicitly unregisters this expectation).
 *       - (Note) If a response in-message arrives, while `*this` has no response expectation registered, it is
 *         an error.  However it is not necessarily a fatal error: There is a mechanism allowing the user
 *         to deal with this situation should it arise.  However a properly designed protocol would eliminate this
 *         possibility in practice (informally speaking).
 *     - A single full-duplex pipe: transport::Channel, one notes, can be a bundle of up to 2 full-duplex pipes (though
 *       at least 1 is of course required): one for blobs only; one for blob/`Native_handle` combos.
 *       The 2 do not interact; therefore if one were to use both in parallel, various questions arise as to how
 *       (and why!) to use this functionality.  struc::Channel elides these details completely and deals with
 *       them internally.  Bottom line:
 *       - struc::Channel API models a single full-duplex pipe capable of transmitting messages, each of which
 *         is *either* just a structured message *or* a structured message plus #Native_handle.  (The latter capability,
 *         naturally, is removed, if #Owned_channel lacks a handles pipe.)  Internally the transport::Channel pipes are
 *         leveraged in such a way as to maximize performance.
 *       - Messages arrive in the same order as they are sent (as determined by the order of send() (et al) calls on the
 *         sender `*this`).
 *         - If a message arrives that has no event handler registered, then it is queued inside `*this`, as explained
 *           above.  They are queued, and therefore popped if/when a handler is registered, in the same order as
 *           they arrived.
 *         - Suppose message with sequence # 5 has arrived but had to be queued due to lacking a registered handler.
 *           Suppose message with sequence # 6 has arrived and *does* have a registered handler.  Message 6 will
 *           then be emitted to the user; in other words the act of being queued does not preclude the emission of
 *           subsequent messages.
 *     - Thirdly, there are some (mandatory) safety features.  These are almost completely hidden from the user in
 *       regular operation.  To the extent they are not, it has to do with the *log-in* feature.  This is discussed
 *       separately below.  Typically one need not worry about it.
 *
 * ### How to use it (deep-dive) (log-in excluded) ###
 * Naturally there are two directions of transmission, sending out-messages and receiving in-messages.  About the
 * former first:
 *
 * To transmit an out-message:
 *   - Call create_msg(); this yields a cheaply-`move()`able out-message:
 *     #Msg_out.  (See Msg_out doc header for formal public API.)  You may also construct this
 *     directly -- even without a struc::Channel in existence at all; create_msg() merely helps by
 *     supplying the serialize/deserialize parameters (a/k/a builder/reader config) for you.
 *   - Fill out its contents: `M->body_root()` is a `Msg_body::Builder*`, where #Msg_body is determined by your
 *     choice of schema (`Message_body` template param); and `Msg_body::Builder` is the capnp-generated
 *     mutating API.  For example: `M->body_root()->initCoolRequest().setCoolField(57);`
 *     - sets the message type to `Msg_body::Which::COOL_REQUEST` and initializes a blank `CoolRequest` capnp-struct;
 *     - mutates the field `coolField` -- presumably an integer of some kind in this example -- to equal 57.
 *   - Send it: `send(M)` or `async_request(M)`.  Each is non-blocking, synchronous, and cannot would-block.
 *     - If `M` is a response to some in-message `I`, use `send/async_request(M, I, ...)`.  Otherwise use
 *       `send/async_request(M, nullptr, ...)`.
 *     - If you expect *response* message(s) to `M`, use `async_request(M, ..., &id, H)`, where:
 *       - Set `&id` to `nullptr` if one expects at most 1 response to `M`; otherwise non-null.
 *         - In the latter case optionally use `undo_expect_responses(id)` to indicate no further responses
 *           to the out-message instance sent by that async_request() call are expected.
 *       - `H` is the handler for the reponse(s); `H(I)` shall be invoked, where `I` is the response in-message.
 *   - Alternatively to async_request(): `sync_request(M)`.  This is similar, but instead of returning it
 *     awaits the response and returns that response once received.
 *     Note that by definition such a request is one-off (exactly 1 response is expected).  sync_request() can
 *     optionally specify a timeout.
 *   - Note: See async_end_sending() doc header.  TL;DR: You should call this when done using `*this`; then destroy
 *     `*this` once the async-handler you passed to async_end_sending() has fired.
 *
 * As usual, the incoming direction is somewhat more complex.  It is also different from lower-level APIs
 * such as boost.asio sockets and our various `Blob_sender`s and the like.
 *
 * Internally, all in-messages that do arrive on the "wire" are received immediately; each in-message at that point
 * is then handled in exactly one of the following ways.  (Here we do not discuss pipe-hosing errors, yet.)
 * A given in-message `I` is handled thus:
 *   - If `I` is not a response to an earlier out-message `M`:
 *     - If `I` is being expected via an earlier expect_msg() or expect_msgs() call: *Emit* `I` to that user handler.
 *     - Else: queue it inside `*this` silently.
 *   - Else:
 *     - If `I` is being expected via a response-expectation, as registered in async_request() or
 *       sync_request() (see above):
 *       *Emit* `I` to that user handler, or return it via sync_request() synchronously (whichever applies).
 *     - Else: it is a non-pipe hosing error condition.  While you should (informally speaking) avoid this, if
 *       it does occur, you can handle it as you see fit.  See set_unexpected_response_handler().
 *       See also set_remote_unexpected_response_handler().  Spoiler alert: This condition is emitted on both
 *       sides: locally, indicating an unexpected response arrived; and remotely, indicating that side *issued*
 *       an unexpected response.
 *
 * Hence there are exactly 3 ways to register a handler for in-messages.
 *   - Call expect_msg() or expect_msgs() to expect a particular type of *unsolicited* (non-response) message.
 *     expect_msg() indicates up to 1 such message shall be expected; expect_msgs() indicates 0+ such messages,
 *     until undo_expect_msgs().
 *     - If expect_msg() or expect_msgs() is invoked, and matching message(s) are queued inside `*this` (per above)
 *       at that time, `*this` behaves as-if those messages had just arrived.  I.e., they are immediately
 *       *emitted*.
 *   - Call async_request() to expect a response to a particular out-message.
 *   - Call sync_request() to send request and synchronously await one-off response to that particular out-message.
 *
 * Example:
 *   - I am a server, and I expect GET and POST messages.  I invoke `expect_msgs(Msg_which_in::GET_REQ, handle_get_req)`
 *     and `expect_msgs(Msg_which::POST_REQ, handle_post_req)`.  `handle_get_req(x)` shall be invoked for each
 *     incoming GET_REQ `x` and similarly for the POSTs.
 *   - I am a client, and I issue GET and POST messages.  I invoke `async_request(x, ..., nullptr, handle_get_rsp)`
 *     for a GET and `async_request(y, ..., nullptr, handle_post_rsp)`, where earlier one did:
 *     - `x = ....create_msg()`; `x->body_root()->initGetReq().set{Url|HostHeader}(...)`;
 *     - `y = ....create_msg()`; `y->body_root()->initPostReq().set{Url|HostHeader|PostBody}(...)`.
 *
 * Last but not least: How does one read the contents of an incoming message?  For both paths -- unsolicited message
 * expectation via expect_msg()/expect_msgs() + response message expectation via async_request() alike -- the
 * user-provided handler shall be of the form: `H(I&&)`, where `I` is a #Msg_in_ptr, a ref-counted handle to a new
 * in-message.  (See Msg_in doc header for the formal public API.)  Note the `&&`: it is an rvalue reference
 * to the `shared_ptr`, rather than a `const &`, in case you'd like to `move()` it somewhere for perf instead of
 * ref-counting increment/decrement.
 *   - `I->body_root()` is a `const Msg_body::Reader&`, where #Msg_body is as explained earlier, and
 *     `Msg_body::Reader` is the capnp-generated accessor API.
 *     For example: `I->body_root().getCoolRequest().getCoolField()` shall equal 57 on the other end, following
 *     the earlier out-message example.
 *   - In the case of unsolicited messages: `I->body_root().which()` shall equal the #Msg_which_in passed
 *     to expect_msg() or expect_msgs().  Therefore one can perform `I->body_root().getCoolRequest()` without
 *     fear it's not actually a `COOL_REQUEST` (which would yield a capnp exception).
 *   - In the case of response messages: `I->body_root().which()` can be anything (i.e., `*this` will not enforce
 *     anything about it).  It is up to you to design that protocol.
 *     - I (ygoldfel) considered enforcing a #Msg_which_in expectation for responses, to be supplied with
 *       async_request().  However it seemed too restrictive to mandate it.
 * @todo Consider adding the *optional* expectation of a particular #Msg_which_in when registering expected
 * responses in struc::Channel::async_request().
 *
 * ### Thread safety; handler invocation semantics ###
 * In-message handlers, and all other handlers (including those from start(), set_unexpected_response_handler(),
 * set_remote_unexpected_response_handler(), async_end_sending()), are invoked from an unspecified thread guaranteed not
 * to be among the user invoking threads.  No 2 such handlers shall be invoked concurrently to each other.
 *
 * Messages are emitted in the order received, except reordered as required by the queueing of not-yet-expected
 * unsolicited messages (as explained earlier).
 *
 * Attention!  Regarding general thread safety: A given `*this` features stronger safety than most other APIs:
 *   - **It is safe** to invoke a method concurrently with another method call (to the same or other API)
 *     on the same `*this`...
 *   - ...except that **it is not safe** to invoke sync_request() concurrently with sync_request() on the same `*this`.
 *
 * You *may* call `*this` APIs directly from any in-message handler or other handler.
 * Exception: You may *not* invoke sync_request() (or any other `sync_*()` that may be added over time)
 * from an in-message handler or other handler.  Doing so can disrupt the timeliness of delivering
 * other async results.
 *
 * Informal recommendation:
 * You should off-load most or all handling of in-messages onto your own thread(s) (such as via boost.asio
 * `post()` or the flow.async infrastructure).  For example, suppose your program has some main worker thread U,
 * implemented as a `flow::async::Single_thread_task_loop U`.  Then, e.g.:
 *   - In thread U: `async_request(M, false, H)`:
 *     sends `M`; async-calls `H(I)` when response `I` to `M` arrives.
 *   - In (unspecified thread): `H(I)` fires.  Your `H()` body:
 *     `U.post([I = std::move(I)]() { handle_i(std::move(I)); })`.
 *   - Actual handling of `I` occurs in your `void handle_i(...::Msg_in_ptr&&) { ... }`.
 *     `handle_i(){}` can be written with the assumption execution is back in thread U, same as the async_request()
 *     call that triggered all this (asynchronously).
 *
 * This is *not* a requirement.  It is entirely conceivable one would want to piggy-back onto our internal
 * unspecified thread (or threads -- formally speaking -- but the guarantee is handlers are called
 * non-concurrently, so in most senses one can treat it as a single thread).  However please be mindful:
 *   - If your handler code is "heavy," then you may slow down internal transport::Channel and struc::Channel
 *     processing on a number of layers (though, only relevant to `*this`).
 *   - Let's say we call your own thread, thread U, and the internal/unspecified `*this` thread where
 *     it invokes handlers, thread W.  Then it is your responsibility to ensure you do not call
 *     a `this->sync_request()` concurrently to another `this->sync_request()` method.  If you choose a design
 *     where you make such calls both from thread U and thread W, you must protect `*this` access
 *     with a mutex or other mechanism -- and moreover, most likely, you'll need to design your protocol
 *     to ensure instant responses to `sync_request()`s.
 *
 * ### Pipe-hosing error semantics ###
 * The underlying transport::Channel, which every struc::Channel takes over in the first place at construction,
 * can fail -- encounter a pipe-hosing error.  Briefly speaking, a transport::Channel out-pipe can -- depending
 * on the actual low-level transport involved -- be hosed while the in-pipe(s) remain unaffected; and indeed when
 * a transport::Channel internally consists of 2 bidirectional pipes, it is even possible for 1 out-pipe to be hosed,
 * while the other is not.  (The same holds of in-pipes.)  However, these details strongly depend on the actual
 * low-level transports involved.  To simplify error handling, these details are hidden away and normalized to
 * the following straightforward behavior.
 *
 * As noted before, struc::Channel models a single bidirectional full-duplex pipe of discrete messages.
 * The out-pipe is controlled simply: call send(), async_request(), or sync_request() to send a message, synchronously,
 * non-blockingly (if not sync_request()).
 * The in-pipe takes in all messages as soon as they arrive on the transport::Channel "wire"; they may be queued inside
 * or immediately (or later) emitted, but this is not relevant to error handling.
 *
 * The entire bidirectional pipe is either not-hosed, or it is hosed (forever).  A pipe-hosing condition,
 * indicated by a truthy #Error_code, occurs (if it occurs at all) exactly once.  (Note: Receiving the
 * `transport::Channel`-level graceful-close message -- transport::error::Code:: S_RECEIVES_FINISHED_CANNOT_RECEIVE --
 * is not "special" -- it is also a pipe-hosing error.)  When it occurs, it is immediately emitted to the
 * `*this` user.  There are 2 possible ways this can occur (again: only 1 of these can occur per `*this`):
 *   - Triggered by send(), async_request(), or sync_request(): Then that method shall emit the truthy #Error_code
 *     directly/synchronously via out-arg or exception.
 *   - Triggered spuriously in the background via incoming-direction processing: Then it shall be emitted via
 *     the on-error handler registered by the user.  (If this occurs during sync_request(), then it shall be
 *     emitted via sync_request(), not via on-error handler.)
 *     - To receive any in-messages, you must call start() which takes the mandatory on-error handler as arg.
 *       Until then, any lower-level in-traffic shall be safely queued inside the #Owned_channel.  Still, it is
 *       best to call start() early, to avoid RAM backup and such (informally speaking).
 *
 * After the pipe-hosing error is emitted, via exactly 1 of the above methods, further API calls on `*this` shall
 * no-op and return `false` or a null value.
 *
 * Lastly, there is async_end_sending().  See its doc header.  TL;DR: Although the pipe is hosed at the
 * struc::Channel level once a pipe-hosing #Error_code is emitted, it is still a good idea to execute
 * async_end_sending() and only destroy `*this` once the async-handler passed to it has fired.  (Though, if
 * send() (et al) emitted the truthy #Error_code, there is no point: async_end_sending() will no-op.)
 *
 * Informal recommendations:
 *   - Recall that we recommend any event handling, from within an in-message handler or any other handler,
 *     be immediately off-loaded onto a user worker thread.  (In particular you may not call `*this` APIs from these
 *     handlers.)
 *   - When such a user-thread handler, or other user-thread function, detects the emitted truthy #Error_code
 *     (from on-error handler, or via send(), async_request(), or sync_request() emitting it synchronously), mark
 *     `*this`, somehow, as hosed (e.g., via a flag or saved truthy #Error_code).
 *   - When about to do work on `*this`, check that flag or mark, so as to be aware `*this` is hosed and shutting
 *     down -- and no-op instead of trying to call whatever API.  Though, it may be viable to simply count on
 *     the fact `*this` APIs shall all (except async_end_sending()) no-op and return `false` or null in that case
 *     anyway.
 *   - Lastly, recommend then calling `async_end_sending(F)`; and in `F()` destroy `*this` finally.
 *   - If you have 1 thread (e.g., via `flow::async::Single_thread_task_loop`) dedicated to processing `*this`,
 *     then this is particularly straightforward; no need to synchronize on `*this` and/or the `*this`-is-hosed
 *     flag/mark.
 *
 * ### Lifecycle of an out-message ###
 * #Msg_out represents the message payload.  It is a data structure instance -- nothing more.  Its outer shell sits
 * wherever it is instantiated; typically either on the stack or in regular heap.  It allocates space for the
 * the serialization mutated by its `.body_root()` capnp `Builder` based on the `Struct_builder_config` type
 * chosen at compile-time.  (If you use create_msg(), this light-weight object -- essentially a few scalars -- is
 * supplied to the `Msg_out` ctor for you.  Otherwise you have to construct one directly and pass-in a
 * `Builder_config` or `Builder` of the same type.)  This builder *engine* determines where this internal
 * bulk -- the serialization backing -- lives.  For the rest of this discussion let us assume it is
 * `shm::classic::Builder::Config` (or jemalloc equivalent); meaning the engine is zero-copy-enabling,
 * for high perf and other benefits, and therefore allocates backing buffer(s) in SHM.
 *
 * #Msg_out, therefore, is similar to a container backed by a SHM-allocating allocator.  It continues to take RAM
 * resources, and the serialization continues to be accessible, on all ends until *both* of the following occur:
 *   - The #Msg_out is destroyed.
 *   - Every #Msg_in_ptr obtained in *any* opposing struc::Channel to have received it (via expect_msg(),
 *     expect_msgs(), and response-accepting handlers), after it had been send()ed (et al) there, is destroyed --
 *     along with all derived `Msg_in_ptr`s.  (#Msg_in_ptr is `shared_ptr<Msg_in>`.)
 *
 * Suppose you create a #Msg_out and send() it.  At this point there is "virtual" #Msg_in in existence:
 * the underlying serialization will not be destroyed yet, even if #Msg_out is.  Normally the opposing
 * struc::Channel will be used to receive the message, in a real #Msg_in, via an aforementioned in-message
 * handler.  If this does not occur, the message leaks until the containing SHM arena is cleaned-up.
 * (If you use ipc::session to manage SHM arena(s), which is the normal scenario, then ipc::session will take
 * care of this for you.  See ipc::session documentation, starting with the namespace doc header.)
 *
 * Suppose in your application, at this point, you will never again write to the #Msg_out further (modify it).
 * You may freely send() the same #Msg_out again.  You may send it through a different struc::Channel or
 * the same one.  You may do so at will.  You may receive it, obviously, at will as well.  Note, however, that
 * #Msg_in is not fully symmetrical to #Msg_out: It represents not the container but rather the in-message
 * *instance*: that particular time you received a message, with a handle to the payload controlled by
 * the original #Msg_out.  So for one #Msg_out, there may be 2+ `Msg_in`s in existence, across various processes
 * to have received it.  If one process has received it 2x, then there are potentially 2 `Msg_in`s in existence
 * in that process.  The original #Msg_out and each #Msg_in each contributes 1 to the cross-process
 * ref-count.
 *
 * Lastly suppose all the same is true, except that you would like to continue modifying the #Msg_out post-`send()`.
 * This is allowed.  However, this must be done with care.  Of course there is the matter of
 * synchronization as usual: Behavior is undefined if one reads a message while the #Msg_out is being mutated
 * in the original process.  While it is conceivable one could use an outside synchronization mechanism for this --
 * an interprocess mutex for example -- informally we expect the best way is to use your struc::Channel
 * messaging protocol.  In particular it make sense to avoid any modification of a #Msg_out until one has received
 * a response to any async_request() of it, with the guarantee that the module receiving that *particular* #Msg_in
 * in the opposing process shall not further read its contents after issuing the async_request() for that response.
 *
 * Lastly suppose `Struct_builder_config` is *not* SHM-based; e.g. Heap_fixed_builder::Config.
 * In that case, simply, #Msg_out is the original message container backed by an in-local-heap serialization;
 * and every #Msg_in emitted by a receive-handler is a *copy*, also backed by an in-local-to-receiver-heap
 * serialization.  While #Msg_out can certainly be send()ed repeatedly, and modified in-between,
 * this simply has no effect on any resulting `Msg_in`s, as they are copies.
 *
 * ### Log-in ###
 * Internally, each out-message is supplied by `*this` with a *session token*.  In regular operation, every
 * out-message's session token is the same value through the lifetime of a `*this`.  Every in-message must also
 * contain that same session token value.  If an in-message arrives with an unexpected session token value, it
 * is a pipe-hosing error.  (The rationale for this is beyond the scope of discussion here; it is a safety feature.)
 * This all happens internally.
 *
 * In regular operation, at struc::Channel construction time, the constant session token value must be supplied
 * as a ctor argument.  This session token must not be nil.  While, formally speaking, any non-nil value will do --
 * provided the same one is given to the struc::Channel on the other end -- correct use (in terms of safety)
 * is to provide a properly generated unique token -- on both sides.  Where to get this value, though?  Answer:
 *
 * Before constructing any regular-operation `struc::Channel`s, in a given session between two processes A and B,
 * one shall first construct *one* special-purpose transport::Channel, called the *session master channel*, and wrap it
 * in a special-purpose struc::Channel on each side.  This is the 2nd ctor form, the one without a `session_token`
 * arg; one side must pass the value `is_server = true`; the other conversely `is_server = false`.
 *
 * This special channel is not yet in logged-in phase; it is in logging-in-as-client phase on 1 side and
 * logging-in-as-server phase on the other side.  The two `struc::Channel`s must then undergo a rigid, but short,
 * sequence of steps to get to the logged-in phase (at which point the session master channel enters its own
 * regular-operation phase; the purpose of that channel from that point on is unspecified here -- but read on).
 *
 * Namely: the client peer must create a message as normal (create_msg() or #Msg_out construction),
 * fill out the returned out-message as desired, and async_request() it.
 * The server peer conversely must invoke expect_log_in_request(), exactly once, handle the incoming request
 * by filling out a response out-message, and send() that (response-supplying form is mandatory).
 *   - The server peer `*this` enters logged-in (regular-operation) phase upon `send()`ing the log-in response.
 *     Did you invoke send() successfully?  Congrats: you're in regular-operation.
 *   - The client peer `*this` enters logged-in (regular-operation) phase upon receiving that log-in response
 *     Did your handler for the response get fired?  Congrats: you're in regular-operation.
 *
 * While this mechanism is fairly general, and hopefully reasonably easy to operate, in practice most users will
 * not need to ever do so themselves.  Instead: On the server side, establish a `Server_session` (see
 * session::Session_server).  On the client side, establish a `Client_session`.  They will operate an adequate
 * session master channel internally.  `Server_session` and `Client_session` shall then each reach their PEER state,
 * at which point they each satisfy the formal session::Session concept (see session::Session doc header).  Now,
 * session::Session::session_token() shall return the uniquely generated session token.  This value shall be passed to
 * all subsequent `struc::Channel`s in the session, into their ctors (possibly via internal delegation).
 * These subsequent `struc::Channel`s do not need a log-in phase and are immediately in regular-operation.
 *
 * Therefore, the only time a user must worry about session tokens at all is when creating a regular-operation
 * struc::Channel, and even then only if no tag-form struc::Channel ctor is applicable.  In that case:
 *   - Access session::Session::session_token().
 *   - Pass the result into struc::Channel ctor on each side.
 *
 * Incidentally, `Session` (as implemented by `Server_session`, `Client_session`, and variants) can also easily open new
 * `Channel`s.  Internally they do so by leveraging various features of struc::Channel as applied to the *one*
 * (per process A-B session) session master channel.  Each `Channel` obtained this way can then be fed (upgraded) into
 * a new struc::Channel, if indeed structured messaging is desired for that transport::Channel.  The information in
 * this paragraph is, formally speaking, outside the purview of struc::Channel API, but I mention it here
 * for context.  For more detail see session::Session (and its impls session::Server_session, session::Client_session
 * and variants).
 *
 * @todo struc::Channel should probably be made move-constructible and move-assignable.  No concept requires
 * this, unlike with many other classes and class templates in ::ipc, so it is less essential; but both for
 * consistency and usability it would be good.  It would also make some APIs possible that currently would require
 * the user to explicitly wrap this class in a `unique_ptr`.  For example, imagine
 * a `Session::Structured_channel Session::structured_channel_upgrade(Channel_obj&& channel, ...)` that
 * constructs a suitably-typed struc::Channel, subsuming the raw `channel` just opened in that session::Session, and
 * returns that guy.  Currently it would need to return `unique_ptr<Session::Structured_channel>` or something.
 *
 * @internal
 * ### Implementation overview (threads U, W) ###
 * struc::Channel sits atop several layers of concepts and objects implementing them: from the raw
 * transports (transport::asio_local_stream_socket namespace, Persistent_mq_handle concept and impls); to
 * the component Blob_sender, Blob_receiver, Native_handle_sender, Native_handle_receiver impls; to the
 * transport::Channel (#Owned_channel) bundling them together; and lastly -- and most immediately relevantly --
 * to the struc::sync_io::Channel core.  (The latter is aliased as struc::Channel::Sync_io_obj.)
 *
 * In implementing struc::Channel, then, we already rely on much logic that is abstracted-away; we only
 * need to "decorate" their work by, essentially, some pre- and post-processing -- to provide, specifically,
 * the features explicitly outlined above publicly ("What does struc::Channel add, broadly speaking, that
 * transport::Channel does not already have?").
 *
 * Perf, in particular low latency, is essential for this work-horse class template.  For performance -- or perhaps
 * more accurately for most direct/deterministic flow control/thread structure --
 * we use `sync_io` objects throughout; our #Sync_io_obj core (`m_sync_io`) and the #Owned_channel subsumed by it
 * create no handler-invoking threads.  We, however, are ourselves an async-I/O-pattern object, and therefore
 * essentially by definition of how that pattern works we start a worker thread W in which to (1) perform all
 * asynchronously-delayed (i.e., contingent on event async-waits) work of all the aforementioned layers below
 * us (helpfully encapsulated in #Sync_io_obj `m_sync_io`); and (2) invoke user-supplied completion handlers.
 *
 * This thread W (`m_worker`) is the only async-worker thread.  Everything else is (to reiterate) `sync_io`-pattern.
 *
 * Thread U is (as per usual) the term for the user's calling thread (as usual they may of course use 2+ threads if
 * desired; but via #m_mutex we essentially make all such calls non-concurrent... so as usual we call it thread U,
 * as if it is one thread).  We do allow transmission APIs to be invoked from within completion handlers of our
 * own other transmission APIs (e.g., `send()` or `expect_msg()` directly within `expect_msg()` handler).
 * (sync_request(), being blocking, is the exception.)
 *
 * Since there are 2+ threads, U and W, with async-processing in the latter potentially occurring concurrently with
 * API calls in the former -- and with our explicitly allowing concurrent calls on the same `*this` except for
 * sync_request() -- there's a big fat mutex `m_mutex` locked around most code.  That said thread contention
 * is unlikely in most use cases, unless the user causes it in their own code.  (Details omitted here.)
 *
 * ### Implementation ###
 * Unlike in the past, when `*this` template was monolithic, now it is split into the core functionality
 * in sync_io::Channel (#Sync_io_obj `m_sync_io`) and the (relatively limited) logic necessary on top
 * of this to adapt that into the async-I/O pattern, wherein user handlers fire from thread W, and
 * any necessary internal async ops occur automatically in thread W without user participation.
 *
 * Therefore the really complicated stuff is abstracted away in the #Sync_io_obj.  The adapting logic is
 * simple enough to be understandable by reading the code and inline comments, especially on the data members.
 * @endinternal
 *
 * @tparam Channel_obj
 *         An instance of transport::Channel class template.  See transport::Channel doc header.  It must have
 *         `Channel_obj::S_IS_SYNC_IO_OBJ == true`.  Reminder: the template params to
 *         transport::Channel template (along with certain documented requirements on how to use its API when
 *         initializing it before feeding it to struc::Channel ctor) determine the number and nature of underlying
 *         transport pipes of the channel.  struc::Channel will work with anything you specify, as long as it
 *         results in a `sync_io`-core-bearing transport::Channel object in PEER state fed to `*this` ctor.
 * @tparam Message_body
 *         The capnp-generated class for the capnp-`struct` describing the schema of messages you intend to transmit
 *         over this channel.  (Therefore, in particular, `Message_body::Builder` and `Message_body::Reader` shall
 *         be capnp-generated classes as well.)  The contents of this capnp-`struct` are arbitrary -- whatever suits
 *         one's purpose -- except for the following requirement: It must contain a top-level anon capnp-`union`.
 *         #Msg_which shall therefore be the enumeration of the possible active value of this capnp-`union`.
 *         This union conceptually corresponds to the different types of messages one might transmit over the channel.
 * @tparam Struct_builder_config
 *         A type satisfying the concept Struct_builder::Config, with the additional requirement that
 *         each `flow::util::Blob B` emitted by `Struct_builder_config::Builder::emit_serialization()` satisfies
 *         the following condition:
 *         `B.size() <= M`, where `M` is the lower of `Owned_channel::send_blob_max_size()`,
 *         `Owned_channel::send_meta_blob_max_size()`, for the applicable 1-2 pipes in #Owned_channel.
 *         An object of this type is passed to ctor.
 *         This determines how, internally, `*this` shall transform structured out-messages provided by the user
 *         into binary blobs sent via the #Owned_channel.
 *         This Struct_builder_config must match the opposing peer's choice of Struct_reader_config.
 *         E.g., if this is Heap_fixed_builder::Config, then the other guy's
 *         thingie is Heap_reader::Config.
 *         In practice it is usually best to use a struc::Channel alias (as of this writing
 *         Channel_via_heap, shm::classic::Channel, shm::arena_lend::jemalloc::Channel)
 *         and accordingly to use one of the Channel_base tags:
 *         Channel_base::S_SERIALIZE_VIA_HEAP, Channel_base::S_SERIALIZE_VIA_APP_SHM,
 *         Channel_base::S_SERIALIZE_VIA_SESSION_SHM.
 *         See the documentation on the latter 3 for useful suggestions.
 * @tparam Struct_reader_config
 *         A type satisfying the concept Struct_reader::Config.  An object of this type is passed to ctor.
 *         This determines how, internally, `*this` shall transform unstructured in-blobs received from the other
 *         peer over the #Owned_channel into structured in-messages emitted to the user.
 *         This Struct_reader_config must match the opposing peer's choice of Struct_builder_config.
 *         See `Struct_builder_config` notes just above.
 */
template<typename Channel_obj, typename Message_body,
         typename Struct_builder_config, typename Struct_reader_config>
class Channel :
  private boost::noncopyable,
  public flow::log::Log_context
{
public:
  // Types.

  /// Useful for generic programming, the `sync_io`-pattern counterpart to `*this` type.
  using Sync_io_obj = sync_io::Channel<Channel_obj, Message_body,
                                       Struct_builder_config, Struct_reader_config>;
  /// You may disregard.
  using Async_io_obj = Null_peer;

  /// Short-hand for the transport::Channel type passed into ctor and returned by owned_channel().
  using Owned_channel = typename Sync_io_obj::Owned_channel;

  /// Short-hand for the builder engine type.
  using Builder_config = typename Sync_io_obj::Builder_config;
  /// Short-hand for the reader engine type.
  using Reader_config = typename Sync_io_obj::Reader_config;

  /// Short-hand for the `Message_body` template param, this is the capnp message schema transmissible over `*this`.
  using Msg_body = typename Sync_io_obj::Msg_body;

  /**
   * Short-hand for the `Message_body` top-level anon capnp-`union` enumeration.
   * This important type determines the type of in-message or out-message the (remote or local, respectively) user
   * chose.
   */
  using Msg_which = typename Sync_io_obj::Msg_which;

  /// Stylistic nicety, indicating that a #Msg_which applies to an in-message rather than an out-message.
  using Msg_which_in = typename Sync_io_obj::Msg_which_in;

  /// Stylistic nicety, indicating that a #Msg_which applies to an out-message rather than an in-message.
  using Msg_which_out = typename Sync_io_obj::Msg_which_out;

  /**
   * Encapsulation of any out-message *payload* sent or meant to be sent via send() (et al) by a `*this`
   * of this type.  A #Msg_out is container-like, in that there is only one of these regardless of how
   * many times one has sent it -- even in other `*this`es of the same struc::Channel concrete type.
   *
   * @see "Lifecycle of an out-message" section of this class's doc header.
   */
  using Msg_out = typename Sync_io_obj::Msg_out;

  /**
   * Encapsulation of any in-message *instance* received by a `*this` of this type.
   *
   * @see "Lifecycle of an out-message" section of this class's doc header.
   *
   * @see #Msg_in_ptr
   */
  using Msg_in = typename Sync_io_obj::Msg_in;

  /**
   * A ref-counted handle to #Msg_in.  Generally one obtains a #Msg_in via a handle thereto via
   * handler passed to event registration APIs such as expect_msgs() or async_request(); then one
   * accesses its contents via capnp-generated accessors on Msg_in::body_root().
   */
  using Msg_in_ptr = typename Sync_io_obj::Msg_in_ptr;

  /// Short-hand for convenience for the same thing in non-parameterized base Channel_base.
  using msg_id_out_t = typename Sync_io_obj::msg_id_out_t;

  // Constructors/destructor.

  /**
   * All explicit constructors: signatures match exactly those of struc::sync_io::Channel (a/k/a #Sync_io_obj).
   *
   * @see struc::sync_io::Channel ctor doc headers.  They're important!
   *
   * @tparam Ctor_args
   *         Arguments per above.
   * @param logger_ptr
   *        See above.
   * @param channel
   *        See above.
   * @param ctor_args
   *        See above.
   */
  template<typename... Ctor_args>
  explicit Channel(flow::log::Logger* logger_ptr, Channel_obj&& channel, Ctor_args&&... ctor_args);

  /**
   * Invokes the destructor on the #Owned_channel.  In particular:
   *   - If the async_end_sending() async-handler is pending, it shall fire with
   *     transport::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER, and the dtor shall return only once it
   *     has finished executing.
   *   - No other handler from `*this` shall fire.  (struc::Channel does not -- outside of async_end_sending(), a
   *     `transport::Channel`-level method -- follow the boost.asio-like one-off `async_*()` model.)
   *
   * Reminder: It is recommended, before invoking this destructor, to:
   *   - Call `async_end_sending(F)`.
   *   - Invoke this destructor once `F()` fires.
   *
   * See discussion in async_end_sending() doc header or shorter version in class doc header.
   */
  ~Channel();

  // Methods.

  /**
   * Permanently memorizes the incoming-direction on-error handler, thus authorizing the emission of any
   * in-messages to handlers registered via expect_msg(), expect_msgs(), async_request() (as well as related handlers
   * set_unexpected_response_handler() and set_remote_unexpected_response_handler()).  Until this is invoked,
   * any incoming lower-level traffic is nevertheless saved in this process's user RAM (in fact inside the
   * #Owned_channel, though that's an implementation detail of sorts) to be emitted as required after start().
   *
   * @tparam Task_err
   *         Handler type matching signature of `flow::async::Task_asio_err`.
   * @param on_err_func
   *        The permanent on-channel-hosed error handler.  See class doc header for discussion of error semantics.
   * @return `true` on success; `false` if already start()ed, or if a prior error has
   *         hosed the owned transport::Channel (then no-op).
   */
  template<typename Task_err>
  bool start(Task_err&& on_err_func);

  /**
   * Utility for use when #Builder_config template param is Heap_fixed_builder::Config,
   * this returns such a `Config` constructed with the most efficient yet safe values.
   * The resulting Heap_fixed_builder::Config, and the #Owned_channel `channel`, can then
   * be safely passed to the future struc::Channel's non-tag ctor form.
   *
   * ### Do not use this unless necessary ###
   * It is only necessary for advanced purposes, particularly if you choose to use a non-tag ctor form.
   * See the 1st non-tag ctor's doc header for when that might be.  Long story short, it is typically easier and
   * sufficient to use the Channel_base::Serialize_via_heap tag ctor.
   *
   * ### What about Heap_reader::Config counterpart? ###
   * See heap_reader_config().
   *
   * @param channel
   *        The channel you plan to pass as `channel` into struc::Channel ctor.
   *        Note `channel.get_logger()` is supplied as the `Logger` to the returned `Config`.
   *        Behavior is undefined (assertion may trip) if `channel` is not transport::Channel::initialized().
   *        Behavior is undefined if `channel` is not in PEER state.
   * @return See above.
   */
  static Heap_fixed_builder::Config heap_fixed_builder_config(const Owned_channel& channel);

  /**
   * Deserializing counterpart to heap_fixed_builder_config().  It is mostly provided for consistency,
   * as it simply returns `Heap_reader::Config(channel.get_logger())`.
   *
   * @param channel
   *        See heap_fixed_builder_config().
   * @return See above.
   */
  static Heap_reader::Config heap_reader_config(const Owned_channel& channel);

  /**
   * Returns the (light-weight) #Builder_config specified (directly or indirectly) at construction
   * time.  Informally: in particular this could be used to generate a `Builder_config::Builder` to fill out
   * manually via Struct_builder::payload_msg_builder() (a direct #Capnp_msg_builder_interface), without yet specifying
   * any schema, and eventually `move()` into a new #Msg_out, via the Msg_out 2nd/advanced ctor form.
   *
   * Tip: If you lack a `*this` from which to invoke struct_builder_config(), you may be able to cheaply generate
   * a #Builder_config via session::Session impls' methods (e.g., session::Session_mv::heap_fixed_builder_config(),
   * session::shm::classic::Session_mv::app_shm_builder_config(), etc.).
   *
   * @return See above.
   */
  const Builder_config& struct_builder_config() const;

  /**
   * Returns reference to immutable `Builder_config::Builder::Session` (a light-weight value, as of this writing
   * at most a pointer) specified (directly or indirectly) at construction time.  See that ctor argument's docs;
   * the arg is also named `struct_lender_session`.
   *
   * @return See above.
   */
  const typename Builder_config::Builder::Session& struct_lender_session() const;

  /**
   * Deserializing counterpart to struct_builder_config().
   * @return See above.
   */
  const Reader_config& struct_reader_config() const;

  /**
   * Access to immutable transport::Channel moved-into `*this` at construction.
   * @return See above.
   */
  const Owned_channel& owned_channel() const;

  /**
   * Access to mutable transport::Channel moved-into `*this` at construction.
   *
   * Formally only the following non-`const` methods of the returned pointee may be accessed:
   *   - transport::Channel::auto_ping();
   *   - transport::Channel::idle_timer_run().
   *
   * Informally: those are both safe and potentially useful.  Since #Owned_channel is `sync_io`-core-bearing,
   * to invoke either of those directly on the `Channel` *before* subsuming it into `*this` ctor, one would have
   * to do `Channel::start_*_ops()`; but `*this` ctor requires a fresh #Owned_channel without such setup having
   * been performed.  Catch-22!  However, with the availability of owned_channel_mutable(), one can simply call them
   * via `this->owned_channel_mutable->...()` and move on.
   *
   * Informally: any other use = at your own risk.  It may interfere with our operations => undefined behavior.
   *
   * @see async_end_sending().
   *
   * @todo Consider adding `struc::Channel::auto_ping()` and `struc::Channel::idle_timer_run()` and, for safety,
   * removing struc::Channel::owned_channel_mutable().
   *
   * @return See above.  Not null.
   */
  Owned_channel* owned_channel_mutable();

  /**
   * Returns the (non-nil) logged-in session token; or nil if not in logged-in phase, or if a prior error has
   * hosed the owned transport::Channel.
   *
   * Informally: If `*this` is the session master channel, then: Outside of, perhaps, logging/reporting, it
   * should not be necessary to call this.
   *
   * @return See above.  Note: The reference returned is always one of 2: a certain unspecified internal
   *         item, or to transport::struc::NULL_SESSION_TOKEN.
   */
  const Session_token& session_token() const;

  /**
   * Creates/returns empty out-message, optionally also holding a native handle,
   * to mutate and later send() (et al).  Alternatively you may simply directly construct a #Msg_out;
   * create_msg() is only syntactic sugar, since `*this` memorizes the builder engine and will pass-it-through
   * to the aforementioned constructor.
   *
   * @see "Lifecycle of an out-message" section of this class's doc header.
   *
   * @note The decision as to whether the returned message is unsolicited or a response to an in-message shall
   *       be indicated at send() (et al) time via its `originating_msg_or_null` arg.
   *
   * @param hndl_or_null
   *        The native handle to pair with the structured message; or default-cted (null) handle to
   *        pair no such thing.  It becomes `.null()` -- `*this` is in charge of returning it to
   *        OS at the proper time.  You may use `dup()` in a Unix-like OS, if you want to keep a functioning
   *        handle (FD) locally.
   * @return The new out-message payload, blank to start.  Note #Msg_out is cheaply move-assignable and
   *         move-constructible (but not copyable).
   */
  Msg_out create_msg(Native_handle&& hndl_or_null = Native_handle()) const;

  /**
   * Registers the expectation of up to 1 *notification* in-message whose #Msg_which equals `which`.
   * No-op and return `false` if `which` is already being expected, if log-in phase is not yet completed, or if
   * a prior error has hosed the owned transport::Channel.
   *
   * The expectation is unregistered upon receipt of the applicable in-message (and firing it with that in-message
   * #Msg_in_ptr as arg).
   *
   * @tparam On_msg_handler
   *         Handler type for in-messages; see class doc header for in-message handling signature.
   * @param which
   *        Top-level #Msg_body union `which()` value to expect.
   * @param on_msg_func
   *        `on_msg_func(M)` shall be invoked in the manner explained in class doc header,
   *        on receipt of message with `which() == which`.
   * @return `true` on success; `false` due to one of the above conditions.
   */
  template<typename On_msg_handler>
  bool expect_msg(Msg_which_in which, On_msg_handler&& on_msg_func);

  /**
   * Registers the expectation of 0+ *notification* in-messages whose #Msg_which equals `which`.
   * No-op and return `false` if `which` is already being expected, if log-in phase is not yet completed, or if
   * a prior error has hosed the owned transport::Channel.
   *
   * The expectation is unregistered upon subsequent `undo_expect_msgs(which)`.
   *
   * @tparam On_msg_handler
   *         See expect_msg().
   * @param which
   *        See expect_msg().
   * @param on_msg_func
   *        See expect_msg().
   * @return See expect_msg().
   */
  template<typename On_msg_handler>
  bool expect_msgs(Msg_which_in which, On_msg_handler&& on_msg_func);

  /**
   * Unregisters the expectation earlier-registered with expect_msgs().
   * No-op and return `false` if `which` is not being expected via expect_msgs(), if log-in phase is not yet
   * completed, or if a prior error has hosed the owned transport::Channel.
   *
   * @param which
   *        See expect_msgs().
   * @return See above.
   */
  bool undo_expect_msgs(Msg_which_in which);

  /**
   * In the synchronous/non-blocking manner of Blob_sender::send_blob() or Native_handle_sender::send_native_handle()
   * sends the given message to the opposing peer, as a *notification* (without expecting response).
   * The message may contain a #Native_handle.
   *
   * send() shall no-op (and return `false`) if one of the following is true.
   *   - Log-in phase, as a client, has not yet been completed.
   *     Use the other form of send() instead, as a log-in response (to the log-in request `msg`) is required.
   *   - Log-in phase, as a server, has not yet been completed, and `originating_msg_or_null` is null.
   *     Use non-null instead.
   *
   * Further documentation of behavior shall assume this no-op condition is not the case.
   *
   * @see async_request() overload which sends a *request* (same thing as here but *with* expecting response(s)).
   *
   * @note You may use this in logged-in phase; or in logging-in phase, as a server.
   *
   * ### Send mechanics (applies also to async_request() and sync_request()) ###
   * The purpose of this important method is hopefully clear.  That it's, essentially, a wrapper for
   * one of the aforementioned transport::Channel methods is also probably obvious.  However its error-reporting
   * semantics are somewhat different from transport::Channel and the `Blob_sender`/`Native_handle_sender` concept(s) it
   * implements.  Long story short, it reports errors similarly to the core Blob_sender::send_blob() or
   * Native_handle_sender::send_native_handle() with the following differences:
   *   - If `msg` contains a #Native_handle, but #Owned_channel has no native-handles pipe: no-op, return `false`.
   *   - If called after async_end_sending(): no-op, return `false`.
   *   - If called after a background receive or preceding send() / async_request() / sync_request() emitted error:
   *     no-op, return `false`.
   *   - Otherwise it'll attempt the core `send_*()` and return `true`.
   *     - If that is successful, send() is successful.
   *     - If not -- if it emitted error E -- then send() emits error E.
   *       - And `*this` shall be considered hosed for any subsequent transmission.
   *
   * ### In more detail (applies also to async_request() and sync_request()) ###
   * Here is what the method does, ignoring logged-in phase checks and potential change inside.
   *   -# If `msg` contains a #Native_handle, but Channel::S_HAS_BLOB_PIPE_ONLY is `true`, then returns `false`
   *      and otherwise no-ops.  This is a misuse of the chosen #Owned_channel as configured: a handles pipe
   *      is required to send handles.  Else:
   *   -# Checks whether the pipe is hosed by a preceding error (which would have been reported via the
   *      on-error handler registered by user on `*this`; or by the previous `send()`/etc. synchronously, if that's what
   *      discovered the pipe is hosed instead).  If so returns `false` and otherwise no-ops.  Else:
   *      (Note the previously discovered/emitted #Error_code is not re-emitted this time.)  Else:
   *   -# If you've called async_end_sending() already: Returns `false` and otherwise no-ops.  Else:
   *   -# Invokes either `Owned_channel::send_blob()` or `Owned_channel::send_native_handle()` 1+ times.
   *      (If Channel::S_HAS_BLOB_PIPE_ONLY: the former.  If Channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY: the latter,
   *      even if `msg` stores no #Native_handle (it's not required to send a handle).  If Channel::S_HAS_2_PIPES:
   *      the latter or the former depending on whether `msg` stores a #Native_handle.)
   *      This determines the possible #Error_code emitted; see doc header for the chosen
   *      sender concept impls for #Owned_channel.  However:
   *      - Note: transport::error::Code::S_SENDS_FINISHED_CANNOT_SEND shall not be emitted; instead send() will return
   *        `false` as noted above.
   *      - Note: Since we are in charge, internally, of invoking the low-level `send_*()` method, we shall ensure
   *        transport::error::Code::S_INVALID_ARGUMENT is not a possibility.
   *      - Serialization errors are also possible (can be thought of similarly to `send_*()` failing).
   *   -# If such a call emits an error, the present method shall forward (emit) that error (but return `true`).
   *      Moreover the pipe shall be considered hosed:
   *      - The triggering #Error_code will *not* be emitted to user via on-error handler registered on `*this`,
   *        because it will be emitted to the caller synchronously here by send().
   *      - No futher on-in-message user handlers (such as from expect_msg()) shall fire.
   *      - Any future transmission API calls will no-op indicating this in various ways:
   *        - expect_msg(), expect_msgs(), `set_*_handler()` shall return `false`;
   *        - `create_*()` shall return null;
   *        - send() shall return `false` (as noted above).
   *   -# Otherwise, cool.  It shall return `true` and not emit a truthy #Error_code.
   *
   * @see async_end_sending() for properly flushing out-messages near the end of `*this` lifetime.
   *
   * @param msg
   *        The out-message, which may or may not store a #Native_handle.
   * @param originating_msg_or_null
   *        Pointer to the in-message to which this is responding.  If the in-message is from another
   *        struc::Channel, behavior is undefined.  Set to null if this message is unsolicited (not a response).
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        See above.  Not `INVALID_ARGUMENT`, not `SENDS_FINISHED_CANNOT_SEND`.
   *        Also possible: serialization-failed errors (at least error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG
   *        unless `Struct_builder_config` is SHM-based).
   * @return `false` if no-op-ing due to one of the following conditions:
   *         invoked in logging-in phase as client; invoked in logging-in phase as server but
   *         `originating_msg_or_null` is null; `msg` with-handle, but #Owned_channel has no handles pipe;
   *         an #Error_code was previously emitted via on-error handler or send() / async_request() / sync_request();
   *         async_end_sending() has already been called.
   *         `true` otherwise (but some #Error_code may be emitted due to send failure).
   */
  bool send(const Msg_out& msg, const Msg_in* originating_msg_or_null = 0, Error_code* err_code = 0);

  /**
   * Identical to send() sends the out-message not as a *notification* but as a *request*,
   * registering the expectation of 1+ response(s) to it, async-invoking the provided response handler once reponse(s)
   * arrive(s).  `id_unless_one_off = nullptr` means up to exactly 1 response in-message shall be allowed, hence
   * `on_rsp_func()` shall execute at most once; at which point the expectation shall be unregistered.
   * `id_unless_one_off != nullptr` means responses shall be allowed (and passed to response handler) arbitrarily
   * many times -- until one calls undo_expect_responses() (specifically `undo_expect_responses(*id_unless_one_off)`).
   *
   * This variation on send() shall no-op (and return `false`) if one of the following is true.
   *   - Log-in phase, as a client, has not yet been completed, and `originating_msg_or_null` is non-null.
   *     Use null instead.
   *   - Log-in phase, as a server, has not yet been completed, and `originating_msg_or_null` is null.
   *     Use non-null instead.
   *
   * Further documentation of behavior shall assume this no-op condition is not the case.
   *
   * @see send() which sends a *notification* (same thing as here but *without* expecting response(s)).
   * @note You may use this in any phase (logged-in; or logging-in, as server or client).
   * @note Informally: In log-in phase, as a server, it would be unorthodox to use this method: `msg`
   *       by definition must be the log-in response; and it would be unusual for the log-in response to itself
   *       be a request for another response (or even multiple).  Nevertheless this *is* allowed; use cases are
   *       conceivable, and there is no reason to restrict this.  However note: As discussed elsewhere,
   *       logged-in phase is reached (as a server) the moment async_request() succeeds.  Any further responses are
   *       irrelevant to that (but could in theory be useful for some other purpose).
   *
   * All non-conflicting notes from send() apply here.
   *
   * @see async_end_sending() for properly flushing out-messages near the end of `*this` lifetime.
   *
   * @tparam On_msg_handler
   *         See expect_msg().
   * @param msg
   *        See send().
   * @param originating_msg_or_null
   *        Pointer to the in-message to which this is responding.  If the in-message is from another
   *        struc::Channel, behavior is undefined.  Set to null if this message is unsolicited (not a response).
   * @param id_unless_one_off
   *        See above.
   * @param on_rsp_func
   *        See expect_msg().
   *        `on_rsp_func(M)` shall be invoked in the manner explained in class doc header,
   *        on receipt of message M that indicates `msg` was its originating request.
   *        (On truthy `id_unless_one_off` it may be invoked more than once, until undo_expect_responses().)
   * @param err_code
   *        See send().  Note: No error is possible on account of `on_rsp_func` and `id_unless_one_off`, additions
   *        over the other overload().
   * @return `false` if no-op-ing due to one of the following conditions:
   *         invoked in logging-in phase as client but `originating_msg_or_null` is non-null; invoked in
   *         logging-in phase as server but `originating_msg_or_null` is null;
   *         `msg` with-handle, but #Owned_channel has no handles pipe;
   *         an #Error_code was previously emitted via on-error handler or send() / async_request() / sync_request();
   *         async_end_sending() has already been called.
   *         `true` otherwise (but some #Error_code may be emitted due to send failure).
   */
  template<typename On_msg_handler>
  bool async_request(const Msg_out& msg, const Msg_in* originating_msg_or_null,
                     msg_id_out_t* id_unless_one_off, On_msg_handler&& on_rsp_func, Error_code* err_code = 0);

  /**
   * Equivalent to the other sync_request() overload but with no timeout; meaning it shall exit only
   * once either the expected one-off response arrives, or a pipe-hosing error occurs.
   * Formally: this is simply `return sync_request(msg, originating_msg_or_null, Fine_duration::max(), err_code)`.
   *
   * @param msg
   *        See other sync_request().
   * @param originating_msg_or_null
   *        See other sync_request().
   * @param err_code
   *        See other sync_request().
   * @return See other sync_request().
   */
  Msg_in_ptr sync_request(const Msg_out& msg, const Msg_in* originating_msg_or_null = 0, Error_code* err_code = 0);

  /**
   * A blocking (subject to timeout) operation that first acts as-if async_request() was invoked
   * in one-off-request mode; then awaits the one-off response and synchronously returns that reponse
   * (or pipe-hosing error, or timeout error).
   *
   * Unlike send() and async_request(), you *may not* invoke this method directly from an in-message handler or other
   * handler.  Doing so can disrupt the timeliness of delivering async results and formally results in undefined
   * behavior.
   *
   * sync_request() shall no-op (and return null without a truthy `Error_code` emitted) if:
   *   - log-in phase has not yet been completed.
   *
   * Further documentation of behavior (except "Thread safety, concurrency") shall assume this no-op condition is
   * not the case.
   *
   * ### Thread safety, concurrency ###
   * Per class doc header: yes, you can invoke `this->sync_request()` concurrently to all other `*this` methods
   * except sync_request() itself.
   *
   * Furthermore, during the potentially-blocking portion of sync_request(), wherein it awaits the response or timeout,
   * other API calls shall execute non-blockingly rather than potentially-block waiting for sync_request() to return.
   * For example, a send() or async_request() will execute immediately.
   *
   * ### Send mechanics  ###
   * As the first step this method invokes essentially async_request().  Hence the following notes apply as they do
   * for async_request():
   * Long story short, it reports errors similarly to the core Blob_sender::send_blob() or
   * Native_handle_sender::send_native_handle() with the following differences:
   *   - If `msg` contains a #Native_handle, but #Owned_channel has no native-handles pipe: no-op, return null
   *     with falsy `Error_code`.
   *   - If called after async_end_sending(): no-op, return null with falsy `Error_code`.
   *   - If called after a background receive or preceding send() / async_request() / sync_request()
   *     emitted error: no-op, return null with falsy `Error_code`.
   *   - Otherwise it'll attempt the core `send_*()`.
   *     - If that yields a (pipe-hosing) error, then: return null with truthy `Error_code` emitted.
   *       - And `*this` shall be considered hosed for any subsequent transmission.
   *     - Otherwise the method moves on to the wait phase.
   *
   * The result of the wait phase shall be one of:
   *   - non-null response returned, no truthy `Error_code` emitted (success);
   *   - null response returned, truthy `Error_code` that is not transport::error::Code::S_TIMEOUT (pipe hosed during
   *     wait);
   *   - null response returned, `Error_code == S_TIMEOUT` (pipe is fine, but the wait timed out).
   *
   * ### Late-response semantics ###
   * If the response, or pipe-hosing error, occurs during this method's wait, then hopefully everything is clear.
   * Now suppose it does time out and hence emits `Error_code` transport::error::Code::S_TIMEOUT.
   * In the background `*this` continues to await the one-off response.  (Moreover it is not possible to
   * "un-expect" it or expect a response some other way; as response expectation can only be specified
   * at async_request() or sync_request() time, and you've already done the latter for that out-message, and a one-off
   * request expectation by convention cannot be undone as of this writing.)
   *
   * If the response (after timeout) never arrives, there's nothing further to discuss.
   *
   * Now suppose it does arrive at some point (after the timeout).
   * It shall be handled as-if an unexpected response was received (albeit with slightly different args as noted
   * just below).  Namely:
   *   - If set_unexpected_response_handler() is in effect, it shall be invoked (with `expected_but_timed_out` set).
   *   - The opposing peer is informed of this event, and if on that side set_remote_unexpected_response_handler()
   *     is in effect, the opposing-side user shall be informed via that handler (again
   *     with a flag indicating expected-but-timed-out).
   *
   * @param msg
   *        See async_request().
   * @param originating_msg_or_null
   *        See async_request().
   * @param timeout
   *        If no appropriate response to `msg` occurs, nor does a pipe-hosing error, before this time elapses
   *        sync_request() shall emit transport::error::Code::S_TIMEOUT.  Note that the latter does *not* indicate
   *        the pipe has been hosed.  To disable timeout you *must* use the special value
   *        `util::Fine_duration::max()`.  (Rationale: Internally, a certain boost.thread API may use
   *        `pthread_cond_timedwait()` internally; this function in at least some Linuxes barfs on very high values
   *        of `Fine_duration`, as they translate into negative absolute `timespec` components.  Therefore we
   *        detect the specific desire to have unlimited timeout and in that case use un-timed wait.)
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated: See
   *        async_request().  If the initial non-blocking send succeeds then other #Error_code possibilities
   *        include: Any pipe-hosing error that might be otherwise emitted outside a `sync_*()` due to
   *        in-traffic; and transport::error::Code::S_TIMEOUT.  Note that the latter does *not* indicate the pipe has
   *        been hosed.
   * @return Non-null response message on success within timeout; otherwise null.
   *         If null, emitted `S_TIMEOUT` indicates non-fatal timeout; emitted other truthy `Error_code` indicates
   *         either the non-blocking send emitted pipe-hosing error, or during the wait the pipe got hosed.
   *         If null, but no `Error_code` is emitted, then the method was a non-blocking no-op for one
   *         of the reasons listed above:
   *         invoked in logging-in phase; `msg` with-handle, but #Owned_channel has no handles pipe;
   *         an #Error_code was previously emitted via on-error handler or send() / async_request() / sync_request();
   *         async_end_sending() has already been called.
   */
  Msg_in_ptr sync_request(const Msg_out& msg, const Msg_in* originating_msg_or_null,
                          util::Fine_duration timeout, Error_code* err_code);

  /**
   * Unregisters the expectation earlier-registered with the `id_unless_one_off != nullptr` form of
   * async_request().  No-op and return `false` if response to `originating_msg_id_or_none` is not being
   * expected, if log-in phase is not yet completed, or if a prior error has hosed the owned transport::Channel.
   *
   * @note It is not possible to unregister an expected response without first `async_request()`ing the thing to which
   *       such a response would pertain.  If you don't want responses to a thing that's not yet sent, then
   *       don't send it.
   *
   * @param originating_msg_id
   *        The value `x` from an earlier successful `async_request(M, ..., &x, F)` call, where `M` is the request
   *        out-message, F is the response handler.
   * @return `true` on success; `false` due to one of the above conditions.
   */
  bool undo_expect_responses(msg_id_out_t originating_msg_id);

  /**
   * Registers the handler to invoke when a response in-message arrives, but no response-expectation
   * has been registered (via async_request()), or the one-off request had been previously satisfied
   * with another response, or undo_expect_responses() has been issued for an open-ended request,
   * or the response is to sync_request() that has timed out.  No-op and return `false` if a handler is already
   * registered.
   *
   * Note that, regardless of whether this handler is registered: If an unsolicited response does arrive,
   * the opposing (offending) peer shall be informed of this.  See set_remote_unexpected_response_handler() for
   * what that is like from their point of view.
   *
   * Lastly note that, while this and unset_unexpected_response_handler() can be invoked during logging-in phase,
   * `on_func()` won't fire until the logged-in phase.  To be clear: Nothing is "deferred"; that condition simply
   * cannot occur until the logged-in phase.  An unexpected response during the highly rigid logging-in phase
   * is treated as a channel-hosing (fatal) condition.
   *
   * @tparam On_unexpected_response_handler
   *         Functor type with signature compatible with `void F(Msg_in_ptr&&)`.
   * @param on_func
   *        `on_func(M, B)` shall be fired, where `M` is the offending in-message;
   *        `B` is `true` if this is a response to a timed-out sync_request(), `false` otherwise.
   * @return `true` on success; `false` due to one of the above conditions.
   */
  template<typename On_unexpected_response_handler>
  bool set_unexpected_response_handler(On_unexpected_response_handler&& on_func);

  /**
   * Undoes set_unexpected_response_handler().  No-op and return `false` if no handler is registered anyway.
   *
   * @return See above.
   */
  bool unset_unexpected_response_handler();

  /**
   * Registers the handler to invoke when the *remote* peer encounters the condition that would fire
   * the *remote* set_unexpected_response_handler() handler (regardless of whether one is indeed remotely
   * registered), causing that peer to inform `*this` of that event in the background.
   * No-op and return `false` if a handler is already registered.
   *
   * Note that, while this and unset_remote_unexpected_response_handler() can be invoked during logging-in phase,
   * `on_func()` won't fire until the logged-in phase.  To be clear: Nothing is "deferred"; that condition simply
   * cannot occur until the logged-in phase.  An unexpected response during the highly rigid logging-in phase
   * is treated as a channel-hosing (fatal) condition.
   *
   * @tparam On_remote_unexpected_response_handler
   *         Functor type with signature compatible with `void F(msg_id_out_t, std::string&&)`.
   * @param on_func
   *        `on_func(M, B, S)` shall be fired, where `M` is the offending out-message's #msg_id_out_t (for
   *        logging/reporting only); `S` a (movable) `string` containing brief abitrary info suitable for logging
   *        about the message; and `B` is `true` if this is a response to a timed-out sync_request() on the opposing
   *        side, `false` otherwise.
   * @return `true` on success; `false` due to one of the above conditions.
   */
  template<typename On_remote_unexpected_response_handler>
  bool set_remote_unexpected_response_handler(On_remote_unexpected_response_handler&& on_func);

  /**
   * Undoes set_remote_unexpected_response_handler().  No-op and return `false` if no handler is registered anyway.
   *
   * @return See above.
   */
  bool unset_remote_unexpected_response_handler();

  /**
   * Operating at the unstructured layer, executes a graceful-close send by forwarding to the async
   * transport::Channel::async_end_sending() on the owned #Owned_channel.  Reminder: Assuming not called
   * after async_end_sending() again: it queues a graceful-close after any other messages sent previously;
   * and executes `on_done_func(E)` once that graceful-close is actually sent into the low-level transport
   * (then `E` is falsy), or a transport error is encountered at any point, including in the past (then `E`
   * is truthy).  Because the owned channel's lifetime is equal to that of `*this`, if `on_done_func()` has
   * not executed when `*this` dtor runs, it will be executed at that time with the usual
   * transport::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER.
   *
   * `on_done_func()` shall, as usual, execute from an unspecified thread that is not among the user calling thread(s);
   * and it shall not execute concurrently to any other handler invoked by `*this`.
   *
   * Formally -- that is all it does, period.  Informally it is important to understand the relationship between
   * this call and the rest of the API which operates at the structured layer, particularly insofar as it relates
   * to the error semantics on that layer.  (Please read the class doc header first.)
   *
   * ### Why and when to use this ###
   * The short answer as to when and how to use it: While optional, the best practice is: If you are done using
   * the channel (for any reason, whether due to error -- as reported via the error handler -- but especially
   * *without* any error), you should call `async_end_sending(F)`, asynchronously wait for `F()` to execute, and in
   * or after `F()` destroy `*this` to free resources.
   *
   * The long answer which hopefully explains the short answer is as follows.  It's long, but the short answer is
   * sufficient arguably.  So read if desired.
   *
   * Let us first consider the case where no `*this` error has occurred so far, meaning the `start()`-passed error
   * handler has not been invoked, and you no longer need the channel.  The immediate thought might be, simply,
   * this means a graceful-close is in order as a matter of decent behavior, which is fair enough; although it
   * would not be wrong to implement your own application-layer message exchange for this (by adding these into
   * your #Msg_body capnp schema and sending it like any other message).  However that is *not*, I (ygoldfel)
   * contend, the main reason `async_end_sending(F)` is a good idea in this context.  That real reason: send() (et al),
   * externally, behaves as-if it is both synchronous and non-blocking yet cannot yield a would-block condition.
   * However, per Native_handle_sender and Blob_sender concept model docs, `send*()` actually may (though this should be
   * rare in practice) be internally asynchronous, in that it may encounter a would-block low-level transport
   * condition (e.g., MQ full, or local-stream-socket would-block), save a copy of the payload, and only succeed in
   * passing it to the out-wire later.  `async_end_sending(F)` can and should be viewed as the async *out-flush*: it
   * ensures all the queued sends have truly succeeded -- or a transport error triggered -- and only *then* executes
   * `F()` asynchronously.  Hence by waiting until `F()` before destroying `*this` you will have ensured that
   * all those care-free synchronous non-blocking sends have truly gone out; therefore `*this` can be destroyed
   * without accidentally cutting off the end of the conversation.
   *
   * Next let's talk about the error cases.  There are roughly 2 as of this writing.  One, a "soft" non-channel-hosing
   * situation can occur, namely information about a local or remote application-protocol error might trigger
   * a handler like those set by set_unexpected_response_handler() and set_remote_unexpected_response_handler().
   * While this may well indicate a serious problem to the user, `*this` does not consider it that way.  It is up
   * to the user to decide what to do when such a "soft" handler executes.  They could do nothing: conceivably they
   * coded in a loose way and consider this not an issue.  Or they could consider this worth ending the conversation.
   * In that case, the situation is not *too* different from the non-error case discussed above.  Want to end `*this`?
   * Flush any sends via `async_end_sending(F)`, then after `F()` destroy `*this`.  At least this features less
   * entropy and is better, or at least not worse, than abruptly destroying the transport::Channel peer (and possibly
   * whole channel).
   *
   * Lastly: Two, an actual fatal error may occur.  This is where there is some subtlety, due to the subtle difference
   * between the unstructured layer (#Owned_channel operation) and structured layer (the protocol in struc::Channel
   * on top of that).  Suppose a channel-hosing error occurs: the `start()`-passed error handler *has* been invoked; by
   * definition this means `*this` can transmit no more *structured* messages in either direction; and in fact all
   * further APIs, with the exception of async_end_sending(), will return `false` (or similar) and no-op.  However:
   * a channel-hosing error is (this is the subtlety!) one of 2 rough types.
   *   -# It might be an unstructured-layer error: the #Owned_channel was trying to send or receive some blob or handle,
   *      on behalf of `*this` internal code, and it encountered a pipe-hosing error E.  E was then immediately
   *      emitted by `*this` via the `start()`-passed error handler.  Most public APIs from that point will no-op
   *      indicating prior pipe-hosing error.
   *   -# It might be a structured-layer error: that is the #Owned_channel is transmitting stuff fine still, but
   *      the logical contents of what has been received is corrupted or unexpected -- maybe the other side misbehaved,
   *      or there's a bug somewhere, maybe auth failed for some reason.  Blobs and handles could continue to be sent,
   *      but `*this` considers that bad, because the logical consistency of its world cannot be guaranteed.
   *      In this case `*this` internally generates an appropriate E (for example
   *      error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_BAD_AUTH) and emits it via the `start()`-passed error handler.
   *      Most public APIs from that point will no-op indicating prior pipe-hosing error.
   *
   * Type 1 is more straightforward to think about.  If a prior send emitted E due to #Owned_channel layer error E,
   * then async_end_sending() will simply emit E as well.  If a prior receive emitted E similarly, then
   * async_end_sending() may re-emit E as well or not; for Native_socket_stream (wherein internally there's a single
   * local-socket connection for both directions despite being full-duplex -- an error in one direction hoses the
   * other direction) yes; for the MQs (wherein the 2 pipes are fully decoupled) no.  That's not all that
   * straightforward already.
   *
   * Type 2 is even less straightforward.  In this case #Owned_channel is fine: so async_end_sending() will yield a
   * successful E most likely -- but meanwhile `*this` is generally behaving as if it's hosed; and indeed it is -- just
   * at the higher (strucuted) layer!
   *
   * The point of all this: It is pointless to worry about how async_end_sending()'s emitted #Error_code relates to
   * what was emitted to the `start()`-passed error handler.  It may be the same thing; it may be different; and it may
   * well even be a falsy (success) #Error_code!  `async_end_sending(F)` -- again -- accomplishes a lower-level task:
   * it waits until any unsent messages are flushed out and then informs user via `F()`; and if this is at any stage
   * impossible (including immediately, due to prior error) then it indicates it right then via `F(E)` as well.
   * *They operate at different layers, even though their error may overlap depending on the situation.*
   * Hence, you should indeed invoke `async_end_sending(F)` from or after the error handler is invoked, then destroy
   * `*this` after `F()`.
   *   - If #Owned_channel out-pipe is hosed anyway, this will simply get to `*this` destruction stage immediately,
   *     which is fine: it's what one would do if they decide to not mess with async_end_sending() at all.
   *   - If #Owned_channel out-pipe is fine (despite something else -- possibly in-pipe, possibly structured protocol --
   *     being not-at-all-fine), this will in civilized fashion flush anything that might still be pending for whatever
   *     reason -- and gracefully inform the other side that, at any rate, this peer does not want to talk anymore.
   *     This reduces entropy; and at worst does not help anything but does not hurt.
   *
   * @todo Consider adding blocking `struc::Channel::sync_end_sending()`, possibly with timeout, since
   * `async_end_sending()` to completion is recommended (but not required) to execute at EOL of any struc::Channel.
   * The "blocking" part is good because it's convenient to not have to worry about handling completion with async
   * semantics boiler-plate.  The "timeout" part is good because it's a best-effort operation, when in a bad/slow
   * environment, and blocking -- even during deinit -- would be important to avoid.  For example,
   * the `Session` impls' dtors perform this op; we don't want those to block for any significant time if at all
   * avoidable.  The reason it's a mere to-do currently is that a bug-free opposing struc::Channel should
   * not let would-block occur for any real length of time; so blocking is presumably unlikely.  Nevertheless.
   *
   * @tparam Task_err
   *         See transport::Channel::async_end_sending().
   * @param on_done_func
   *         See transport::Channel::async_end_sending().
   * @return See transport::Channel::async_end_sending().
   */
  template<typename Task_err>
  bool async_end_sending(Task_err&& on_done_func);

  /**
   * In log-in phase as server only: Registers the expectation of up to 1 *log-in request* in-message whose
   * #Msg_which equals `which`, firing the provided handler asynchronously once the *log-in response* does
   * arrive.  No-op and return `false` if expect_log_in_request() has already been invoked, if log-in phase is
   * not active or active in the client role, or if a prior error has hosed the owned transport::Channel.
   *
   * Informally the proper behavior is:
   *   -# Construct in log-in-as-server phase.
   *   -# Invoke expect_log_in_request().
   *   -# Await `on_log_in_req_func(X&&)` firing, where X is the log-in request.
   *   -# After `on_log_in_req_func(X&&)` handler: check X for correctness (such as process identity checks).
   *      If it fails checks, destroy `*this`; else:
   *   -# Fill out `X = this->create_msg()` (the log-in response) as needed via `X->body_root()`.
   *   -# `send(X)`.  The latter automatically moves `*this` to logged-in phase locally: the bulk of the
   *      API becomes available.
   *
   * @tparam On_msg_handler
   *         See expect_msg().
   * @param which
   *        See expect_msg().
   * @param on_log_in_req_func
   *        See above.
   * @return `true` on success; `false` due to one of the above conditions.
   */
  template<typename On_msg_handler>
  bool expect_log_in_request(Msg_which_in which, On_msg_handler&& on_log_in_req_func);

private:
  // Types.

  /// Short-hand for #m_mutex type.
  using Mutex = flow::util::Mutex_non_recursive;

  /// Short-hand for #Mutex lock.
  using Lock_guard = flow::util::Lock_guard<Mutex>;

  /**
   * State in #m_sync_op_state, when a sync-op is in fact in progress, meaning #m_sync_op_state is not null.
   * See discussion in #m_sync_op_state doc header.
   */
  struct Sync_op_state
  {
    // Types.

    /// ID of each sync operation: a distinct value is used each time a `*this` is created/placed into #m_sync_op_state.
    using id_t = unsigned int;

    /// A successul result of sync_request().
    struct Request_result
    {
      /// Non-null response to out-message with ID Sync_op_state::m_originating_msg_id.
      Msg_in_ptr m_rsp;
    };

    // Data.

    /// See #id_t; used to check an arriving sync op result as belonging to the currently-waiting sync op.
    id_t m_id;

    /**
     * Current state of the in-progress sync-op (as of this writing, there is only sync_request()).
     *   - `monostate`: It is in progress; result unknown.
     *   - `Error_code`: It finished due to pipe-hosing error which is this truthy `Error_code`.
     *   - `Request_result`: It finished due to receiving response matching #m_id; that response is inside.
     * (If more sync-op APIs are added over time, more `variant` types may be added to express results of other
     * ops than sync_request().)
     */
    std::variant<std::monostate, Error_code, Request_result> m_result_if_any;

    /**
     * In-traffic processing thread W, having set #m_result_if_any to non-`monostate`, use
     * `m_op_done_signal.set_value(void)` to signal thread U to wake up from
     * `m_op_done_signal.get_future().wait*()`.  Thread U will then lock #m_mutex and examine the
     * situation:
     *   - If #m_result_if_any is `monostate`: Timeout.
     *   - If #m_result_if_any is `Error_code`: Got result in time, though it was pipe-hosing error.
     *   - If #m_result_if_any is something else: Got successful result in time.
     *
     * It then nullifies the entire Sync_op_state, unlocks mutex, and returns the result (timeout, error, or
     * success payload) from `sync_*()`.
     *
     * Note that #m_op_done_signal is not state, really, but 100% a signaling device.  Its purpose is
     * for thread W to wake thread U instead of waiting until the timeout period elapses, so that it may
     * return the result as quickly as it is available.
     */
    boost::promise<void> m_op_done_signal;
  }; // struct Sync_op_state

  /**
   * Concrete type corresponding to `On_msg_handler` template param: in-message handler.
   * We at times wrap that guy in a `shared_ptr` to minimize having to copy actual function objects -- we can
   * instead copy/move `shared_ptr<On_msg_func>` instead.
   */
  using On_msg_func = Function<void (Msg_in_ptr&& msg)>;

  // Methods.

  /**
   * Implements the one-off expect-message APIs expect_msg() and expect_log_in_request().
   *
   * @tparam MSG_ELSE_LOG_IN
   *         `true` if expect_msg(), `false` is expect_log_in_request().
   * @tparam On_msg_handler
   *         See expect_msg() and buddy.
   * @param which
   *        See expect_msg() and buddy.
   * @param on_msg_func
   *        See expect_msg() and buddy.
   * @return See expect_msg() and buddy.
   */
  template<bool MSG_ELSE_LOG_IN, typename On_msg_handler>
  bool expect_msg_impl(Msg_which_in which, On_msg_handler&& on_msg_func);

  // Data.

  /**
   * Mutex protecting the in-message/out-message state in `*this`.
   *
   * The concurrency design is summarized in the impl section of our class doc header.
   *
   * In thread U, we generally forward to `m_sync_io.same_thing()`; and that `sync_io` core may request
   * an async-wait which we execute as a literaly `m_worker.async_wait(F)`, where `F()` -- from thread W now --
   * calls back into #m_sync_io code.  (Per `sync_io` pattern it's not a literal API call but can be considered one:
   * it is a callback `(*on_active_ev_func)()` which executes code within #m_sync_io.)  Formally speaking
   * sync_io::Channel forbids its APIs (including that quasi-API) being involved concurrently to each
   * other.  So we lock `m_mutex` both in thread U and within the thread-W `F()`.  Informally -- `m_sync_io`
   * accesses many of the same data structures when within `m_sync_io.same_thing()` as within the async-wait
   * handler `on_active_ev_func` it passes to us to async-wait on its behalf.  For example expect_msg() will
   * register a one-off expectation; whereas receiving an unstructured message that is decoded by
   * #m_sync_io as satisfying that expectation will unregister that one-off expectation in `m_sync_io`'s internal map.
   *
   * Moreover we explicitly allow concurrent calls on the same `*this` (except `.sync_request()` concurrently with
   * itself).  All of the above means methods dealing with mutable state shall lock this #m_mutex around most of
   * the code (with the notable exception of the potentially-blocking wait inside sync_request()).
   *
   * ### Perf ###
   * It's similar to such discussions in Native_socket_stream for example.  The additional items here are as follows.
   *   - We operate potentially 2 pipes' logic in the same thread.  Theoretically all else being equal handling
   *     a blob in one could delay handling a blob+possible native handle in the other and thus add latency.  However
   *     that is not really how the #Sync_io_obj uses them: they're really doing stuff in parallel vast majority of
   *     the time.  It uses the native-handle pipe (if any!) only to transmit native handles, basically.  I'll
   *     skip the pontification about why this is unlikely to create much concurrent traffic -- leaving that as
   *     an exercise to the reader.
   *   - Native_socket_stream allows independent -- concurrent -- operation of the out-pipe versus the in-pipe.
   *     E.g., `send_blob()` can run concurrently to receipt of an in-message due to `async_receive_blob()`.
   *     In our case the #Sync_io_obj forbids any such thing, and indeed we use just one #m_mutex.
   *     (Native_socket_stream has 2, one per direction: one in Async_adapter_sender, one in Async_adapter_receiver.)
   *     We cannot really do otherwise.  (Possibly send() -- not async_request() -- could be factored out.  Maybe
   *     look into it.  Not an official to-do yet.)  Does this create lock contention?  It could in limited
   *     circumstances: if the user protocol is not of ping-pong fashion but rather involving heavy downloads and
   *     uploads concurrently.  If this actually presents a problem in practice we could look into
   *     allowing more concurrency in #Sync_io_obj and then make use of it in `*this` by adding more mutexes
   *     and more complex logic.
   */
  mutable Mutex m_mutex;

  /**
   * The current state of the blocking sync-op mechanism; a `variant` that broadly describes the state as:
   * no sync-op in progress, sync-op in progress sans result, sync-op in progress with result that has not yet
   * been returned to user (sync-op almost over).  Please read on.
   *
   * ### Design of blocking sync-op mechanism ###
   * A `sync_*()` operation is one that synchronously awaits in-traffic of some kind and returns some result
   * based on the contents of this in-traffic, or an error if the pipe got hosed during the wait (in which
   * case that error is *not* reported to the regular on-error handler, in the same way that non-blocking send()
   * returns a pipe-hosing error).  In addition it is subject to a (possibly infinite) user-specified timeout;
   * if neither a successful result or an error appears by that time, then that fact is returned by `sync_*()`.
   * After `sync_*()` returns, further in-pipe errors again go to the regular on-error handler until
   * a `sync_*()` op starts (from thread U/W).
   *
   * As of this writing there is only one such op: sync_request().  In this discussion we discuss that one
   * specifically (while mentally considering how it might be generalized in the future; the mechanism is designed
   * hopefully in such a way as to make it not-too-tough to extend to other `sync_*()`s; but we don't want to
   * over-engineer this and/or confuse the reader needlessly).
   *
   * It is a specific documented requirement that sync_request() is never invoked concurrently with itself.  (Also it
   * is explicitly not to be invoked from thread W (i.e., directly from an on-receive handler), as that would
   * delay other handlers' execution; so it is to be invoked from thread U only.)  This means that, broadly,
   * at any point in time for `*this`, there is either no sync_request() running, or there is exactly one
   * running -- though other APIs might be -- and moreover `*this` is safe from being destroyed.
   *
   * sync_request() first performs essentially a normal non-blocking async_request(), and if that succeeds the blocking
   * part begins.  `m_sync_op_state->m_op_done_signal` is initialized to a promise and a future-wait (with timeout)
   * begins in thread U.  Meanwhile in the background `*this` is operating normally,
   * handling all in-traffic.  If it gets a successful result (namely a response
   * to the particular message the nb-send sent -- not some other in-traffic, which is
   * handled normally) while such a wait is in progress in thread U, it loads the result into #m_sync_op_state and
   * tickles thread U to stop waiting via `m_sync_op_done`.  If it gets any pipe-hosing error, it loads that
   * different result and similarly tickles the `promise`.  If the timeout happens first, the promise is destroyed,
   * and more importantly #m_sync_op_state is reset to steady state.
   *
   * (After this, an error is handled normally through on-error handler.  More interestingly, suppose
   * a response to the formerly-expecting-response-but-now-timed-out out-message does arrive later.
   * That out-message ID is still registered in #m_sync_io as expecting a response due to
   * a sync-op; but our special internal handler for that response detects that there is no longer a sync-request in
   * progress, or if there is it is for a different out-message ID -- so it timed out.  So it is just dropped.
   *
   * That's broadly how it works.  Now specifically here are the states in the state machine.  Note this machine is
   * protected by #m_mutex.
   *   - Null (steady state, no sync-op in progress): #m_sync_op_state is empty/null.  sync_request() is not executing,
   *     or is running before or after the blocking part.
   *   - In-progress, no result: #m_sync_op_state is non-null, and its Sync_op_state::m_result_if_any
   *     holds `monostate`: sync_request() is waiting on the promise inside #m_sync_op_state, subject to a timeout,
   *     and no result has yet been received.  (Or it is just about to enter this wait having unlocked mutex,
   *     or it had just exited it and is about to lock mutex.)
   *     - sync_request() has permanently set Sync_op_state::m_id to equal a unique ID and captured this in the
   *       internal response handler, so it can be checked against `m_id` later.
   *   - In-progress, with result: #m_sync_op_state is non-null, and its Sync_op_state::m_result_if_any
   *     holds *not* `monostate` (but rather either a truthy `Error_code` indicating error; or else the response):
   *     sync_request() is as in the previous bullet.  However, since the result is ready, the code
   *     (in thread W) -- having detected the result condition -- will, just before unlocking the mutex
   *     having set the result in Sync_op_state::m_result_if_any, tickle the promise, waking up the wait
   *     in sync_request().
   *     - Suppose sync_request() wins the race by timing out first.  It will then set #m_sync_op_state to null
   *       (and return timeout to caller, all in thread U).
   *       - If an error occurs in thread W subsequently, we'll see there's no sync-op in progress and
   *         report error normally via on-error handler.
   *       - If response to the ID arrives, we'll see there's no sync-op
   *         in progress, or a new one is in progress but with different `m_id`, so it is
   *         a late-arrival.  Then we drop it.
   */
  std::optional<Sync_op_state> m_sync_op_state;

  /// Next unique ID for Sync_op_state::m_id.
  typename Sync_op_state::id_t m_sync_op_state_id;

  /// Analogous to Async_adapter_sender::m_end_sending_on_done_func_or_empty.
  flow::async::Task_asio_err m_snd_end_sending_on_done_func_or_empty;

  /**
   * Thread W used to (1) fire user-provided handlers and (2) perform any async work requested by
   * #m_sync_io.
   */
  flow::async::Single_thread_task_loop m_worker;

  /**
   * The core `struc::Channel` engine, implementing the `sync_io` pattern (see util::sync_io doc header).
   * See our class doc header for overview of how we use it (the aforementioned `sync_io` doc header talks about
   * the `sync_io` pattern generally).
   *
   * Thus, #m_sync_io is the synchronous engine that we use to perform our work in our asynchronous boost.asio
   * loop running in thread W (#m_worker) while collaborating with user thread(s) a/k/a thread U.
   * (Recall that the user may choose to set up their own event loop/thread(s) --
   * boost.asio-based or otherwise -- and use their own equivalent of an #m_sync_io instead.)
   *
   * ### Order subtlety versus `m_worker` ###
   * When constructing #m_sync_io, we need the `Task_engine` from #m_worker.  On the other hand tasks operating
   * in #m_worker access #m_sync_io.  So in destructor it is important to `m_worker.stop()` explicitly, so that
   * the latter is no longer a factor.  Then when automatic destruction occurs in the opposite order of
   * creation, the fact that #m_sync_io is destroyed before #m_worker has no bad effect.
   */
  Sync_io_obj m_sync_io;
}; // class Channel

// Free functions: in *_fwd.hpp.

// Template implementations.

/**
 * Internally used macro; public API users should disregard.
 * @internal
 * Convenience macro for *this* file only: non-inline class template methods are hideous to read (+ write), when there
 * are so many template parameters.  These are `undef`-ed at the end of the file.
 *
 * ### Doxygen notes ###
 * Ideally these would not output Doxygen documentation, but then Doxygen produces a warning which we cannot accept.
 * `cond` + `endcond` Doxygen directives can easily make Doxygen ignore them; but then another problem pops up:
 * It sees the method bodies and gets confused by what it sees as non-existent class name `CLASS_...` and warns about
 * *that*.  The Doxyfile `MACRO_EXPANSION = YES` tag setting eliminates that problem -- but then `cond` + `endcond`
 * around this makes it also ignore the `"#define"` which brings back the latter warning.  Hence, as far as we
 * can tell with Doxygen 1.9.3, no choice but to Doxygen-document these.
 *
 * @todo Look over the rest of the code base -- possibly in Flow too -- and apply the `define TEMPLATE_`
 * + `define CLASS_` technique where it'd be subjectively beneficial.
 */
#define TEMPLATE_STRUCTURED_CHANNEL \
  template<typename Channel_obj, typename Message_body, \
           typename Struct_builder_config, typename Struct_reader_config>
/// See nearby `TEMPLATE_...` macro; same deal here.
#define CLASS_STRUCTURED_CHANNEL \
  Channel<Channel_obj, Message_body, Struct_builder_config, Struct_reader_config>


TEMPLATE_STRUCTURED_CHANNEL
template<typename... Ctor_args>
CLASS_STRUCTURED_CHANNEL::Channel(flow::log::Logger* logger_ptr,
                                  Channel_obj&& channel, Ctor_args&&... ctor_args) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_sync_op_state_id(0),
  m_worker(get_logger(), // Thread W started just below.
           flow::util::ostream_op_string("struct_ch-", channel.nickname())),
  // Create our sync_io core (the thing without threads).
  m_sync_io(get_logger(), std::move(channel), std::forward<Ctor_args>(ctor_args)...)
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;

  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Created from struc::sync_io::Channel core which was "
                "just created (details above).  Async-worker thread shall start.");

  // We do need this before our start(), in case they call async_end_sending() without start() (which is allowed).
  m_worker.start();

  // We're using a boost.asio event loop, so we need to base the async-waited-on handles on our Task_engine.
#ifndef NDEBUG
  bool ok =
#endif
  m_sync_io.replace_event_wait_handles([this]() -> Asio_waitable_native_handle
                                         { return Asio_waitable_native_handle(*(m_worker.task_engine())); });
  assert(ok);

#ifndef NDEBUG
  ok =
#endif
  m_sync_io.start_ops([this](Asio_waitable_native_handle* hndl_of_interest,
                             bool ev_of_interest_snd_else_rcv,
                             Task_ptr&& on_active_ev_func)
  {
    // We are in thread U or thread W.  m_mutex is locked.

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Sync-IO event-wait request: "
                   "descriptor [" << Native_handle(hndl_of_interest->native_handle()) << "], "
                   "writable-else-readable [" << ev_of_interest_snd_else_rcv << "].");

    // They want this async_wait().  Oblige.
    assert(hndl_of_interest);
    hndl_of_interest->async_wait(ev_of_interest_snd_else_rcv
                                   ? Asio_waitable_native_handle::Base::wait_write
                                   : Asio_waitable_native_handle::Base::wait_read,
                                 [this, on_active_ev_func = std::move(on_active_ev_func)]
                                   (const Error_code& err_code)
    {
      // We are in thread W.  Nothing is locked.

      if (err_code == boost::asio::error::operation_aborted)
      {
        return; // Stuff is shutting down.  GTFO.
      }
      // else

      // They want to know about completed async_wait().  Oblige.

      // Protect m_sync_io, m_sync_op_state, m_snd_end_sending_on_done_func_or_empty (at least).
      Lock_guard lock(m_mutex);

      /* Inform m_sync_io of the event.  This can synchronously invoke one of many handlers we have potentially
       * registered via m_sync_io API.  E.g., there could be an incoming structured message user is expecting.  Or
       * it could be the error handler set up in our start(). */

      (*on_active_ev_func)();
      // (That would have logged sufficiently inside m_sync_io; let's not spam further.)
    }); // hndl_of_interest->async_wait()
  });
  assert(ok && "Bug?  If replace_event_wait_handles() worked, start_ops() should too.");
} // Channel::Channel()

TEMPLATE_STRUCTURED_CHANNEL
CLASS_STRUCTURED_CHANNEL::~Channel()
{
  using flow::async::Single_thread_task_loop;
  using flow::util::ostream_op_string;

  // We are in thread U.

  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Shutting down.  "
                "struc::sync_io::Channel shall free various resources soon.  First as needed: our "
                "internal async handlers (at most end-sending handler) will be canceled; "
                "and worker thread will be joined.");

  /* This (1) stop()s the Task_engine thus possibly preventing any more handlers from running at all
   * (any handler possibly running now is the last one to run).  To be clear that's one of these:
   *   - The async_wait() handler from start_ops() (see ctor); if that's running it's deep inside a peer object
   *     inside m_sync_io (which just forwards to its Owned_channel m_channel... black box to us but just saying).
   *     So like for example it might updating its internal cached in-messages not yet emitted to user, because
   *     they haven't expect_msg()ed it yet.
   *   - We've post()ed (onto thread W -- this very Task_engine/Single_thread_task_loop) a user handler for
   *     an expect_msg(), or the error handler given to start(), or response to a send()ed request... etc.
   *     So that guy is running right now then.
   *
   * Anyway, this (1) will await for that to finish -- as-if the dtor was invoked just after.  Moving on:
   * (2) at that point Task_engine::run() exits, hence thread W exits; (3) it joins thread W (waits for it to
   * exit); (4) returns.  That's a lot, but it's non-blocking. */
  m_worker.stop(); // Task_engine can be accessed concurrently without locking.
  // Thread W is (synchronously!) no more.

  /* As we promised in doc header(s), the destruction of *this shall cause any registered completion
   * handlers to be called from some unspecified thread(s) that aren't thread U, and once
   * they've all finished, the dtor returns.  For us there's only possible such one-off handler:
   * from async_end_sending().  Plus... just...
   *
   * ...See comment in similar spot in Native_socket_stream::~Impl() regarding all that/the following.
   *
   * The 2 tasks, summarized are:
   *   - Task_engine::poll(): Like Native_socket_stream::Impl::~Impl() inline code that does that.
   *   - Fire async_end_sending() un-fired handler if any: Like Async_sender_adapter::~dtor() call that does
   *     that.
   *
   * ### Digression for context ###
   * Non-sync_io transport::Native_socket_stream::Impl uses Async_adapter_sender to adapt a
   * sync_io::Native_socket_stream; so ~Async_adapter_sender() has to give operation-aborted to any
   * one-off async-op handlers that are outstanding; and in that guy that is, just like for us,
   * at worst the F() in async_end_sending(F).  Before that, same as for us, queued-up Task_engine handlers
   * may need to be flushed via Task_engine::poll().
   *
   * We, however, don't use Mr. transport::Native_socket_stream or transport::Blob_stream_mq_sender/receiver
   * but only their sync_io cores sync_io::*_stream* -- they would starts their own threads, and per our
   * class doc header impl section we don't want that overhead.  So we maintain thread W ourselves and deal
   * with the sync_io::Native_socket_stream -- and/or sync_io::Blob/Native_handle_sender and/or whatever
   * other `Channel`-supported guys -- via struc::sync_io::Channel m_sync_io (which deals with those
   * sync_io core peer objects via bundling transport::Channel; lots of layers here).
   *
   * My point: As the async-I/O-pattern N_s_s or B_s_mq_sender/receiver each run a thread W and thus each has to
   * take care of invoking un-fired handlers, so similarly must we.  Blob_stream_mq_sender -- same deal.
   * The logic is therefore quite similar. */

  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Continuing shutdown.  Next we will run pending handlers "
                "from some other thread.  In this user thread we will await those handlers' completion and then "
                "return.");

  Single_thread_task_loop one_thread(get_logger(), "struct_ch-temp_deinit");
  one_thread.start([&]()
  {
    FLOW_LOG_INFO("struc::Channel [" << *this << "]: "
                  "In transient finisher thread: Shall run all pending internal handlers (typically none).");

    const auto task_engine = m_worker.task_engine();
    task_engine->restart();
    const auto count = task_engine->poll();
    if (count != 0)
    {
      FLOW_LOG_INFO("Blob_stream_mq_sender [" << *this << "]: "
                    "In transient finisher thread: Ran [" << count << "] internal handlers after all.");
    }
    task_engine->stop();

    if (!m_snd_end_sending_on_done_func_or_empty.empty())
    {
      FLOW_LOG_INFO("struc::Channel [" << *this << "]: In transient snd-finisher thread: "
                    "Shall run pending graceful-sends-close completion handler.");
      m_snd_end_sending_on_done_func_or_empty(transport::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER);
    } // if (!m_snd_end_sending_on_done_func_or_empty.empty())

    FLOW_LOG_INFO("Transient finisher exiting.");
  });
  // Here thread exits/joins synchronously.
} // Channel::~Channel()

TEMPLATE_STRUCTURED_CHANNEL
template<typename Task_err>
bool CLASS_STRUCTURED_CHANNEL::async_end_sending(Task_err&& on_done_func)
{
  /* We are in thread U, or thread W (executing from a handler, possibly concurrently with other APIs).
   * Nothing is locked regardless (see discussion in expect_msg() where we set up that handler). */

  FLOW_LOG_TRACE("Structured channel [" << *this << "]: async_end_sending(F) invoked.");

  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);

  if (!m_snd_end_sending_on_done_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("Structured channel [" << *this << "]: async_end_sending(F) invoked; but we have a saved "
                     "completion handler -- which has not yet fired -- for it already, so it must be a dupe-call.  "
                     "Ignoring.");
    return false;
  }
  /* else: It might also be .empty() and still be a dupe call (if it has fired already -- then they called us again).
   *       m_sync_io.async_end_sending() will catch that fine.  For now though: */

  /* Save it (instead of capturing it in lambda) in case dtor runs before async-end-sending completes;
   * it would then invoke it with operation-aborted code. */
  m_snd_end_sending_on_done_func_or_empty = std::move(on_done_func);

  Error_code sync_err_code;
  const bool ok = m_sync_io.async_end_sending(&sync_err_code,
                                              [this](const Error_code& err_code) mutable
  {
    /* We are in thread W (async_end_sending() specifically promises to *not* invoke on_done_func(), or
     * remember it in any way, if the async-end-sending is synchronously successful -- which it will be most of
     * the time (only not so if an out-peer object is currently in would-block)).  In this case, though,
     * there was would-block, so we are being invoked async-style.
     *
     * m_mutex is locked!
     *
     * Should we post()?  Yes, for the same reason as in expect_msg() and others (see discussion in that method). */
    m_worker.post([this, err_code]() mutable
    {
      // We are in thread W.  Nothing is locked.

      decltype(m_snd_end_sending_on_done_func_or_empty) on_done_func;
      {
        Lock_guard lock(m_mutex); // Accessing m_snd_end_sending_on_done_func_or_empty.

        assert((!m_snd_end_sending_on_done_func_or_empty.empty()) && "Only we or dtor can clear this.  Bug?");

        on_done_func = std::move(m_snd_end_sending_on_done_func_or_empty);
        m_snd_end_sending_on_done_func_or_empty.clear(); // (Just in case move() didn't do it.)

        // Now dtor won't call it with operation-aborted.

        /* Unlock before invoking handler!  They can call any number of mutex-locking stuff: expect_msg(),
         * even async_end_sending() again, if they're weird people who like to do odd things. */
      } // Lock_guard lock(m_mutex);

      // This should be rare, so (another) INFO message seems okay.
      FLOW_LOG_INFO("struc::Channel [" << *this << "]: Now executing async-end-sending completion handler: "
                    "async path.");
      on_done_func(err_code);
      FLOW_LOG_TRACE("Handler completed.");
    });
  }); // ok = m_sync_io.async_end_sending() (async handler path)

  if (!ok)
  {
    // False start (dupe call).  It logged enough.
    m_snd_end_sending_on_done_func_or_empty.clear();
    return false;
  }
  // else: It either finished synchronously; or it will finish asynchronously.

  if (sync_err_code == transport::error::Code::S_SYNC_IO_WOULD_BLOCK) // It TRACE-logged enough about that.
  {
    // Async-wait needed before we can complete.  Live to fight another day.
    return true;
  }
  // else

  // We are in thread U, possibly (probably).  Never fire handlers except from thread W.
  m_worker.post([this, sync_err_code]()
  {
    // We are in thread W.  Nothing is locked.  Similar to above; keeping comments light.  @todo Bit of code reuse.

    decltype(m_snd_end_sending_on_done_func_or_empty) on_done_func;
    {
      Lock_guard lock(m_mutex); // Accessing m_snd_end_sending_on_done_func_or_empty.
      assert((!m_snd_end_sending_on_done_func_or_empty.empty()) && "Only we or dtor can clear this.  Bug?");

      on_done_func = std::move(m_snd_end_sending_on_done_func_or_empty);
      m_snd_end_sending_on_done_func_or_empty.clear(); // (Just in case move() didn't do it.)
    }
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Now executing async-end-sending completion handler: "
                   "sync path.");
    on_done_func(sync_err_code);
    FLOW_LOG_TRACE("Handler completed.");
  });

  return true;
} // Channel::async_end_sending()

TEMPLATE_STRUCTURED_CHANNEL
template<typename Task_err>
bool CLASS_STRUCTURED_CHANNEL::start(Task_err&& on_err_func)
{
  using flow::util::Blob;
  using boost::movelib::make_unique;
  using std::holds_alternative;
  using std::get;
  using std::monostate;

  // We are in thread U.  Nothing is locked.

  /* Reminder: we must protect against concurrent calls from user threads U1, U2, ... (and against access from W
   * though that could not happen before start() -- is). */
  Lock_guard lock(m_mutex);

  return m_sync_io.start_and_poll([this, on_err_func = std::move(on_err_func)]
                                    (const Error_code& err_code) mutable
  {
    /* Though somewhat unlikely (but definitely possible -- there could be an error on the socket/whatever) --
     * start_and_poll() contract says any relevant handler -- including on_err_func() -- may fire *synchronously*.
     * This is the only such API of (our) m_sync_io.  We however promise to never do that (must always use thread W).
     * So for that we have to post().
     *
     * Otherwise (if indeed we are already in thread W, executing from inside the async_wait() handler as seen inside
     * ctor's start_ops() statement, with m_mutex locked even) this adds a small, unnecessary delay -- no big deal
     * generally and definitely no big deal in this situation.  (We could detect whether it fired synchronously and
     * avoid the post() if not; but it's not worth the effort.) */
    m_worker.post([this, err_code, on_err_func = std::move(on_err_func)]() mutable
    {
      /* We are in thread W.  Nothing is locked!
       *
       * So, if not for a possible sync_request(), we'd literally just do: on_err_func(err_code).  However suppose
       * a sync_request() is currently executing in thread U.  (In the future we could add other sync_*(): E.g.,
       * sync_<wait for a particular message a-la expect_msg()>.)  In that case we must emit the error to the sync_*()
       * waiting for a result instead of the on-error handler.  That is our overall contract in *this: Report a
       * channel-hosing error exactly once (if ever): either through a send() or sync_request(); or through
       * this error reporting handler -- whichever one happens first. */

      {
        Lock_guard lock(m_mutex);

        /* Check that (obviously) there is a sync-op in progress; but also (carefully) ensure it does not have a result
         * stored already.  To show this is technically possible imagine this scenario: sync_request() is waiting in
         * thread U; the expected response was received in time and thus placed into m_sync_op_state->m_result_if_any,
         * and promise m_sync_op_state->m_op_done_signal was tickled to wake up thread U's future.wait_for();
         * but thread U hasn't yet woken up and nullified m_sync_op_state.  So now we let that sync_request() return
         * success; we then emit the error asynchronously.  Basically it's almost as-if the error occurred just after
         * the successful sync_request(). */
        if (m_sync_op_state)
        {
          assert((!holds_alternative<Error_code>(m_sync_op_state->m_result_if_any))
                 && "Only a previous error-handler would have set that to an Error_code; and m_sync_io shall not "
                    "emit error more than once.");
          if (holds_alternative<monostate>(m_sync_op_state->m_result_if_any))
          {
            FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Sync-IO core reported error (details above); "
                           "this occurred while awaiting sync-op (ID [" << m_sync_op_state->m_id << "]) completion.  "
                           "Therefore we emit the error to that guy, loading that result and waking the waiting guy.");

            m_sync_op_state->m_result_if_any = err_code;
            m_sync_op_state->m_op_done_signal.set_value();
            return;
          }
          // else { sync_*() already received a success result; so we let that happen. }
        } // if (m_sync_op_state)
        // else if (!m_sync_op_state) { No sync_*() in progress. }
      } // Lock_guard lock(m_mutex)

      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Calling error handler from async-worker thread.");
      on_err_func(err_code);
      FLOW_LOG_TRACE("Handler completed.");
    }); // m_worker.post()
  }); // m_sync_io.post()
} // Channel::start()

TEMPLATE_STRUCTURED_CHANNEL
typename Heap_fixed_builder::Config // Static.
  CLASS_STRUCTURED_CHANNEL::heap_fixed_builder_config(const Owned_channel& channel)
{
  /* This (and similar below) could have been in sync_io::Channel instead.  Doesn't really matter.
   * I just figured one place for these special static guys is enough, and if we pick one it should be
   * in the higher namespace most users are likelier to use. */

  assert(channel.initialized()
         && "Method contract is that the tested `channel` is .initialized().");

  /* The assumption is they plan to create a Channel like us, passing `channel` into ctor,
   * with Struct_builder_config being Heap_fixed_builder::Config or similar.  They
   * want a Config they can pass to our ctor.  So we need to know what to pass to its ctor.
   * We have logger_ptr already.  So we need:
   *   - segment_sz: Max size of each segment in the serialization of the structured message itself.
   *   - frame_*_sz: We don't need to use this feature.  Leave at zero.
   *
   * And this must fit into the max sendable blob (send_blob()) or meta-blob (send_native_handle()).
   * And if we might invoke either one, then formally speaking we must report the lower of the two.  So let's say
   * that's M.  Hence:
   *   frame_*_sz = 0
   *   segment_sz = M */

  size_t sz;
  if constexpr(Owned_channel::S_HAS_BLOB_PIPE_ONLY)
  {
    // Only send_native_handle() shall be used.  See send().
    sz = channel.send_blob_max_size();
  }
  else if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    // Only send_blob() shall be used.  See send().
    sz = channel.send_meta_blob_max_size();
  }
  else
  {
    static_assert(Owned_channel::S_HAS_2_PIPES, "This code assumes 1 blobs pipe, 1 handles pipe, or both exactly.");
    // Either one may be used.  See send().
    sz = std::min(channel.send_meta_blob_max_size(), channel.send_blob_max_size());
  }

  return Heap_fixed_builder::Config{ channel.get_logger(), sz, 0, 0 };
} // Channel::heap_fixed_builder_config()

TEMPLATE_STRUCTURED_CHANNEL
typename Heap_reader::Config // Static.
  CLASS_STRUCTURED_CHANNEL::heap_reader_config(const Owned_channel& channel)
{
  return Heap_reader::Config{ channel.get_logger(), 0 };
}

TEMPLATE_STRUCTURED_CHANNEL
const typename CLASS_STRUCTURED_CHANNEL::Builder_config&
  CLASS_STRUCTURED_CHANNEL::struct_builder_config() const
{
  // Never changes; no need to lock.
  return m_sync_io.struct_builder_config();
}

TEMPLATE_STRUCTURED_CHANNEL
const typename CLASS_STRUCTURED_CHANNEL::Builder_config::Builder::Session&
  CLASS_STRUCTURED_CHANNEL::struct_lender_session() const
{
  // Never changes; no need to lock.
  return m_sync_io.struct_lender_session();
}

TEMPLATE_STRUCTURED_CHANNEL
const typename CLASS_STRUCTURED_CHANNEL::Reader_config&
  CLASS_STRUCTURED_CHANNEL::struct_reader_config() const
{
  // Never changes; no need to lock.
  return m_sync_io.struct_reader_config();
}

TEMPLATE_STRUCTURED_CHANNEL
typename CLASS_STRUCTURED_CHANNEL::Msg_out CLASS_STRUCTURED_CHANNEL::create_msg(Native_handle&& hndl_or_null) const
{
  // Always safe per contract.
  return m_sync_io.create_msg(std::move(hndl_or_null));
}

TEMPLATE_STRUCTURED_CHANNEL
template<typename On_msg_handler>
bool CLASS_STRUCTURED_CHANNEL::expect_msg(Msg_which_in which, On_msg_handler&& on_msg_func)
{
  return expect_msg_impl<true, On_msg_handler>(which, std::move(on_msg_func));
}

TEMPLATE_STRUCTURED_CHANNEL
template<typename On_msg_handler>
bool CLASS_STRUCTURED_CHANNEL::expect_log_in_request(Msg_which_in which, On_msg_handler&& on_log_in_req_func)
{
  return expect_msg_impl<false, On_msg_handler>(which, std::move(on_log_in_req_func));
}

TEMPLATE_STRUCTURED_CHANNEL
template<bool MSG_ELSE_LOG_IN, typename On_msg_handler>
bool CLASS_STRUCTURED_CHANNEL::expect_msg_impl(Msg_which_in which, On_msg_handler&& on_msg_func)
{
  using boost::make_shared;

  /* We are in thread U or thread W (if invoked from a user handler).
   * Nothing is locked either way due to how we actually invoke user-supplied handlers (which we are about
   * discuss). */

  /* Wrap (a moved version of) their function object in a shared_ptr<Function<>>.  (Perf-wise, ultimately
   * m_sync_io will do this anyway; we probably aren't adding much churn with this op alone.)
   * Why?  Answer: Unfortunately we have to first pass on_msg_func to m_sync_io.expect_*(); and then possibly
   * also invoke it ourselves -- if message(s) immediately available synchronously.  So we can't
   * std::move(on_msg_func), as it'll get blown away (its captures will at least) before we can use it
   * synchronously.  We could capture a copy of on_msg_func, but that is slow: Capturing a copy of a shared_ptr<>
   * is muuuuuch faster. */
  auto on_msg_func_ptr = make_shared<On_msg_func>(std::move(on_msg_func));

  // See below first regarding the purpose of this guy.  Then come back here.
  auto real_on_msg_func = [this, on_msg_func_ptr](Msg_in_ptr&& msg_in) mutable
  {
    // We are in thread W.  m_mutex is locked!

    /* Now comes a key decision: We are in thread W.  We must call user handler on_msg_func().  Thread W isn't
     * thread U: it isn't the user thread.  So it would appear we could just straight-up synchronously
     * `on_msg_func(std::move(msg_in))`.  Yet instead we post() onto thread W again -- and do it there.  Why?
     * Answer:
     *
     * Firstly, just technically speaking, m_mutex is locked (we are right now down the call stack from
     * async_wait() handler inside start_ops() statement inside our ctor: it locks m_mutex before calling
     * (*on_active_ev_func)().  So, it being a non-recursive mutex, if they were to call expect_msg(), or
     * send(), or anything else like that -- which they are explicitly allowed to do -- inside such a call
     * we'd deadlock right inside the hypothetical synchronous on_msg_func().
     *
     * However if we post() that onto thread W, then everything is cool.  So at least we know it's safe.
     * Is it performant?  Before trying to answer that let's consider the alternative.
     *
     * Sure: we could make m_mutex recursive.  Or we could not-lock it inside our various APIs (including the
     * expect_msg() you're reading now -- albeit the async-handler inside it), which would be sort-of a user-space
     * recursive mutex action.  So why not?  My (ygoldfel) answer is arguably hand-wavy, but it's 2-fold.
     * 1, in every instance of my career where I tried to or saw someone tried to solve a deadlocking algorithm
     * by making a recursive mutex, it was the wrong move that was reversed soon enough.  Also generally designs
     * with recursive mutexes that at least appeared to work seemed to do so out of a desire to avoid a few lines
     * of boiler-plate (_impl() methods that had mutex-lock as a pre-condition), in code of quite limited size --
     * not a complex state machine like we have here.  2, while at this exact moment I cannot think of a way
     * that allowing recursive calls into m_sync_io.*() API could lead to algorithmic mayhem/bugs, it is even for me
     * exceedingly difficult to convince myself of that.  It does seem like probably as of this writing Sync_io_obj
     * is pretty clean as far as its algorithm not breaking down due to a handler calling back into its own API -- but
     * will that always be the case?  Will it be reasonable to have to maintain that logic?  It's mind-breaking.
     *
     * So is the alternative a good idea?  Probably not.  I'd like to prove it beyond a doubt but at the moment can't.
     * At the very least it is quite difficult to prove that it's *safe*, as it's so tough to reason-about due to
     * the flow control layers involved.
     *
     * Okay, so then, back to post()ing it onto thread W.  We know it's safe.  Is it performant?  The question really
     * applies to the (presumably common) case where the user, in fact, in on_msg_func() does *not* call back into
     * `*this` API -- they are after all encouraged in our class doc header to keep themselves sane by post()ing
     * their true handling onto their thread U (or whatever their equivalent of post()ing is).  So suppose, indeed,
     * their on_msg_func = { <their loop>.post([msg_in = std::move(msg_in)]() { ...handle it...}); }.
     * Indeed we're adding another m_worker.post() between now and their "...handle it..." actually executing; whereas
     * in *this* particular scenario it would've been perfectly safe to just, like, not.
     *
     * So is that delay okay?  In absolute terms I would say... for now at least... yes.  It'll add a few usec of
     * latency, as boost.asio winds up the stack into Task_engine::run(), then re-enters the post()ed handler of ours
     * and "finally" runs on_msg_func().  It's boost.asio flow control: it is common in boost.asio/async algorithms
     * as the technique to combat "recursive mayhem."
     *
     * @todo It is worth revisiting; possibly profiling. */

    m_worker.post([this, on_msg_func_ptr = std::move(on_msg_func_ptr), msg_in = std::move(msg_in)]() mutable
    {
      // We are in thread W.  Nothing is locked.
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking user on-expect-msg handler (async path).");
      (*on_msg_func_ptr)(std::move(msg_in));
      FLOW_LOG_TRACE("Handler completed.");
    });
  }; // auto real_on_msg_func =

  Msg_in_ptr qd_msg;
  bool ok;

  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);

  if constexpr(MSG_ELSE_LOG_IN)
  {
    ok = m_sync_io.expect_msg(which, &qd_msg, std::move(real_on_msg_func));
  }
  else
  {
    ok = m_sync_io.expect_log_in_request(which, &qd_msg, std::move(real_on_msg_func));
  }

  if (!ok)
  {
    return false;
  }
  // else:

  if (qd_msg)
  {
    // Message expectation immediately met.  Report it immediately via thread-W (as we always must).
    m_worker.post([this, on_msg_func_ptr = std::move(on_msg_func_ptr), msg_in = std::move(qd_msg)]() mutable
    {
      // We are in thread W.  Nothing is locked.
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking user on-expect-msg handler (sync path).");
      (*on_msg_func_ptr)(std::move(msg_in));
      FLOW_LOG_TRACE("Handler completed.");
    });
  }
  // else { It'll fire, if ever, asynchronously. }

  return true;
} // Channel::expect_msg_impl()

TEMPLATE_STRUCTURED_CHANNEL
template<typename On_msg_handler>
bool CLASS_STRUCTURED_CHANNEL::expect_msgs(Msg_which_in which, On_msg_handler&& on_msg_func)
{
  using boost::make_shared;

  /* We are in thread U; or thread W (if invoked from a user handler); always serially.
   * Nothing is locked either way due to how we actually invoke user-supplied handlers. */

  // Wrap (a moved version of) their function object in a shared_ptr<Function<>>.  See explanation in expect_msg_impl().
  auto on_msg_func_ptr = make_shared<On_msg_func>(std::move(on_msg_func));

  auto real_on_msg_func = [this, on_msg_func_ptr](Msg_in_ptr&& msg_in)
  {
    // We are in thread W.  m_mutex is locked!

    /* *Please* see comment in expect_msg_impl() similar spot regarding why we post() here.
     *
     * Orthogonally: The function where this comment sits may be executed 2+ times, unlike expect_msg_impl() which
     * is a one-off message expectation.  So we cannot move(on_msg_func_ptr) -- it'll get nullified the first time,
     * and you're screwed.  In expect_msg() we can though for a bit of extra perf. */
    m_worker.post([this, on_msg_func_ptr, msg_in = std::move(msg_in)]() mutable
    {
      // We are in thread W.  Nothing is locked.
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking user on-expect-msgs handler (async path).");
      (*on_msg_func_ptr)(std::move(msg_in));
      FLOW_LOG_TRACE("Handler completed.");
    });
  };

  typename Sync_io_obj::Msgs_in qd_msgs;

  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);

  if (!m_sync_io.expect_msgs(which, &qd_msgs, std::move(real_on_msg_func)))
  {
    return false;
  }
  // else:

  if (qd_msgs.empty())
  {
    return true; // They may fire asynchronously.  Nothing for now.
  }
  // else

  // Message expectation immediately met.  Report these immediately via thread-W (as we always must).
  m_worker.post([this, on_msg_func_ptr = std::move(on_msg_func_ptr), qd_msgs = std::move(qd_msgs)]() mutable
  {
    // We are in thread W.  Nothing is locked.

    size_t idx = 0;
    for (auto& msg_in : qd_msgs)
    {
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking user on-expect-msg handler (sync path): "
                     "handler [" << idx << "] (0-based) of [" << qd_msgs.size() << "] (1-based).");
      (*on_msg_func_ptr)(std::move(msg_in));

      ++idx;
    }
    FLOW_LOG_TRACE("Handlers completed.");
  });

  // Plus more may fire asynchronously.

  return true;
} // Channel::expect_msgs()

TEMPLATE_STRUCTURED_CHANNEL
bool CLASS_STRUCTURED_CHANNEL::undo_expect_msgs(Msg_which_in which)
{
  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);
  return m_sync_io.undo_expect_msgs(which);
}

TEMPLATE_STRUCTURED_CHANNEL
bool CLASS_STRUCTURED_CHANNEL::undo_expect_responses(msg_id_out_t originating_msg_id)
{
  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);
  return m_sync_io.undo_expect_responses(originating_msg_id);
}

TEMPLATE_STRUCTURED_CHANNEL
bool CLASS_STRUCTURED_CHANNEL::send(const Msg_out& msg, const Msg_in* originating_msg_or_null, Error_code* err_code)
{
  Lock_guard lock(m_mutex);
  return m_sync_io.send(msg, originating_msg_or_null, err_code);
}

TEMPLATE_STRUCTURED_CHANNEL
typename CLASS_STRUCTURED_CHANNEL::Msg_in_ptr
  CLASS_STRUCTURED_CHANNEL::sync_request(const Msg_out& msg, const Msg_in* originating_msg_or_null,
                                         Error_code* err_code)
{
  return sync_request(msg, originating_msg_or_null, util::Fine_duration::max(), err_code);
}

TEMPLATE_STRUCTURED_CHANNEL
template<typename On_msg_handler>
bool CLASS_STRUCTURED_CHANNEL::async_request
       (const Msg_out& msg, const Msg_in* originating_msg_or_null,
        msg_id_out_t* id_unless_one_off, On_msg_handler&& on_rsp_func, Error_code* err_code)
{
  using boost::make_shared;

  /* We are in thread U; or thread W (if invoked from a user handler).
   * Nothing is locked either way due to how we actually invoke user-supplied handlers. */

  /* This is one of the 2 interesting send methods (the other being sync_request() which is a whole other thing).
   * That is we can't just forward to m_sync_io.async_request() all the way, tempting as it is.  The reason is the same
   * as in expect_msg_impl() and expects_msgs() (at least): if we just forward on_rsp_func, it will run in thread W
   * as desired, but m_mutex will be locked already... and... therefore just *please* see the comment about why
   * we post(), in expect_msg_impl().  Same deal here.  (We should add: if for some reason we crazily decided to
   * post() in (e.g.) expect_msg_impl() but not here, then handlers could fire out of order (and did, in testing
   * before fixing it): A successful async-firing expect_msg_impl() post()s the true user handler onto W; then
   * say a response to this async_request() is already in the in-pipe, so m_sync_io reads it and therefore executes
   * on_rsp_func() synchronously.  Only then would the post()ed notification (expect_msg_impl()) handler run, once we
   * got out of the boost.asio task.  That would be a bug, even supposing we didn't run into m_mutex deadlock (which
   * would only occur if they actually tried to use a *this API from directly in on_rsp_func().  I digress though.)
   *
   * The other thing to take care of is is the non-one-off mode of this async_request(): It can fire multiple times; so
   * either (as in expect_msgs()) we either have to make copies of on_rsp_func and all its captures (expensive) or
   * move()it, but move()ing it would blow away the captures and be a bug.  m_sync_io, like our own expect_msgs(),
   * deals with the same thing internally.  So all 3 piece of code do the same thing: wrap on_rsp_func into
   * a shared_ptr, so that we copy `shared_ptr`s around -- cheap compared to copying function objects.
   *
   * Don't bother complicating the code by not-shared_ptr-wrapping in one-off mode.  In addition to being more
   * complex, there's no proof that doing 2 move(on_rsp_func)s is faster than 1 such move() + some shared_ptr
   * transfer(s). */

  auto real_on_rsp_func = [this,
                           on_rsp_func_ptr = make_shared<On_msg_func>(std::move(on_rsp_func))] // Per 2nd para above.
                             (Msg_in_ptr&& msg_in)
  {
    // We are in thread W.  m_mutex is locked!

    // @todo Could save a few cycles by doing `on_rsp_func_ptr = move(on_rsp_func_ptr)` instead if one-off mode.
    m_worker.post([this, on_rsp_func_ptr, msg_in = std::move(msg_in)]() mutable
    {
      // We are in thread W.  Nothing is locked.
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking user on-rsp handler.");
      (*on_rsp_func_ptr)(std::move(msg_in));
      FLOW_LOG_TRACE("Handler completed.");
    });
  }; // auto real_on_rsp_func()

  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);
  return m_sync_io.async_request(msg, originating_msg_or_null,
                                 id_unless_one_off, std::move(real_on_rsp_func), err_code);
} // Channel::async_request()

TEMPLATE_STRUCTURED_CHANNEL
typename CLASS_STRUCTURED_CHANNEL::Msg_in_ptr
  CLASS_STRUCTURED_CHANNEL::sync_request(const Msg_out& msg, const Msg_in* originating_msg_or_null,
                                         util::Fine_duration timeout, Error_code* err_code)
{
  using Request_result = typename Sync_op_state::Request_result;
  using util::Fine_duration;
  using boost::promise;
  using boost::chrono::round;
  using boost::chrono::milliseconds;
  using boost::unique_future;
  using std::holds_alternative;
  using std::get;
  using std::monostate;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(Msg_in_ptr, sync_request, msg, originating_msg_or_null, timeout, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // We are in thread U (not W, by contract, as blocking thread W can delay subsequent handler firing).

  // We comment inline but see m_sync_op_state doc header for overview.

  unique_future<void> done_future;
  bool send_tried;
  typename Sync_op_state::id_t id;
  {
    // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
    Lock_guard lock(m_mutex); // Protect m_sync_io, m_sync_op_state....

    assert((!m_sync_op_state) && "Concurrently invoking sync_request() 2x?  Not allowed.");

    /* Set up the state for this sync-op.  Since from the moment m_sync_io.async_request() (below) successfully returns
     * technically a response can immediately arrive in thread W we need to do this before that async_request(). */

    m_sync_op_state = { id = m_sync_op_state_id++,
                        {}, // Start at default-cted variant (monostate).
                        {} }; // New `promise`.
    done_future = m_sync_op_state->m_op_done_signal.get_future();

    // (Do not use this->async_request() -- it would re-lock m_mutex.)
    send_tried = m_sync_io.async_request(msg, originating_msg_or_null, nullptr, // One-off response expectation.
                                         [this, id](Msg_in_ptr&& msg_in)
    {
      // We are in thread W.  m_mutex is locked!

      /* id was generated by sync_request() (see above).  And now a response to it is here:
       * internally m_sync_io has unregistered the response expectation, as it is a one-off.
       * Now we just need to load the result and tickle the waiting sync_request() in thread U.
       * That is unless it has already timed out. */
      promise<void> op_done_signal;

      /* Ensure we won the race against:
       *   - timeout (sync_request() itself, see timeout handling below);
       *   - incoming-direction error (see start()).
       * If timeout happened, either !m_sync_op_state; or a new m_sync_op_state exists because of an even later
       * sync_request().  If error happened, and sync_request() hasn't fished it out yet, then the result will
       * already be an Error_code. */
      if ((!m_sync_op_state) || (m_sync_op_state->m_id != id)
          || holds_alternative<Error_code>(m_sync_op_state->m_result_if_any))
      {
        /* @todo Barring in-direction error occurring earlier -- not even sure that is really possible -- in case
         * of timeout:
         *
         * We could arrange something where we fire the on-unexpected-response handler locally and
         * send internal message to opposing guy, so they can fire their on-remote-unexpected-response handler/
         * It would require more API for m_sync_io and interacting with it though.  Thing is, before sync_io
         * pattern existed, this Channel was monolithic and could more easily do it by itself
         * (everything else was collectively more complicated but I digress).  So it did do it.  So this is a small
         * regression from that.  We could un-regress it with extra work.  At the moment we just drop the too-late
         * response.  At least the in-message details from m_sync_io would be TRACE/DATA-logged already. */

        FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Received response to sync-request "
                         "(ID [" << id << "]) which is expecting the sync-request "
                         "response; however that sync-request has timed out already (or somehow an error was "
                         "detected in incoming-direction processing previously).  Assuming timeout: This is an "
                         "error on the user's part and thus not fatal.  We ignore the response.  Its details "
                         "should be logged (TRACE/DATA) above.  The ongoing sync-request has ID (0 if N/A): "
                         "[" << (m_sync_op_state ? m_sync_op_state->m_id : 0) << "].");
        return;
      }
      // else: Yay, they're expecting this; let's give it to 'em and tickle them to wake up.

      assert(holds_alternative<monostate>(m_sync_op_state->m_result_if_any)
             && "Only we can set the result, unless an error occurred, but we eliminated that above.");

      m_sync_op_state->m_result_if_any = Request_result{ std::move(msg_in) };

      /* All that's left is to wake up the future.wait_for() that's waiting on op_done_signal.  They might be
       * (unlikely but possible) waking right now anyway.   */
      m_sync_op_state->m_op_done_signal.set_value();
    }, err_code); // send_tried = m_sync_io.async_request()

    if ((!send_tried) || (*err_code))
    {
      // .async_request() logged enough.

      // The nb-send failed for whatever reason; clean this up (though this state is irrelevant but just for neatness).
      m_sync_op_state.reset();

      if (!send_tried)
      {
        /* Technically m_sync_io.async_request() need not touch err_code, if it returned `false` (never tried
         * to actually send). Our contract however specifically says null return + success *err_code =
         * similar to async_request()==false. */
        err_code->clear();
      }
      // else { *err_code is truthy. }

      return Msg_in_ptr();
    } // if ((!send_tried) || (*err_code))
    // else: m_sync_io.async_request() succeeded -- we're off to the races.
  } // Lock_guard lock(m_mutex); // We promised in doc header to not block concurrent non-blocking calls!

  const bool no_timeout = timeout == Fine_duration::max();
  if (no_timeout)
  {
    FLOW_LOG_TRACE("Structured channel [" << *this << "]: "
                   "Sync-request (no timeout): The nb-send succeeded; "
                   "now we await either error or successful response receipt; sync-op ID = "
                   "[" << id << "].");

  }
  else
  {
    FLOW_LOG_TRACE("Structured channel [" << *this << "]: "
                   "Sync-request with timeout [" << round<milliseconds>(timeout) << "]: The nb-send succeeded; "
                   "now we await either timeout or error or successful response receipt; sync-op ID = "
                   "[" << id << "].");
  }

  /* Now sleep until either m_op_done_signal is tickled by thread W, or the timeout passes.
   * Note that we don't, per-se, care for which of those reasons the sleep ends, because the `promise` is
   * no more than a signal to wake us, if we don't wake first: What matters is the result in
   * m_sync_op_state by the time we lock m_mutex just below.  It is even possible, albeit unlikely,
   * that we wake up due to the timeout, yet between now and just below thread W loads a result
   * into m_sync_op_state after all.  In that case we might as well "forgive" the timeout and still use the
   * result.  So that's why we ignore the return value of the .wait_for().
   *
   * Also we use wait() if specifically asked to not be subject to timeout.  The rationale for this semantic,
   * rather than just passing a very high value to wait_for(), is given in our doc header (`timeout` arg).
   * TL;DR: wait_for() barfs on on very high `Fine_duration`s due to Linux pthread_timed_condwait() subtleties. */
  if (no_timeout)
  {
    done_future.wait();
  }
  else
  {
    done_future.wait_for(timeout);
  }

  // The sleep ended either due to tickling of promise or timeout; scan the result once we lock mutex.

  Lock_guard lock(m_mutex);

  assert(m_sync_op_state
         && "Only this method itself can synchronously nullify m_sync_op_state.");
  auto& result = m_sync_op_state->m_result_if_any;

  if (holds_alternative<monostate>(result))
  {
    FLOW_LOG_WARNING("Structured channel [" << *this << "]: "
                     "Sync-request: The nb-send succeeded; "
                     "but no response arrived in time; reporting timeout to user synchronously.");

    *err_code = transport::error::Code::S_TIMEOUT;

    m_sync_op_state.reset();
    return Msg_in_ptr();
  }
  // else
  if (holds_alternative<Error_code>(result))
  {
    // m_sync_io.async_request() did succeed.  But the sync-op failed:
    *err_code = get<Error_code>(result);

    FLOW_LOG_WARNING("Structured channel [" << *this << "]: "
                     "Sync-request (id [" << id << "]): The nb-send succeeded; "
                     "and the sync-request finished in time; but the result was pipe-hosing error "
                     "[" << *err_code << "] [" << err_code->message() << "]; reporting this result to user "
                     "synchronously.");

    m_sync_op_state.reset();
    return Msg_in_ptr();
  }
  // else

  assert(holds_alternative<Request_result>(result));

  FLOW_LOG_TRACE("Structured channel [" << *this << "]: "
                 "Sync-request (id [" << id << "]): The nb-send succeeded; "
                 "and the sync-request finished in time with response having been received (details above); "
                 "reporting this result to user synchronously.");

  const auto rsp = std::move(get<Request_result>(result).m_rsp);
  // Might as well move --^-- the shared_ptr outta there for perf.

  m_sync_op_state.reset();

  assert(!*err_code);
  return rsp;
} // Channel::sync_request()

TEMPLATE_STRUCTURED_CHANNEL
template<typename On_unexpected_response_handler>
bool CLASS_STRUCTURED_CHANNEL::set_unexpected_response_handler(On_unexpected_response_handler&& on_func)
{
  /* We are in thread U; or thread W (if invoked from a user handler); always serially.
   * Nothing is locked either way due to how we actually invoke user-supplied handlers. */

  auto real_on_func = [this, on_func = std::move(on_func)](Msg_in_ptr&& msg_in) mutable
  {
    // We are in thread W.  m_mutex is locked!

    /* *Please* see comment in expect_msg_impl() similar spot regarding why we post() here.
     *
     * Orthogonally: We don't use the shared_ptr<> trick to avoid copying on_func.  We could, but it doesn't
     * seem perf-critical, given that it's an unexpected-response handler -- doubt people will be having these
     * fire constantly.  @todo Probably should still though do it though.  It's a few lines (see expect_msg*()). */
    m_worker.post([this, on_func, msg_in = std::move(msg_in)]() mutable
    {
      // We are in thread W.  Nothing is locked.
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking user on-unexpected-rsp handler.");
      on_func(std::move(msg_in));
      FLOW_LOG_TRACE("Handler completed.");
    });
  };

  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);
  return m_sync_io.set_unexpected_response_handler(std::move(real_on_func));
} // Channel::set_unexpected_response_handler()

TEMPLATE_STRUCTURED_CHANNEL
bool CLASS_STRUCTURED_CHANNEL::unset_unexpected_response_handler()
{
  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);
  return m_sync_io.unset_unexpected_response_handler();
}

TEMPLATE_STRUCTURED_CHANNEL
template<typename On_remote_unexpected_response_handler>
bool CLASS_STRUCTURED_CHANNEL::set_remote_unexpected_response_handler(On_remote_unexpected_response_handler&& on_func)
{
  using std::string;

  /* We are in thread U; or thread W (if invoked from a user handler); always serially.
   * Nothing is locked either way due to how we actually invoke user-supplied handlers. */

  auto real_on_func = [this, on_func = std::move(on_func)]
                        (msg_id_out_t msg_id_out, string&& msg_metadata_text) mutable
  {
    // We are in thread W.  m_mutex is locked!

    /* *Please* see comment in expect_msg_impl() similar spot regarding why we post() here.
     *
     * Orthogonally: We don't use the shared_ptr<> trick to avoid copying on_func.  We could, but it doesn't
     * seem perf-critical, given that it's an unexpected-response handler -- doubt people will be having these
     * fire constantly.  @todo Probably should still though do it though.  It's a few lines (see expect_msg*()). */
    m_worker.post([this, on_func, msg_id_out, msg_metadata_text = std::move(msg_metadata_text)]() mutable
    {
      // We are in thread W.  Nothing is locked.
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking user on-remote-unexpected-rsp handler.");
      on_func(msg_id_out, std::move(msg_metadata_text));
      FLOW_LOG_TRACE("Handler completed.");
    });
  };

  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);
  return m_sync_io.set_remote_unexpected_response_handler(std::move(real_on_func));
} // Channel::set_remote_unexpected_response_handler()

TEMPLATE_STRUCTURED_CHANNEL
bool CLASS_STRUCTURED_CHANNEL::unset_remote_unexpected_response_handler()
{
  // Reminder: we must protect against concurrent calls from user threads U1, U2, ...; and against access from W.
  Lock_guard lock(m_mutex);
  return m_sync_io.unset_remote_unexpected_response_handler();
}

TEMPLATE_STRUCTURED_CHANNEL
const typename CLASS_STRUCTURED_CHANNEL::Owned_channel& CLASS_STRUCTURED_CHANNEL::owned_channel() const
{
  // Always safe per contract.
  return m_sync_io.owned_channel();
}

TEMPLATE_STRUCTURED_CHANNEL
typename CLASS_STRUCTURED_CHANNEL::Owned_channel* CLASS_STRUCTURED_CHANNEL::owned_channel_mutable()
{
  // Always safe per contract.
  return m_sync_io.owned_channel_mutable();
}

TEMPLATE_STRUCTURED_CHANNEL
const Session_token& CLASS_STRUCTURED_CHANNEL::session_token() const
{
  // Gotta lock per contract (can change during log-in processing in thread W).
  Lock_guard lock(m_mutex);
  return m_sync_io.session_token();
}

/// @cond
// -^- Doxygen, please ignore the following.  It gets confused by something here and gives warnings.

TEMPLATE_STRUCTURED_CHANNEL
std::ostream& operator<<(std::ostream& os, const CLASS_STRUCTURED_CHANNEL& val)
{
  /* For now mirroring what Sync_io_obj does.  So our &val is output but the main useful info (owned_channel [])
   * is still visible.  Could instead output &val and val.m_sync_io would show owned_channel[] and whatever
   * else it wanted to.  It's just bulky that way, with 2 `this` addresses.  Look, whatever! */
  return os << "[owned_channel [" << val.owned_channel() << "]]@" << static_cast<const void*>(&val);
}

// -v- Doxygen, please stop ignoring.
/// @endcond

#undef CLASS_STRUCTURED_CHANNEL
#undef TEMPLATE_STRUCTURED_CHANNEL

} // namespace ipc::transport::struc
