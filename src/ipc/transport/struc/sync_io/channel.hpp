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

#include "ipc/transport/struc/channel_base.hpp"
#include "ipc/transport/struc/detail/msg_impl.hpp"
#include "ipc/transport/struc/detail/msg_mdt_out.hpp"
#include "ipc/transport/struc/heap_serializer.hpp"
#include "ipc/transport/channel.hpp"
#include "ipc/transport/error.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <capnp/pretty-print.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/array.hpp>
#include <boost/move/make_unique.hpp>
#include <queue>

namespace ipc::transport::struc::sync_io
{

// Types.

/**
 * `sync_io`-pattern counterpart to async-I/O-pattern transport::struc::Channel.
 *
 * @see transport::struc::Channel and util::sync_io doc headers.  The latter describes the general pattern which
 *      we implement here; it also contrasts it with the async-I/O pattern, which the former implements.
 *      In general we recommend you use a transport::struc::Channel rather than a `*this` --
 *      but you may have particular needs (summarized in util::sync_io doc header) that would make you decide
 *      otherwise.
 *
 * All notes on functionality in transport::struc::Channel (a/k/a alias Channel::Async_io_obj) doc
 * header apply to us.  The difference, as usual given `sync_io` and async-I/O mutual counterparts, is in how
 * results are reported.  Because struc::Channel in general stands apart from the many concept impls
 * of `Blob_sender`, `Blob_receiver`, etc. -- e.g., Native_socket_stream -- it may be helpful to summarize
 * how this `sync_io`-pattern type reports results.  The APIs look somewhat different from those
 * core-layer guys owing to the more complex mission statement here versus there.
 *
 *   - `expect_*(..., F)`: This says you want messages of a certain type, and when one arrives to invoke `F(I)`,
 *     `I` being the in-message.  However, depending on the API 1+ message(s) may already have been cached and
 *     are immediately available.  These are synchronously emitted via an out-arg; for example expect_msgs() takes
 *     a list-of-in-messages out-arg.  Depending on the API future messages may be relevant and will be emitted
 *     via `F()`.
 *   - `async_request(M, ..., F)`: This says you want `M` sent and expect 1 or 0+ (depending on args) response(s),
 *     and when one arrives to invoke `F(I)`, `I` being a response.  Since it is not possible to have received a
 *     response to a not-yet-sent out-message, that is just how it works.  There are no synchronous out-args.
 *   - `set_*unexpected_response_handler()`: If it's set, and an unexpected-response event occurs, this fires.
 *     If not, it doesn't.  There are no synchronous out-args.
 *   - `start_and_poll(F)`: This begins incoming-direction operation.  `F()` is the on-error handler.  It fires
 *     up to once per `*this` to indicate underlying `Owned_channel` or other channel-hosing error (including
 *     receiving graceful-close).  An error may synchronously occur right then, plus any `expect_*()`- and
 *     `async_request()`-triggered in-messages may be synchronously detected.  Therefore -- and this is quite
 *     rare (singular as of this writing in Flow-IPC):
 *     - `start_and_poll(F)` may *synchronously* (!) call a number of queued-up (in a sense) handlers including:
 *       - `expect_*()` handlers;
 *       - `async_request()` handlers;
 *       - `set_*unexpected_response_handler()` handlers;
 *       - on-error handler `F()` itself.
 *
 * Tip: If you'd rather not worry about `start_and_poll()`-executed handlers -- except on-error handler which
 * might occur regardless -- then avoid calling `expect_*()`, async_request(), `set_*()` before
 * start_and_poll().  (No in-messages are ever dropped, so you won't "miss" something by registering an
 * expectation too late.)  That said there's nothing wrong per se with setting those things up before
 * start_and_poll() either.  Just be ready for the `_and_poll` part.
 *
 * @internal
 * ### Implementation overview ###
 * Like all `sync_io`-core object types, this one is essentially a linear state machine with no
 * actual asynchronicity or concurrency.  (Some `sync_io`s do need to start threads in order to perform
 * unavoidable blocking operations -- namely the `Blob_stream_mq_*` guys with at least the bipc MQ type which
 * lacks an FD-tickling API -- but this can be thought of similarly to a background kernel service.  Anyway
 * sync_io::Channel doesn't even feature that caveat.)
 *
 * So as with other `sync_io` cores, it's a linear sequence of API calls into `*this` that may not be
 * mutually concurrent (when non-`const`).  As with those others in this context an API call is either a
 * method call (duh) or an async-wait `Event_wait_func` calling into `*this` via `(*on_active_ev_func)()`.
 * Hence we don't care what thread a given thing runs in, as long as our user (e.g., transport::struc::Channel)
 * doesn't invoke things concurrently.
 *
 * So it's "just" a matter of implementing each direction -- outgoing (send() et al), incoming
 * (`expect_*()`, async_request()), and their (reasonably limited) interplay (e.g., async_request() conceptually =
 * a send() + registering expectation of particular future response in-message(s)).
 *
 * ### Implementation: layers ###
 * In the outgoing direction, the layers involved are fairly straightforward: send() is synchronous.  When it is
 * called, the zero-copy serialization of the structured out-message #Msg_out has been completed: the raw blobs
 * comprised by it are easily accessible from the #Msg_out (Msg_out::emit_serialization()) passed to send()
 * (as is the optional #Native_handle).  The 1+ blobs + the #Native_handle are synchronously `Channel::send_*()`ed,
 * and then send() returns.  In addition, but still synchronously within send(), each such user message is internally
 * accompanied by another similarly-serialized message: the *metadata* message.  This message, with a schema
 * in structured_msg.capnp (a detail/ file), represents a *description* of the associated #Msg_out.  Its schema
 * is controlled by us and is sufficiently small to require just one segment (blob) and no #Native_handle.
 * This contains information about the message (a/k/a metadata), notably: its message ID (discussed below),
 * the ID of the message to which it responds (or 0 if unsolicited), and the session token.
 * To summarize, then, send() of a user message in fact sends exactly 1 metadata message
 * (Msg_mdt_out, a special-purpose sub-class of Msg_out, adding no data members but a convenient
 * internally-used APIs to set its fields), across exactly 1 blob (segment), plus 1+ blobs (segments)
 * serializing the user message.  (async_request() = internally such a send() more or less, plus registering
 * that future in-message(s) with certain metadata content are expected.)
 *
 * Lastly, for some special purposes, we sometimes send internal messages.  An internal message consists of
 * *only* the metadata (Msg_mdt_out, alias #Msg_mdt_out), with the optional internal-message-body field
 * filled-out, and no user message.  Hence in this case exactly 1 `Blob` (segment) is sent.  In the few cases
 * where an internal message is required, internal code calls send_core() directly (with the #Msg_out arg
 * set to null); whereas a user message goes through send() or async_request() which invokes send_core()
 * (with #Msg_out arg non-null but rather the user-supplied #Msg_out).
 *
 * In the incoming direction, it is much more complex.  Receiving a blob, or blob+handle pair, is by definition
 * a (potentially) asynchronous operation; and a given structured in-message may consist of more than 1 blob/blob+handle
 * pair.  The zero-copy deserialization invoked by the user can only occur once all of them are received, so
 * we must build up 1 given structured in-message over 1+ async-read ops and only emit the result
 * (a newly allocated/cted Msg_in a/k/a #Msg_in_ptr) to the user once the last of the 1+ async-read ops
 * finishes.  So that's 2 layers: async-read op at the #Owned_channel level; then the structured level once a
 * Msg_in is ready.  Msg_in represents, conceptually, a pointer to 1-2 serialized
 * in-messages: the metadata in-message (always present) and, except for internal messages, the user-supplied
 * in-message (supplied originally via #Msg_out).  However, to the user, only the latter is accessible,
 * and an internal message is by definition never emitted to the user (if it were, there would be nothing for them
 * to access).
 *
 * The preceding paragraph talks only of *one* Channel pipe.  It is possible #Owned_channel contains 2 pipes
 * (Channel::S_HAS_2_PIPES), operating potentially in parallel.  (This may be useful for perf; discussion omitted
 * here.)  Naturally this creates the possibility of reordering of in-messages.  Hence, optionally, there is
 * another layer between completing a Msg_in (#Msg_in_ptr) and emitting it: a simple reassembly queue.
 * To make this work: each in-message (hence each out-message instance) is supplied with a unique message ID
 * (required anyway for, at least, request-response correlation); and that message ID doubles as a *sequence #*
 * (1, 2, ...).  #Reassembly_q is a reassembly queue that is (probably infrequently) used to temporarily
 * store out-of-order structured in-messages, flushing them once the gap between last-emitted-to-next-layer
 * in-message-sequence-# and lowest-in-message-sequence-#-in-reassembly-queue is filled.
 *
 * Okay; let's assume an in-message has gone through the above 2-3 layers (Channel layer, (optional) reassembly
 * queue layer, Msg_in completion layer).  Can we finally emit it to the user?  Often yes... but generally
 * no:
 *
 * In the incoming-direction, we add *notification* and *response* expectation APIs (these are
 * mandatory before the `*this` user receives an emitted in-message via handler).  This adds a further layer
 * of processing.  Once a Msg_in (#Msg_in_ptr) is assembled and ready to deserialize, one of 2 things
 * happens: either an expect_msg() or async_request() has registered a handler for this in-message -- then it
 * is immediately emitted to that handler (done!); or not -- then it is stored inside `*this`.  (Once a handler
 * is *subsequently* registered, the appropriate stored in-messages shall be emitted via synchronous out-args
 * such as `qd_msgs` for expect_msgs().)
 *
 * That's the overview.  Various doc headers on `private` types and/or data members should fill in the details.
 *
 * ### Protocol negotiation ###
 * A good place to familiarize oneself with this topic is a similar section in the doc header of a core-layer
 * (lower level) facility; sync_io::Native_socket_stream::Impl is probably best, but sync_io::Blob_stream_mq_sender_impl
 * and sync_io::Blob_stream_mq_receiver_impl are similar (albeit split up, because an MQ is unidirectional).
 * We urge you to be comfortable with that before reading on, as a certain facility with the subject matter is
 * needed for the following discussion.
 *
 * Consider the big picture: transport::Channel (unstructured), over which a `*this` operates, is composed of those
 * lower-level transports, namely Blob_sender, Blob_receiver, etc., impls bundled in a certain way.  We sit at the
 * next-higher layer (structured layer).  We operate a protocol (multiple cooperating protocols actually) on top
 * of the protocol(s) at that unstructured layer, and that protocol can evolve (gain versions) over time too.
 * The question is how to arrange forward-compatible (in some sense) protocol negotiation at this higher layer.
 *
 * One approach would be to not add independent protocol versioning/negotiation at our layer at all and simply rely
 * on that work done already at the aforementioned unstructured (lower) layer.  Certainly that would mean less code
 * for us and a bit of perf/reponsiveness gain from not adding more negotiation traffic (that part not being super
 * significant, as this is all only at the start of a channel's lifetime).  The negative shows up in the scenario
 * where the low-level protocol needs no change, but our higher-level protocol does.  Now we have to bump up the
 * version for the lower layer, explaining this is only to support differences at unrelated higher layers.
 * (Formally speaking the lower-layer APIs can be used without as at all, and there are surely use cases for that.)
 * Moreover -- we don't often talk about this outside the top-level common.hpp -- but the `ipc_core` module
 * is (as of this writing) a separate library from ours, `ipc_transport_structured`; so one would need to change the
 * code in a different library, release it, etc., despite no other behavior changes in that library.  That's not great.
 *
 * So we've decided to keep it clean (if more complex and involving more code and designing) and have each layer do its
 * own negotiation.
 *
 * Well, no problem.  Having read about this as recommended above, you'll know it's just a matter of keeping a
 * Protocol_negotiator object in a `*this` and sending and receiving a message/payload before any other message/payload.
 * As of this writing there *is* only the initial version of the protocol (1), so we don't yet need to worry about
 * potential backward-compatibility/knowing which version to actually speak from a range of 2 or more; etc.
 *
 * That said, the challenge here is more subtle than that.  Sure, we can negotiate *a* version at this layer, no
 * problem.  The problem: there are *multiple* protocols operating at this layer.  There are at least two:
 *   -# How do blob/handle pairs transmitted over the transport::Channel translated into structured capnp messages?
 *      This even depends on template params `Struct_builder_config` and `Struct_reader_config`: As of this writing
 *      there are at least:
 *      - Heap_fixed_builder + Heap_reader: Heap-based arrangement, wherein all messages are transmitted directly
 *        (via copy) as blobs going through the unstructured `Channel`: The lead message blob is a capnp
 *        `struct StructuredMessage`, potentially followed by N (where N >= 1 and is communicated via
 *        that `StructuredMessage`) blobs, each representing a capnp-segment, those segments together making up
 *        a user message's serialization.
 *      - shm::Builder + shm::Reader: SHM-based arrangement, wherein all messages are transmitted
 *        as tiny SHM-handle-only-storing blobs, and then on the receiving side each such SHM-handle is taken
 *        to point to a list-of-capnp-segments (living in SHM), those segments together making up a
 *        user message's serialization.  (Hence end-to-end zero-copy, only the SHM-handles actually transmitted
 *        via the underlying unstructured `Channel`.)
 *   -# The capnp messages aren't just user messages either; internally we have internal messages and, more
 *      more importantly, the metadata-bearing leading messages containing key stuff like the message ID,
 *      notification/response info, etc.  The (internally used) schema in structured_msg.capnp is a protocol;
 *      certainly it could change over time: E.g., more types of internal messages might be added.
 *
 * This implementation is (we hope) thoughfully layered, but the question of where exactly lies the separation
 * between various protocols is a tough one.  These aren't just "wire protocols" anymore; there's a programmatic
 * element to it involving the Struct_builder and Struct_reader concepts.  So should there be a separate
 * negotiation/version for each of these protocols?  Which protocols, even?  How would this work?
 *
 * That's probably the wrong question, we think.  Let's instead be pragmatic.  Firstly, as of this writing, there *is*
 * only version 1 of everything; so it's all moot, until more versions pop up.  So all we're trying to do here is
 * not "shoot ourselves in the foot": Provide some kind of negotiation protocol "base" that all future versions of
 * the software can count on safely (in the same way we did for the lowest layer).  So, really, what we want here
 * is to have *enough separate protocol versions to negotiate* to be reasonably future-proof, so that future software
 * won't need component X to be changed even though a protocol in component Y is the one that is changing.  So,
 * pragmatically, speaking:
 *   - We know that just *one* Protocol_negotiator at the struc::sync_io::Channel layer is *sufficient*: In the worst
 *     case the version in it will need to be bumped up without struc::sync_io::Channel code otherwise needing it, but
 *     on account of something outside that class needing a protocol change (e.g., if shm::Builder + shm::Reader encode
 *     SHM stuff differently in the future).  So any other `Protocol_negotiator`s we add around here are gravy.
 *   - Exchanging more version numbers (via more `Protocol_negotiator`s) at the same time is nice and efficient and
 *     simple, if indeed we do want the aforementioned "gravy."  We just need to identify likely candidates for
 *     future protocol changes.  However many such versions we'll decide upon, we can have those Protocol_negotiator
 *     members (at least 1; more for the "gravy") inside `*this`, and we can compactly exchange these in a leading
 *     message sent in either direction.  E.g., if there are 3 `Protocol_negotiator`s, we'll send 3 versions
 *     (as of this writing `1`, `1`, `1`) and expect 3 versions to be received similarly.  (As usual, on receipt,
 *     each Protocol_negotiator::compute_negotiated_proto_ver() will determine the version to speak -- as of this
 *     writing either 1, or *explode the channel due to negotiation failure*.)
 *   - To identify the likely candidates, we should think of it in terms of pieces of software/modules that would
 *     change -- as opposed to the highly subjective notion of which things constitute separate protocols.
 *     This is really not so daunting:
 *     - There is *us*: struc::sync_io::Channel, and the satellite code, especially structured_msg.capnp.
 *       This covers a ton of stuff/logic/concepts/terminology; really everything up to but not including the
 *       zero-copy/SHM layer(s) *potentially* also involved.
 *       - It *does* include Heap_fixed_builder and Heap_reader.  Sure, they're "merely" impls of the concepts
 *         Struct_builder and Struct_reader, respectively, but those heap-based impls (1) live in our same namespace
 *         ipc::transport::struc and in the same module/library `ipc_transport_structured`; and (2) in practice
 *         are essentially-necessary building blocks for any zero-copy-based `Struct_*er`s including
 *         shm::Builder and shm::Reader.  That is to say, there is no point in having some kind of separate version
 *         for struc::sync_io::Channel and co., versus `struc::Heap_*er`.
 *     - There is our sub-namespace, transport::struc::shm -- especially shm::Builder and shm::Reader: this protocol
 *       determines how SHM handles are encoded inside the unstructured blobs transmitted via unstructured `Channel`,
 *       and how they are then interpreted as capnp messages (whether internal or user ones).
 *       - Formally speaking, though, all that is determined at compile time via `Struct_*er_config` template params
 *         to `*this`.  As of this writing those are likely to just be either vanilla `Heap_*er::Config`, or
 *         shm::Builder::Config + shm::Reader::Config; but formally any Struct_builder / Struct_reader concept impls
 *         are allowed.  (The user may well implement their own fanciness.)
 *       - So really it's not about transport::struc::shm specifically but more like, formally speaking,
 *         `Struct_builder_config` + `Struct_reader_config` protocol code, excluding the base/vanilla
 *         Heap_fixed_builder and Heap_reader (which, we've established, are already covered by the first, non-gravy
 *         Protocol_negotiator).
 *
 * Thus it should be sufficient to have:
 *   - Protocol_negotiator for us.  That is #m_protocol_negotiator.
 *   - (Possibly unused) Protocol_negotiator for any additional protocol(s) involved in `Struct_*er_config` layer,
 *     such as the existing shm::Builder and shm::Reader.  That is #m_protocol_negotiator_aux.
 *     The actual preferred (highest) and lowest-supported Protocol_negotiator::proto_ver_t values shall
 *     as of this writing simply be 1; but once that changes we'll likely add some `static constexpr` values
 *     in Struct_builder and Struct_reader concepts (and impls), and `*this` will pass those to Protocol_negotiator
 *     ctor (instead of simply passing `1`s).
 *     - (Depending on whether the hypothetical multi-version protocol(s) support backwards-compatibility with
 *       earlier protocol-versions (i.e., a given version range is not [V, V]), we might need more logic/APIs,
 *       so that a given module knows which protocol-version to in fact speak.  To reiterate, currently, we need not
 *       worry about it: just don't shoot selves in foot in this version 1 of everything.)
 *
 * Of course even *more* protocols may be involved inside `Struct_*er_config` layer; as of this writing there isn't --
 * it's just shm::Builder and shm::Reader in practice -- but there can be other `Struct_*er` concept impls which
 * could be arbitrarily complex.  However, we needn't invent some complicated ultra-dynamic/future-proof thing to cover
 * those possibilities: if it comes down to it, more protocol negotiation can be coded within those protocols themselves
 * as needed, or one can just bump up the #m_protocol_negotiator_aux version to cover all protocol changes at that
 * layer.
 *
 * That brings us to the mechanics of actually exchanging the highest-protocol-version for each of
 * #m_protocol_negotiator and #m_protocol_negotiator_aux.  We choose to use a very simple and small
 * capnp-backed (see `struct ProtocolNegotiation` in structured_msg.capnp) blob sent before any other out-messages
 * (and therefore expected before any in-messages).  Following the lead of the lower levels, we do this in lazy
 * fashion in both directions:
 *   - Outgoing: Just before invoking the first send API (`.send_blob()`, `.send_native_handle()`) -- if any --
 *     on #m_channel, `.send_blob()` the `ProtocolNegotiation`-storing blob.  The `.send*()`s are all non-blocking
 *     and synchronous, so that's simple.
 *   - Incoming: Just before invoking the first receive API (`.async_receive_blob()`, `.async_receive_native_handle()`)
 *     -- which occurs in start() -- on #m_channel, `.async_receive_blob()` the `ProtocolNegotiation`-storing blob.
 *     Only proceed with the first "real" receive on successful async-receive of the `ProtocolNegotiation`-storing
 *     blob.
 *
 * @endinternal
 *
 * @tparam Channel_obj
 *         See #Async_io_obj doc header.
 * @tparam Message_body
 *         See #Async_io_obj doc header.
 * @tparam Struct_builder_config
 *         See #Async_io_obj doc header.
 * @tparam Struct_reader_config
 *         See #Async_io_obj doc header.
 */
template<typename Channel_obj, typename Message_body,
         typename Struct_builder_config, typename Struct_reader_config>
class Channel :
  public Channel_base,
  private boost::noncopyable,
  public flow::log::Log_context
{
public:
  // Types.

  /// Useful for generic programming, the async-I/O-pattern counterpart to `*this` type.
  using Async_io_obj = transport::struc::Channel<Channel_obj, Message_body,
                                                 Struct_builder_config, Struct_reader_config>;
  /// You may disregard.
  using Sync_io_obj = Null_peer;

  /// See #Async_io_obj counterpart.
  using Owned_channel = Channel_obj;

  static_assert(Owned_channel::S_IS_SYNC_IO_OBJ,
                "struc::Channel (whether sync_io or async-I/O) subsumes sync_io-pattern-peer-bearing "
                  "`Channel`s only.  Fortunately ipc::session at least emits such `Channel`s.");

  /// See #Async_io_obj counterpart.
  using Builder_config = Struct_builder_config;
  /// See #Async_io_obj counterpart.
  using Reader_config = Struct_reader_config;

  /// See #Async_io_obj counterpart.
  using Msg_body = Message_body;

  /// See #Async_io_obj counterpart.
  using Msg_which = typename Message_body::Which;

  /// See #Async_io_obj counterpart.
  using Msg_which_in = Msg_which;

  /// See #Async_io_obj counterpart.
  using Msg_which_out = Msg_which;

  /// See #Async_io_obj counterpart.
  using Msg_out = struc::Msg_out<Msg_body, typename Builder_config::Builder>;

  /// See #Async_io_obj counterpart.
  using Msg_in = struc::Msg_in<Msg_body, Reader_config>;

  /// See #Async_io_obj counterpart.
  using Msg_in_ptr = boost::shared_ptr<Msg_in>;

  /// List of in-messages used in certain APIs like expect_msgs() to synchronously emit cached in-messages.
  using Msgs_in = std::vector<Msg_in_ptr>;

  /// See #Async_io_obj counterpart.
  using msg_id_out_t = Channel_base::msg_id_out_t;

  // Constructors/destructor.

  /**
   * Non-tag, no-log-in ctor form: Creates structured channel peer with no log-in phase (log-in phase must have occurred
   * in a preceding struc::Channel called *session master channel*), the session token already known and passed-in
   * to this ctor.  Consider also:
   *   - A tag-form ctor (usually simpler than manually cting `struct_builder_config` and
   *     `struct_reader_config`).
   *   - With-log-in ctor (usually needed for session master channel only).
   *
   * @note This ctor form is forwarded-to from #Async_io_obj ctor.  Documentation below applies to both ctors
   *       except where noted.
   *
   * If `*this` ctor is for sync_io::Channel: You'll need to start_ops() (possibly preceded by
   * replace_event_wait_handles()) per `sync_io` pattern.  (If `*this` ctor is for #Async_io_obj: not applicable.)
   * Then:
   *
   * You may send() (et al) immediately after this.  However no in-messages shall be emitted, regardless of registered
   * expectations such as via expect_msg(), until you invoke start_and_poll().  (For #Async_io_obj:
   * transport::struc::Channel::start().)
   *
   * Consider, also, `owned_channel_mutable()->auto_ping()` and/or `owned_channel_mutable()->idle_timer_run()`,
   * if you desire these features to be ambiently enabled.  See Channel::auto_ping(), Channel::idle_timer_run()
   * for background.
   *
   * ### How/whether to obtain a #Builder_config `struct_builder_config` (and related) ###
   * It is easiest, usually, to use a different ctor form instead; namely use a tagged-ctor version that will
   * set everything up.  See `Channel_base::Serialize_via_*`.  Only use the present ctor form:
   *   - IF: your serialization strategy of choice is simply not covered by the tagged-ctor versions available
   *     (you have a custom builder/reader you want to use for advanced purposes).  OR:
   *   - IF: you want to use a different serialization strategy in the outgoing direction versus incoming direction:
   *     e.g., this side sends only short things -- and you want to use Heap_fixed_builder here and
   *     Heap_reader there; but the other side sends huge things, so you want SHM-based serialization there
   *     and deserialization here.  The tag ctors assume symmetry and don't support this.
   *
   * If you've chosen to use this ctor form:
   *
   * Formally speaking it's simply up to you to construct these args before calling this ctor; see the docs for
   * the particular #Builder_config class of choice.  Informally: if you're constructing the #Builder_config
   * and/or specifying `struct_lender_session` (less so #Reader_config, but for consistency it too) directly, you're
   * probably not doing the right thing.  The following places are available to obtain tese for safety and
   * efficiency (and code maintainability):
   *   - From another, compatible, `Channel` via struct_builder_config(), struct_lender_session(),
   *     struct_reader_config().
   *   - Heap-backed:
   *     - `struct_lender_session` is always `NULL_SESSION`.  As for the other 2:
   *     - If you have the target #Owned_channel: via `static` heap_fixed_builder_config() and heap_reader_config().
   *     - Otherwise: session::Session_mv::heap_fixed_builder_config() (`static` or non-`static`),
   *       session::Session_mv::heap_reader_config() (ditto).
   *   - SHM-backed (using SHM-classic as example):
   *     - session::shm::classic::Session_mv::session_shm_builder_config() (ditto reader),
   *       session::shm::classic::Session_mv::session_shm_lender_session(),
   *       session::shm::classic::Session_mv::app_shm_builder_config() (ditto reader),
   *       session::shm::classic::Session_mv::app_shm_lender_session().  Requires a `Session` object.
   *     - session::shm::classic::Session_server::app_shm_builder_config() (requires session::Client_app).
   *       Server-only; if a `Session` is not available or applicable.
   *
   * @param logger_ptr
   *        Logger to use for logging subsequently.
   * @param channel
   *        Channel in PEER state with no prior traffic.  All configurations (1 pipe, 2 pipes) are supported.
   *        If `channel.initialized()` is `false`, behavior is undefined (assertion may trip): suggest
   *        sanity-checking this prior to this ctor.
   *        Behavior is undefined if it is not in PEER state or has had prior traffic.  (Reminder: PEER state
   *        does not mean it isn't hosed from error/whatever: This is handled gracefully.)
   *        The channel is moved-into `*this`.
   * @param struct_builder_config
   *        The serialization engine config to use for serializing out-messages.  This small object is copied.
   * @param struct_lender_session
   *        See Struct_builder::Session (and/or Struct_builder::emit_serialization()) concept doc header.
   *        This small value (typically a pointer) is copied; or the type may be Null_session which is empty;
   *        then there is nothing to even copy.
   * @param struct_reader_config
   *        The deserialization engine config to use for deserializing in-messages.  This small object is copied.
   * @param session_token_non_nil
   *        The session token to place into all out-messages and for which value to check against all in-messages.
   *        If a check fails, the channel is hosed immediately.  Behavior undefined if it equals nil
   *        (assertion may trip).  See class doc header regarding how to obtain the value to pass-in here.
   */
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   const Builder_config& struct_builder_config,
                   const typename Builder_config::Builder::Session& struct_lender_session,
                   const Reader_config& struct_reader_config,
                   const Session_token& session_token_non_nil);

  /**
   * Non-tag, with-log-in ctor form: Creates structured channel peer (endpoint of the *session master channel*)
   * with a log-in phase, this peer being the *server* or *client* depending on the arg `is_server`.  Consider also:
   *   - A tag-form ctor (usually simpler than manually cting `struct_builder_config` and
   *     `struct_reader_config`).
   *   - No-log-in ctor (since present ctor is usually needed for session master channel only).
   *
   * No general user communication can occur until the log-in phase is completed.  As such `*this` would be
   * the *session master channel* with, presumably, exactly 1 such `*this` in the entire process at a given time.
   * See #Async_io_obj class doc header for background.
   *
   * @note This ctor form is forwarded-to from #Async_io_obj ctor.  Documentation below applies to both ctors
   *       except where noted.
   *
   * If `*this` ctor is for sync_io::Channel: You'll need to start_ops() (possibly preceded by
   * replace_event_wait_handles()) per `sync_io` pattern.  (If `*this` ctor is for #Async_io_obj: not applicable.)
   * Then:
   *
   * You may send() (et al) immediately after this.  However no in-messages shall be emitted, regardless of registered
   * expectations such as via expect_msg(), until you invoke start_and_poll().  (For #Async_io_obj:
   * transport::struc::Channel::start().)
   *
   * Consider, also, `owned_channel_mutable()->auto_ping()` and/or `owned_channel_mutable()->idle_timer_run()`,
   * if you desire these features to be ambiently enabled.  See Channel::auto_ping(), Channel::idle_timer_run()
   * for background.
   *
   * ### As server ###
   * To begin the log-in phase -- and thus start to move to the logged-in phase, in which general traffic can
   * occur -- you must use expect_log_in_request().  `*this` will proceed to logged-in phase automatically once
   * the log-in request message `M` arrives, and you then invoke `send(X, M)`, where `X` is the log-in response
   * out-message you create/fill-out.  To be clear the phase change occurs as the last step in that
   * successful send().
   *
   * ### As client ###
   * To begin the log-in phase -- and thus start to move to the logged-in phase, in which general traffic can
   * occur -- you must `send(X, nullptr, nullptr, F)`, where `X` is the log-in request out-message you create/fill-out,
   * while `F()` is the handler for the log-in response.  `*this` will proceed to logged-in phase
   * automatically once the response to X arrives; and will then invoke `F()` for any further validation the
   * user may desire.
   *
   * ### How/whether to obtain a #Builder_config `struct_builder_config` (and #Reader_config similarly) ###
   * See notes for the other (no-log-in) non-tag ctor overload.
   *
   * @param logger_ptr
   *        See other ctor overload.
   * @param channel
   *        See other ctor overload.
   * @param struct_builder_config
   *        See other ctor overload.
   * @param struct_lender_session
   *        See other ctor overload.
   * @param struct_reader_config
   *        See other ctor overload.
   * @param is_server
   *        See above.
   */
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   const Builder_config& struct_builder_config,
                   const typename Builder_config::Builder::Session& struct_lender_session,
                   const Reader_config& struct_reader_config,
                   bool is_server);

  /**
   * Tag version of non-tag, no-log-in ctor:
   * Serialize_via_heap (bidirectional heap-based, non-zero-copy message serialization).
   *
   * @see Serialize_via_heap doc header for serialization-related background.
   * @see non-tag, no-log-in ctor.  As directed there, use the present form whenever sufficient, and indeed
   *      you want this type of serialization setup.  Consider also the other tag forms for other serialization
   *      methods which may match your requirements better.
   *
   * @param logger_ptr
   *        See non-tag ctor form.
   * @param channel
   *        See non-tag ctor form.
   * @param tag
   *        Ctor-selecting tag.
   * @param session_token_non_nil
   *        See non-tag ctor form.
   */
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_heap tag,
                   const Session_token& session_token_non_nil);

  /**
   * Tag version of non-tag, with-log-in ctor:
   * Serialize_via_heap (bidirectional heap-based, non-zero-copy message serialization).
   *
   * @see Serialize_via_heap doc header for serialization-related background.
   * @see non-tag, with-log-in ctor.  As directed there, use the present form whenever sufficient, and indeed
   *      you want this type of serialization setup.  Consider also the other tag forms for other serialization
   *      methods which may match your requirements better.
   *
   * @param logger_ptr
   *        See non-tag ctor form.
   * @param channel
   *        See non-tag ctor form.
   * @param tag
   *        Ctor-selecting tag.
   * @param is_server
   *        See non-tag ctor form.
   */
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_heap tag,
                   bool is_server);

  /**
   * Tag version of non-tag, no-log-in ctor:
   * Serialize_via_session_shm (bidirectional SHM-based provider, zero-copy message serialization,
   * per-session-scope arena).
   *
   * @see Serialize_via_session_shm doc header for serialization-related background.
   * @see non-tag, no-log-in ctor.  As directed there, use the present form whenever sufficient, and indeed
   *      you want this type of serialization setup.  Consider also the other tag forms for other serialization
   *      methods which may match your requirements better.
   *
   * @tparam Session
   *         One of, at least:
   *         session::shm::classic::Client_session, session::shm::classic::Server_session,
   *         session::shm::arena_lend::jemalloc::Client_session, session::shm::arena_lend::jemalloc::Server_session.
   * @param logger_ptr
   *        See non-tag ctor form.
   * @param channel
   *        See non-tag ctor form.
   * @param tag
   *        Ctor-selecting tag.
   * @param session
   *        `session->session_shm()` shall be used as the backing SHM space.
   *        `*session` must be in PEER state, or behavior is undefined.
   * @param session_token_explicit
   *        See non-tag ctor form.  However, for convenience, if you instead supply value equal to
   *        `NULL_SESSION_TOKEN` (i.e., nil) (which is default), then `session->session_token()` shall
   *        be used.  After all... why not?  That's probably what you want.
   */
  template<typename Session>
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_session_shm tag, Session* session,
                   const Session_token& session_token_explicit = NULL_SESSION_TOKEN);

  /**
   * Tag version of non-tag, with-log-in ctor:
   * Serialize_via_session_shm (bidirectional SHM-based provider, zero-copy message serialization,
   * per-session-scope arena).
   *
   * @see Serialize_via_session_shm doc header for serialization-related background.
   * @see non-tag, no-log-in ctor.  As directed there, use the present form whenever sufficient, and indeed
   *      you want this type of serialization setup.  Consider also the other tag forms for other serialization
   *      methods which may match your requirements better.
   *
   * @tparam Session
   *         One of, at least:
   *         session::shm::classic::Client_session, session::shm::classic::Server_session,
   *         session::shm::arena_lend::jemalloc::Client_session, session::shm::arena_lend::jemalloc::Server_session.
   * @param logger_ptr
   *        See non-tag ctor form.
   * @param channel
   *        See non-tag ctor form.
   * @param tag
   *        Ctor-selecting tag.
   * @param session
   *        `session->session_shm()` shall be used as the backing SHM space.
   *        `*session` must be in PEER state, or behavior is undefined.
   * @param is_server
   *        See non-tag ctor form.
   */
  template<typename Session>
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_session_shm tag, Session* session,
                   bool is_server);

  /**
   * Tag version of non-tag, no-log-in ctor:
   * Serialize_via_app_shm (bidirectional SHM-based provider, zero-copy message serialization,
   * per-app-scope arena).
   *
   * @see Serialize_via_app_shm doc header for serialization-related background.
   * @see non-tag, no-log-in ctor.  As directed there, use the present form whenever sufficient, and indeed
   *      you want this type of serialization setup.  Consider also the other tag forms for other serialization
   *      methods which may match your requirements better.
   *
   * @tparam Session
   *         One of, at least:
   *         session::shm::classic::Client_session, session::shm::classic::Server_session,
   *         session::shm::arena_lend::jemalloc::Server_session (not
   *         session::shm::arena_lend::jemalloc::Client_session -- will not compile).
   * @param logger_ptr
   *        See non-tag ctor form.
   * @param channel
   *        See non-tag ctor form.
   * @param tag
   *        Ctor-selecting tag.
   * @param session
   *        `session->app_shm()` shall be used as the backing SHM space.
   *        `*session` must be in PEER state, or behavior is undefined.
   * @param session_token_explicit
   *        See `session_token_explicit` in the Serialize_via_session_shm counterpart to this ctor form.
   *        Spoiler alert: probably you'll want to leave this at default.
   */
  template<typename Session>
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_app_shm tag, Session* session,
                   const Session_token& session_token_explicit = NULL_SESSION_TOKEN);

  /**
   * Tag version of non-tag, no-log-in ctor:
   * Serialize_via_app_shm (bidirectional SHM-based provider, zero-copy message serialization,
   * per-app-scope arena).
   *
   * @see Serialize_via_app_shm doc header for serialization-related background.
   * @see non-tag, no-log-in ctor.  As directed there, use the present form whenever sufficient, and indeed
   *      you want this type of serialization setup.  Consider also the other tag forms for other serialization
   *      methods which may match your requirements better.
   *
   * @tparam Session
   *         One of, at least:
   *         session::shm::classic::Client_session, session::shm::classic::Server_session,
   *         session::shm::arena_lend::jemalloc::Server_session (not
   *         ession::shm::arena_lend::jemalloc::Client_session -- will not compile).
   * @param logger_ptr
   *        See non-tag ctor form.
   * @param channel
   *        See non-tag ctor form.
   * @param tag
   *        Ctor-selecting tag.
   * @param session
   *        `session->app_shm()` shall be used as the backing SHM space.
   *        `*session` must be in PEER state, or behavior is undefined.
   * @param is_server
   *        See non-tag ctor form.
   */
  template<typename Session>
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_app_shm tag, Session* session,
                   bool is_server);

  /**
   * Invokes the destructor on the #Owned_channel.
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
   * Analogous to transport::sync_io::Native_handle_sender::replace_event_wait_handles().
   *
   * @tparam Create_ev_wait_hndl_func
   *         See above.
   * @param create_ev_wait_hndl_func
   *        See above.
   * @return See above.
   */
  template<typename Create_ev_wait_hndl_func>
  bool replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func);

  /**
   * Sets up the `sync_io`-pattern interaction between `*this` and the user's event loop; required before
   * async_accept() will work (as opposed to no-op/return `false`).
   *
   * `ev_wait_func()` -- with signature matching util::sync_io::Event_wait_func -- is a key function memorized
   * by `*this`.  It shall be invoked by `*this` operations when some op cannot complete synchronously and requires
   * a certain event (readable/writable) to be active on a certain native-handle.
   *
   * @see util::sync_io::Event_wait_func doc header for useful and complete instructions on how to write an
   *      `ev_wait_func()` properly.  Doing so correctly is the crux of using the `sync_io` pattern.
   *
   * This is a standard `sync_io`-pattern API per util::sync_io doc header.
   *
   * @tparam Event_wait_func_t
   *         Function type matching util::sync_io::Event_wait_func.
   * @param ev_wait_func
   *        See above.
   * @return `false` if this has already been invoked; no-op logging aside.  `true` otherwise.
   */
  template<typename Event_wait_func_t>
  bool start_ops(Event_wait_func_t&& ev_wait_func);

  /**
   * Permanently memorizes the incoming-direction on-error handler, thus authorizing the emission of any
   * in-messages (and/or an error) to handlers registered via this call, `expect_*()`, async_request(),
   * `set_*unexpected_response_handler()`; and *synchronously* executes any immediately relevant such handlers
   * due to pending in-traffic.
   *
   * To be clear: the caller must be ready for 0+ (potentially many) handlers to be synchronously invoked by
   * this call.  Even `on_err_func()` itself may be executed.  That said, recursive-mayhem is unlikely to be a concern,
   * since start_and_poll() itself is a one-time operation for a given `*this`.
   *
   * Until this is invoked, any incoming lower-level traffic is nevertheless saved in this process's or
   * opposing process's user RAM to be emitted as required by, and after, start_and_poll().  Therefore there is no
   * need to worry about in-traffic disappearing without a trace due to start_and_poll() being invoked too late.
   *
   * @tparam Task_err
   *         Handler type matching signature of `flow::async::Task_asio_err`.
   * @param on_err_func
   *        The permanent on-channel-hosed error handler.  See class doc header for discussion of error semantics.
   * @return `true` on success; `false` if start_ops() not yet called, or if already `start_and_poll()`ed, or if a
   *         prior error outgoing-direction error has hosed the owned Channel (then no-op).
   */
  template<typename Task_err>
  bool start_and_poll(Task_err&& on_err_func);

  /**
   * See #Async_io_obj counterpart.
   * @return See above.
   */
  const Builder_config& struct_builder_config() const;

  /**
   * See #Async_io_obj counterpart.
   * @return See above.
   */
  const typename Builder_config::Builder::Session& struct_lender_session() const;

  /**
   * See #Async_io_obj counterpart.
   * @return See above.
   */
  const Reader_config& struct_reader_config() const;

  /**
   * See #Async_io_obj counterpart.
   * @return See above.
   */
  const Owned_channel& owned_channel() const;

  /**
   * See #Async_io_obj counterpart.  Reminder: can be useful for `->auto_ping()` and `->idle_timer_run()`.
   * @return See above.
   */
  Owned_channel* owned_channel_mutable();

  /**
   * See #Async_io_obj counterpart.
   * @return See above.
   */
  const Session_token& session_token() const;

  /**
   * See #Async_io_obj counterpart.
   *
   * @param hndl_or_null
   *        See above.
   * @return See above.
   */
  Msg_out create_msg(Native_handle&& hndl_or_null = Native_handle()) const;

  /**
   * Registers the expectation (which may be immediately met) of up to 1 *notification* in-message
   * whose #Msg_which equals `which`.  No-op and return `false` if `which` is already being expected, if log-in phase
   * is not yet completed, or if a prior error has hosed the owned Channel.
   *
   * The expectation is unregistered upon receipt of the applicable in-message (and firing it with that in-message
   * #Msg_in_ptr as arg).  In addition:
   *
   * The 1 expected in-message may already be available synchronously.  In that case:
   *   - `*qd_msg` shall be loaded with that message; and it is purged from `*this`.  You must handle it as you see
   *     fit upon return from this method.
   *   - The expectation is immediately unregistered.
   *   - `on_msg_func` is ignored (it is not memorized).
   *
   * Therefore `on_msg_func()` can *only* execute upon a future `sync_io`-pattern async-wait firing its
   * `(*on_active_ev_func)()`.
   *
   * @tparam On_msg_handler
   *         Handler type for in-messages; see class doc header for in-message handling signature.
   * @param which
   *        Top-level #Msg_body union `which()` value to expect.
   * @param qd_msg
   *        `*qd_msg` is set to null if no message is immediately available; else set to that message (see above).
   * @param on_msg_func
   *        `on_msg_func(M)` shall be invoked in the manner explained in class doc header,
   *        on receipt of message with `which() == which`; unless it is immediately available and
   *        therefore loaded into `*qd_msg`.
   * @return `true` on success; `false` due to one of the above conditions.
   */
  template<typename On_msg_handler>
  bool expect_msg(Msg_which_in which, Msg_in_ptr* qd_msg, On_msg_handler&& on_msg_func);

  /**
   * In log-in phase as server only: Registers the expectation (which may be immediately met) of up to 1
   * *log-in request* in-message whose #Msg_which equals `which`.  No-op and return `false` if
   * expect_log_in_request() has already been invoked, if log-in phase is
   * not active or active in the client role, or if a prior error has hosed the owned Channel.
   *
   * The expectation is unregistered upon receipt of the applicable in-message (and firing it with that in-message
   * #Msg_in_ptr as arg).  In addition:
   *
   * The 1 expected in-message may already be available synchronously.  In that case:
   *   - `*qd_msg` shall be loaded with that message; and it is purged from `*this`.  You must handle it as you see
   *     fit upon return from this method.
   *   - The expectation is immediately unregistered.
   *   - `on_log_in_req_func` is ignored (it is not memorized).
   *
   * Therefore `on_log_in_req_func()` can *only* execute upon a future `sync_io`-pattern async-wait firing its
   * `(*on_active_ev_func)()`.
   *
   * ### Tips ###
   * Informally the proper behavior is:
   *   -# Construct in log-in-as-server phase.
   *   -# Invoke expect_log_in_request().
   *   -# Await `on_log_in_req_func(X&&)` firing, or immediate delivery of `X` via `*qd_msg`,
   *      where X is the log-in request.
   *   -# After `on_log_in_req_func(X&&)` handler, or immediate delivery pf `X` via `*qd_msg`:
   *      check X for correctness (such as process identity checks).
   *      If it fails checks, destroy `*this`; else:
   *   -# Fill out `X = this->create_msg()` (the log-in response) as needed via `X->body_root()`.
   *   -# `send(X)`.  The latter automatically moves `*this` to logged-in phase locally: the bulk of the
   *      API becomes available.
   *
   * @tparam On_msg_handler
   *         See expect_msg().
   * @param which
   *        See expect_msg().
   * @param qd_msg
   *        See expect_msg().
   * @param on_log_in_req_func
   *        See above.
   * @return `true` on success; `false` due to one of the above conditions.
   */
  template<typename On_msg_handler>
  bool expect_log_in_request(Msg_which_in which, Msg_in_ptr* qd_msg, On_msg_handler&& on_log_in_req_func);

  /**
   * Registers the expectation (some of which may be immediately met) of 0+ *notification* in-messages whose
   * #Msg_which equals `which`.  No-op and return `false` if `which` is already being expected, if log-in phase
   * is not yet completed, or if a prior error has hosed the owned Channel.
   *
   * The expectation is unregistered upon subsequent `undo_expect_msgs(which)`.
   *
   * 1+ expected in-messages may already be available synchronously.  In that case:
   *   - `*qd_msgs` is loaded with those messages; and they are purged from `*this`.  You must handle them as you
   *     see fit upon return from this method.
   *   - The expectation continues to be registered; `on_msg_func()` may be invoked in the future.
   *
   * Therefore `on_msg_func()` can *only* execute upon a future `sync_io`-pattern async-wait firing its
   * `(*on_active_ev_func)()`.
   *
   * @tparam On_msg_handler
   *         See expect_msg().
   * @param which
   *        See expect_msg().
   * @param qd_msgs
   *        `*qd_msgs` is cleared; then filled with any messages (possibly none) immediately available.
   * @param on_msg_func
   *        See above and expect_msg().
   * @return See expect_msg().
   */
  template<typename On_msg_handler>
  bool expect_msgs(Msg_which_in which, Msgs_in* qd_msgs, On_msg_handler&& on_msg_func);

  /**
   * See #Async_io_obj counterpart.
   *
   * @param which
   *        See above.
   * @return See above.
   */
  bool undo_expect_msgs(Msg_which_in which);

  /**
   * See #Async_io_obj counterpart.
   *
   * @param msg
   *        See above.
   * @param originating_msg_or_null
   *        See above.
   * @param err_code
   *        See above.
   * @return See above.  In addition `false` returned if called before start_ops().
   */
  bool send(const Msg_out& msg, const Msg_in* originating_msg_or_null = 0, Error_code* err_code = 0);

  /**
   * See #Async_io_obj counterpart; though naturally `on_rsp_func()` is invoked in the `sync_io`-pattern fashion.
   * There is no possibility of the response expectation being immediately (synchronously) met.
   *
   * @tparam On_msg_handler
   *         See expect_msg().
   * @param msg
   *        See above.
   * @param originating_msg_or_null
   *        See above.
   * @param id_unless_one_off
   *        See above.
   * @param on_rsp_func
   *        See above.
   * @param err_code
   *        See above.
   * @return See above.  In addition `false` returned if called before start_ops().
   */
  template<typename On_msg_handler>
  bool async_request(const Msg_out& msg, const Msg_in* originating_msg_or_null,
                     msg_id_out_t* id_unless_one_off, On_msg_handler&& on_rsp_func, Error_code* err_code = 0);

  /**
   * See #Async_io_obj counterpart.
   *
   * @param originating_msg_id
   *        See above.
   * @return See above.
   */
  bool undo_expect_responses(msg_id_out_t originating_msg_id);

  /**
   * See #Async_io_obj counterpart; though naturally `on_func()` is invoked in the `sync_io`-pattern fashion.
   *
   * @tparam On_unexpected_response_handler
   *         See above.
   * @param on_func
   *        See above.
   * @return See above.
   */
  template<typename On_unexpected_response_handler>
  bool set_unexpected_response_handler(On_unexpected_response_handler&& on_func);

  /**
   * See #Async_io_obj counterpart.
   * @return See above.
   */
  bool unset_unexpected_response_handler();

  /**
   * See #Async_io_obj counterpart; though naturally `on_func()` is invoked in the `sync_io`-pattern fashion.
   *
   * @tparam On_remote_unexpected_response_handler
   *         See above.
   * @param on_func
   *        See above.
   * @return See above.
   */
  template<typename On_remote_unexpected_response_handler>
  bool set_remote_unexpected_response_handler(On_remote_unexpected_response_handler&& on_func);

  /**
   * See #Async_io_obj counterpart.
   * @return See above.
   */
  bool unset_remote_unexpected_response_handler();

  /**
   * See #Async_io_obj counterpart; except (1) naturally `on_done_func()` is invoked in the `sync_io`-pattern fashion,
   * and (2) the operation may (and is very likely to) complete synchronously and thus ignore `on_done_func`.
   *
   * @note It is highly recommended to read the #Async_io_obj transport::struc::Channel::async_end_sending()
   *       doc header's recommendations on how/when/why to use the method.
   *
   * The sync-versus-async-completion dichotomy is exactly forwarded from #Owned_channel, and you can read
   * the details in sync_io::Native_handle_sender::async_end_sending() doc header.
   *
   * @tparam Task_err
   *         See above.
   * @param sync_err_code
   *        See above.
   * @param on_done_func
   *        See above.
   * @return See above.  In addition `false` returned if called before start_ops().
   */
  template<typename Task_err>
  bool async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func);

private:
  // Types.

  /// Exact equivalent of #Msg_out but with the internal-use (intended for us!) interface exposed.
  using Msg_out_impl = struc::Msg_out_impl<Msg_body, typename Builder_config::Builder>;

  /// Exact equivalent of #Msg_in but with the internal-use (intended for us!) interface exposed.
  using Msg_in_impl = struc::Msg_in_impl<Msg_body, Reader_config>;

  /**
   * Metadata message: internal-use out-message describing the associated #Msg_out; or describing/containing
   * internal message body (then not associated with a user-message).
   */
  using Msg_mdt_out = struc::Msg_mdt_out<Builder_config>;

  /// Clarifying short-hand for incoming-message IDs.
  using msg_id_in_t = msg_id_t;

  /// Concrete type corresponding to `On_msg_handler` template param: in-message handler.
  using On_msg_func = Function<void (Msg_in_ptr&& msg)>;

  /// Ref-counted wrapper of #On_msg_func.
  using On_msg_func_ptr = boost::shared_ptr<On_msg_func>;

  /**
   * Concrete type corresponding to `On_unexpected_response_handler` template param
   * (see set_unexpected_response_handler()).
   */
  using On_unexpected_response_func = Function<void (Msg_in_ptr&& msg)>;

  /**
   * Concrete type corresponding to `On_remote_unexpected_response_handler` template param
   * (see set_remote_unexpected_response_handler()).
   */
  using On_remote_unexpected_response_func = Function<void (msg_id_out_t msg_id_out,
                                                            std::string&& msg_metadata_text)>;

  /// The state of `*this` (given start_ops() success); see #m_phase.
  enum class Phase
  {
    /**
     * Regular-operation phase: the general API (expect_msg(), etc.) is available.  Most `struc::Channel`s
     * begin in this phase, via ctor that takes `session_token_non_nil`.  This is the terminal phase.
     * Phases that lead to it are `S_SRV_LOG_IN` and `S_CLI_LOG_IN`.
     */
    S_LOGGED_IN,

    /**
     * Logging-in phase, as server process: The general API (expect_msg(), etc.) is unavailable; only
     * expect_log_in_request() and send() are available.  This is an initial phase for a *session master channel*,
     * via ctor that does not take `session_token_non_nil` and has `is_server == true`.  Leads to `S_LOGGED_IN`.
     */
    S_SRV_LOG_IN,

    /**
     * Logging-in phase, as client process: The general API (expect_msg(), etc.) is unavailable; only
     * create_msg() and send() (response-expecting form) are available.  This is an initial phase for
     * a *session master channel*, via ctor that does not take `session_token_non_nil` and has `is_server == false`.
     * Leads to `S_LOGGED_IN`.
     */
    S_CLI_LOG_IN
  }; // enum class Phase

  /**
   * Policy for how to act upon receiving a response in-message that indicates its originating out-message is
   * the message associated with this object in #Expecting_response_map.
   */
  struct Expecting_response
  {
    // Types.

    /// Short-hand to cheap handle type.
    using Ptr = boost::movelib::unique_ptr<Expecting_response>;

    // Data.

    /**
     * `true` if receiving a response means request is satisfied; `false` if undo_expect_responses() must be invoked
     * to mark request satisfied.  This controls the mechanism by which an Expecting_response is removed
     * from the expected-responses map, freeing the non-zero RAM (at least) resource involved.
     */
    bool m_one_expected;

    /**
     * The handler to invoke on receiving a response to the requesting #Msg_out (see send() response-expecting form).
     *
     * The `shared_ptr` wrapper is because in case of #m_one_expected being `false` we have to copy it when
     * `handlers_post()`ing; `move()` could destroy it (leave it `.empty()`) instead.
     */
    On_msg_func_ptr m_on_msg_func;
  }; // struct Expecting_response

  /**
   * Policy for how to act upon receiving an in-message whose top-level-union #Msg_which_in `enum` equals the
   * #Msg_which_in associated with this object in #Expecting_msg_map.
   *
   * It is aliased to Expecting_response for brevity, since that `struct`'s fields are easily reworked
   * to the present application.
   *   - `m_one_expected` is repurposed to apply to undo_expect_msgs().
   *   - `m_on_msg_func` is repurposed as the handler for a #Msg_which_in value (see expect_msg(), expect_msgs()).
   */
  using Expecting_msg = Expecting_response;

  /**
   * Table mapping originating out-message to the policy for handling an in-message that indicates that out-message
   * as the originating message (via message ID -- *after* the out-message is sent via send()).
   */
  using Expecting_response_map = boost::unordered_map<msg_id_out_t, typename Expecting_response::Ptr>;

  /**
   * Table mapping in-message #Msg_which_in `enum` value to the policy for handling an in-message with that
   * `which()` value.
   */
  using Expecting_msg_map = boost::unordered_map<Msg_which_in, typename Expecting_msg::Ptr>;

  /// Like #Msg_in_ptr but `unique_ptr` instead of `shared_ptr`.  Note the latter can upgrade-from a `move()`d former.
  using Msg_in_ptr_uniq = boost::movelib::unique_ptr<Msg_in_impl>;

  /// Short-hand for queue (FIFO) of in-messages.
  using Msg_in_q = std::queue<Msg_in_ptr_uniq>;

  /**
   * Reassembly queue type: "queue" of all in-messages with #msg_id_in_t exceeding #m_rcv_msg_next_id, sorted
   * in increasing order by that #msg_id_in_t (sequence #).  Relevant only if `Owned_channel::S_HAS_2_PIPES == true`.
   */
  using Reassembly_q = std::map<msg_id_in_t, Msg_in_ptr_uniq>;

  /**
   * Data and policy with respect to receipt of the next/currently-incomplete in-message.  Depending on whether
   * 1 or 2 in-pipes are enabled in #Owned_channel, and in the former case what kind of in-pipe it is
   * (see Channel::S_HAS_BLOB_PIPE, Channel::S_HAS_NATIVE_HANDLE_PIPE, and similar), there
   * may be 1 or 2 of these objects (sets of policy + state) in `*this`.  See #m_rcv_pipes
   * (`std::optional` being used to permanently disable up to 1 of them) of this type.
   *
   * ### Algorithm/background ###
   * #Owned_channel, potentially being a Native_handle_receiver,
   * is possibly capable of receiving unstructured messages, each
   * message bearing a blob and, optionally, a #Native_handle.  Suppose the opposing peer wants to send
   * a structured message M, plus #Native_handle S.  The latter can just be sent in a single message.
   * The former (M) is serialized into 1 or 2+ RAM blobs (a/k/a segments in capnp parlance), depending on its
   * size.  This shall *always* be 1 if we are SHM-backed.
   *
   * Then: the whole structured message M + #Native_handle S shall be sent as:
   *   - 1 *lead* message containing mandatory metadata (containing notably the message ID); *and* S;
   *   - 1 *continuation* message containing the 1st segment serializing M itself;
   *   - 0+ further *continuation* messages serializing the rest of M.  (Usually 0; always 0 if SHM-backed.)
   *
   * There are also internal messages -- presumed rare.  These are represented by the 1st bullet point only:
   * there are no continuation messages at all.  The algorithm here operates, therefore, under the assumption
   * that each structured message = 1 lead message; plus 0+ continuation messages.  However in perf analysis
   * one should assume 1 continuation message, as most messages are user messages, and those usually are serializable
   * as 1 continuation message each.
   *
   * If M = just a lead message (internal-message case): then it is simply
   * sent in one unstructured message, received in a single async_receive_native_handle() call.  In this case
   * `m_incomplete_msg` is created just before that async-read, completed fully just upon completing that async-read,
   * and constructed/assigned again (and then the next async-read is executed).  S is S, and the internal-message
   * is deserialized from the 1 segment.  (As of this writing there never is an S paired with an internal message;
   * but to future-proof this let's assume it is possible.)
   *
   * Otherwise (user-message case): the sender shall send:
   *   -# *Lead* message (containing amother other things how many continuation messages are coming next; and S);
   *   -# 1+ *continuation* message(s), the first containing the first segment (blob); the rest (if any) containing
   *      the rest of them.  S is never paired with a continuation message.
   *
   * `m_incomplete_msg` shall store that state across the 2+ async-reads, up until the last of the N segments has
   * been received (then it is constructed/assigned again, and then the next async-read is executed).
   * As with the lead-message-only case, just before being replaced with a new one, `m_incomplete_msg` is fed into the
   * next layer within `*this`, where the structured message is fed to the user, etc.
   *
   * Which pipe is used for these 2+ messages?
   *   - If Channel::S_HAS_BLOB_PIPE_ONLY is `true`, it is a fatal error.  No #Native_handle can be transmitted.
   *   - If Channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY is `true`, then naturally the handles pipe is used
   *     (Channel::async_receive_native_handle()) -- it's the only pipe available at any rate.
   *   - If Channel::S_HAS_2_PIPES is `true`, then:
   *     - The lead message is expected over the handles pipe (Channel::async_receive_native_handle()), as it's the
   *       only pipe capable of transporting S.
   *     - (Attn: subtlety!) The continuation messages are *also* expected over that pipe, even though
   *       the blobs pipe *is* capable of transporting these.  Why?  Answer: At this stage we're not dealing with
   *       the structured layer yet (pre-deserialization which isn't yet possible), so there are no sequence numbers.
   *       It would require absurd contraptions to distinguish a non-handle-bearing continuation message
   *       with a handle-bearing lead message from a non-handle-bearing lead message;
   *       to resolve races between the 2 parallel in-pipes; etc.  More on this around the to-do below.
   *
   * Now assume no #Native_handle S wants to be sent with M.  The serialization logic is all the same -- there's
   * just no S transmitted along with the lead message.  The transport media are different; as any pipe can
   * transmit these unstructured messages, not needing to transmit these strange native handle thingies.  So then
   * which pipe is used for these messages?
   *   - If Channel::S_HAS_BLOB_PIPE_ONLY is `true`, then naturally the blobs pipe is used -- being the only 1 avail.
   *   - If Channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY is `true`, then the handles pipe is used -- being the only 1 avail
   *     (and perfectly capable of transmitting just-blobs).
   *   - If Channel::S_HAS_2_PIPES is `true`, then: Although either pipe *could* be used, we use the
   *     blobs pipe; the idea being that the only reason to configure both pipes in a Channel is for performance
   *     (certainly it doesn't make anything *simpler* after all), and since the handles-pipe alone was deemed
   *     insufficient, we'll reserve it for handle-bearing structured user messages only.
   *     - Perf caveat: Isn't it "giving away" perf to (for the sake of simplicity) send messages
   *       pertaining to sans-handle lead messages along the handles pipe (and not the blobs pipe)?
   *       Well, yes, at least a bit.  In practice every user message has a lead message -- the metadata-bearing
   *       one (with message ID, originating-message-if-any ID, etc.), and at least 1 continuation message
   *       (the actual user message -- plus, if the message is so big it can't fit into 1 segment, then more
   *       message(s) to carry the serialization).  (Let's assume usually there's only 1 continuation message:
   *       if too-big messages are common, much more impact-heavy perf measures are necessary anyway -- namely
   *       using SHM-aware serialization which will *never* transmit bulk data.  Those measures are available and
   *       encouraged!)  So typically, if the user message is indeed handle-bearing, we'll send the metadata
   *       pre-message along the handles pipe (with a handle) immediately followed by the user payload
   *       (without handle).  The user continuation message could indeed have been sent over the blobs pipe, so we
   *       are giving away a bit of perf with every message.  I (ygoldfel) simply did not deem this giveaway
   *       worth the complexity in the reassembly logic needed to compensate for this.  It could be something to
   *       look at when/if a perf deficit is identified, or we're doing a hardcore optimization pass at this code.
   *       (That said -- this only applies to handle-bearing user messages in the first place.  Further, again,
   *       we are talking about the low-level perf of copying small amounts of data after already being backed
   *       by SHM to essentially eliminate bulk copying.  Intuitively this should be minor in the big picture.)
   *       Here's the to-do:
   *
   * @todo Look into the algorithm documented in Channel::Msg_in_pipe wherein (with 2 pipes in the channel)
   * some low-level messages associated with handle-bearing user structured messages are sent over the (presumably
   * somewhat slower) handles pipe despite, themselves, not containing a native handle being transmitted.  See text just
   * above this to-do in the code.
   *
   * That describes what happens for each structured message M, paired with #Native_handle S -- or not paired with it.
   * So the question is how to make these possibilities work together.  Answer:
   *
   * To get this to work, depending entirely on `Channel::S_HAS_*`, there are 1 or 2 `Msg_in_pipe`s:
   * #m_rcv_pipes, each one's Msg_in_pipe::m_lead_msg_mode (which is `const`!)
   * determining (1) which `async_receive_*()` API will be called; and (2) how the async non-error
   * result will be handled.  (See start_and_poll() where this is kicked off for the first time,
   * meaning each of the 2 `Msg_in_pipe`s is cted, setting #m_lead_msg_mode forever.)
   *   - For a given Msg_in_pipe: when no incomplete message is in-progress (such as the start of `*this`):
   *     create empty structured #Msg_in (assign to `m_incomplete_msg`), do the appropriate
   *     `m_channel.async_receive_*()`; then when it async-yields result store the first segment blob
   *     in it (and native handle if any).  If no more segments
   *     are expected for this structured message, done (`*m_incomplete_msg` can be fed to the next layer).
   *     Repeat this bullet point (create new #Msg_in, async-read, etc.).  Otherwise:
   *   - `m_incomplete_msg` stays (it is still incomplete).  `m_channel.async_receive_blob()` (meaning expect
   *     an unstructured continuation in-message, and if it somehow includes a #Native_handle, then it's a low-level
   *     `Channel`-hosing error).  When it async-yields result, add it to the structured message
   *     #Msg_in #m_incomplete_msg.  Repeat this bullet point until it is completed, at which point
   *     done -- #m_incomplete_msg can be fed to the next layer; back to the preceding bullet point
   *     (create new #Msg_in, async-read, etc.).
   *
   * If `Owned_channel::S_HAS_2_PIPES`, then 2 `async_receive_*()`s are possible simultaneously,
   * and there are indeed 2x Msg_in_pipe in `*this`; one for
   * just blobs (Msg_in_pipe::S_RCV_SANS_HNDL_ONLY), one for just blob-and-handle combos
   * (Msg_in_pipe::S_RCV_WITH_HNDL_ELSE_ERROR).  They can be interleaved.
   * Otherwise there's only 1 Msg_in_pipe, namely:
   *   - If the 1 pipe is blobs-only, `Owned_channel::S_HAS_BLOB_PIPE_ONLY`: it's, obviously, for just
   *     blobs (Msg_in_pipe::S_RCV_SANS_HNDL_ONLY).
   *   - If the 1 pipe is blobs-and-handles, `Owned_channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY`: it's for
   *     both types of messages (Msg_in_pipe::S_RCV_WITH_OR_SANS_HNDL_DEMUX).
   */
  struct Msg_in_pipe
  {
    // Data.

    /**
     * The immutable mode of behavior along this pipe, which must be enabled: Whether to use
     * Native_handle_receiver::async_receive_native_handle() or Blob_receiver::async_receive_blob();
     * and what to accept/do on receipt.
     *
     * See start_and_poll() for background discussion to clarify this nicely.
     */
    enum
    {
      /**
       * Use Native_handle_receiver::async_receive_native_handle(); accept with-handle and sans-handle
       * lead in-message; then demultiplex into the slightly different async code path 1 or 2 depending on which
       * occurred.  (However, any continuation message along either code path must be sans-handle.)
       */
      S_RCV_WITH_OR_SANS_HNDL_DEMUX,

      /**
       * Use Native_handle_receiver::async_receive_native_handle(); accept with-handle message only;
       * sans-handle message means fatally hosed `*this`.  (However, any continuation message
       * must be sans-handle.)
       *
       * @note For context: If this is in use, then another Msg_in_pipe must be in `S_RCV_SANS_HNDL_ONLY`
       * #m_lead_msg_mode also (in `*this`).
       */
      S_RCV_WITH_HNDL_ELSE_ERROR,

      /**
       * Use Blob_receiver::async_receive_blob(); accept sans-handle message only;
       * with-handle message means fatally hosed `*this` automatically at the lower level (inside Channel).
       * (Any continuation message must be sans-handle as well.)
       *
       * @note For context: If this is in use, then another Msg_in_pipe *might* be in `S_RCV_SANS_HNDL_ONLY`
       * #m_lead_msg_mode also (in `*this`).  It also might *not* exist; namely if Channel::S_HAS_BLOB_PIPE_ONLY.
       * Then, simply, `*this` can never receive native handles (which is an entirely possible use).
       */
      S_RCV_SANS_HNDL_ONLY
    } m_lead_msg_mode;

    /**
     * During an async-read (note: given start_and_poll() and until channel is hosed, there is *always* an async-read
     * happening): the number of segments needed *after* the currently-async-awaited next segment does arrive
     * to complete the message.  When the lead in-message is being async-read, this shall be set to 0.
     * (If that lead in-message indicates the total # of *further* segments to expect is 0, then the 0 is "correct";
     * if the # is 1+, then this member is "corrected" to equal that #.  Before each continuation async-read
     * it is decremented by 1, including ahead of the first continuation async-read.  So if original seg-count is 1,
     * then this is 0 ahead of lead read, 0 ahead of continuation read.  If it's 2, then this is 0 ahead of lead read,
     * 1 ahead of continuation read one, 0 ahead of continuation read two.  And so on.)
     */
    size_t m_n_segs_left_after_this_read;

    /**
     * Target buffer for the `async_receive_*()` operation in-progress.  (Note: given start_and_poll() and until channel
     * is hosed, there is *always* an async-read in-progress.)
     * `m_target_blob.capacity()` is always a positive constant, the max length accepted along the underlying
     * Channel in-pipe.  `m_target_blob->capacity() == m_target_blob->size()`,
     * and the async-receive was passed `m_target_blob->mutable_buffer()` with that `.size()`.
     * If it is completed, then `m_target_blob->size()` is "corrected" to the # of bytes (successfully) received in
     * the unstructured message -- and `m_target_blob` is immediately re-assigned to point to the *next* `Blob.
     *
     * To be clear: #m_target_blob points to a `Blob` inside `*m_incomplete_msg`.  The #Msg_in is in charge of
     * supplying memory area to read-to; depending on the deserialization engine it might be obtaining that memory
     * from the regular heap, or some pool, or....
     *
     * So when `m_target_blob->size()` is "corrected," the correction is as required by the #Msg_in contract
     * for Msg_in::add_serialization_segment().  That way it knows the actual size of the segment
     * within the serialization, as opposed to the max size of such a segment.
     */
    flow::util::Blob* m_target_blob;

    /**
     * Target #Native_handle for the `async_receive_native_handle()` operation in-progress or last-completed;
     * null if the last or in-progress operation is `async_receive_blob()`.
     *
     * Therefore, regardless of which async-receive just completed, one can view pair (#m_target_blob, #m_target_hndl)
     * as its result (it shall be null if and only if the received message is sans-handle -- in any context).
     *
     * Analogously to #m_target_blob, this member is reinitialized immediately upon successful async-read;
     * if it is non-null, then it is moved-into #m_incomplete_msg (and thus nullified); if it is null, then...
     * it remains null (a `.null()` #Native_handle moved-to another #Native_handle simply remains `.null()`).
     */
    Native_handle m_target_hndl;

    /**
     * Target (incomplete) in-message for the `async_receive_*()` operation in-progress; one is always in-progress
     * once `start_and_poll()` has been called, until the pipe is hosed.  It's used as follows:
     *   - When the async-read for the lead unstructured in-message is about to be called, this is constructed,
     *     and its `add_serialization_segment()` returns a `Blob*` to the next blob (inside `*m_incomplete_msg`)
     *     to async-read-into.  That `Blob*` is saved to #m_target_blob, and the async-read begins.
     *   - Once it is complete, `*m_target_blob` is `resize()`d to account for the actual segment size.
     *     - If the message consists of only 1 segment, done.  Emit completed structured in-message to
     *       the next layer; and go back to step 1 (get next lead message).  Else
     *       must get first continuation message:
     *   - Call `add_serialization_segment()` again; save the returned `Blob*` into #m_target_blob again;
     *     begin async-read again.
     *   - Once it is complete, `*m_target_blob` is `resize()`d again to account for the actual seg-size again.
     *     - If that's the last segment, done.  Emit completed structured in-message to
     *       the next layer; and go back to step 1 (get next lead message).  Go back to step 1 (get the next
     *       lead message).  Else must get next continuation message: Go to step 3 again.
     *
     * Subtlety: It's a `unique_ptr` (not #Msg_in_ptr `shared_ptr`) for anti-leak safety/perf.
     * It is moved and thus upgraded to a `shared_ptr` (#Msg_in_ptr) when message completed.
     */
    Msg_in_ptr_uniq m_incomplete_msg;
  }; // struct Msg_in_pipe

  /**
   * Upon receiving an unstructured message along 1 given pipe of #Owned_channel this indicates the next such message
   * expected if any.
   */
  enum class Rcv_next_step
  {
    /**
     * Next message is lead message.  The case in these situations:
     *   - Initial state (start_and_poll()).
     *   - The last message was lead unstructured message comprised by a structured internal message.
     *     (An internal structured message = always 1 unstructured lead message.)
     *   - The last message was continuation unstructured message N of N comprised by a structured user message's
     *     serialization; where N is the segment count for the serialization of the user message.  N itself is
     *     communicated in the lead unstructured message (the metadata).  Ideally, and typically, N=1.
     */
    S_READ_LEAD_MSG,

    /**
     * Next message is continuation message.  The case in these situations:
     *   - The last message was lead unstructured message comprised by a structured user message message.
     *     (An user structured message = always 1 unstructured lead message containing metadata + 1+
     *     continuation messages comprised by structured user message's serialization.)
     *   - The last message was continuation unstructured message less-than-N of N comprised by a structured user
     *     message's serialization; where N is the segment count for the serialization of the user message.
     *     N itself is communicated in the lead unstructured message.  Ideally, and typically, N=1.
     */
    S_READ_CONT_MSG,

    /// The pipe should no longer be read-from ever (the case on error).
    S_STOP
  }; // enum class Rcv_next_step

  // Methods.

  /**
   * Record handler to invoke in handlers_poll() soon.  The context for this is: we're scanning an unstructured
   * (low-level) message received along a pipe of #Owned_channel; and it completed something such that the user
   * shall be informed of this via a completion handler they had registered.  As of this writing the relevant
   * completion handlers are:
   *   - those from `expect_*()`;
   *   - those from async_request();
   *   - those from `set_*unexpected_response_handler()`.
   *
   * (async_end_sending() completion handler is a separate, lower-level thing.)
   *
   * @see Doc header for #m_sync_io_handlers which discusses the design+trade-offs here.
   *
   * @tparam Task
   *         Function type matching `void F()`.
   * @param context
   *        Brief context string for logging.
   * @param handler
   *        Code to run in handlers_poll().
   */
  template<typename Task>
  void handlers_post(util::String_view context, Task&& handler);

  /**
   * Executes what was recorded recently in handlers_post().  As of this writing this is done after scanning
   * each unstructured (low-level) in-message.  If Channel::S_HAS_2_PIPES is `true`, then 1+ handlers may
   * execute here; otherwise at most 1 can execute here.
   *
   * @see Doc header for #m_sync_io_handlers which discusses the design+trade-offs here.
   *
   * @param context
   *        Brief context string for logging.
   */
  void handlers_poll(util::String_view context);

  /**
   * Helper that returns the max receive-buffer size for any async-read into a given Msg_in_pipe.
   *
   * @param mode
   *        Msg_in_pipe::m_lead_msg_mode.  The reason we don't take a `const Msg_in_pipe&` here is that
   *        there is a use case where this value is necessary just before the Msg_in_pipe is actually constructed.
   * @return See above.
   */
  size_t rcv_blob_max_size(decltype(Msg_in_pipe::m_lead_msg_mode) mode) const;

  /**
   * This key method acts on the pre-condition that the given in-pipe is not known to be in would-block state;
   * and therefore we should read (and process) as many unstructured in-messages as synchronously possible
   * until reaching would-block state, at which point we should `return` with a pending async-wait/read on
   * that in-pipe.
   *
   * This shall be called in exactly the following situations:
   *   - initially (start_and_poll());
   *   - upon this sequence occurring:
   *     -# An async-read (from this very method) yielded would-block.
   *     -# The would-block ended -- a message was received asynchronously.
   *     -# rcv_on_async_read_lead_msg() or rcv_on_async_read_continuation_msg() processed that in-message and
   *        would like to now read as many in-messages as possible that follow it (synchronously).
   *
   * To avoid growing the stack proportionally to the # of pending in-messages on the low-level transport, this
   * method does not call itself -- it reads all available messages in a loop until would-block or pipe-hosing error.
   * Therefore the following is *not* a trigger for calling this method:
   *   - we've executed an async-read, and it yielded an in-message synchronously, which we then processed;
   *     and would like to now read as many in-messages as possible that follow it (synchronously).
   *
   * @param lead_else_cont
   *        `true` if seeking lead unstructured in-message; `false` if continuation.
   *        It may be helpful to look at Rcv_next_step docs for brief overview of when it would be one versus
   *        the other.  Or if you love reading and being confused by ygoldfel's verbiage then Msg_in_pipe doc
   *        header.  (The preceding sentence was written by ygoldfel.  Self-shade only.)
   * @param pipe
   *        Pointer into #m_rcv_pipes array.
   */
  void rcv_async_read_lead_or_continuation_msg(Msg_in_pipe* pipe, bool lead_else_cont);

  /**
   * To execute upon completing an `m_channel.async_receive_*()` of an expected lead message along the given in-pipe,
   * this processes the result (message or error) and returns what should be done next; does *not* invoke
   * handlers_poll().
   *
   * @param pipe
   *        See rcv_async_read_lead_or_continuation_msg().
   *        `*pipe->m_target_blob` and `*pipe->m_target_hndl` may have been
   *        received-into.
   * @param err_code
   *        From `Channel::async_receive_*()`.
   * @param sz
   *        From `Channel::async_receive_*()`.
   * @return What to do next: read another lead message; read continuation message; or stop (pipe-hosing error).
   */
  Rcv_next_step rcv_on_async_read_lead_msg(Msg_in_pipe* pipe, const Error_code& err_code, size_t sz);

  /**
   * Same as rcv_on_async_read_lead_msg() but for an expected continuation message instead of lead.
   *
   * @param pipe
   *        See rcv_on_async_read_lead_msg().
   * @param err_code
   *        See rcv_on_async_read_lead_msg().
   * @param sz
   *        See rcv_on_async_read_lead_msg().
   * @return What to do next: read another continuation message; read lead message; or stop (pipe-hosing error).
   */
  Rcv_next_step rcv_on_async_read_continuation_msg(Msg_in_pipe* pipe, const Error_code& err_code, size_t sz);

  /**
   * Helper from `rcv_on_async_read_*()`, processes a newly completed structured in-message, as it exits
   * the `Channel`-layer and enters into the structured-layer of processing.
   *
   * #m_channel_err_code_or_ok must be falsy.
   *
   * @param msg_in
   *        The completed structured in-message: after Msg_in::deserialize_mdt() but before
   *        Msg_in::deserialize_body().
   * @return `false` if something in `msg_in` triggers truthy #m_channel_err_code_or_ok; hence
   *          the async-read chain must end now.  Otherwise `true`.
   */
  bool rcv_struct_new_msg_in(Msg_in_ptr_uniq&& msg_in);

  /**
   * Helper from `rcv_struct_new_msg_in()`: the case where the in-message has a sentinel message ID value, indicating
   * an internal message rather that one from a user send() (et al).
   *
   * #m_channel_err_code_or_ok must be falsy.
   *
   * @param msg_in
   *        Deserialized in-message with sentinel message ID value.
   * @return See rcv_struct_new_msg_in().
   */
  bool rcv_struct_new_internal_msg_in(const Msg_in_impl& msg_in);

  /**
   * Helper from `rcv_struct_new_msg_in()`: the case where #m_phase is Phase::S_CLI_LOG_IN, and the in-message has
   * the log-in-response value 1 + session-token=nil as required: Processes the in-message according to the rigid
   * log-in phase algorithm.  rcv_struct_new_msg_in() returns what this returns, immediately.
   *
   * #m_channel_err_code_or_ok must be falsy.
   *
   * @param msg_in
   *        Deserialized in-message with message ID 1 and session-token=nil.
   * @return See rcv_struct_new_msg_in().
   */
  bool rcv_struct_new_msg_in_during_log_in_as_cli(Msg_in_ptr_uniq&& msg_in);

  /**
   * Like rcv_struct_new_msg_in_during_log_in_as_cli() but for Phase::S_SRV_LOG_IN instead of CLI_LOG_IN.
   *
   * @param msg_in
   *        See rcv_struct_new_msg_in_during_log_in_as_cli().
   * @return See rcv_struct_new_msg_in_during_log_in_as_cli().
   */
  bool rcv_struct_new_msg_in_during_log_in_as_srv(Msg_in_ptr_uniq&& msg_in);

  /**
   * Helper from `rcv_struct_new_msg_in()` (possibly indirectly): the case where the in-message's session-token
   * and message ID have been validated (and the latter indicates a user, not internal, message), and
   * the message ID (seq #) indicates this is the next expected in-order message (hence #m_rcv_msg_next_id was
   * just `++`ed).  #m_phase may be any of the 3 values; but the special processing in
   * rcv_struct_new_msg_in_during_log_in_as_cli() or rcv_struct_new_msg_in_during_log_in_as_srv() must have
   * already completed if not LOGGED_IN.
   *
   * Assuming no detected error therein, this method processes taking `msg_in` from the lower 2-3 layers
   * (Channel-layer, structured message layer, possibly reassembly queue layer) into the next 1-2
   * (possibly caching in #m_rcv_pending_msgs, emission to user).
   *
   * Handles just `msg_in` itself: Does not check whether the gap to any messages in #m_rcv_reassembly_q
   * has been filled by this.  That is, it's not self-perpetuating: caller must call us again for any
   * messages dequeued from #m_rcv_reassembly_q.
   *
   * #m_channel_err_code_or_ok must be falsy.
   *
   * @param msg_in
   *        Deserialized in-message with sentinel message ID value.
   * @return See rcv_struct_new_msg_in().
   */
  bool rcv_struct_new_msg_in_is_next_expected(Msg_in_ptr_uniq&& msg_in);

  /**
   * Helper from rcv_struct_new_msg_in_is_next_expected() that reacts to receiving an otherwise valid
   * reponse in-message when no such response is expected.  Hence it deals with
   * set_unexpected_response_handler() (if set) handler invocation and informing the opposing side, so that
   * it might invoke its set_remote_unexpected_response_handler() (if set) handler.
   *
   * @param msg_in
   *        See rcv_struct_new_msg_in_is_next_expected().
   */
  void rcv_struct_inform_of_unexpected_response(Msg_in_ptr_uniq&& msg_in);

  /**
   * Helper for async handlers:
   * returns `true` if and only if `err_code` indicates a new error or #m_channel_err_code_or_ok
   * indicates a previously-occurred one or both; updates #m_channel_err_code_or_ok to `err_code` in the
   * former case.  Logs appropriately.
   *
   * @param err_code
   *        Code from the async op.
   * @param context
   *        Brief context string for logging.
   * @return `true` if `err_code` or a prior condition indicate the channel is hosed; `false` otherwise.
   */
  bool handle_async_err_code(const Error_code& err_code, util::String_view context);

  /**
   * Helper that handles the situation where #m_channel_err_code_or_ok is falsy, and processing has
   * found a new error condition.  Sets #m_channel_err_code_or_ok accordingly and emits to on-error handler.
   *
   * Do *not* use when synchronously emitting an error (send_core() only as of this writing).
   *
   * @param err_code_not_ok
   *        Truthy code (or assertion may trip).
   * @param context
   *        Brief context string for logging.
   */
  void handle_new_error(const Error_code& err_code_not_ok, util::String_view context);

  /**
   * Helper that returns `true` if and only if start_ops() has not yet been called.  This guard is required for
   * APIs that trigger #m_channel transmission API calls.  Outgoing-direction such calls would be, e.g.,
   * send() and async_end_sending().  Incoming-direction transmission is all triggered by start_and_poll() (which
   * begins the async-read chain(s) indefinitely.)
   *
   * @param context
   *        Brief context string for logging.
   * @return `true` if not started (do not proceed); else `false`.
   */
  bool check_not_started_ops(util::String_view context) const;

  /**
   * Helper for some public APIs to use at the top: ensures that no prior error has been detected
   * (by incoming-direction processing or send()); returns `true` if not; `false` if so.
   * I.e., please no-op if returns `true`.
   *
   * @param context
   *        Brief context string for logging.
   * @return See above.  `true` bad.  `false` good.
   */
  bool check_prior_error(util::String_view context) const;

  /**
   * Helper for most public APIs to use at the top: ensures check_prior_error() passes, and that #m_phase equals
   * `required_phase`; returns `true` if not; `false` if so.  I.e., please no-op if returns `true`.
   *
   * @param required_phase
   *        #m_phase must equal this to return `false`.
   * @param context
   *        Brief context string for logging.
   * @return See above.  `true` bad.  `false` good.
   */
  bool check_phase_and_prior_error(Phase required_phase, util::String_view context) const;

  /**
   * Core of expect_msg(), expect_msgs(), and expect_log_in_request().  Records the expectation of the given
   * message type (`which`) in #m_rcv_pending_msgs; then scans messages queued-up in #m_rcv_pending_msgs with matching
   * #Msg_which_in, in order.  It async-emits one (if `one_off`) or all (otherwise) such messages into `*qd_msgs`
   * out-arg.
   *
   * Note that, if `one_off`, and a matching in-message is queued, #m_rcv_pending_msgs will not have changed
   * when comparing pre- and post-state.
   *
   * @param qd_msgs
   *        See expect_msgs() and similar.
   * @param one_off
   *        `true` for expect_msg() and expect_log_in_request() (which are almost identical); `false`
   *        for expect_msgs().  In the latter case undo_expect_msgs() is a thing.
   * @param which
   *        Message type to expect.
   * @param on_msg_func
   *        See expect_msg() among others.
   * @return `true` usually; `false` if `which` expectation is already in #m_rcv_pending_msgs; or if
   *         #m_phase is LOGGED_IN, and #m_rcv_pending_msgs does contain a queued-up in-message already,
   *         *and* its #Msg_which_in does not equal `which`.  The latter indicates that the logging-in opposing peer
   *         disagrees with the expected protocol and issued a log-in request of unexpected type;
   *         the on-error handler shall fire asynchronously+immediately, similarly to having received
   *         the erroneous in-message *after* expect_msgs_impl() returns.
   */
  bool expect_msgs_impl(Msgs_in* qd_msgs, bool one_off, Msg_which_in which, On_msg_func_ptr&& on_msg_func);

  /**
   * send() and async_request() implementation.
   * Note this is still the higher-level, public send() or async_request() really;
   * it handles *user* messages; and once it has decided to indeed attempt the actual sending over
   * #Owned_channel, it synchronously invokes send_core().  To send an internal message use send_core()
   * directly.
   *
   * `check_unsendable(msg)` and `check_prior_error()` must have returned `false`.
   *
   * @param msg
   *        See send() and sync_request().
   * @param originating_msg_or_null
   *        See send() and sync_request().
   * @param err_code
   *        Non-null target #Error_code.
   * @param on_rsp_func_or_null
   *        For async_request() non-null pointer to `on_rsp_func`; else null.
   * @param id_unless_one_off
   *        Ignored if `on_rsp_func_or_null` is null: See async_request().
   * @return See send() and async_request().
   */
  bool send_impl(const Msg_out& msg, const Msg_in* originating_msg_or_null, Error_code* err_code,
                 const On_msg_func_ptr& on_rsp_func_or_null, msg_id_out_t* id_unless_one_off);

  /**
   * Core of send() or internal-message-send: Serializes the given structured out-messages (metadata out-message;
   * user out-message optionally) and `Owned_channel::send*()`s the result (synchronously).
   *
   * This is the last step of send() and async_request() (for user out-messages) or an internal attempt to send
   * internal out-message.  As such (being the last step):
   * If user message (`msg` not null), and send_core() succeeds in Phase::S_SRV_LOG_IN, #m_phase moves to
   * Phase::S_LOGGED_IN within this method.
   *
   * As of this writing `msg` being null (internal message) means
   * we are logged-in; behavior is undefined (assertion may trip) otherwise.  Rationale: We aim to keep log-in
   * phase very rigid.  This detail could change in the future.
   *
   * @param mdt
   *        Finalized metadata out-message (message about user message; or the internal message itself if it's not
   *        "about" anything) to serialize and send over #Owned_channel.
   *        If internal message: `msg` shall be null, `mdt`-held ID shall be 0 and a filled-out
   *        Msg_mdt_out::internal_msg_body_root().  Otherwise the opposite of all 3 shall hold.
   * @param msg
   *        Pointer to user out-message (as from create_msg() or direct #Msg_out ction); or null
   *        is this is an internal message send.
   * @param err_code_or_ignore
   *        See send_impl() (analogous meaning, even if send_impl() is not calling send_core() in this case).
   *        Or, additionally, leave it null to ignore errors as follows:
   *        #m_channel_err_code_or_ok is not touched even on error; the idea being this is a best-effort
   *        internal message; and if it exposes error on an #m_channel out-pipe, then the next user send
   *        will just see the same error and emit it that time.  Otherwise an internal error could detect
   *        the problem -- and our caller would not emit it.  Thus it would get lost.
   * @return See send_impl() (analogous meaning, even if send_impl() is not calling send_core() in this case).
   *         As of this writing, at this stage, there is only one remaining reason this might return `false`:
   *         `Owned_channel::send*()` yielded transport::error::Code::S_SENDS_FINISHED_CANNOT_SEND.
   */
  bool send_core(const Msg_mdt_out& mdt, const Msg_out_impl* msg, Error_code* err_code_or_ignore);

  /**
   * send() and async_request() helper that returns `true` if and only if `msg` contains a native handle, but
   * #Owned_channel is compile-time-incapable of transporting them.  No `*this` state other than
   * logging context is accessed.
   *
   * @param msg
   *        See, e.g., send().
   * @return `true` if unsendable; else `false`.
   */
  bool check_unsendable(const Msg_out& msg) const;

  // Constants.

  /// Sufficient space to capnp-serialize `schema::detail::ProtocolNegotiation` in one segment.
  static constexpr size_t S_PROTO_NEGOTIATION_SEG_SZ = 256;

  // Data.

  /**
   * The builder engine config for out-messages: Small data store, such that the Struct_builder
   * `Builder_config::Builder` (really the containing class of #Builder_config) main-ctor
   * takes an object of this type, its contents being knobs controlling the behavior of the resulting Struct_builder.
   *   - The *type* #Builder_config (a template parameter) determines the logic part of the serialization policy:
   *     what is the source of RAM for segments (heap? SHM?), as the user mutates an out-message?  How big is each
   *     segment?
   *   - The *members* inside #m_struct_builder_config further tweak the application of that logic by being
   *     knobs: Should prefix/postfix space be left in the serialization?  How quickly should the segments grow?
   *     Where to log?
   *     - This `Config` is passed to `Builder` ctor to configure it.
   *
   * ### Rationale ###
   * By storing this value, copied from the one passed to ctor, we delegate the policy decision (as to what kind
   * of capnp-structured-to-binary serialization engine to use) to the `*this` config (user).  The knobs
   * controlling the configuration of that engine, each time it's created for a given out-message, depend
   * on the builder concrete type, so we don't know what they are or even their nature or number.
   * They are stored opaquely (to us) inside this item.
   *
   * This member is used in two ways.
   *   - Every out-message (whether user-created or an internal one) is accompanied by/is an internal-use
   *     *metadata* message, which is just another Msg_out albeit with an internal friendly API, Msg_mdt_out,
   *     tailored to the stuff we need to put into these.  The builder config used to create this
   *     is #m_struct_builder_config.
   *     - So the schema template param is the internal-use metadata stuff from structured_msg.capnp.
   *   - Every user out-message created via create_msg() is supplied by the latter with #m_struct_builder_config
   *     as the builder config.
   *     - So the schema template param is `<Message_body>`, the `Channel` template param from the user.
   *     - The user is allowed to simply direct-construct the #Msg_out without any create_msg().  They have to
   *       supply the builder config themselves again, though.  It has to be the same type as
   *       #m_struct_builder_config, or it won't compile; but in theory it could have different knob values.
   *       E.g., it could have a null `Logger`, while `*this` one logs-galore.  Usually one would want to keep
   *       it equal (but certainly need not be the same actual #Builder_config object).
   *
   * So it's an important member, but arguably its type is more important than its contents (knobs).
   */
  const Builder_config m_struct_builder_config;

  /**
   * Value (possibly of size 0 depending on this type) to pass to Msg_out::emit_serialization()
   * to indicate the opposing side of the #Owned_channel.  If the type is Null_session (zero-sized), then
   * no information is necessary to indicate this.
   */
  const typename Builder_config::Builder::Session m_struct_lender_session;

  /**
   * Analogous to #m_struct_builder_config but for deserialization.
   * @see #m_struct_builder_config
   */
  const Reader_config m_struct_reader_config;

  /**
   * Handles the protocol negotiation at the start of the pipe, as pertains to algorithms perpetuated by
   * `*this` class's code.
   *
   * @see Protocol_negotiator doc header for key background on the topic.
   */
  Protocol_negotiator m_protocol_negotiator;

  /**
   * Handles the protocol negotiation at the start of the pipe, as pertains to algorithms perpetuated outside
   * of `*this` class's code but instead in #Builder_config #m_struct_builder_config and
   * #Reader_config #m_struct_reader_config and related.
   *
   * @see Protocol_negotiator doc header for key background on the topic.
   */
  Protocol_negotiator m_protocol_negotiator_aux;

  /**
   * Whether start_ops() has been called yet or not.  We could've used #m_channel built-in guards against that,
   * but then as of this writing there's no way to check that at the top of our own various APIs
   * (check_not_started_ops()).
   */
  bool m_started_ops;

  /**
   * The channel-hosed error reporting handler.  See start_and_poll() + class doc header for discussion of
   * error semantics.
   */
  flow::async::Task_asio_err m_on_err_func;

  /**
   * 1 or 2 active (via `optional`) `struct`s containing policy and state w/r/t receipt of low-level
   * (unstructured) messages with the aim to complete the next structured in-message(s).
   *
   * @see Msg_in_pipe doc header for detailed algorithm overview.
   */
  boost::array<std::optional<Msg_in_pipe>, 2> m_rcv_pipes;

  /// See set_unexpected_response_handler().
  On_unexpected_response_func m_on_unexpected_response_func_or_empty;

  /// See set_remote_unexpected_response_handler().
  On_remote_unexpected_response_func m_on_remote_unexpected_response_func_or_empty;

  /**
   * Session token: in Phase::S_LOGGED_IN to send in every out-message and check-against every in-message;
   * in log-in phase *as client* equals nil; in log-in phase *as server* it is auto-generated at construction.
   * In non-log-in phase, it's either provided by user in ctor (if log-in phase skipped), or:
   *   - inherited from log-in phase (if *as server*);
   *   - deserialized from the log-in response (if *as client*).
   *
   * It's used in 2 ways:
   *   - It is sent in every (structured) message -- namely in the lead unstructured message.
   *     SO whenever an `mdt` message is created, we load #m_session_token into it at that time.
   *   - Every message is checked against it (if not correct, channel is hosed immediately).
   *     - There is exactly one exception to this: The log-in request (received in log-in phase *as server*)
   *       must have nil session token: we have not yet sent #m_session_token (which we generated at ction)
   *       to it in the response message, so the client could not have known it.
   */
  Session_token m_session_token;

  /// Next out-message ID (for the next send() or async_request()).
  msg_id_out_t m_snd_msg_next_id;

  /**
   * Phase (w/r/t log-in or lack thereof) of `*this` peer.  It is either Phase::S_LOGGED_IN throughout; or
   * starts as one of the other 2 values, then is assigned LOGGED_IN and unmodified further.
   *
   * If not LOGGED_IN from the start (see ctors) then it is written exactly once:
   *   - Phase::S_SRV_LOG_IN => Phase::S_LOGGED_IN (see ctors): When send() of the log-in response is invoked.
   *   - Phase::S_CLI_LOG_IN => Phase::S_LOGGED_IN (see ctors): When response to the async_request() of the
   *     log-in request is received.
   *
   * It is generally checked at the start of all or most APIs (check_phase_and_prior_error()).
   * In particular many APIs, such as expect_msg(), no-op and return sentinel unless LOGGED_IN; ~one
   * (expect_log_in_request()) does the same unless in a proper `*_LOG_IN` phase.
   * If the user is acting sanely, they would invoke the log-in APIs before
   * entry to LOGGED_IN and vice versa for the logged-in APIs; namely:
   *   - SRV_LOG_IN -> LOGGED_IN: After send() of the log-in response, which itself would be only
   *     after the handler passed to expect_log_in_request() fires.
   *   - CLI_LOG_IN -> LOGGED_IN: After the handler, passed to async_request() along with log-in request out-message,
   *     fires indicating receiving the expected log-in response.
   *
   * If the user disregards these best practices and invokes a logged-in API before those points, then they're
   * engaging in a race.  Worst-case, they'll get an unexpected sentinel return of something like expect_msg() --
   * their own fault.
   */
  Phase m_phase;

  /**
   * Used when #m_phase is Phase::S_SRV_LOG_IN, this starts `false` and is changed
   * to `true` at thread-U call of expect_log_in_request(); if already `true` then a 2nd (illegal)
   * expect_log_in_request() call has been made and shall be ignored (no-op, return sentinel).
   */
  bool m_phase_log_in_started;

  /**
   * Starts falsy; becomes forever truthy (with a specific #Error_code that will not change thereafter)
   * when one of the following detects the first channel-hosing condition: send() or async_request(), on-receive
   * handler rcv_on_async_read_lead_msg() or rcv_on_async_read_continuation_msg().  Once that becomes the case:
   *   - Any *subsequent* on-receive handler (possibly none but at most one per ongoing async-read chain, of which
   *     there are 1-2) will immediately no-op and end async chain.
   *   - Any *subsequent* send() or async_request() will immediately no-op and return `false`.
   *   - Any subsequent other mutable-state-touching API -- e.g., session_token(), expect_msg() -- will immediately
   *     no-op and return `false`/null/sentinel value.  Exception: async_end_sending() (see below).
   *     create_msg() is a convenience thing that does not touch mutable state, so it won't care either.
   *
   * @note async_end_sending() is orthogonal to this.  It is a Channel-level call that (per its doc header) simply
   *       forwards to Channel::async_end_sending(), no questions asked.  The `F()` passed to async_end_sending()
   *       may well trigger synchronous emission of `E`, with `E == m_channel_err_code_or_ok`; or not; but nothing
   *       should or does count on this.
   */
  Error_code m_channel_err_code_or_ok;

  /// Map storing current policy for expecting responses.  See #Expecting_response_map doc header for details.
  Expecting_response_map m_rcv_expecting_response_map;

  /**
   * Map storing current policy for expecting non-response messages.  See #Expecting_msg_map and #Expecting_msg
   * doc headers for details.
   */
  Expecting_msg_map m_rcv_expecting_msg_map;

  /**
   * Next expected in-message ID (sequence #), incremented from initial value 1 to 2, 3, ... on in-order message
   * receipt.  It is used purely for sanity checking of the opposing sender's payloads, when #Owned_channel stores
   * only 1 pipe (Channel::S_HAS_2_PIPES is `false`).  Or: it is used with
   * #m_rcv_reassembly_q if #Owned_channel stores 2 pipes (Channel::S_HAS_2_PIPES is `true`).
   *
   * ### Algorithm summary: 2 in-pipes ###
   * In that latter case a handle-less in-message may arrive along the blobs in-pipe;
   * while a handle-paired message may arrive along the handles in-pipe; and since the two might race, the one
   * sent 1st might arrive 2nd, and vice versa.  Then the out-of-order one would be stored temporarily in
   * #m_rcv_reassembly_q.  (In theory this could occur for several in-messages.)  This is detected by noting that
   * an in-message's message ID (sequence number) exceeds #m_rcv_msg_next_id.  If it equals it, then it is in-order
   * and skips #m_rcv_reassembly_q.  If it is lower, then that is misbehavior by the opposing peer and hoses
   * the pipe.
   *
   * Technically we could be more stringent in our sanity checking: while races may occur between the 2 in-pipes,
   * for a *given* 1 of 2 pipes (the blobs pipe; the handles pipe), the message IDs must always be increasing,
   * as these are reliable, in-order pipes.  However we choose to not worry about this enforcement; in-order
   * behavior of our various transports shall (possibly) be checked elsewhere and is a hard assumption.  On
   * the other hand it is cheap to check for non-repeating (always increasing) message IDs along the totality of
   * the pipes, so we do.
   *
   * ### Algorithm summary: 1 in-pipe ###
   * Each message received must contain ID, in fact, equal #m_rcv_msg_next_id which is then incremented.  There is no
   * reassembly, as there is only 1 in-pipe, and it must be in-order.  This is (as noted just above) easy to
   * sanity-check.
   */
  msg_id_in_t m_rcv_msg_next_id;

  /**
   * When `Owned_channel::S_HAS_2_PIPES`, stores the reassembly queue of in-messages to feed into
   * #m_rcv_pending_msgs once in-messages filling the #msg_id_in_t (sequence #) gap between
   * #m_rcv_msg_next_id and `m_rcv_reassembly_q.front()` is filled.  Once that occurs, the maximally long
   * sequence of in-messages at the front of this queue are moved into #m_rcv_pending_msgs (or fed to waiting
   * handlers immediately if possible).  Left uninitialized if `!S_HAS_2_PIPES`.
   *
   * @see #m_rcv_msg_next_id for algorithm summary; #Reassembly_q doc header for data structure details.
   */
  std::optional<Reassembly_q> m_rcv_reassembly_q;

  /**
   * Queues of in-messages, keyed by #Msg_which_in, each in #msg_id_in_t (sequence #) order, for which no
   * #Msg_which_in handler has yet been registered by the user (expect_msg(), expect_msgs(), expect_log_in_request()).
   * They'll stay there until such a thing is indeed registered; once it is, that queue's elements are fed, in order,
   * to that handler, and the element is removed from #m_rcv_pending_msgs.
   *
   * ### Corner case: a response arrives to an originating out-message for which no response is expected ###
   * It is relatively normal, if ideally avoided (informally speaking), that there is no #Msg_which_in
   * listener yet registered for a given in-message.  (One can be registered later: then such messages will be
   * flushed to that handler.)  By contrast: It is a *user protocol* error that a response
   * arrives unexpectedly.  Let's break down the realistic possibilities for how that might happen:
   *   - A response arrives to message M, but the local user never issued a request for that message as
   *     triggered by async_request().  The opposing user must have erroneously issued a response to
   *     a non-request.
   *   - A response arrives to message M, and the local user *did* issue an async_request() for that message;
   *     but either it was a one-off request -- and this is the 2nd/3rd/...
   *     response -- or it was an open-ended request, but the local user called undo_expect_responses() for it since.
   *     Typically this is a miscommunication at the user level: e.g., opposing user indicated a previous
   *     response was the last one but then erroneously sent another response anyway.  Or, more simply,
   *     it sent 2+ responses to a one-off request (again erroneously).
   *
   * Both situations involve non-trivial user-level protocol mistakes.  What to do?
   *   - We could hose the whole channel: fire all handlers with some fatal #Error_code and disallow further work.
   *     This feels too draconian and inflexible at such a high layer.
   *   - Or we could inform the user of the problem.
   *
   * We choose the latter.  In particular:
   *   - We fire the user-protocol-error handler (see start_and_poll()) locally.  We feed them the problem
   *     in-message and then forget that message: they can do with it as they wish.
   *   - We send an internal message to the opposing peer indicating the issue including the triggering
   *     in-message ID (to them, out-message ID).  The opposing peer `Channel` shall fire their own
   *     user-protocol-error handler similarly.
   *
   * Other than that the channel continues operating.  The user can choose to close it, or ignore the problem,
   * or alert, or whatever.
   *
   * To wit:
   * @see set_unexpected_response_handler().
   * @see set_remote_unexpected_response_handler().
   */
  boost::unordered_map<Msg_which_in, Msg_in_q> m_rcv_pending_msgs;

  // (End of m_rcv_* section.)

  /**
   * The Channel taken-over in ctor, lifetime until dtor.
   *
   * @see also #m_channel_err_code_or_ok.
   */
  Owned_channel m_channel;

  /**
   * The handlers as pushed by handlers_post() to be flushed via handlers_poll() (internally by `*this`).
   *
   * handlers_poll() is only called internally as opposed to from a user API: there is no method the user
   * can invoke that is immediately servable; except:
   *   - `expect_*()` (all going through expect_msg_impl()): But they emit immediate results avoiding the
   *     handler function.
   *   - async_sync_end_sending(): Same.
   *
   * Unlike the core-layer guys, which always deal with at most 1 in-message at a given time, we have a situation
   * where there can be more than 1 handler pending at a time; so this is a list as opposed to just 1 item.
   * That said, there is indeed only 1 at a time if Channel::S_HAS_2_PIPES is `false`; but 1+ may be saved, and
   * fired in handlers_poll() soon, if HAS_2_PIPES is `true`.  At the risk of verbosity to try to help understanding
   * let's examine each case.
   *
   * ### `HAS_2_PIPES = false`: How do we keep it to 1 handler (or 0) at a time? ###
   * Well, why do handlers fire?  Answer:
   *   - If a user is expecting an in-message (a notification via `expect_*()` or a response via async_request()),
   *     and that message arrives, then a saved handler shall fire.
   *   - If a user is not expecting a response, but one arrives, then a special handler shall fire if set.
   *   - If the *opposing* user is not expecting a response, but one arrives, then they send an internal
   *     message which arrives to *us*, and then a special handler *here* shall fire if set.
   *   - If something bad is reported to an `async_receive_*()` handler via `Error_code`, then the channel is
   *     hosed, and #m_on_err_func shall fire.
   *
   * Now, as noted, any cached in-messages to `expect_*()` are not a part of this equation (they are reported
   * differently); nor is the special case of the #m_channel async_end_sending() stuff which is
   * at a lower level and handled separately.  So the above 4 bullet points are the only relevant things.
   *
   * Naturally, then, a handler can fire only as a reaction to an `m_channel.async_receive_*()` receiving an
   * unstructured msg which then completes a structured message (either by itself or by combining with a preceding one).
   * So we choose to fire (synchronously call) any such handler immediately upon processing an unstructured message's
   * contents.  You can see this in rcv_async_read_lead_or_continuation_msg().  So we handlers_post() while
   * processing it... then handlers_poll() right after finishing doing so.
   *
   * That leaves the possibility of said processing `handlers_post()`ing 2+ handlers.  However, no, that is
   * not possible as of this writing:
   *   - If an in-message is a response (it either is or isn't: it is a metadata header), then either
   *     - it is a response to a message earlier marked as awaiting response; or
   *     - it is not -- and it is invalid and is reported (if so configured) via unexpected-response handler.
   *   - If it is not a response, then either
   *     - it is expected via earlier expect_msg_impl() (fire handler); or
   *     - it is not expected (cache it for later non-handler-emission, possibly, via expect_msg_impl()).
   *   - Otherwise it is malformed somehow (internal protocol error a/k/a bug here or in opposing guy).
   *
   * Therefore you can see the fate of a given message is to go down one of these paths, each of which ends
   * either with no handler being posted; or one being posted.
   *
   * Then we handlers_poll() it before trying to read the next unstructured message (or stopping forever).
   *
   * ### `HAS_2_PIPES = true`: Why there can be 1+ handlers at a time ###
   * The key difference is just after the structured message has been fully deserialized but before it is then
   * given to the layer where its structured contents are either cached or emitted to the user.  Namely, we check
   * its ID against the next expected ID (sequence number).  If there is only 1 pipe, it must always have the
   * next expected ID.  However if there are 2, as is the case here, then the seq # may exceed the expected ID
   * causing message to be stored in #m_rcv_reassembly_q.  No problem so far....
   *
   * Now say an in-message arrives that closes the gap between the next expected seq # and the lowest seq #
   * in #m_rcv_reassembly_q.  Now potentially the entire #m_rcv_reassembly_q must be emitted to the user!
   * E.g., if we receive 1, 2, 4, 5, 6, 7, 3, then after 3 we must emit 4-5-6-7 as well.  So in total upon
   * receiving 1 unstructured message (the last continuation message comprised by structured message 3, we then
   * emit five messages in one go).
   *
   * So that's why there can be multiple handlers.
   *
   * ### Rationale: Expressing this as a function object ###
   * I (ygoldfel) suspect the semantics of this guy and handlers_post() and
   * handlers_poll() might be confusing.  It may look like some sort of
   * generalized boost.asio-`post()`-lite scheme, but in reality it is much simpler than that.  In actual reality:
   * we read an in-message; as a result we either need to do nothing user-handler-wise, or we need to invoke
   * 1 relevant handler.  (As of this writing no one in-message can mean 2+ user handlers are applicable.  See above.
   * Conceivably that could change but....)  This calling-of-user-handler just needs to occur a few stack frames up
   * (synchronously at that) and can be triggered by all manner of situations a few stack frames down.  So, really,
   * this could be communicated in some kind of union-enumeration combo.  Expressing it as an actual function to
   * run just seemed like the most code-economical way to express it.  It's easier to code "execute this" rather than
   * "decode this union-enumeration thing to tell you what you should execute."  Perf-wise it might be worse... maybe...
   * as `Function<>` is a polymorphic data structure.  (Premature optimization is the root of all ev... and so on.)
   *
   * ### Rationale: Communicating this via handlers_post() to handlers_poll() ###
   * Orthogonally, saving it (whether a union-enumeration thing or a callback) in this data structure
   * (#m_sync_io_handlers) *felt* more economical (in terms of lines of code) than returning it
   * up a function-call chain.  On the other hand, though, the algorithm would be more rigid-looking and robust
   * that other way; generally speaking I (ygoldfel) always prefer to keep less state and be more functional
   * in style -- this here going against that.  Indeed Native_socket_stream::Impl does it that more-functional way
   * albeit in a totally different setting.  Well, we'll see.  Anyway it works at least.
   */
  std::vector<boost::movelib::unique_ptr<util::Task>> m_sync_io_handlers;
}; // class Channel

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in ../structured_channel.hpp).
#define TEMPLATE_SIO_STRUCT_CHANNEL \
  template<typename Channel_obj, typename Message_body, \
           typename Struct_builder_config, typename Struct_reader_config>
/// Internally used macro; public API users should disregard (same deal as in ../structured_channel.hpp).
#define CLASS_SIO_STRUCT_CHANNEL \
  Channel<Channel_obj, Message_body, Struct_builder_config, Struct_reader_config>

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Channel_obj&& channel,
                                  const Builder_config& struct_builder_config,
                                  const typename Builder_config::Builder::Session& struct_lender_session,
                                  const Reader_config& struct_reader_config,
                                  const Session_token& session_token_non_nil) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_struct_builder_config(struct_builder_config),
  m_struct_lender_session(struct_lender_session),
  m_struct_reader_config(struct_reader_config),

  /* Initial protocol = 1!
   * @todo This will get quite a bit more complex, especially for m_protocol_negotiator_aux,
   *       once at least one relevant protocol gains a version 2.  See class doc header for discussion. */
  m_protocol_negotiator(get_logger(), std::string("struc-") + channel.nickname(), 1, 1),
  m_protocol_negotiator_aux(get_logger(), std::string("struc-aux-") + channel.nickname(), 1, 1),

  m_started_ops(false),

  // Skip log-in phase.  Ready to rock immediately.
  m_session_token(session_token_non_nil),
  m_snd_msg_next_id(1),

  m_phase(Phase::S_LOGGED_IN),

  m_rcv_msg_next_id(1),

  m_channel(std::move(channel))
{
  // (Deets on m_channel are in printing *this.)
  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Created, owning the aforementioned (SIO) Channel.  "
                "Log-in phase bypassed; session token = [" << m_session_token << "].  "
                "(User must invoke start_ops() and (if needed) replace_event_wait_handles().  Assume it occurs soon.)  "
                "Async-read chain will not start until start_and_poll().  Sends possible now.");

  if constexpr(Owned_channel::S_HAS_2_PIPES)
  {
    m_rcv_reassembly_q.emplace();
  }
  // else { Leave the optional<> uninitialized forever. }

  assert((!session_token_non_nil.is_nil())
           && "Pre-logged-in channel must be supplied a non-nil session token, presumably "
                "from session master channel.");
} // Channel::Channel()

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Channel_obj&& channel,
                                  const Builder_config& struct_builder_config,
                                  const typename Builder_config::Builder::Session& struct_lender_session,
                                  const Reader_config& struct_reader_config,
                                  bool is_server) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_struct_builder_config(struct_builder_config),
  m_struct_lender_session(struct_lender_session),
  m_struct_reader_config(struct_reader_config),

  m_started_ops(false),

  // Start in log-in phase.  They can use only the small (log-in-related) part of the public API for now.
  m_session_token(is_server
                    /* @todo In practice this is done rarely per process, probably, but docs do suggest to
                     * save the uuids::random_generator "if you can" due to possible non-trivial startup cost.
                     * We'd have to deal with thread safety + static then though.  Again in reality log-in
                     * probably occurs in only one channel per process and quite rarely. */
                    ? boost::uuids::random_generator()() // Server *creates* the UUID (later sends it to client).
                    : NULL_SESSION_TOKEN), // Client lacks UUID until server sends it in log-in reponse.
  m_snd_msg_next_id(1),
  m_phase(is_server ? Phase::S_SRV_LOG_IN : Phase::S_CLI_LOG_IN),
  m_phase_log_in_started(false),

  m_rcv_msg_next_id(1),

  m_channel(std::move(channel))
{
  // (Deets on m_channel are in printing *this.)
  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Created, owning the aforementioned (SIO) Channel.  "
                "Log-in phase [" << (is_server ? "server" : "client") << "] in effect; session token is nil "
                "until log-in phase completes.  "
                "(User must invoke start_ops() and (if needed) replace_event_wait_handles().  Assume it occurs soon.)  "
                "Async-read chain will not start until start_and_poll().  Sends possible now.");

  if constexpr(Owned_channel::S_HAS_2_PIPES)
  {
    m_rcv_reassembly_q.emplace();
  }
  // else { Leave the optional<> uninitialized forever. }
} // Channel::Channel()

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Channel_obj&& channel,
                                  Serialize_via_heap,
                                  const Session_token& session_token_non_nil) :
  Channel(logger_ptr, std::move(channel),
          Async_io_obj::heap_fixed_builder_config(channel), NULL_SESSION,
          Async_io_obj::heap_reader_config(channel),
          session_token_non_nil)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Channel_obj&& channel,
                                  Serialize_via_heap,
                                  bool is_server) :
  Channel(logger_ptr, std::move(channel),
          Async_io_obj::heap_fixed_builder_config(channel), NULL_SESSION,
          Async_io_obj::heap_reader_config(channel),
          is_server)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Session>
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Channel_obj&& channel,
                                             Serialize_via_session_shm, Session* session,
                                             const Session_token& session_token_explicit) :
  Channel(logger_ptr, std::move(channel),
          session->session_shm_builder_config(), session->session_shm_lender_session(),
          session->session_shm_reader_config(),
          (session_token_explicit == NULL_SESSION_TOKEN)
            ? session->session_token()
            : session_token_explicit)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Session>
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Channel_obj&& channel,
                                             Serialize_via_session_shm, Session* session,
                                             bool is_server) :
  Channel(logger_ptr, std::move(channel),
          session->session_shm_builder_config(), session->session_shm_lender_session(),
          session->session_shm_reader_config(),
          is_server)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Session>
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Channel_obj&& channel,
                                             Serialize_via_app_shm, Session* session,
                                             const Session_token& session_token_explicit) :
  Channel(logger_ptr, std::move(channel),
          session->app_shm_builder_config(), session->app_shm_lender_session(),
          session->app_shm_reader_config(),
          (session_token_explicit == NULL_SESSION_TOKEN)
            ? session->session_token()
            : session_token_explicit)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Session>
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Channel_obj&& channel,
                                             Serialize_via_app_shm, Session* session,
                                             bool is_server) :
  Channel(logger_ptr, std::move(channel),
          session->app_shm_builder_config(), session->app_shm_lender_session(),
          session->app_shm_reader_config(),
          is_server)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::~Channel()
{
  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Shutting down.  "
                "Owned-channel and async-worker shall shut down presently, in that order.");
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Create_ev_wait_hndl_func>
bool CLASS_SIO_STRUCT_CHANNEL::replace_event_wait_handles(const Create_ev_wait_hndl_func& create_ev_wait_hndl_func)
{
  return m_channel.replace_event_wait_handles(create_ev_wait_hndl_func);
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Event_wait_func_t>
bool CLASS_SIO_STRUCT_CHANNEL::start_ops(Event_wait_func_t&& ev_wait_func)
{
  using util::sync_io::Asio_waitable_native_handle;
  using util::sync_io::Task_ptr;

  if (m_started_ops)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Start-ops requested, but they were already started.  "
                     "Ignoring.");
    return false;
  }
  // else
  m_started_ops = true;

  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Start-ops requested.  Accordingly we forward start-ops "
                "requests to owned channel [" << m_channel << "] peers.");
  /* @todo Maybe Channel should offer a start_ops() that'll do what we do?  At the moment one must do it individually
   * peer-by-peer; and that must exist, so that one can lock a different mutex per pipe or whatever -- or formally
   * for it to continue implementing the 4 sync_io::_sender/receiver concepts.  However for a case like ours, where
   * all the op-types are handled by the same code, it would be nice to add a start_ops().  Then what we do here
   * would be available for other Channel users. */

#ifndef NDEBUG
  bool ok;
#endif
  constexpr auto HAS_BLOB = Owned_channel::S_HAS_BLOB_PIPE;
  constexpr auto HAS_HNDL = Owned_channel::S_HAS_NATIVE_HANDLE_PIPE;
  if constexpr(HAS_BLOB)
  {
#ifndef NDEBUG
    ok =
#endif
    m_channel.start_send_blob_ops([this,
                                   ev_wait_func] // Capture a copy (no choice; we need to reuse it below).
                                    (Asio_waitable_native_handle* hndl_of_interest,
                                     bool ev_of_interest_snd_else_rcv,
                                     Task_ptr&& on_active_ev_func) mutable
    {
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: SIO blob-pipe send-op module: event-wait requested.");
      ev_wait_func(hndl_of_interest, ev_of_interest_snd_else_rcv, std::move(on_active_ev_func));
    });
    assert(ok && "m_started_ops should have guarded against this failing.");
    /* This added compile-time branching is to optimize away the copying of ev_wait_func if not needed again below.
     * This is not a perf-critical path, probably, but this code isn't that long.  Might as well.
     * I (ygoldfel) don't think the capture can be conditionally specified to use the move-ctor
     * using some compile-time ternary thing.  So wordy `if constexpr()` it is. */
    if constexpr(HAS_HNDL)
    {
#ifndef NDEBUG
      ok =
#endif
      m_channel.start_receive_blob_ops([this,
                                        ev_wait_func] // Copy again.
                                         (Asio_waitable_native_handle* hndl_of_interest,
                                          bool ev_of_interest_snd_else_rcv, Task_ptr&& on_active_ev_func) mutable
      {
        FLOW_LOG_TRACE("struc::Channel [" << *this << "]: SIO blob-pipe receive-op module: event-wait requested.");
        ev_wait_func(hndl_of_interest, ev_of_interest_snd_else_rcv, std::move(on_active_ev_func));
      });
      assert(ok && "m_started_ops should have guarded against this failing.");
    }
    else
    {
#ifndef NDEBUG
      ok =
#endif
      m_channel.start_receive_blob_ops([this,
                                        ev_wait_func = std::move(ev_wait_func)] // Do not copy.
                                         (Asio_waitable_native_handle* hndl_of_interest,
                                          bool ev_of_interest_snd_else_rcv, Task_ptr&& on_active_ev_func) mutable
      {
        FLOW_LOG_TRACE("struc::Channel [" << *this << "]: SIO blob-pipe receive-op module: event-wait requested.");
        ev_wait_func(hndl_of_interest, ev_of_interest_snd_else_rcv, std::move(on_active_ev_func));
      });
      assert(ok && "m_started_ops should have guarded against this failing.");
    }
  } // if constexpr(HAS_BLOB)
  if constexpr(HAS_HNDL)
  {
#ifndef NDEBUG
    ok =
#endif
    m_channel.start_send_native_handle_ops([this,
                                            ev_wait_func] // See above.
                                             (Asio_waitable_native_handle* hndl_of_interest,
                                              bool ev_of_interest_snd_else_rcv,
                                              Task_ptr&& on_active_ev_func) mutable
    {
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: SIO handle-pipe send-op module: event-wait requested.");
      ev_wait_func(hndl_of_interest, ev_of_interest_snd_else_rcv, std::move(on_active_ev_func));
    });
    assert(ok && "m_started_ops should have guarded against this failing.");
#ifndef NDEBUG
    ok =
#endif
    m_channel.start_receive_native_handle_ops([this,
                                               ev_wait_func = std::move(ev_wait_func)] // Don't need it again.  Eat it.
                                                (Asio_waitable_native_handle* hndl_of_interest,
                                                 bool ev_of_interest_snd_else_rcv,
                                                 Task_ptr&& on_active_ev_func) mutable
    {
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: SIO handle-pipe receive-op module: event-wait requested.");
      ev_wait_func(hndl_of_interest, ev_of_interest_snd_else_rcv, std::move(on_active_ev_func));
    });
    assert(ok && "m_started_ops should have guarded against this failing.");
  } // if constexpr(HAS_HNDL)

  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Start-ops requested.  Done.");
  return true;
} // Channel::start_ops()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::check_not_started_ops(util::String_view context) const
{
  if (!m_started_ops)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: This operation (context [" << context << "]) requires "
                     "start-*-ops which has not been invoked.  Ignoring.");
    return true; // [sic]
  }
  return false;
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Task_err>
bool CLASS_SIO_STRUCT_CHANNEL::start_and_poll(Task_err&& on_err_func)
{
  using flow::util::Blob;
  using boost::movelib::make_unique;

  if (check_not_started_ops("start_and_poll()"))
  {
    return false;
  }
  // else

  if (m_channel_err_code_or_ok)
  {
    // As promised do not re-emit: just follow the no-op contingency.
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Start requested, but the error "
                     "[" << m_channel_err_code_or_ok << "] [" << m_channel_err_code_or_ok.message() << "] "
                     "was previously emitted by either another send() or to the on-error user handler.  Ignoring.");
    return false;
  } // else

  if (!m_on_err_func.empty())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Start requested, but we have already started.  Ignoring.");
    return false;
  }
  // else

  m_on_err_func = std::move(on_err_func);
  assert(!m_on_err_func.empty());

  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Start requested.  Proceeding to start "
                "async-receive chain(s).  On-receive handlers may now execute.");

  /* This is a new structured channel.  So our goal is to receive our 1st structured in-message sans handle
   * or 1st structured in-message with handle -- we have to be ready for both.  This is where we initialize
   * that algorithm forever by setting up m_rcv_pipes 2-array of optional<>s (currently both storing nothing).
   * From that point on, there will be 1 or 2 async-read chains going, and each will get -- and keep passing along
   * via captures and function-args -- one pointer into m_rcv_pipes.
   *
   * See Msg_in_pipe doc header.  Then come back here.  This should be clear then.  Hope you like reading! */

  static_assert(decltype(m_rcv_pipes)::size() == 2, "Maintenance bug?!  m_rcv_pipes[] array must have size 2.");

  if constexpr(Owned_channel::S_HAS_BLOB_PIPE_ONLY)
  {
    // Always start with empty message and a fresh Blob, enough for a lead mdt-message (including segment count).
    auto msg_in = make_unique<Msg_in_impl>(m_struct_reader_config);
    m_rcv_pipes.front()
      = {
          // Always use m_channel.async_receive_blob(); if sender sends a handle it's internal Channel fatal error.
          Msg_in_pipe::S_RCV_SANS_HNDL_ONLY,
          0, // Segment count unknown until lead message received.
          msg_in->add_serialization_segment(rcv_blob_max_size(Msg_in_pipe::S_RCV_SANS_HNDL_ONLY)),
          Native_handle(), // Always start with null target handle; in this case will remain that way always.
          std::move(msg_in)
        };
    if (!m_rcv_pipes.front()->m_target_blob) // I.e., if add_serialization_segment() returned null:
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Unable to obtain the first lead unstructured "
                       "in-message's target blob from in-message builder engine (details likely logged above).  "
                       "Emitting channel-hosing error.");
      handle_new_error(error::Code::S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED,
                       "start_and_poll()");
      return true; // We (the async-read chain) died before we could be born.
    }
    // else

    rcv_async_read_lead_or_continuation_msg(&(*(m_rcv_pipes.front())), true);
    // `*` gets Msg_in_pipe&; `&` converts ref to ptr. --^

    assert(!m_rcv_pipes.back());
  }
  else if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    // As above.
    auto msg_in = make_unique<Msg_in_impl>(m_struct_reader_config);
    m_rcv_pipes.front()
      = {
          // Always use m_channel.async_receive_native_handle(); accept both with- and sans-handle lead messages.
          Msg_in_pipe::S_RCV_WITH_OR_SANS_HNDL_DEMUX,
          0, // As above.
          msg_in->add_serialization_segment(rcv_blob_max_size(Msg_in_pipe::S_RCV_WITH_OR_SANS_HNDL_DEMUX)),
          Native_handle(), // Always start with null target handle; may remain that way or not (per async-receive).
          std::move(msg_in)
        };
    if (!m_rcv_pipes.front()->m_target_blob) // I.e., if add_serialization_segment() returned null:
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Unable to obtain the first lead unstructured "
                       "in-message's target blob from in-message builder engine (details likely logged above).  "
                       "Emitting channel-hosing error.");
      handle_new_error(error::Code::S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED,
                       "start_and_poll()");
      return true; // We (the async-read chain) died before we could be born.
    }
    // else

    rcv_async_read_lead_or_continuation_msg(&(*(m_rcv_pipes.front())), true); // As above.

    assert(!m_rcv_pipes.back());
  }
  else // Reminder: The following is only compiled if the above two compile-time conditions are false.
  {
    static_assert(Owned_channel::S_HAS_2_PIPES, "Maintenance bug?!  Channel must have 1 blobs, 1 handles pipe/both.");

    // As above.
    auto msg_in = make_unique<Msg_in_impl>(m_struct_reader_config);
    m_rcv_pipes.front()
      = {
          /* Always use m_channel.async_receive_native_handle(); accept only with-handle lead messages.
           * The m_rcv_pipes.back() below will in-parallel handle sans-handle lead messages. */
          Msg_in_pipe::S_RCV_WITH_HNDL_ELSE_ERROR,
          0, // As above.
          msg_in->add_serialization_segment(rcv_blob_max_size(Msg_in_pipe::S_RCV_WITH_HNDL_ELSE_ERROR)),
          Native_handle(), // Always start with null target handle; won't remain that way per async-receive.
          std::move(msg_in)
        };

    if (m_rcv_pipes.front()->m_target_blob) // (Just skip this if that guy's add_serialization_segment() failed.)
    {
      // As above.
      msg_in = make_unique<Msg_in_impl>(m_struct_reader_config);
      m_rcv_pipes.back()
        = {
            // Always use m_channel.async_receive_blob(); if sender sends a handle it's internal Channel fatal error.
            Msg_in_pipe::S_RCV_SANS_HNDL_ONLY,
            0, // As above.
            msg_in->add_serialization_segment(rcv_blob_max_size(Msg_in_pipe::S_RCV_SANS_HNDL_ONLY)),
            Native_handle(), // Always start with null target handle; in this case will remain that way always.
            std::move(msg_in)
          };
    } // if (m_rcv_pipes.front()->m_target_blob)

    if ((!m_rcv_pipes.front()->m_target_blob) // I.e., if an add_serialization_segment() returned null:
        || (!m_rcv_pipes.back()->m_target_blob))
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Unable to obtain a first lead unstructured "
                       "in-message's target blob from in-message builder engine (details likely logged above).  "
                       "Emitting channel-hosing error.");
      handle_new_error(error::Code::S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED,
                       "start_and_poll()");
      return true; // We (the async-read chain) died before we could be born.
    }
    // else

    rcv_async_read_lead_or_continuation_msg(&(*(m_rcv_pipes.front())), true); // As above.
    rcv_async_read_lead_or_continuation_msg(&(*(m_rcv_pipes.back())), true); // As above.
  } // else if (Owned_channel::S_HAS_2_PIPES)

  return true;
} // Channel::start_and_poll()

TEMPLATE_SIO_STRUCT_CHANNEL
size_t CLASS_SIO_STRUCT_CHANNEL::rcv_blob_max_size(decltype(Msg_in_pipe::m_lead_msg_mode) mode) const
{
  switch (mode)
  {
  case Msg_in_pipe::S_RCV_WITH_OR_SANS_HNDL_DEMUX:
  case Msg_in_pipe::S_RCV_WITH_HNDL_ELSE_ERROR:
    if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE)
    {
      return m_channel.receive_meta_blob_max_size();
    }
    else
    {
      assert(false && "Should never reach this code path.");
    }
    break;
  case Msg_in_pipe::S_RCV_SANS_HNDL_ONLY:
    if constexpr(Owned_channel::S_HAS_BLOB_PIPE)
    {
      return m_channel.receive_blob_max_size();
    }
    else
    {
      assert(false && "Should never reach this code path.");
    }
  } // switch (mode)
  assert(false && "Did compiler not warn about missed enum value in switch?");
  return 0;
} // Channel::rcv_blob_max_size()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::rcv_async_read_lead_or_continuation_msg(Msg_in_pipe* pipe, bool lead_else_cont)
{
  using boost::make_shared;
  using std::optional;

  /* Our basic algorithm along each *pipe is:
   *   - Read unstructured message:
   *     [ m_channel.async_receive_<pipe>() ]
   *   - Deal with it (might post handlers via handlers_post() -- might need another message to do so -- etc.):
   *     [ rcv_on_async_read_lead/continuation_msg() ]
   *   - Run the handlers that were handlers_post()ed:
   *     [ handlers_poll() ]
   *   - Back to step 1, unless we should stop (details below).
   *
   * Suppose 1,000 message are queued in the in-pipe at this time.  Then the above algorithm would look just
   * like that in code, synchronously: do { ...that stuff... } while (<no more>).
   *
   * Small complication: rcv_on_async_read_lead/continuation_msg() returns whether the next expected message
   * is lead or continuation, meaning it decides which of those 2 use on the *next* message.  Next small
   * complication: on error (whether from the async_receive_*() or due to contents of message or ???) it can specify
   * to stop the chain (forever).  So all in all it returns one of READ_LEAD/CONT_MSG or STOP; STOP ends the
   * loop.
   *
   * Large complication: There may not be 1,000 messages; there may 0, or 3, or 1, say.  And eventually we
   * run out, so step 1 up there will yield sync_err_code=would-block, meaning it started an async-wait,
   * and we need to resume the algorithm from that point on once the async-wait yields a message or channel-hosing
   * error.  So that makes step 1 more like:
   *   - read unstructured message:
   *     [ m_channel.async_receive_<pipe>() ]
   *     - If not would-block, then yeah, continue as written above synchronously.
   *     - Otherwise, next step *here* is STOP.  But on completed async-receive resume above algorithm:
   *       - Deal with it (might post handlers via handlers_post() -- might need another message to do so -- etc.):
   *         [ rcv_on_async_read_lead/continuation_msg() ]
   *       - Run the handlers that were handlers_post()ed:
   *         [ handlers_poll() ]
   *        - If it decided STOP, exit algorithm.
   *   - Back to step 1.
   *     [ Call us!  rcv_async_read_lead_or_continuation_msg()! ]
   *
   * Now to do it. */

  // We know the type of the first message.  Then upon processing it we'll decide the next expected type.
  auto next_step = lead_else_cont ? Rcv_next_step::S_READ_LEAD_MSG : Rcv_next_step::S_READ_CONT_MSG;
  do
  {
    Error_code sync_err_code;
    size_t sync_sz;

    /* These conditions are documented in Msg_in_pipe doc header.  We hereby decide that they are to always be
     * re-upped immediately upon becoming false -- so in the receive handlers: not here.  Initially they're set
     * this way in start_and_poll(). */
    assert((pipe->m_target_blob && (!pipe->m_target_blob->zero()))
           && "Bug: m_target_blob must always point to the fresh one from add_serialization_segment().");
    assert((pipe->m_target_blob->size() == pipe->m_target_blob->capacity())
           && "Bug: m_target_blob must always be sized=capacity upon assignment.");
    assert(pipe->m_target_hndl.null()
           && "Bug: m_target_hndl must always be moved-from (become null) immediately upon Native_handle receipt.");
    assert(((next_step == Rcv_next_step::S_READ_CONT_MSG)
            || ((pipe->m_n_segs_left_after_this_read) == 0))
           && "Bug: m_n_segs_left_after_this_read must be unknown (zero) before async-reading lead msg.");

    // This is the "But on completed async-receive resume above algorithm:" part in the pseudocode above.
    auto on_recv_func = [this, pipe, next_step]
                          (const Error_code& err_code, size_t sz) mutable
    {
      // "Deal with it."
      next_step
        = (next_step == Rcv_next_step::S_READ_LEAD_MSG)
            ? rcv_on_async_read_lead_msg(pipe, err_code, sz)
            : rcv_on_async_read_continuation_msg(pipe, err_code, sz);

      const bool stop = next_step == Rcv_next_step::S_STOP;
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: The above on-received-message logic was invoked "
                     "after an async-wait.  Therefore read chain continues synchronously unless stopping on error "
                     "(are we? = [" << stop << "]).  Invoking result handlers (if any) now.");

      // "Run the handlers."
      handlers_poll("rcv_async_read_lead_or_continuation_msg(1)"); // !!!

      if (stop)
      {
        // It's over.  Get out of the loop and function and read chain.
        return;
      }
      // else

      // "Back to step 1."
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Performing next async-read.");
      (next_step == Rcv_next_step::S_READ_LEAD_MSG)
        ? rcv_async_read_lead_or_continuation_msg(pipe, true)
        : rcv_async_read_lead_or_continuation_msg(pipe, false);
    }; // auto on_recv_func =

    // With that in mind, here in sync-land: "Read unstructured message."

    switch (pipe->m_lead_msg_mode)
    {
    case Msg_in_pipe::S_RCV_WITH_OR_SANS_HNDL_DEMUX:
    case Msg_in_pipe::S_RCV_WITH_HNDL_ELSE_ERROR:
      if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE) // `#if 0` code that wouldn't compile (and never runs).
      {
        if (next_step == Rcv_next_step::S_READ_LEAD_MSG)
        {
          FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-read starting for next lead message; "
                         "with-handle messages accepted only? = "
                         "[" << (pipe->m_lead_msg_mode == Msg_in_pipe::S_RCV_WITH_HNDL_ELSE_ERROR) << "].");
        }
        else
        {
          FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-read starting for continuation message; "
                         "with-handle *lead* messages accepted only? = "
                         "[" << (pipe->m_lead_msg_mode == Msg_in_pipe::S_RCV_WITH_HNDL_ELSE_ERROR) << "].");
        }

  #ifndef NDEBUG
        const bool ok =
  #endif
        m_channel.async_receive_native_handle(&pipe->m_target_hndl, pipe->m_target_blob->mutable_buffer(),
                                              &sync_err_code, &sync_sz,
                                              std::move(on_recv_func));
        assert(ok); // PEER state is an advertised pre-condition for `*this` ctor.
        break;
      }
      else // if constexpr()
      {
        assert(false && "Should never get here with this type of low-level transports configured.");
      }
    case Msg_in_pipe::S_RCV_SANS_HNDL_ONLY:
      if constexpr(Owned_channel::S_HAS_BLOB_PIPE) // `#if 0` code that wouldn't compile (and never runs).
      {
        if (next_step == Rcv_next_step::S_READ_LEAD_MSG)
        {
          FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-read starting for next lead message; "
                         "sans-handle messages accepted only? = yep.");
        }
        else
        {
          FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-read starting for continuation message; "
                         "sans-handle messages accepted only? = yep.");
        }

  #ifndef NDEBUG
        const bool ok =
  #endif
        m_channel.async_receive_blob(pipe->m_target_blob->mutable_buffer(), &sync_err_code, &sync_sz,
                                     std::move(on_recv_func));
        assert(ok); // Same as above.
      }
      else // if constexpr()
      {
        assert(false && "Should never get here with this type of low-level transports configured.");
      }
    // default: Compiler should warn.
    } // switch (m_lead_msg_mode)

    if (sync_err_code == transport::error::Code::S_SYNC_IO_WOULD_BLOCK)
    {
      next_step = Rcv_next_step::S_STOP;
      continue; // Loop will end.  Live to fight another day: async-wait outstanding.
    }
    // else: Got a message or error synchronously.  Handle it right here (a-la on_recv_func() which won't run).

    // "Deal with it."
    next_step
      = (next_step == Rcv_next_step::S_READ_LEAD_MSG)
          ? rcv_on_async_read_lead_msg(pipe, sync_err_code, sync_sz)
          : rcv_on_async_read_continuation_msg(pipe, sync_err_code, sync_sz);

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: The above on-received-message logic was invoked "
                   "synchronously (message was immediately pending).  Therefore read chain continues "
                   "synchronously unless stopping on error (are we? = "
                   "[" << (next_step == Rcv_next_step::S_STOP) << "]).  Invoking result handlers (if any) now.");

    // "Run the handlers."
    handlers_poll("rcv_async_read_lead_or_continuation_msg(2)"); // !!!

    // "Back to step 1 (unless we should stop)."
  }
  while (next_step != Rcv_next_step::S_STOP);
} // Channel::rcv_async_read_lead_or_continuation_msg()

TEMPLATE_SIO_STRUCT_CHANNEL
typename CLASS_SIO_STRUCT_CHANNEL::Rcv_next_step
  CLASS_SIO_STRUCT_CHANNEL::rcv_on_async_read_lead_msg(Msg_in_pipe* pipe, const Error_code& err_code, size_t sz)
{
  using flow::util::Blob;
  using boost::movelib::make_unique;

  assert(err_code != transport::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER); // sync_io must not do this.
  assert(err_code != boost::asio::error::operation_aborted); // Or definitely this.

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-receive handler for would-be lead unstructured "
                 "in-message invoked; result [" << err_code << "] [" << err_code.message() << "]; size received = "
                 "[" << sz << "].");

  if (handle_async_err_code(err_code, "rcv_on_async_read_lead_msg()"))
  {
    return Rcv_next_step::S_STOP; // It's over, either because of us or an earlier pipe hosing.
  }
  // else if (all good (including !err_code)):

  const auto mode = pipe->m_lead_msg_mode;
  const bool got_hndl = !(pipe->m_target_hndl.null());
  assert((!((mode == Msg_in_pipe::S_RCV_SANS_HNDL_ONLY) && got_hndl))
         && "Bug?  Somehow we have truthy target_hndl yet should've invoked async_receive_blob() which can't do that.");
  if ((mode == Msg_in_pipe::S_RCV_WITH_HNDL_ELSE_ERROR) && (!got_hndl))
  {
    /* This is possible but only due to remote-peer misbehavior (sending us weird stuff/over wrong channel).
     * So it's not an assert() situation firstly.  What it's similar-ish to is when, say, a lower-level
     * Native_socket_stream (not that it has to be in Owned_channel -- just using it as an example/foil)
     * was invoked with async_receive_blob() but internally detected a Native_handle arrived too.
     * It's a protocol misbehavior on the part of the user of the low-level/unstructured Channel.
     * In our case it is also that (we are the user of said Channel); it's just that Channel cannot detect that
     * for us by itself (it simply lacks the API for "require Native_handle!").  If it had that capability,
     * it would've emitted the err_code stating that (as it does in the analogy scenario:
     * error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB).  So: we'll do just that ourselves. */
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Async-receive handler for would-be lead unstructured "
                     "in-message invoked sans low-level error; but received sans-handle low-level message "
                     "along the handles-only pipe (of 2 pipes).  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_HNDL,
                     "rcv_on_async_read_lead_msg()");
    return Rcv_next_step::S_STOP;
  }
  /* else if (still all good): All other combinations of `mode` and got_hndl (etc.) are legal.
   * No need to worry about `mode` now; just handle whatever it is (even if certain possibilities, like
   * got_hndl being true with a certain `mode` are not possible).  That is, `if (<must be false>)` is no biggie. */

  /* Another corner case is a message with an empty blob.  At the transport::Channel (unstructured, low) level it's
   * allowed.  We (struc::Channel) never use this ability however.  It would be a protocol error on the remote peer's
   * part. */
  if (sz == 0)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Async-receive handler for would-be lead unstructured "
                     "in-message invoked sans low-level error; but got no (meta-)blob at all.  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_BLOB,
                     "rcv_on_async_read_lead_msg()");
    return Rcv_next_step::S_STOP;
  }
  // else

  auto& target_blob = *pipe->m_target_blob;
  auto& target_hndl = pipe->m_target_hndl;
  static_assert(std::is_same_v<decltype(target_blob), Blob&>, "m_target_blob must be Blob container.");
  static_assert(sizeof(Blob::value_type) == 1, "Blob holds bytes, we assume.");

  // Ignore garbage after the received bytes (this is permanent).
  target_blob.resize(sz);

  // OK, target_blob inside m_incomplete_msg is finalized.  Finalize the Native_handle (if any) in m_incomplete_msg.
  pipe->m_incomplete_msg->store_native_handle_or_null(std::move(target_hndl));
  assert(target_hndl.null() && "Moving-from Native_handle should have nullified it.");

  // target_blob and target_hndl are ready.  Because of the former we can now access:
  {
    Error_code err_code;

    pipe->m_n_segs_left_after_this_read = pipe->m_incomplete_msg->deserialize_mdt(get_logger(), &err_code);
    if (err_code) // It logged on error.
    {
      handle_new_error(err_code, "rcv_on_async_read_lead_msg()");
      return Rcv_next_step::S_STOP;
    }
    // else

    // (W/r/t to the pretty-print -- see any comments near the somewhat-mirrored call in send_core().  May apply here.)
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Deserialized (zero-copy) new structured in-message: "
                   "mandatory metadata portion; further segment count: "
                   "[" << pipe->m_n_segs_left_after_this_read << "]; "
                   "here is the metadata header: "
                   "\n" << ::capnp::prettyPrint(pipe->m_incomplete_msg->mdt_root()).flatten().cStr());
  }

  // Now to finalize dealing with the in-message.

  if (pipe->m_n_segs_left_after_this_read == 0)
  {
    // Sweet: Lead unstructured message encoded entire structured message.
    if (!rcv_struct_new_msg_in(std::move(pipe->m_incomplete_msg)))
    {
      // Channel-hosing deserialization error (it logged).
      return Rcv_next_step::S_STOP;
    }
    // else

    // So get the next one (async)!  Re-init these similarly to start_and_poll().
    pipe->m_incomplete_msg = make_unique<Msg_in_impl>(m_struct_reader_config);

    pipe->m_target_blob
      = pipe->m_incomplete_msg->add_serialization_segment(rcv_blob_max_size(pipe->m_lead_msg_mode));
    if (!pipe->m_target_blob)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Unable to obtain a lead unstructured "
                       "in-message's target blob from in-message builder engine (details likely logged above).  "
                       "Emitting channel-hosing error.");
      handle_new_error(error::Code::S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED,
                       "rcv_on_async_read_lead_msg()");
      return Rcv_next_step::S_STOP;
    }
    // else

    // And now indeed do the read.
    return Rcv_next_step::S_READ_LEAD_MSG;
  } // if (pipe->m_n_segs_left_after_this_read == 0)
  // else if (pipe->m_n_segs_left_after_this_read >= 1):

  // Need more segments to come in.  Keep m_incomplete_msg, as it's still incomplete.  Get next blob to async-read-to.
  --pipe->m_n_segs_left_after_this_read;

  pipe->m_target_blob
    = pipe->m_incomplete_msg->add_serialization_segment(rcv_blob_max_size(pipe->m_lead_msg_mode));
  if (!pipe->m_target_blob)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Unable to obtain the 1st continuation unstructured "
                     "in-message's target blob from in-message builder engine (details likely logged above).  "
                     "Emitting channel-hosing error.");
    handle_new_error(error::Code::S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED,
                     "rcv_on_async_read_lead_msg()");
    return Rcv_next_step::S_STOP;
  }
  // else

  // Get the 1st continuation message!
  return Rcv_next_step::S_READ_CONT_MSG;
} // Channel::rcv_on_async_read_lead_msg()

TEMPLATE_SIO_STRUCT_CHANNEL
typename CLASS_SIO_STRUCT_CHANNEL::Rcv_next_step
  CLASS_SIO_STRUCT_CHANNEL::rcv_on_async_read_continuation_msg(Msg_in_pipe* pipe,
                                                               const Error_code& err_code, size_t sz)
{
  using flow::util::Blob;
  using boost::movelib::make_unique;

  assert(err_code != transport::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER); // sync_io must not do this.
  assert(err_code != boost::asio::error::operation_aborted); // Or definitely this.

  // In its initial basics this method is much like rcv_on_async_read_lead_msg().  Consider reading that one first.

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-receive handler for would-be continuation unstructured "
                 "in-message invoked; result [" << err_code << "] [" << err_code.message() << "]; size received = "
                 "[" << sz << "].");

  if (handle_async_err_code(err_code, "rcv_on_async_read_continuation_msg()"))
  {
    return Rcv_next_step::S_STOP; // It's over, either because of us or an earlier pipe hosing.
  }
  // else if (all good (including !err_code)):

  const bool got_hndl = !(pipe->m_target_hndl.null());
  assert((!((pipe->m_lead_msg_mode == Msg_in_pipe::S_RCV_SANS_HNDL_ONLY) && got_hndl))
         && "Bug?  Somehow we have truthy target_hndl yet should've invoked async_receive_blob() which can't do that.");
  if (got_hndl)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Async-receive handler for would-be continuation "
                     "unstructured in-message invoked sans low-level error; but it contains native handle; "
                     "this is not allowed for continuation messages by definition.  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_HNDL,
                     "rcv_on_async_read_continuation_msg()");
    return Rcv_next_step::S_STOP;
  }
  // else

  // Continuation message is just the blob (we've now ensured this), and all of it is simply the next segment.

  if (sz == 0)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Async-receive handler for would-be continuation "
                     "unstructured in-message invoked sans low-level error; but got no (meta-)blob at all.  "
                     "Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_BLOB,
                     "rcv_on_async_read_continuation_msg()");
    return Rcv_next_step::S_STOP;
  }
  // else

  auto& target_blob = *pipe->m_target_blob;

  // Ignore garbage after the received bytes (this is permanent).
  target_blob.resize(sz);
  // OK, target_blob inside m_incomplete_msg is finalized.

  // target_blob and target_hndl are reset.  Now to finalize dealing with the in-message.
  if (pipe->m_n_segs_left_after_this_read == 0)
  {
    // Sweet: This is the last expected continuation message.
    if (!rcv_struct_new_msg_in(std::move(pipe->m_incomplete_msg)))
    {
      // Channel-hosing deserialization error (it logged).
      return Rcv_next_step::S_STOP;
    }
    // else

    // So get the next one (async)!  Re-init these similarly to start_and_poll().
    pipe->m_incomplete_msg = make_unique<Msg_in_impl>(m_struct_reader_config);

    pipe->m_target_blob
      = pipe->m_incomplete_msg->add_serialization_segment(rcv_blob_max_size(pipe->m_lead_msg_mode));
    if (!pipe->m_target_blob)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Unable to obtain a lead unstructured "
                       "in-message's target blob from in-message builder engine (details likely logged above).  "
                       "Emitting channel-hosing error.");
      handle_new_error(error::Code::S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED,
                       "rcv_on_async_read_continuation_msg()");
      return Rcv_next_step::S_STOP;
    }
    // else

    // And now indeed do the read.
    return Rcv_next_step::S_READ_LEAD_MSG;
  } // if (pipe->m_n_segs_left_after_this_read == 0)
  // else if (pipe->m_n_segs_left_after_this_read >= 1):

  // Need more segments to come in.  Keep m_incomplete_msg, as it's still incomplete.  Get next blob to async-read-to.
  --pipe->m_n_segs_left_after_this_read;

  pipe->m_target_blob
    = pipe->m_incomplete_msg->add_serialization_segment(rcv_blob_max_size(pipe->m_lead_msg_mode));
  if (!pipe->m_target_blob)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Unable to obtain the 2nd/3rd/... continuation unstructured "
                     "in-message's target blob from in-message builder engine (details likely logged above).  "
                     "Emitting channel-hosing error.");
    handle_new_error(error::Code::S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED,
                     "rcv_on_async_read_continuation_msg()");
    return Rcv_next_step::S_STOP;
  }
  // else

  // Get the next continuation message!
  return Rcv_next_step::S_READ_CONT_MSG;
} // Channel::rcv_on_async_read_continuation_msg()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_msg_in(Msg_in_ptr_uniq&& msg_in_moved)
{
  Msg_in_ptr_uniq msg_in(std::move(msg_in_moved));
  // msg_in_moved is now nullified as promised.  We own the Msg_in.
  auto& msg_in_privileged = *msg_in;

  /* Pre-condition: msg_in is with-handle or sans-handle; and has all the serialization segments inside it that
   * the remote peer alleged are needed for deserialization to work; and msg_in.deserialize_msg() has already
   * successfully executed.  So let us attempt to deserialize the body -- if any.
   * Reminder: This does not involve copying or heavy-weight decoding operations, at its core: It initializes
   * a capnp-generated Reader class that simply reads bits out of those segments in RAM.
   *
   * That's the core thing.  But the preceding .deserialize_mdt() call does perform checks on the basics, namely
   * everything that:
   *   - doesn't require knowledge of other in-messages and out-messages; and
   *   - doesn't require going *inside* .body_root() or .internal_msg_body_root() (whichever is applicable).
   *
   * Anyway, that's the rule of thumb, but in practice when we're checking some piece of data via
   * accessors after deserialize_*(), we must simply be aware whether a particular correctness has already been
   * ensured by deserialize_*(); if so great; if not either add it in there (if it fits the above rule of thumb)
   * or ensure it in `*this` code. */

  if (msg_in_privileged.id_or_none() != 0)
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: About to deserialize (zero-copy) new structured in-message: "
                   "optional user-message-body portion.");

    Error_code err_code;
    msg_in_privileged.deserialize_body(&err_code);
    if (err_code)
    {
      handle_new_error(err_code, "rcv_struct_new_msg_in()");
      return false;
    }
    // else

    // (W/r/t to the pretty-print -- see any comments near the somewhat-mirrored calls in send_core().  May apply here.)
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Deserialized user message [" << msg_in_privileged << "].");
    FLOW_LOG_DATA("struc::Channel [" << *this << "]: The complete user message: "
                  "\n" << ::capnp::prettyPrint(msg_in_privileged.body_root()).flatten().cStr());
  }
  // else { msg_in->body_root() will never be accessed (internal message). }

  /* ASAP let's make the famous session-token check that is required for all in-messages.
   * First refer to structured_msg.capnp StructuredMessage.AuthHeader.sessionToken and the mandated rules
   * for checking.  They're important, so restating them here in our context:
   *   - Each Channel is either LOGGED_IN from ction; or starts SRV_LOG_IN/CLI_LOG_IN and
   *     enters LOGGED_IN once a very rigid log-in exchange occurs OK.
   *   - When *_LOG_IN: only two messages are allowed in the world; call them LogInReq and LogInRsp
   *     in this comment.
   *     - SRV_LOG_IN:
   *       - m_session_token is (randomly) generated locally and remains unchanged throughout `*this`;
   *       - LogInReq is the only in-message allowed -- exactly 1 of them -- and its .sessionToken *must* be nil.
   *     - CLI_LOG_IN:
   *       - m_session_token is nil throughout this phase; it is changed to non-nil just before LOGGED_IN entry.
   *       - LogInRsp is the only in-message allowed -- exactly 1 of them -- and its .sessionToken *must* not
   *         be nil; but there is no *correct* value beyond that against which to check.
   *   - When LOGGED_IN:
   *     - m_session_token is non-nil and unchanged throughout this phase (until `*this` dies).
   *     - Every in-message must have .sessionToken equal to m_session_token.
   *
   * Due to the way our state is set up, then, we can confidently perform the following check early and simply.
   * (E.g., we need not worry about sequence #s yet -- can check this right away.) */
  const auto& session_token = msg_in_privileged.session_token();
  if (m_phase == Phase::S_SRV_LOG_IN)
  {
    if (!session_token.is_nil())
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in (as server): Deserialized new structured "
                       "in-message, which must be the log-in request, but the session token is not nil as "
                       "required (is [" << session_token << "]).  Other side misbehaved?");
      handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA,
                       "rcv_struct_new_msg_in()");
      return false;
    }
    // else: OK.
  }
  else if (m_phase == Phase::S_CLI_LOG_IN)
  {
    if (session_token.is_nil())
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in (as client): Deserialized new structured "
                       "in-message, which must be the log-in response, but the session token is nil and "
                       "not non-nil as required.  Other side misbehaved?");
      handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA,
                       "rcv_struct_new_msg_in()");
      return false;
    }
    // else: OK.
  }
  else
  {
    assert(m_phase == Phase::S_LOGGED_IN);
    if (session_token != m_session_token)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured "
                       "in-message, but its session token [" << session_token << "] does not match the "
                       "established session token [" << m_session_token << "]; auth failed.");
      handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_BAD_AUTH,
                       "rcv_struct_new_msg_in()");
      return false;
    }
  }
  // Got here: session_token is OK.  On to ID/seq #.

  const auto id = msg_in_privileged.id_or_none();
  if (id == 0)
  {
    // Internal message.  We'll deal with the various possibilities in here.
    return rcv_struct_new_internal_msg_in(msg_in_privileged);
  }
  // else: User message!

  /* Let's recap.  (All the background info necessary is in the various data member doc headers and possibly class
   * doc header; here we orient ourselves within that body of knowledge.)  We have a valid (in and of itself, maybe
   * not relative to other messages) in-message bearing a user message body.  So it's in the structured layer
   * for the 1st time.  Where to emit it now?  Ultimately the layers are (goal being to move down this list ASAP):
   *   - Just entered structured layer <- we are here.  We will immediately move it to one of these:
   *   - Reassembly queue m_rcv_reassembly_q.
   *     - Moves in to this layer if: the message's ID (seq #) is not the next one expected.
   *     - Moves down from this layer when: subsequent in-message(s) bridge(s) the gap to the message's ID (seq #).
   *     - Skips this layer if: the message's ID (seq #) *is* the next one expected.
   *       - Note: This is *always* the case unless Owned_channel::S_HAS_2_PIPES.  Otherwise a skipped seq # =
   *         protocol error.
   *       - Side effect: if that also fills the gap to the lowest-seq-# in m_rcv_reassembly_q, then
   *         feed 1+ in-messages in m_rcv_reassembly_q to the next layers.
   *   - Unregistered-Msg_which in-message store m_rcv_pending_msgs.
   *     - Moves in to this layer if: *both* hold:
   *       - the message is not a response (originating_msg_id_or_none() == 0); and
   *       - neither expect_msg() or expect_msgs() has been registered to wait for its body_root().which() yet.
   *     - Moves down from this layer if: expect_msg() or expect_msgs() registers a handler for body_root().which().
   *     - Skips this layer if: the message is a response; or it's not, and its body_root().which() has a registered
   *       handler from a prior expect_msg() or expect_msgs().
   *   - ONE OF:
   *     - Response handler F in m_rcv_expecting_response_map is called F(msg_in) and (if one-off) then removed
   *       from said map.
   *       - Moves in to this layer if: the message is a response (originating_msg_id_or_none() != 0).
   *         - m_rcv_expecting_response_map[<that ID>] must exist.
   *         - If it doesn't, then it's an error at the user's protocol level and reported as such
   *           (set_unexpected_response_handler()).
   *     - Msg_which-handler F in m_rcv_expecting_msg_map is called F(msg_in) and (if one-off) then removed
   *       from said map.
   *       - Moves in to this layer if: the message is not a response (originating_msg_id_or_none() == 0).
   *         See above.
   *
   * So let's get to it: Deal with the message ID (seq #). */
  if (id != m_rcv_msg_next_id)
  {
    if (id < m_rcv_msg_next_id)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured in-message, but its ID "
                       "[" << id << "] is duplicate: next expected ID [" << m_rcv_msg_next_id << "] exceeds it.  "
                       "Other side misbehaved?");
      handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA,
                       "rcv_struct_new_msg_in()");
      return false;
    }
    // else

    if constexpr(!Owned_channel::S_HAS_2_PIPES) // C++17: #if-like compile-time check.
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured in-message, but its ID "
                       "[" << id << "] is not sequential to next expected ID [" << m_rcv_msg_next_id << "], even "
                       "though the channel has only 1 in-pipe.  Other side misbehaved?");
      handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA,
                       "rcv_struct_new_msg_in()");
      return false;
    }
    else // if constexpr(Owned_channel::S_HAS_2_PIPES) // Note: it's like #else; so shouldn't just omit it after return.
    {
      if (m_phase != Phase::S_LOGGED_IN)
      {
        FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured in-message, but its ID "
                         "[" << id << "] is not sequential to next expected ID [" << m_rcv_msg_next_id << "] -- and "
                         "we are in a log-in phase (during which only the log-in request or response, with seq#=1, "
                         "is allowed).  Other side misbehaved?");
        handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA,
                         "rcv_struct_new_msg_in()");
        return false;
      }
      // else

      const auto insert_result = m_rcv_reassembly_q->emplace(id, std::move(msg_in));
      if (!insert_result.second)
      {
        FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured in-message, but its ID "
                         "[" << id << "] is not sequential to next expected ID [" << m_rcv_msg_next_id << "] -- and "
                         "the reassembly queue already has an in-message with that ID.  So it's a duplicate ID.  "
                         "Other side misbehaved?");
        handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA,
                         "rcv_struct_new_msg_in()");
        return false;
      }
      // else: inserted OK.  msg_in is now null.
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Deserialized new structured in-message, but its ID "
                     "[" << id << "] is not sequential to next expected ID [" << m_rcv_msg_next_id << "]; "
                     "inserted it into the reassembly queue which now has size "
                     "[" << m_rcv_reassembly_q->size() << "].");
      return true; // That's it for now.  Await the seq # gap to be filled by future in-message(s).
    } // else if constexpr(Owned_channel::S_HAS_2_PIPES)
    assert(false && "Either invalid in-message, or it should've been inserted into reassembly queue + returned.");
  } // if (id != m_rcv_msg_next_id)
  // else if (id == m_rcv_msg_next_id):

  // Corner case possibility for IDs >= 2, during non-LOGGED_IN:
  if ((m_rcv_msg_next_id > 1) && (m_phase != Phase::S_LOGGED_IN))
  {
    assert((m_phase == Phase::S_SRV_LOG_IN)
           && "Phase is CLI_LOG_IN with seq#>=2, but seq#=1 in-message should have immediately moved to "
                "LOGGED_IN or hosed channel... bug?");

    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured in-message, and its ID "
                     "[" << id << "] is sequential to next expected ID [" << m_rcv_msg_next_id << "] -- but "
                     "we are in SRV_LOG_IN phase, during which we are to receive at most 1 in-message.  "
                     "Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA,
                     "rcv_struct_new_msg_in()");
    return false;
  }
  // else

  ++m_rcv_msg_next_id;

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Deserialized new structured in-message, and its ID "
                 "[" << id << "] is sequential to next expected ID which has now been incremented to that value.  "
                 "Handling in-order in-message now.");

  /* We have a good, in-order message.  When logged-in: we should indeed handle the bottom 2 layers (see summary above)
   * as written.  But during log-in we have to handle that specific, somewhat rigid message exchange.
   * It is easier to follow if we just handle that in a separate method (where we can assume many simplifications
   * -- e.g., no reassembly queue to deal with) instead of interspersing the general (logged-in) code path with
   * confusing if()finess that handles the somewhat-special log-in messages. So: */
  switch (m_phase)
  {
  case Phase::S_CLI_LOG_IN:
    return rcv_struct_new_msg_in_during_log_in_as_cli(std::move(msg_in));
  case Phase::S_SRV_LOG_IN:
    return rcv_struct_new_msg_in_during_log_in_as_srv(std::move(msg_in));
  case Phase::S_LOGGED_IN:
    // Fall through:
    break;
  } // Compiler should warn if we missed an enum value.

  // Yay, this is the mainstream (logged-in) case.

  // This handles the next layers (see summary above) for a given in-order message.
  if (!rcv_struct_new_msg_in_is_next_expected(std::move(msg_in)))
  {
    return false;
  }
  // else

  // As noted in the summary we may have bridged gap to m_rcv_reassembly_q.begin() seq #.  Let's handle those too.
  if constexpr(Owned_channel::S_HAS_2_PIPES)
  {
    typename Reassembly_q::iterator first_reassembly_q_it;
    while ((!m_rcv_reassembly_q->empty())
           && (first_reassembly_q_it = m_rcv_reassembly_q->begin())->first == m_rcv_msg_next_id)
    {
      Msg_in_ptr_uniq msg_in(std::move(first_reassembly_q_it->second));
      m_rcv_reassembly_q->erase(first_reassembly_q_it);

      ++m_rcv_msg_next_id;
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: ID (sequence #) gap bridged to "
                     "[" << m_rcv_msg_next_id << "].  Handling in-order in-message now.");

      // As above for the new message:
      if (!rcv_struct_new_msg_in_is_next_expected(std::move(msg_in)))
      {
        return false;
      }
      // else: continue.
    } // while ((!m_rcv_reassembly_q->empty()) && (m_rcv_reassembly_q lowest seq # == m_rcv_msg_next_id))
  } // if constexpr(Owned_channel::S_HAS_2_PIPES)

  return true;
} // Channel::rcv_struct_new_msg_in()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Task>
void CLASS_SIO_STRUCT_CHANNEL::handlers_post(util::String_view context, Task&& handler)
{
  using util::Task;
  using boost::movelib::make_unique;

  auto& handlers = m_sync_io_handlers;

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Handler added in context "
                 "[" << context << "]; handlers count will rise to [" << (handlers.size() + 1) << "].");

  if constexpr(!Owned_channel::S_HAS_2_PIPES)
  {
    assert(handlers.empty()
           && "Each low-level in-message can result in at most one handler due to lacking reassembly-queue in one-pipe"
              "Owned_channel; and we poll handlers after each in-message; "
              "and no handler can add another handler; so how did we end up with 2+ pending handlers?  Bug?");
  }
  // else { As explained in m_sync_io_handlers doc header indeed there can be 2+ handlers collected together. }

  handlers.emplace_back(make_unique<Task>(std::move(handler)));
}

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::handlers_poll(util::String_view context)
{
  auto& handlers_ref = m_sync_io_handlers;

  if (handlers_ref.empty())
  {
    return; // Don't log even.
  }
  // else

  /* Empty it first.  It is possible the handler itself will un-empty it: we don't want to trip over ourselves.
   * (Well, no, it is not possible actually, and we'll assert() to that effect below.  However doing it this way
   * is cleaner in terms of future-proofing/maintenance.) */
  const auto handlers = std::move(handlers_ref);
  assert(handlers_ref.empty() && "The move() should've cleared it.");

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Flushing [" << handlers.size() << "] ready handlers "
                 "in context [" << context << "].");

  for (size_t idx = 0; idx != handlers.size(); ++idx)
  {
    const auto& handler = *(handlers[idx]);
    handler();

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Handler [" << idx << "] (0-based) of "
                   "[" << handlers.size() << "] (1-based) ready handlers: completed.");
  } // for (idx in [0, handlers.size()))

  assert(handlers_ref.empty() && "Did handler re-push a handler?  Should not be possible with struc::Channel.");
} // Channel::handlers_poll()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_internal_msg_in(const Msg_in_impl& msg_in_privileged)
{
  using std::string;

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Deserialized new structured in-message: it is an internal "
                 "message, meaning it does not carry a user-generated body.");
  const auto& root = msg_in_privileged.internal_msg_body_root();

  /* Maintenance note: When InternalMessageBody becomes a union -- adds more message types --
   * replace the following with `switch (root.which())`.  For now though there's only one possibility: */

  if (!root.hasUnexpectedResponse())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured in-message: it is an internal "
                     "message, but its type is unknown: currently only `unexpectedResponse` is supported.  "
                     "This should not be possible under proper remote peer behavior, as we maintain a strict "
                     "protocol version/capability system.  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_INTERNAL_MSG_TYPE_UNKNOWN,
                     "rcv_struct_new_internal_msg_in()");
    return false;
  }
  // else

  const auto originating_msg_id = msg_in_privileged.originating_msg_id_or_none();
  const auto rsp_root = root.getUnexpectedResponse();
  const string mdt_text = rsp_root.getOriginatingMessageMetadataText();
  FLOW_LOG_TRACE("Unexpected-response message regarding earlier out-message with ID [" << originating_msg_id << "]; "
                 "metadata text = [" << mdt_text << "]; will inform user via special handler if registered.");

  if (originating_msg_id == 0)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured in-message: it is an internal "
                     "message indicating unexpected response out-message, but the ID of that out-message is 0 which "
                     "is a violation of our internal protocol.  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA,
                     "rcv_struct_new_internal_msg_in()");
    return false;
  }
  // else

  handlers_post("rcv_struct_new_internal_msg_in()", [this, originating_msg_id, mdt_text = std::move(mdt_text)]
                                                      () mutable
  {
    if (m_on_remote_unexpected_response_func_or_empty.empty())
    {
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Would invoke on-remote-unexpected-rsp user handler; "
                     "but none is configured.  Restating the details: "
                     "out-message with ID [" << originating_msg_id << "]; metadata text = [" << mdt_text << "].");
    }
    else
    {
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking on-remote-unexpected-rsp user handler.");
      m_on_remote_unexpected_response_func_or_empty(originating_msg_id, std::move(mdt_text));
    }
  }); // handlers_post()

  return true;
} // Channel::rcv_struct_new_internal_msg_in()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_msg_in_during_log_in_as_cli(Msg_in_ptr_uniq&& msg_in_moved)
{
  Msg_in_ptr_uniq msg_in(std::move(msg_in_moved));
  // msg_in_moved is now nullified as promised.  We own the Msg_in.
  auto& msg_in_privileged = *msg_in;

  /* We are the logging-in client (CLI_LOG_IN).  During this phase the only message we can receive is
   * a single log-in response to the log-in request we'd earlier sent.  So now we shall ascertain that is
   * what is happening.  Namely:
   *   - m_rcv_expecting_response_map must have exactly 1 registered-for-response message (the log-in).
   *     - How this can break:
   *       - If they hadn't (successfully, meaning with an on-response handler provided) send()t the log-in request.
   *       - If they hadn't even constructed the latter at all.
   *   - msg_in is a response, and it indeed indicates the log-in out-message (by necessity having msg_id_out_t = 1)
   *     is the originating message to which it is a response.
   *     - It has a non-nil session-token... which, by the way, we save onto our currently-nil m_session_token!
   *       Oh and m_phase shall go to LOGGED_IN foreva!
   *     - How these can break: By the other side misbehaving.
   *   - Note: We could assert() a bunch of other things (@todo maybe we should), but I'm (ygoldfel) fairly
   *     confident we wouldn't have allowed any other ways of breaking this; that's why we prohibit general
   *     expect_msg(), etc., etc., until logged-in phase.
   *
   * Let's go.... */

  if (m_rcv_expecting_response_map.size() != 1)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in: Would-be log-in response message "
                     "received outside the situation where we'd sent the log-in request and are awaiting response; "
                     "the await-rsp map should have 1 element but instead has "
                     "[" << m_rcv_expecting_response_map.size() << "].  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA,
                     "rcv_struct_new_msg_in_during_log_in_as_cli()");
    return false;
  }
  // else

  assert(flow::util::key_exists(m_rcv_expecting_response_map, 1)
         && "During CLI_LOG_IN the message expecting response must be the log-in request, with ID 1.");

  if (msg_in_privileged.originating_msg_id_or_none() != 1)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in: Would-be log-in response message "
                     "received as expected -- except it does not specify originating message ID=1 as required "
                     "in this specific situation; it specifies "
                     "[" << msg_in_privileged.originating_msg_id_or_none() << "].  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA,
                     "rcv_struct_new_msg_in_during_log_in_as_cli()");
    return false;
  }
  // else

  assert(m_session_token.is_nil() && "While CLI_LOG_IN our saved session_token must be nil.");
  m_session_token = msg_in_privileged.session_token();
  if (m_session_token.is_nil())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in: Would-be log-in response message "
                     "received as expected and is a response to the log-in request; but it lacks a non-nil "
                     "session-token!  Cannot complete log-in.  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA,
                     "rcv_struct_new_msg_in_during_log_in_as_cli()");
    return false;
  }
  // else if (we've updated m_session_token to its foreva-value):

  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Log-in: Would-be log-in response message "
                "received as expected and is a response to the log-in request; session token "
                "[" << m_session_token << "] saved; entering LOGGED_IN phase.");
  m_phase = Phase::S_LOGGED_IN;

  /* Awesome!  So now we can just use the general code path, same as what the logged-in case invokes for the
   * new in-order message (individually -- it doesn't worry about then scanning the reassembly queue).
   * It will do some slight re-computations like the above is-it-a-response check, but that's fine; they're
   * quick and easy, and it's worth it for the simpler code.
   *
   * We just needed to check that and save the session-token and update m_phase; now everything is normal
   * forever (in the shiny new LOGGED_IN phase). */
  return rcv_struct_new_msg_in_is_next_expected(std::move(msg_in));
} // Channel::rcv_struct_new_msg_in_during_log_in_as_cli()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_msg_in_during_log_in_as_srv(Msg_in_ptr_uniq&& msg_in_moved)
{
  Msg_in_ptr_uniq msg_in(std::move(msg_in_moved));
  // msg_in_moved is now nullified as promised.  We own the Msg_in.
  const auto which = msg_in->body_root().which();
  auto& msg_in_privileged = *msg_in;

  /* We are the logged-in-to server (SRV_LOG_IN).  During this phase the only message we can receive is
   * a single log-in request which either is expected due to expect_log_in_request() -- or isn't, but then
   * they can call it later (and if so they'll only be allowed to call it with the Which received here --
   * or it's a protocol error... but that's not our problem here).  So now we shall ascertain that is what is
   * happening.  Namely:
   *   - msg_in isn't a response to anything (is unsolicited: notification or request).
   *     Subtlety: It can't be a response to anything anyway: in SRV_LOG_IN we cannot send
   *     anything except the log-in response (which cannot happen until we process the present log-in,
   *     by advertised contract of expect_log_in_request()), so there's nothing to which to respond, and this would
   *     be detected in regular rcv_struct_new_msg_in_is_next_expected() we'd call below.
   *     However, receiving an unexpected response
   *     is not (unlike most mistmatches) considered a pipe-hosing error but instead a user error normally
   *     reported via set_unexpected_response_handler() (etc.) -- which they can treat how they want.
   *     In this case, during log-in, we want to be totally rigid and just outlaw the whole situation totally.
   *     - How this can break: Remote peer could send a reponse.  Actually this wouldn't be easy: there's no
   *       API that'll work for it in the CLI_LOG_IN phase... but who knows what code they have; we err on the
   *       side of <whatever>.
   *   - m_rcv_expecting_msg_map has either 0 or 1 registered expectation.
   *     - If the latter: its Msg_which_in must equal the one in msg_in (in-message received here).
   *       - How this can break: If they called expect_log_in_request() but with a different `which` than
   *         delivered here.  Could be local user error; remote user error; or both.
   *     - If the former: this one will be cached to match up to a future expect_log_in_request() message... but
   *       it must be the only one thus cached (only one log-in request is allowed).
   *       - How this can break: Remote peer could send a request 2x.  Actually this wouldn't be easy: there's no
   *         API that'll work for it in the CLI_LOG_IN phase... but who knows what code they have; we err on the
   *         side of <whatever>.
   *   - Note: We could assert() a bunch of other things (some, we do; @todo maybe we should do more),
   *     but I'm (ygoldfel) fairly confident we wouldn't have allowed any other ways of breaking this; that's why
   *     we prohibit general expect_msg(), etc., etc., until logged-in phase.
   *
   * Let's go.... */

  assert(m_rcv_expecting_response_map.empty() && "Only a single expect_log_in_request() allowed in SRV_LOG_IN phase.");
  if (msg_in_privileged.originating_msg_id_or_none() != 0)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in: Would-be log-in request message "
                     "received -- except it's a response to something; this is illegal.  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA,
                     "rcv_struct_new_msg_in_during_log_in_as_srv()");
    return false;
  }
  // else if (it's not a response; is unsolicited): OK, perform the other checks above.

  if (m_rcv_expecting_msg_map.size() == 1)
  {
    const auto exp_which = m_rcv_expecting_msg_map.begin()->first;
    if (exp_which != which)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in: Would-be log-in request message "
                       "received as expected -- except it does not specify union-which = [" << int(exp_which) << "] "
                       "as specified by earlier local expect_log_in_request() call but rather "
                       "[" << int(which) << "].  This might be a protocol error on the user's part on this "
                       "or other side or both.");
      /* Subtlety: Normally we'd emit S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA; but that's for
       * internal errors -- in this case the error can/is the user's by saying they expect X locally but sending
       * Y remotely; both X and Y are user decisions.  So it's not an internal error. */
      handle_new_error(error::Code::S_STRUCT_CHANNEL_GOT_UNEXPECTED_LOG_IN_REQUEST,
                       "rcv_struct_new_msg_in_during_log_in_as_srv()");
      return false;
    }
    // else

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Log-in: Would-be log-in request message "
                   "received as expected -- and it specifies union-which = [" << int(exp_which) << "] "
                   "as specified by earlier local expect_log_in_request() call.");
  } // if (m_rcv_expecting_msg_map.size() == 1)
  else // if (m_rcv_expecting_msg_map.size() == 0, or >= 2)
  {
    assert(m_rcv_expecting_msg_map.empty() && "Only a single expect_log_in_request() allowed in SRV_LOG_IN phase.");

    if (!m_rcv_pending_msgs.empty())
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in: Would-be log-in request message "
                       "received as expected -- but no expect_log_in_request() registered yet.  Would await that "
                       "and ensure its union-which matches -- except apparently such a would-be log-in request "
                       "has already been received and cached; this is not allowed.  Other side misbehaved?");
      handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA,
                       "rcv_struct_new_msg_in_during_log_in_as_srv()");
      return false;
    }
    // else

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Log-in: Would-be log-in request message "
                   "received as expected -- but no expect_log_in_request() registered yet.  Will await that "
                   "and ensure its union-which matches.");
  } // else if (m_rcv_expecting_msg_map.size() == 0)

  /* Awesome!  So now we can just use the general code path, same as what the logged-in case invokes for the
   * new in-order message (individually -- it doesn't worry about then scanning the reassembly queue).
   * It will do some slight re-computations like the above is-it-a-response check, but that's fine; they're
   * quick and easy, and it's worth it for the simpler code.
   *
   * We just needed to check the above.  Though, note, we're still not in LOGGED_IN phase; that will occur
   * only once they send() their response to this log-in request. */
  return rcv_struct_new_msg_in_is_next_expected(std::move(msg_in));
} // Channel::rcv_struct_new_msg_in_during_log_in_as_srv()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_msg_in_is_next_expected(Msg_in_ptr_uniq&& msg_in_moved)
{
  using boost::promise;
  using std::holds_alternative;
  using std::monostate;

  Msg_in_ptr_uniq msg_in(std::move(msg_in_moved));
  // msg_in_moved is now nullified as promised.  We own the Msg_in.
  auto& msg_in_privileged = *msg_in;

  const auto id = msg_in_privileged.id_or_none();
  assert(id != 0);

  /* Refer to the summary comment (regarding layers of handling structured in-messages) in our caller.
   * We are handling the last 2 layers mentioned there in this method.
   *
   * So either it's a response or unsolicited. */
  const auto originating_msg_id = msg_in_privileged.originating_msg_id_or_none();
  if (originating_msg_id != 0)
  {
    // It's a response.
    const auto exp_rsp_it = m_rcv_expecting_response_map.find(originating_msg_id);
    if (exp_rsp_it == m_rcv_expecting_response_map.end())
    {
      /* It's a response to something that doesn't exist or didn't express it desired a response.
       * See rationale for how we handle it in m_rcv_pending_msgs doc header.  So do that: namely,
       *   - send an internal-message (out-of-band, meaning seq #s are irrelevant -- message ID 0)
       *     informing remote peer;
       *   - emit offending msg_in to special handler (if registered) and forget it. */
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Structured in-message ID "
                       "[" << id << "] is itself in-order but is a response to "
                       "alleged out-message ID [" << originating_msg_id << "] which either never existed or "
                       "is not registered as awaiting a response.  This is an error on the user's part and thus "
                       "not fatal.  Will send this as an internal-message to remote peer; and will fire "
                       "registered on-unexpected-rsp handler (if any) locally.");

      rcv_struct_inform_of_unexpected_response(std::move(msg_in));
      return true;
    } // if (exp_rsp_it not found)
    // else if (exp_rsp_it found)

    const Expecting_response& exp_rsp = *exp_rsp_it->second;
    const bool one_off = exp_rsp.m_one_expected; // Delete exp_rsp_it below if true (single response expected).
    auto on_msg_func = exp_rsp.m_on_msg_func;
    assert(!on_msg_func->empty());

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Structured in-message ID "
                   "[" << id << "] is itself in-order and is a response to "
                   "out-message ID [" << originating_msg_id << "] which is expecting the response "
                   "(one-off? = [" << one_off << "]).  "
                   "Will delete expectation if one-off; post handler.");

    handlers_post("rcv_struct_new_msg_in_is_next_expected(1)",
                  [this, msg_in = Msg_in_ptr(msg_in.release()), // Upgrade to shared_ptr<> by the way.
                   on_msg_func = std::move(on_msg_func)]() mutable
    {
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking on-rsp user handler.");
      (*on_msg_func)(std::move(msg_in));
    });

    if (one_off)
    {
      m_rcv_expecting_response_map.erase(exp_rsp_it);
    }
    return true;
  } // if (originating_msg_id != 0) (is response)
  // else if (originating_msg_id == 0) (is unsolicited)

  /* Refer once again to the summary comment in calling method.  Unsolicited message may need to be cached -- or not.
   * (That's unlike responses: Either something was expecting it; or not and it's an error.) */

  const Msg_which_in which = msg_in->body_root().which();
  const auto exp_msg_it = m_rcv_expecting_msg_map.find(which);
  if (exp_msg_it == m_rcv_expecting_msg_map.end())
  {
    auto& q = m_rcv_pending_msgs[which]; // Create queue if needed.
    q.emplace(std::move(msg_in)); // Move the unique_ptr into cached queue.
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Structured in-message ID "
                   "[" << id << "] is itself in-order and unsolicited; currently no handler is registered "
                   "for its union-which = [" << int(which) << "]; cached into per-union-which queue now sized "
                   "[" << q.size() << "].");
    return true;
  }
  // else if (exp_msg_it exists)

  // This is much like the response handling above.

  const Expecting_msg& exp_msg = *exp_msg_it->second;
  const bool one_off = exp_msg.m_one_expected; // Delete exp_msg_it below if true (single response expected).
  auto on_msg_func = exp_msg.m_on_msg_func;
  assert(!on_msg_func->empty());

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Structured in-message ID "
                 "[" << id << "] is itself in-order and unsolicited; currently expecting its "
                 "union-which = [" << int(which) << "] (one-off? = [" << one_off << "]).  "
                 "Will fire handler and delete expectation if one-off.");

  handlers_post("rcv_struct_new_msg_in_is_next_expected(2)",
                [this, msg_in = Msg_in_ptr(msg_in.release()), // Upgrade to shared_ptr<> by the way.
                 on_msg_func = std::move(on_msg_func)]() mutable
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking on-msg user handler (uncached).");
    (*on_msg_func)(std::move(msg_in));
  });

  if (one_off)
  {
    m_rcv_expecting_msg_map.erase(exp_msg_it);
  }

  return true;
} // Channel::rcv_struct_new_msg_in_is_next_expected()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::rcv_struct_inform_of_unexpected_response(Msg_in_ptr_uniq&& msg_in)
{
  using flow::util::ostream_op_string;

  /* There are 2 actions below: send_core() an internal message about the unexpected response, for the other
   * side's benefit (it can fire its on-*remote*-unexpected-response handler if applicable); and -- separately --
   * fire our on-*local*-unexpected-response handler.  The latter definitely should be handlers_post()ed to
   * avoid recursive mayhem as usual.  Should the send_core() action also be handlers_post()ed, or could we just
   * do it synchronously?  Back when Channel was async-I/O all the way (before sync_io pattern existed)
   * I had post()ed it onto thread W, which at the time was used exclusively for firing user handlers; the idea
   * was to emulate the user themselves send()ing a thing.  Now, though, that appears unnecessary: it's an internal
   * best-effort message; we don't even care about any error, and it cannot be replied-to. */

  auto& msg_in_privileged = *static_cast<Msg_in_impl*>(msg_in.get());
  Msg_mdt_out mdt(m_struct_builder_config, m_session_token, msg_in_privileged.id_or_none(),
                  0, 0);
  // Indicate we're referencing offending msg *msg_in. -----^

  /* For the metadata-text, shove in a pretty-printing of the metadata header with lots of goodies in there --
   * but reasonably capped in length (and in compute used, though certainly not super-quick either) and
   * *not* including the user message body itself.  However do add the top-level union-which as well. */
  auto rsp_root = mdt.internal_msg_body_root().initUnexpectedResponse();
  rsp_root.setOriginatingMessageMetadataText
             (ostream_op_string("user-msg-union-which = ", int(msg_in_privileged.body_root().which()),
                                ", metadata-header =\n",
                                ::capnp::prettyPrint(msg_in_privileged.mdt_root()).flatten().cStr()));

  // Send the internal message.  Forego some of the vanilla-send() steps and ignore errors (spirit = do our best).
  if (!m_channel_err_code_or_ok)
  {
    send_core(mdt, nullptr, nullptr); // Last nullptr => ignore error.
  }

  handlers_post("rcv_struct_inform_of_unexpected_response()",
                [this, msg_in = Msg_in_ptr(msg_in.release())] // Upgrade to shared_ptr<> by the way.
                 () mutable
  {
    auto& msg_in_privileged = *static_cast<Msg_in_impl*>(msg_in.get());

    if (m_on_unexpected_response_func_or_empty.empty())
    {
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Would invoke on-unexpected-rsp user handler; "
                     "but none is configured.  Restating some details: "
                     "Structured in-message ID [" << msg_in_privileged.id_or_none() << "] is a response to "
                     "alleged out-message ID [" << msg_in_privileged.originating_msg_id_or_none() << "] which either "
                     "never existed or is not registered as awaiting a response.");
    }
    else
    {
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking on-unexpected-rsp user handler.");
      m_on_unexpected_response_func_or_empty(std::move(msg_in));
    }
  }); // handlers_post()
} // Channel::rcv_struct_inform_of_unexpected_response()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::handle_async_err_code(const Error_code& err_code, util::String_view context)
{
  const auto this_is_hosed = bool(err_code);
  const auto overall_hosed = bool(m_channel_err_code_or_ok);

  if (!this_is_hosed)
  {
    if (overall_hosed)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: An operation [" << context << "] yielded success, "
                       "but the channel was previously hosed; so ignoring the operation's result and potentially "
                       "ending the calling async chain.");
      return true;
    }
    // else if (!overall_hosed)
    return false;
  } // else if (this_is_hosed):

  if (overall_hosed) // && this_is_hosed
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: An operation [" << context << "] yielded failure "
                     "[" << err_code << "] [" << err_code.message() << "]; but the channel was previously already "
                     "hosed for reason "
                     "[" << m_channel_err_code_or_ok << "] [" << m_channel_err_code_or_ok.message() << "]; "
                     "so ignoring the operation's result and potentially ending the calling async chain.");
  }
  else // if (!overall_hosed) // && this_is_hosed
  {
    handle_new_error(err_code, context);
  }

  return true;
} // Channel::handle_async_err_code()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::handle_new_error(const Error_code& err_code_not_ok, util::String_view context)
{
  assert(err_code_not_ok && "Bug?  Call this only when error detected.");
  assert((!m_channel_err_code_or_ok) && "Bug?  Call this only ascertaining the channel isn't already hosed.");
  const auto& err_code = err_code_not_ok; // For brevity.

  FLOW_LOG_WARNING("struc::Channel [" << *this << "]: An operation [" << context << "] yielded failure "
                   "[" << err_code << "] [" << err_code.message() << "] thus hosing the channel; "
                   "saving that fail-reason and potentially ending the calling async chain.");
  m_channel_err_code_or_ok = err_code; // It will now never change.

  handlers_post("handle_new_error()", [this]() mutable
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Invoking on-error user handler.");
    assert((!m_on_err_func.empty()) && "Bug?  Error shall be reported at most once.");
    m_on_err_func(m_channel_err_code_or_ok);

    m_on_err_func.clear(); // Save RAM.  Also help detect any breaking of promise that we'd invoke it at most once.
  }); // handlers_post()
} // Channel::handle_new_error()

TEMPLATE_SIO_STRUCT_CHANNEL
const typename CLASS_SIO_STRUCT_CHANNEL::Builder_config&
  CLASS_SIO_STRUCT_CHANNEL::struct_builder_config() const
{
  return m_struct_builder_config;
}

TEMPLATE_SIO_STRUCT_CHANNEL
const typename CLASS_SIO_STRUCT_CHANNEL::Builder_config::Builder::Session&
  CLASS_SIO_STRUCT_CHANNEL::struct_lender_session() const
{
  return m_struct_lender_session;
}

TEMPLATE_SIO_STRUCT_CHANNEL
const typename CLASS_SIO_STRUCT_CHANNEL::Reader_config&
  CLASS_SIO_STRUCT_CHANNEL::struct_reader_config() const
{
  return m_struct_reader_config;
}

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::check_unsendable(const Msg_out& msg) const
{
  if constexpr(Owned_channel::S_HAS_BLOB_PIPE_ONLY) // C++17: This is #if-like.
  {
    const auto hndl = msg.native_handle_or_null();
    if (!hndl.null())
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send native handle "
                       "[" << hndl << "], but the owned channel is incapable of such "
                       "transport.  Ignoring.  Bug by user?");
      return true;
    }
    // else
  }
  return false;
} // Channel::check_unsendable()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::send(const Msg_out& msg, const Msg_in* originating_msg_or_null, Error_code* err_code)
{
  if (check_not_started_ops("send()"))
  {
    return false;
  }
  // else

  if (check_unsendable(msg))
  {
    return false;
  }
  // else

  if (check_prior_error("send()"))
  {
    return false;
  }
  // else

  return send_impl(msg, originating_msg_or_null, err_code, On_msg_func_ptr(), nullptr);
} // Channel::send()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename On_msg_handler>
bool CLASS_SIO_STRUCT_CHANNEL::async_request(const Msg_out& msg, const Msg_in* originating_msg_or_null,
                                             msg_id_out_t* id_unless_one_off, On_msg_handler&& on_rsp_func,
                                             Error_code* err_code)
{
  using boost::make_shared;

  if (check_not_started_ops("async_request()"))
  {
    return false;
  }
  // else

  if (check_unsendable(msg))
  {
    return false;
  }
  // else

  if (check_prior_error("async_request()"))
  {
    return false;
  }
  // else

  return send_impl(msg, originating_msg_or_null, err_code,
                   make_shared<On_msg_func>(std::move(on_rsp_func)), id_unless_one_off);
} // Channel::async_request()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::send_impl(const Msg_out& msg_public, const Msg_in* originating_msg_or_null,
                                         Error_code* err_code,
                                         /* @todo Make it && for a little perf boost.  T&& doesn't play well
                                          * with FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(), as I recall, so it'll take
                                          * a bit of maneuvering. */
                                         const On_msg_func_ptr& on_rsp_func_or_null, msg_id_out_t* id_unless_one_off)
{
  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, Channel::send_impl, flow::util::bind_ns::cref(msg_public),
                                     originating_msg_or_null, _1, flow::util::bind_ns::cref(on_rsp_func_or_null),
                                     id_unless_one_off);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  const auto& msg = static_cast<const Msg_out_impl&>(msg_public);
  const bool one_off = !bool(id_unless_one_off);

  // See send() doc header which summarizes our course of action.  See also m_channel doc header for context.

  // Send time: generate ID (and seq #; it is important we only generate it now, at sync send() time).
  const auto id = m_snd_msg_next_id++;
  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Send request: Generated out-message ID [" << id << "].");

  if ((id != 1) && (m_phase != Phase::S_LOGGED_IN))
  {
    assert((id == 2) && "How in the hell did we get past this error last time?");
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in: Send request wants to send out-message, "
                     "but in this phase one can send at most one (log-in request on client, response on "
                     "server), and this is message 2.  Ignoring.");
    // Undo any ID generation we did above....
    --m_snd_msg_next_id;
    return false;
  }
  // else

  /* Before sending there's this to take care of: now that the send-time has come, we know the ID (a/k/a seq #,
   * though that doesn't matter here), and can therefore register the response expectation, if any,
   * in m_rcv_expecting_msg_map.
   *
   * on_rsp_func_or_null non-null => They've supplied async handler for response.
   * m_rcv_expecting_response_map marks down that a response is being awaited. */

  if (on_rsp_func_or_null)
  {
    if (!one_off)
    {
      *id_unless_one_off = id; // Let them know the out-message ID: they can use it in undo_expect_responses().

      // @todo Maybe require one_off=true in CLI_LOG_IN?  I guess it could be fine; just odd.
    }

    /* A response expectation.  Mental sanity-check for various phases:
     *   - LOGGED_IN: Allowed, of course.
     *   - CLI_LOG_IN: Required (log-in request => await response to it).
     *   - SRV_LOG_IN: Allowed, though perhaps a bit unorthodox (typically log-in request => log-in response; and
     *     that ends any exchange).  However, we don't care.  We enter LOGGED_IN phase below upon successful send;
     *     and if they want to get a reponse to that, that's their business. */

#ifndef NDEBUG
    const auto result =
#endif
    m_rcv_expecting_response_map.emplace(id,
                                         new Expecting_response{ one_off, on_rsp_func_or_null });
    assert(result.second && "IDs do not repeat, so dupe-insertion should not be possible.");

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Registered a (one-off? = [" << one_off << "], "
                   "response expectation (request about to be sync-nb-sent); that raises their total count to "
                   "[" << m_rcv_expecting_response_map.size() << "].");
    /* Note: Any failure to send() below is fatal to *this, meaning no further send()s (or receives for that matter)
     * are going to happen.  Hence it does not matter how we've changed m_snd_msg_next_id and other things;
     * it's not like they can be "put back" and "reused." */
  } // if (on_rsp_func_or_null)
  else if (m_phase == Phase::S_CLI_LOG_IN) // && (!on_rsp_func_or_null)
  {
    /* No response expectation.  That is certainly fine, usually, except in CLI_LOG_IN, this *must* be the
     * log-in request and hence *must* expect response. */
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in (as client): Send attempt of what must be "
                     "the log-in request, but they failed to specify it expects a (log-in) response (used send() "
                     "instead of async_request()).  Ignoring.  "
                     "In theory they can try sending this log-in request again with the proper API which is "
                     "async_request().");
    // Undo any ID generation we did above....
    --m_snd_msg_next_id;
    return false;
  } // else if (!on_rsp_func_or_null)
  /* else if (SRV_LOG_IN or LOGGED_IN) && (!on_rsp_func_or_null)
   * { In LOGGED_IN and SRV_LOG_IN, this is allowed.  Nothing more to do about it though. } */

  // Check for invalid situations w/r/t whether this is a response to something or unsolicited.
  if ((!originating_msg_or_null) && (m_phase == Phase::S_SRV_LOG_IN))
  {
    /* Unsolicited.  That is certainly fine, usually, except in SRV_LOG_IN, this *must* be the
     * log-in reponse and hence *must* be a... response. */
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in (as server): Send attempt of what must be "
                     "the log-in response, but they failed to specify that it *is* a response.  Ignoring.  "
                     "In theory they can try sending this log-in response again with the proper form of send().");
    // Undo any ID generation we did above....
    --m_snd_msg_next_id;
    return false;
  } // if ((!originating_msg_or_null) || (m_phase == Phase::S_SRV_LOG_IN))
  // else
  if (originating_msg_or_null && (m_phase == Phase::S_CLI_LOG_IN))
  {
    /* Response.  That is certainly fine, usually, except in CLI_LOG_IN, this *must* be the
     * log-in request which cannot be replying to anything else.  A little hard to imagine how the rest of the
     * logic would let things get this far, but anyway better safe than sorry. */
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in (as server): Send attempt of what must be "
                     "the log-in request, but somehow they specified it's a response itself.  Ignoring.  "
                     "In theory they can try sending this log-in request again with the proper form of send().");
    // Undo any ID generation we did above....
    --m_snd_msg_next_id;
    return false;
  } // else if (originating_msg_or_null && (m_phase == Phase::S_CLI_LOG_IN))

  // else (any other message can be a response or not; all allowed)

  /* From this point on, if anything fails, then *this is hosed (at least in the send direction).
   * So, like, in particular m_snd_msg_next_id does not matter anymore and need not be re-decremented or what-not. */

  // Prepare the metadata (internal message describing the user message right after it).
  Msg_mdt_out mdt(m_struct_builder_config, m_session_token,
                  originating_msg_or_null
                    ? static_cast<const Msg_in_impl*>(originating_msg_or_null)->id_or_none()
                    : 0,
                  id, msg.n_serialization_segments());

  // Lastly actually send both things.
  return send_core(mdt, &msg, err_code);
} // Channel::send_impl()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::send_core(const Msg_mdt_out& mdt, const Msg_out_impl* msg,
                                         Error_code* err_code_or_ignore)
{
  /* `sink` used only if err_code_or_ignore is null -- we are to ignore any error and let the next send*() catch it.
   * As of this writing used only for internal messages which are best-effort. */
  Error_code sink;
  Error_code* const err_code = err_code_or_ignore ? err_code_or_ignore : &sink;

  // See send() doc header which summarizes our course of action.  See also m_channel doc header for context.

  /* Let's send!  This is the other side of the logic on the receive side.  See start_and_poll()
   * where that's kicked off.  It will refer you to Msg_in_pipe doc header and so on.  The below should follow
   * from that understanding.  So we'll keep inline comments fairly svelte, unless there's new info.
   *
   * One thing that wouldn't be clear from the above is: A *sent* out-message really consists of two
   * sub-messages: mdt (the description and/or internal message), and msg (the user message -- or none).
   * A *received* in-message consists of *both* together in one Msg_in (user being able to access only
   * the user message, if message is even emitted to them -- internal messages aren't directly so emitted).
   * On the Msg_in this works transparently: we just call add_serialization_segment() 1+ times, and it
   * figures out which of those encodes `mdt` and which `msg`.  So... how?  Answer: It expects (by
   * its contract) the first segment to be the 1-segment serialization of `mdt`; while all the following
   * (0 of them, if there's no user message = internal message) encode `msg`.  So that's what we do:
   * mdt.emit_serialization(); send that; msg->emit_serialization() (if not null); send all those.
   * That is what Msg_in expects by contract.  On the other side it's transparent: all 1+
   * segments are fed into Msg_in, and it figures out what's what.
   *
   * ...And another thing: see "Protocol negotiation" in class doc header.  Exactly once we will pre-pend
   * a blob containing a simple ProtocolNegotiation capnp-struct with the 2 protocol versions. */

  const auto protocol_ver_to_send_if_needed = m_protocol_negotiator.local_max_proto_ver_for_sending();
  const auto protocol_ver_to_send_if_needed_aux = m_protocol_negotiator_aux.local_max_proto_ver_for_sending();
  const bool proto_negotiating = protocol_ver_to_send_if_needed != Protocol_negotiator::S_VER_UNKNOWN;
  assert(proto_negotiating == (protocol_ver_to_send_if_needed_aux != Protocol_negotiator::S_VER_UNKNOWN));

  Segment_ptrs blobs_out;
  blobs_out.reserve(1 + (msg ? msg->n_serialization_segments() : 0) // Tiny optimization.
                      + (proto_negotiating ? 1 : 0));

  if (proto_negotiating)
  {
    const Heap_fixed_builder::Config cfg{ get_logger(), S_PROTO_NEGOTIATION_SEG_SZ, 0, 0 };
    const Heap_fixed_builder builder(cfg);

    auto root = builder.payload_msg_builder()->initRoot<schema::detail::ProtocolNegotiation>();
    root.setMaxProtoVer(protocol_ver_to_send_if_needed);
    root.setMaxProtoVerAux(protocol_ver_to_send_if_needed_aux);

    // See comment re. similar prettyPrint() below w/r/t perf.  Applies here, but this is smaller still.
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Send request wants to send first out-message; here is "
                   "the pre-pended protocol-negotiation header:"
                   "\n" << ::capnp::prettyPrint(root.asReader()).flatten().cStr());

    Error_code err_code_ok;
    builder.emit_serialization(&blobs_out, NULL_SESSION, &err_code_ok);
    assert((!err_code_ok) && "Very simple structure; no way should it overflow segment.");
    assert((blobs_out.size() == 1) && "Very simple structure; no way it should need more than 1 segment.");
  } // if (proto_negotiating)

  blobs_out.push_back(mdt.emit_serialization(m_struct_lender_session, err_code));
  if (*err_code)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send out-message, "
                     "but the metadata (description/internal) serialization is illegal (error [" << *err_code << "] "
                     "[" << err_code->message() << "]).  This is a channel-hosing error; emitting it "
                     "unless internally-ignoring (are we? = [" << (!err_code_or_ignore) << "]).");
    err_code_or_ignore && (m_channel_err_code_or_ok = *err_code);
    return true; // This is similar to send_*() failing below, as far as the user is concerned.
  }
  // else

  /* Note this does not rise to the level of a user-data dump; so TRACE level (versus DATA) is defensible.
   * It ain't quick, but its size is capped pretty well.  Possible exception: an internal message containing
   * a dump of an unexpected response (see where m_on_unexpected_response_func_or_empty is invoked)
   * could be huge; but actually we cap that pretty well too (before it gets here). */
  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Send request wants to send out-message; here is "
                 "the metadata header (there may also be a DATA message below with the complete user "
                 "message payload if any; and/or TRACE message with similar):"
                 /* @todo The ->asReader() thing should not be necessary according to pretty-print.h doc header,
                  * but, perhaps, gcc-8.3 gets confused with all the implicit conversions; so we "help out."
                  * Revisit; also for the prettyPrint() higher up in this method. */
                 "\n" << ::capnp::prettyPrint(mdt.body_root()->asReader()).flatten().cStr());

  if (msg)
  {
    msg->emit_serialization(&blobs_out, m_struct_lender_session, err_code); // Appends to blobs_out.
    if (*err_code)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send out-message, "
                       "but the user-message serialization is illegal (error [" << *err_code << "] "
                       "[" << err_code->message() << "]).  This is a channel-hosing error; emitting it "
                       "unless internally-ignoring (are we? = [" << (!err_code_or_ignore) << "]).");
      err_code_or_ignore && (m_channel_err_code_or_ok = *err_code);
      return true; // This is similar to send_*() failing below, as far as the user is concerned.
    }
    // else:

    // This has some other interesting info plus a (possibly truncated) one-line representation of body_root() contents.
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Send request wants to send (user) out-message "
                   "[" << *msg << "].");

    /* Print the entire contents with indentation/newlines.
     * The user message could be giant.  By definition that is for DATA verbosity only.
     * In addition even the mere computation of what to print (e.g., if we wanted to truncate it before printing
     * at TRACE level) is potentially cripplingly slow; so absolutely do not do it outside the log macro. */
    FLOW_LOG_DATA("struc::Channel [" << *this << "]: Here is the complete user "
                  // @todo See above to-do regarding asReader().
                  "message:\n" << ::capnp::prettyPrint(msg->body_root()->asReader()).flatten().cStr());

    // Fall through.
  } // if (msg)
  else
  {
    assert((m_phase == Phase::S_LOGGED_IN)
           && "We do not mess with internal messages until log-in has finished.  Bug?");
  }

  // That's it.  Now it's just blobs to send out, regardless of what they represent.

  const auto hndl = msg ? msg->native_handle_or_null() : Native_handle();
  [[maybe_unused]] const bool has_hndl = !hndl.null();

  // Synchronous helpers.
  [[maybe_unused]] const auto send_blobs = [&]()
  {
    if constexpr(Owned_channel::S_HAS_BLOB_PIPE)
    {
      for (const auto& blob : blobs_out)
      {
  #ifndef NDEBUG
        const bool ok =
  #endif
        m_channel.send_blob(blob->const_buffer(), err_code);
        assert(ok); // Only false if not PEER state; we promised undefined behavior in that case.

        if (*err_code)
        {
          return;
        }
      }
    }
    // else if constexpr(true) { No-op: We won't be called; see below. }
  }; // const auto send_blobs =
  [[maybe_unused]] const auto send_meta_blobs = [&]()
  {
    if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE)
    {
      bool first = true;
      for (const auto& blob : blobs_out)
      {
  #ifndef NDEBUG
        const bool ok =
  #endif
        m_channel.send_native_handle(first ? (first = false, hndl)
                                           : Native_handle(),
                                     blob->const_buffer(), err_code);
        assert(ok); // Same as above.

        if (*err_code)
        {
          return;
        }
      }
    }
    // else if constexpr(true) { No-op: We won't be called; see below. }
  }; // const auto send_meta_blobs =

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Send request wants to send out-message, "
                 "and the serialization shall now be sent: total of [" << blobs_out.size() << "] segments, 1 per blob; "
                 "1st blob contains metadata including ID and the further-segments count, plus native handle (unless "
                 "null) = [" << (msg ? msg->native_handle_or_null() : Native_handle()) << "]; if 1st message then "
                 "protocol-negotiation 0th blob is there too.");

  if constexpr(Owned_channel::S_HAS_BLOB_PIPE_ONLY)
  {
    assert((!has_hndl) && "Attempting to send a native-handle over an Owned_channel without a native handles pipe.");
    send_blobs();
  }
  else if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    send_meta_blobs();
  }
  else
  {
    static_assert(Owned_channel::S_HAS_2_PIPES, "This code assumes 1 blobs pipe, 1 handles pipe, or both exactly.");
    has_hndl ? send_meta_blobs() : send_blobs();
  } // else if (S_HAS_2_PIPES)

  // `msg` may now be safely destroyed.

  if (*err_code)
  {
    // A send_*() failed.  There is one special case however which we promised to emit in a special way.
    if (*err_code == transport::error::Code::S_SENDS_FINISHED_CANNOT_SEND)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send out-message, "
                       "and we tried to send the serialization: total of [" << blobs_out.size() << "] segs, 1/blob; "
                       "1st blob contains 1st segment including further segment count (0 if internal msg), "
                       "plus native handle (unless null) = [" << hndl << "].  However Channel reports "
                       "we had already sent graceful-close.  Therefore ignoring send attempt after all.");
      /* Subtlety regarding bool(err_code_or_ignore):
       *   - Regardless of that, we are not to record this "error" into m_channel_err_code_or_ok by our contract.
       *   - What remains, then, is whether to return true or false.
       *     - If bool(err_code_or_ignore) is true (not ignoring errors) then return false by contract.
       *     - Otherwise, 1, still return false as promised; but 2, the caller won't care anyway.
       * So return false. */
      return false;
    }
    // else
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send out-message, "
                     "and we tried to send the serialization: total of [" << blobs_out.size() << "] segs, 1/blob; "
                     "1st blob contains 1st segment including futher segment count (0 if internal msg), "
                     "plus native handle (unless null) = [" << hndl << "].  However Channel reports "
                     "a send failed (error [" << *err_code << "] [" << err_code->message() << "]).  "
                     "This is a channel-hosing error; emitting it "
                     "unless internally-ignoring (are we? = [" << (!err_code_or_ignore) << "]).");
    err_code_or_ignore && (m_channel_err_code_or_ok = *err_code);
    return true;
  }
  // else cool!  But one last thing possibly:

  if (m_phase == Phase::S_SRV_LOG_IN)
  {
    FLOW_LOG_INFO("struc::Channel [" << *this << "]: Log-in (as server): Successfully sent what must be the "
                  "log-in response message.  Changing phase to logged-in.");
    m_phase = Phase::S_LOGGED_IN;
  }

  assert(!*err_code);
  return true;
} // Channel::send_core()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Task_err>
bool CLASS_SIO_STRUCT_CHANNEL::async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func)
{
  if (check_not_started_ops("async_end_sending()"))
  {
    return false;
  }
  // else

  return m_channel.async_end_sending(sync_err_code, on_done_func);
}

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::check_prior_error(util::String_view context) const
{
  if (m_channel_err_code_or_ok)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: In context [" << context << "]: A prior error "
                     "[" << m_channel_err_code_or_ok << "] [" << m_channel_err_code_or_ok.message() << "] shall "
                     "prevent further action in this context: bailing out of this operation.");
    return true;
  }
  // else
  return false;
}

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::check_phase_and_prior_error(Phase required_phase, util::String_view context) const
{
  if (check_prior_error(context))
  {
    return true;
  }
  // else

  if (m_phase != required_phase)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: In context [" << context << "]: The required log-in "
                     "phase for this context is not in effect: bailing out of this operation.");
    return true;
  }
  // else

  return false;
} // Channel::check_phase_and_prior_error()

TEMPLATE_SIO_STRUCT_CHANNEL
typename CLASS_SIO_STRUCT_CHANNEL::Msg_out CLASS_SIO_STRUCT_CHANNEL::create_msg(Native_handle&& hndl_or_null) const
{
  // We are just a convenience helper really; all we do is pass-in m_struct_builder_config memorized for convenience.

  Msg_out msg(m_struct_builder_config);
  if (!hndl_or_null.null()) // Tiny optimization to skip past some boring checks inside store_...().
  {
    msg.store_native_handle_or_null(std::move(hndl_or_null));
  }
  return msg;
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename On_msg_handler>
bool CLASS_SIO_STRUCT_CHANNEL::expect_msg(Msg_which_in which, Msg_in_ptr* qd_msg, On_msg_handler&& on_msg_func)
{
  using boost::make_shared;

  assert(qd_msg);
  if (check_phase_and_prior_error(Phase::S_LOGGED_IN, "expect_msg()"))
  {
    return false;
  }
  // else

  Msgs_in qd_msgs;
  const bool ok = expect_msgs_impl(&qd_msgs,
                                   true, // One-off expectation (one message expected).
                                   which,
                                   make_shared<On_msg_func>(std::move(on_msg_func)));

  if (ok && (!qd_msgs.empty()))
  {
    assert((qd_msgs.size() == 1) || "Desiring 1 message -- should have emitted at most 1 already-ready message.");
    *qd_msg = std::move(qd_msgs.front());
  }
  else
  {
    qd_msg->reset();
  }

  return ok;
} // Channel::expect_msg()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename On_msg_handler>
bool CLASS_SIO_STRUCT_CHANNEL::expect_msgs(Msg_which_in which, Msgs_in* qd_msgs, On_msg_handler&& on_msg_func)
{
  using boost::make_shared;

  assert(qd_msgs);
  if (check_phase_and_prior_error(Phase::S_LOGGED_IN, "expect_msgs()"))
  {
    return false;
  }
  // else

  return expect_msgs_impl(qd_msgs,
                          false, // Open-ended expectation (0+ messages expected).
                          which,
                          make_shared<On_msg_func>(std::move(on_msg_func)));
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename On_msg_handler>
bool CLASS_SIO_STRUCT_CHANNEL::expect_log_in_request(Msg_which_in which, Msg_in_ptr* qd_msg,
                                                     On_msg_handler&& on_log_in_req_func)
{
  using boost::make_shared;

  assert(qd_msg);
  if (check_phase_and_prior_error(Phase::S_SRV_LOG_IN, "expect_log_in_request()"))
  {
    return false;
  }
  // else

  if (m_phase_log_in_started)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Log-in: expect_log_in_request() called again.  "
                     "Ignoring.");
    return false;
  }
  // else
  m_phase_log_in_started = true;

  Msgs_in qd_msgs;
  const bool ok = expect_msgs_impl(&qd_msgs,
                                   true, // One-off expectation (one message -- the log-in response -- expected).
                                   which,
                                   make_shared<On_msg_func>(std::move(on_log_in_req_func)));
  if (ok && (!qd_msgs.empty()))
  {
    assert((qd_msgs.size() == 1) && "Only one log-in request would have been accepted without error.");
    *qd_msg = std::move(qd_msgs.front());
  }
  else
  {
    qd_msg->reset();
  }

  return ok;
} // Channel::expect_log_in_request()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::expect_msgs_impl(Msgs_in* qd_msgs,
                                                bool one_off, Msg_which_in which, On_msg_func_ptr&& on_msg_func_moved)
{
  using std::vector;

  const auto insert_result
    = m_rcv_expecting_msg_map.emplace(which, new Expecting_msg{ one_off, on_msg_func_moved });
  auto& exp_msg_it = insert_result.first;
  auto on_msg_func = std::move(on_msg_func_moved); // We may need to immediately fire/pop this below.
  const bool inserted = insert_result.second;

  if (!inserted)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Wanted to register a (one-off? = [" << one_off << "]) "
                     "message expectation (union-which = [" << int(which) << "]); but there is already one registered "
                     "for that union-which.  Ignoring.");
    return false;
  }
  // else if (inserted):

  const bool logging_in = m_phase != Phase::S_LOGGED_IN;
  const auto msgs_in_q_it = m_rcv_pending_msgs.find(which);
  const bool found = msgs_in_q_it != m_rcv_pending_msgs.end();

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Registered a (one-off? = [" << one_off << "]) "
                 "message expectation (union-which = [" << int(which) << "]); that raises their total count to "
                 "[" << m_rcv_expecting_msg_map.size() << "].  Will immediately return qd-msgs if "
                 "we had collected 1+ messages matching that union-which; had we? = [" << found << "].");
  if (!found)
  {
    if (logging_in && (!m_rcv_pending_msgs.empty()))
    {
      assert((m_phase == Phase::S_SRV_LOG_IN) && "Unless LOGGED_IN we should only be called by expect_log_in_request() "
                                                   "which implies SRV_LOG_IN.");
      assert((m_rcv_pending_msgs.size() == 1)
             && "Other code is supposed to ensure we cache at most the log-in request and no more in-messages while "
                  "in SRV_LOG_IN phase.");
      assert((m_rcv_expecting_msg_map.size() == 1)
             && "Other code is supposed to prevent logged-in APIs like expect_msg() in this phase; and more than one "
                  "successful call to expect_log_in_request(); yet somehow we'd saved more expectations than exactly "
                  "1 in m_rcv_expecting_msg_map.");

      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Registered a (one-off? = [" << one_off << "]) "
                       "*log-in request* expectation (union-which = [" << int(which) << "]); that raises their "
                       "total count to 1.  Would immediately queue handler(s) to fire if "
                       "we had collected 1 messages matching that union-which; but we had not; however we had "
                       "cached 1 message with different union-which = "
                       "[" << int(m_rcv_pending_msgs.begin()->first) << "].  "
                       "So: peer expects log-in request X, while other side sent log-in request Y.  "
                       "This might be a protocol error on the user's part on this or other side or both.");
      // Subtlety: See similar comment in the on-receive handler where it also emits this error.
      handle_new_error(error::Code::S_STRUCT_CHANNEL_GOT_UNEXPECTED_LOG_IN_REQUEST, "expect_msgs_impl()");
      return false;
    }
    // else

    return true; // Cool -- just await future in-message(s) with `which`.
  } // if (!found)
  // else if (found):

  qd_msgs->clear();

  auto& msgs_in_q = msgs_in_q_it->second;
  assert((!msgs_in_q.empty()) && "We always erase m_rcv_pending_msgs[] queue once it's empty... yet a queue is empty.");

  if (one_off)
  {
    qd_msgs->reserve(1); // Little optimization.  Maybe it'll make it reserve 1 exactly; maybe it's a no-op.

    Msg_in_ptr_uniq msg_in(std::move(msgs_in_q.front()));
    msgs_in_q.pop();
    if (msgs_in_q.empty()) // Empty -> immediately erase.
    {
      m_rcv_pending_msgs.erase(msgs_in_q_it); // msg_in_q now points to garbage (do not touch it).
      FLOW_LOG_TRACE("Popped 1 (because one-off expectation); queue is now empty.");
    }
    else
    {
      FLOW_LOG_TRACE("Popped 1 (because one-off expectation); queue is now sized [" << msgs_in_q.size() << "].");
    }

    qd_msgs->emplace_back(Msg_in_ptr(msg_in.release())); // Upgrade to shared_ptr<> by the way.

    // ...and un-insert the registered expectation, since it was immediately met.  @todo Can perf this up probably.
    m_rcv_expecting_msg_map.erase(exp_msg_it);
    return true;
  }
  // else if (!one_off):

  // Emit the entire queue of waiting messages.
  qd_msgs->reserve(msgs_in_q.size()); // Little optimization.

  do // while (!msgs_in_q.empty())
  {
    qd_msgs->emplace_back(msgs_in_q.front().release()); // Upgrade to shared_ptr<> by the way.
    msgs_in_q.pop();
  }
  while (!msgs_in_q.empty());
  m_rcv_pending_msgs.erase(msgs_in_q_it); // Empty -> immediately erase.

  FLOW_LOG_TRACE("Popped all [" << qd_msgs->size() << "] cached in-message from queue.");

  // And leave m_rcv_expecting_msg_map alone: new element will stay there until undo_expect_msgs().

  return true;
} // Channel::expect_msgs_impl()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::undo_expect_msgs(Msg_which_in which)
{
  if (check_phase_and_prior_error(Phase::S_LOGGED_IN, "undo_expect_msgs()"))
  {
    return false;
  }
  // else

  const bool erased = m_rcv_expecting_msg_map.erase(which) == 1;
  if (!erased)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Attempt to erase in-message expectation "
                     "[" << int(which) << "]: Failed, as there was no such expectation registered.  "
                     "Their total number remains [" << m_rcv_expecting_msg_map.size() << "].");
    return false;
  }
  // else

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Attempt to erase in-message expectation "
                 "[" << int(which) << "]: Success.  Their total number is now "
                 "[" << m_rcv_expecting_msg_map.size() << "].");
  return true;
} // Channel::undo_expect_msgs()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::undo_expect_responses(msg_id_out_t originating_msg_id)
{
  assert((originating_msg_id != 0) && "send() would never set *id_unless_one_off = 0.");

  if (check_phase_and_prior_error(Phase::S_LOGGED_IN, "undo_expect_responses()"))
  {
    return false;
  }
  // else

  const bool erased = m_rcv_expecting_response_map.erase(originating_msg_id) == 1;
  if (!erased)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Attempt to erase response expectation for sent "
                     "message with ID [" << originating_msg_id << "]: Failed, as there was no such "
                     "expectation registered.  Their total number remains "
                     "[" << m_rcv_expecting_response_map.size() << "].");
    return false;
  }
  // else

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Attempt to erase response expectation for sent "
                 "message with ID [" << originating_msg_id << "]: Success.  Their total number is now "
                 "[" << m_rcv_expecting_response_map.size() << "].");
  return true;
} // Channel::undo_expect_responses()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename On_unexpected_response_handler>
bool CLASS_SIO_STRUCT_CHANNEL::set_unexpected_response_handler(On_unexpected_response_handler&& on_func)
{
  if (!m_on_unexpected_response_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Attempt to register unexpected-response handler, "
                     "when one is already set.");
    return false;
  }
  // else

  m_on_unexpected_response_func_or_empty = On_unexpected_response_func(std::move(on_func));
  return true;
} // Channel::set_unexpected_response_handler()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::unset_unexpected_response_handler()
{
  if (m_on_unexpected_response_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Attempt to unregister unexpected-response handler, "
                     "when one is not set.");
    return false;
  }
  // else

  m_on_unexpected_response_func_or_empty.clear();
  return true;
} // Channel::set_unexpected_response_handler()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename On_remote_unexpected_response_handler>
bool CLASS_SIO_STRUCT_CHANNEL::set_remote_unexpected_response_handler(On_remote_unexpected_response_handler&& on_func)
{
  if (!m_on_remote_unexpected_response_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Attempt to register remote-unexpected-response handler, "
                     "when one is already set.");
    return false;
  }
  // else

  m_on_remote_unexpected_response_func_or_empty = On_remote_unexpected_response_func(std::move(on_func));
  return true;
} // Channel::set_remote_unexpected_response_handler()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::unset_remote_unexpected_response_handler()
{
  if (m_on_remote_unexpected_response_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Attempt to unregister remote-unexpected-response handler, "
                     "when one is not set.");
    return false;
  }
  // else

  m_on_remote_unexpected_response_func_or_empty.clear();
  return true;
} // Channel::set_remote_unexpected_response_handler()

TEMPLATE_SIO_STRUCT_CHANNEL
const typename CLASS_SIO_STRUCT_CHANNEL::Owned_channel& CLASS_SIO_STRUCT_CHANNEL::owned_channel() const
{
  return m_channel;
}

TEMPLATE_SIO_STRUCT_CHANNEL
typename CLASS_SIO_STRUCT_CHANNEL::Owned_channel* CLASS_SIO_STRUCT_CHANNEL::owned_channel_mutable()
{
  return &m_channel;
}

TEMPLATE_SIO_STRUCT_CHANNEL
const Session_token& CLASS_SIO_STRUCT_CHANNEL::session_token() const
{
  if (check_phase_and_prior_error(Phase::S_LOGGED_IN, "session_token()"))
  {
    return NULL_SESSION_TOKEN;
  }
  // else

  return m_session_token;
} // Channel::session_token()

/// @cond
// -^- Doxygen, please ignore the following.  It gets confused by something here and gives warnings.

TEMPLATE_SIO_STRUCT_CHANNEL
std::ostream& operator<<(std::ostream& os, const CLASS_SIO_STRUCT_CHANNEL& val)
{
  return os << "SIO[owned_channel [" << val.owned_channel() << "]]@" << static_cast<const void*>(&val);
}

// -v- Doxygen, please stop ignoring.
/// @endcond

#undef CLASS_SIO_STRUCT_CHANNEL
#undef TEMPLATE_SIO_STRUCT_CHANNEL

} // namespace ipc::transport::struc::sync_io
