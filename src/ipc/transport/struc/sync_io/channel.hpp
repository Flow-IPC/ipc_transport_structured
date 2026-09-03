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

#include "ipc/transport/struc/sync_io/channel_stats.hpp"
#include "ipc/transport/struc/channel_base.hpp"
#include "ipc/transport/struc/detail/msg_impl.hpp"
#include "ipc/transport/struc/heap_serializer.hpp"
#include "ipc/transport/struc/util.hpp"
#include "ipc/transport/channel.hpp"
#include "ipc/transport/error.hpp"
#include "ipc/transport/protocol_negotiator.hpp"
#include "ipc/util/process_credentials.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/blob.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/array.hpp>
#include <boost/move/make_unique.hpp>
#include <boost/logic/tribool.hpp>
#include <queue>
#include <optional>
#include <type_traits>
#include <variant>

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
 *   - `async_request(&M, ..., F)`: This says you want `M` sent and expect 1 or 0+ (depending on args) response(s),
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
 * ### Thready safety ###
 * The thread safety notes in transport::struc::Channel (a/k/a alias Channel::Async_io_obj) doc
 * header do not apply here.  (If you're familiar with the `sync_io` pattern, this probably won't surprise you.)
 * To wit, given one particular `*this`:
 *   - The `const` accessors struct_builder_config(), struct_lender_session(), struct_reader_config(),
 *     owned_channel(), owned_channel_mutable(), owned_channel_mutable() and key utility create_msg()
 *     are always safe to call concurrently with any other ops.
 *   - Now consider the remaining methods plus any `on_active_ev_func()` passed to the function you furnished via
 *     `this->start_ops()`:
 *     - You may not call any 2+ operations from this set concurrently.
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
 *
 * #### Outgoing direction ####
 * In the outgoing direction, the layers involved are fairly straightforward: send() is synchronous.  When it is
 * called, the zero-copy serialization of the structured out-message #Msg_out has been completed: the raw blobs
 * comprised by it are easily accessible from the #Msg_out (Msg_out_impl::emit_serialization()) passed to send()
 * (as is the optional #Native_handle).  The 1+ blobs + the #Native_handle are synchronously `Channel::send_*()`ed,
 * and then send() returns.  In addition, but still synchronously within send(), each such user message obj internally
 * contains another similarly-serialized message: the *metadata* message.  (For performance it is stored as a header
 * therein.)  This message, with a schema in structured_msg.capnp (a detail/
 * file), represents a *description* of the daddy #Msg_out.  Its schema
 * is controlled by us and is sufficiently small to require just one segment (blob) and no #Native_handle.
 * This contains information about the message (a/k/a metadata), notably: its message ID (discussed below),
 * the ID of the message to which it responds (or 0 if unsolicited), and the session token.  (For perf this header
 * is contained in the same memory buffer as the first, and often only, capnp-segment of the user message.)
 * To summarize, then, send() of a user message in fact sends exactly 1 metadata message/header within
 * a blob that also stores segment 1 of the user messages, plus 0+ blobs (segments)
 * serializing the rest of the user message.  (async_request() = internally such a send() more or less, plus registering
 * that future in-message(s) with certain metadata content are expected.)
 *
 * Lastly, for some special purposes, we sometimes send internal messages.  An internal message consists of
 * *only* the metadata; hence we just generate that same thing directly into a little blob, fill out the
 * optional internal-message-body field; and that fact indicates there is no user message (no segment 1, no anything)
 * to also scan.  Hence in this case exactly 1 `Blob` (segment) is sent, as for a typical (1-segment)
 * user message but without the actual user message.
 *
 * In the few cases where an internal message is required, internal code calls send_core() directly, passing a
 * 1-`vector` of `Blob_const`s.  Note that no #Msg_out is involved at all.
 *
 * As for a user message, it goes through send() or async_request() each of which
 * invokes send_impl() (for the actual sending part).  That guy converts a #Msg_out (facaded-into a Msg_out_impl,
 * to get at its internal representation, which is `private` to the user) into an N-`vector` (where N is 1+)
 * of `Blob_const`s.  Then send_core() takes those and sends them out.
 *
 * #### Incoming direction ####
 * In the incoming direction, it is much more complex.  Receiving a blob, or blob+handle pair, is by definition
 * a (potentially) asynchronous operation; and a given structured in-message may consist of more than 1 blob/blob+handle
 * pair.  The zero-copy deserialization invoked by the user can only occur once all of them are received, so
 * we must build up 1 given structured in-message over 1+ single-message async-read ops and only emit the result
 * (a newly allocated/cted Msg_in a/k/a #Msg_in_ptr) to the user once the last of the 1+ async-read ops
 * finishes.  So that's 2 layers: async-read op at the #Owned_channel level; then the structured level once a
 * Msg_in is ready.  Msg_in represents, conceptually, a pointer to 1-2 serialized
 * in-messages: the metadata in-message (always present) and, except for internal messages, the user-supplied
 * in-message (supplied originally via #Msg_out).  However, to the user, only the latter is accessible,
 * and an internal message is by definition never emitted to the user (if it were, there would be nothing for them
 * to access).  We access the mdt-stuff of a #Msg_in via the Msg_in_impl facade.
 *
 * @note Detour: Batch-receiving!  Please see concept class transport::Native_handle_receiver doc header first,
 *       then come back here.  In short, batch-receiving is the activity of making 1 `transport::Channel`-layer
 *       `.async_receive_*_batch()` call to "simultaneously" (ideally with 1 OS-call, using a reused
 *       Msg_batch_in object) receiving potentially-many unstructured in-messages.  We use this, because we want
 *       to be fast.  (The max size of the batch = `Msg_in_pipe::S_RCV_BATCH_SIZE`; e.g., 64 or 1; this # comes from
 *       `Owned_channel::S_RCV_NATIVE_HANDLE_BATCH_SZ_RECOMMENDATION` or
 *       `Owned_channel::S_RCV_BLOB_BATCH_SZ_RECOMMENDATION` depending on the
 *       pipe in question.)  Now, this code path would work perfectly well even if it is 1.  However, we do have a
 *       code-path specifically tailored to unbatched-receiving, and we've left this code-path in because (1) it was
 *       available, and (2) perf here is very important (we know from profiling experience).  So
 *       `if constexpr(RCV_BATCH_SIZE == 1)`, we always invoke that code-path; otherwise the other code-path.  In either
 *       case a Msg_batch_in is still used (so, of capacity 1 in one of the two cases), but with 1-batches it is an
 *       extremely simple Generic_msg_batch_in, and the non-batching code-path just always targets its slot 0, using
 *       none of the other features of the Msg_batch_in concept (not even Msg_batch_in::n_used(), which remains 0).
 *
 * The preceding non-detour paragraph talks only of *one* Channel pipe.  It is possible #Owned_channel contains 2 pipes
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
 * (lower level) facility; sync_io::Native_socket_stream_impl is probably best, but sync_io::Blob_stream_mq_sender_impl
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
 * Update: we have made changes and now are at protocol version V (for simplicity, across the board... the "board" of
 * which I speak is about to be described).  For simplicity we speak only that version; there
 * is no backward-compatibility with version-1-speaking software.
 * (V is #S_PROTOCOL_VERSION_MIN and #S_PROTOCOL_VERSION_MAX.)
 *
 * That said, the challenge here is more subtle than that.  Sure, we can negotiate *a* version at this layer, no
 * problem.  The problem: there are *multiple* protocols operating at this layer.  There are at least two:
 *   -# How do blob/handle pairs transmitted over the transport::Channel translated into structured capnp messages?
 *      This even depends on template params `Builder_config_t` and `Reader_config_t`: As of this writing
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
 *        - HOWEVER!!! Since the aforementioned metadata `StructuredMessage` is tiny (and of capped size), it is still
 *          (as with the heap-based arrangement) directly inside the (only, in the SHM case) blob transmitted via IPC.
 *          It is again the header; and for user messages after the header comes the encoding of the SHM-handle
 *          (pointing to a list-of-capnp-segments).  This was an important optimization actually; historically we
 *          used to transmit the mdt-`StructuredMessage` as a totally separate in-SHM thing; this did all kinds of
 *          ultimately costly stuff including unneeded heap-allocations.
 *   -# The capnp messages aren't just user messages either; internally we have internal messages and, more
 *      more importantly, the metadata-bearing leading messages containing key stuff like the message ID,
 *      notification/response info, etc.  The (internally used) schema in structured_msg.capnp is a protocol;
 *      certainly it could change over time: E.g., more types of internal messages might be added.
 *
 * This implementation is (we hope) thoughtfully layered, but the question of where exactly lies the separation
 * between various protocols is a tough one.  These aren't just "wire protocols" anymore; there's a programmatic
 * element to it involving the Struct_builder and Struct_reader concepts.  So should there be a separate
 * negotiation/version for each of these protocols?  Which protocols, even?  How would this work?
 *
 * That's probably the wrong question, we think.  Let's instead be pragmatic.  Firstly, as of this writing, there *is*
 * only version V of everything; so it's all moot, until more versions pop up.  So all we're trying to do here is
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
 *     (as of this writing V, V, V) and expect 3 versions to be received similarly.  (As usual, on receipt,
 *     each Protocol_negotiator::compute_negotiated_proto_ver() will determine the version to speak -- as of this
 *     writing either V, or *explode the channel due to negotiation failure*.)
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
 *         to `*this`.  As of this writing those are likely to just be either vanilla `"Heap_*er::Config"`, or
 *         shm::Builder::Config + shm::Reader::Config; but formally any Struct_builder / Struct_reader concept impls
 *         are allowed.  (The user may well implement their own fanciness.)
 *       - So really it's not about transport::struc::shm specifically but more like, formally speaking,
 *         #Builder_config` + #Reader_config protocol code, excluding the base/vanilla
 *         Heap_fixed_builder and Heap_reader (which, we've established, are already covered by the first, non-gravy
 *         Protocol_negotiator).
 *
 * Thus it should be sufficient to have:
 *   - Protocol_negotiator for us.  That is #m_protocol_negotiator.
 *   - (Possibly unused) Protocol_negotiator for any additional protocol(s) involved in `Struct_*er_config` layer,
 *     such as the existing shm::Builder and shm::Reader.  That is #m_protocol_negotiator_aux.
 *     The actual preferred (highest) and lowest-supported Protocol_negotiator::proto_ver_t values shall
 *     as of this writing simply be V; but once that changes we'll likely add some `static constexpr` values
 *     in Struct_builder and Struct_reader concepts (and impls), and `*this` will pass those to Protocol_negotiator
 *     ctor (instead of simply passing Vs).
 *     - (Depending on whether the hypothetical multi-version protocol(s) support backwards-compatibility with
 *       earlier protocol-versions (i.e., a given version range is not [V, V]), we might need more logic/APIs,
 *       so that a given module knows which protocol-version to in fact speak.  To reiterate, currently, we need not
 *       worry about it: just don't shoot selves in foot in this version V of everything.)
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
 * (and therefore expected before any in-messages).  Following the lead of the lower levels:
 *   - Outgoing: ASAP (so start_ops()), `.send_blob/native_handle()` the `ProtocolNegotiation`-storing blob.  The
 *     `.send*()`s are all non-blocking and synchronous, so that's simple.
 *   - Incoming: Just before invoking the first receive API (`.async_receive_blob*()`,
 *     `.async_receive_native_handle*()`) -- which occurs in start_and_poll() -- on #m_channel, `.async_receive_blob()`
 *     the `ProtocolNegotiation`-storing blob.  Only proceed with the first "real" receive on successful async-receive
 *     of the `ProtocolNegotiation`-storing blob.  (Note: all this begins in start_and_poll().)
 *
 * There is one slight complication there: underlying #Owned_channel might have 2 pipes (at compile-time), one for
 * native-handle-bearing messages, one for the converse.  The unlikely-but-possible racing of messages being sent
 * over 2 pipes in ~parallel is normally resolved via message IDs (sequence numbers), but of course we don't want to
 * include these trivial negotiating leading messages in that mechanism.  So: we simply send (and expect)
 * the negotiation message along *each* pipe, before any others in that pipe.  Chronologically speaking, then, the 1st
 * negotiation-in-message to be received is processed; the other ignored (and trusted to be duplicate of the winner).
 * There's a bit of negligible overhead, but the important thing is that no "real" messages are processed before
 * protocol-negotiation phase passes.
 *
 * @endinternal
 *
 * @tparam Owned_channel_t
 *         See #Async_io_obj doc header.
 * @tparam Msg_body_t
 *         See #Async_io_obj doc header.
 * @tparam Builder_config_t
 *         See #Async_io_obj doc header.
 * @tparam Reader_config_t
 *         See #Async_io_obj doc header.
 */
template<typename Owned_channel_t, typename Msg_body_t,
         typename Builder_config_t, typename Reader_config_t>
class Channel :
  public Channel_base,
  private boost::noncopyable,
  public flow::log::Log_context
{
public:
  // Types.

  /// Useful for generic programming, the async-I/O-pattern counterpart to `*this` type.
  using Async_io_obj = transport::struc::Channel<Owned_channel_t, Msg_body_t,
                                                 Builder_config_t, Reader_config_t>;
  /// You may disregard.
  using Sync_io_obj = Null_peer;

  /// See #Async_io_obj counterpart.
  using Owned_channel = Owned_channel_t;

  static_assert(Owned_channel::S_IS_SYNC_IO_OBJ,
                "struc::Channel (whether sync_io or async-I/O) subsumes sync_io-pattern-peer-bearing "
                  "`Channel`s only.  Fortunately ipc::session at least emits such `Channel`s.");

  /// See #Async_io_obj counterpart.
  using Builder_config = Builder_config_t;
  /// See #Async_io_obj counterpart.
  using Reader_config = Reader_config_t;

  /// See #Async_io_obj counterpart.
  using Msg_body = Msg_body_t;

  /// See #Async_io_obj counterpart.
  using Msg_which = typename Msg_body_t::Which;

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
   *     - Via `static` heap_fixed_builder_config() and heap_reader_config().
   *     - Via session::Session_mv::heap_reader_config().
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
   * the log-in request message `M` arrives, and you then invoke `send(&X, M)`, where `X` is the log-in response
   * out-message you create/fill-out.  To be clear the phase change occurs as the last step in that
   * successful send().
   *
   * ### As client ###
   * To begin the log-in phase -- and thus start to move to the logged-in phase, in which general traffic can
   * occur -- you must `send(&X, nullptr, nullptr, F)`, where `X` is the log-in request out-message you create/fill-out,
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
   * @param segment1_sz
   *        Knob that controls the default value optionally overridden by `msg_sz_cap_estimate_or_zero` arg
   *        to the relevant create_msg() overload; used when that arg is set to 0, or if the other
   *        create_msg() overload is used.  In this ctor form, internally this controls the size of the
   *        seg 1 `calloc()`-or-equivalent heap alloc/zeroing call.
   */
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_heap tag,
                   const Session_token& session_token_non_nil,
                   size_t segment1_sz = sizeof(::capnp::word) * ::capnp::SUGGESTED_FIRST_SEGMENT_WORDS);

  //XXXadd segment1_sz knob to manual / add manual perf section?

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
   * @param segment1_sz
   *        See non-log-in ctor.
   */
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_heap tag,
                   bool is_server,
                   size_t segment1_sz = sizeof(::capnp::word) * ::capnp::SUGGESTED_FIRST_SEGMENT_WORDS);

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
   * @param segment1_sz
   *        Knob that controls the default value optionally overridden by `msg_sz_cap_estimate_or_zero` arg
   *        to the relevant create_msg() overload; used when that arg is set to 0, or if the other
   *        create_msg() overload is used.  In this ctor form, internally this controls the size of the
   *        seg 1 SHM-allocate call and, especially perf-significantly, of the `memset(0)`-or-equivalent call
   *        that follows it (at least conceptually speaking).
   */
  template<typename Session>
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_session_shm tag, Session* session,
                   const Session_token& session_token_explicit = NULL_SESSION_TOKEN,
                   size_t segment1_sz = sizeof(::capnp::word) * ::capnp::SUGGESTED_FIRST_SEGMENT_WORDS);

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
   * @param segment1_sz
   *        See non-log-in ctor.
   */
  template<typename Session>
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_session_shm tag, Session* session,
                   bool is_server,
                   size_t segment1_sz = sizeof(::capnp::word) * ::capnp::SUGGESTED_FIRST_SEGMENT_WORDS);

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
   * @param segment1_sz
   *        See `Serialize_via_session_shm` ctor.
   */
  template<typename Session>
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_app_shm tag, Session* session,
                   const Session_token& session_token_explicit = NULL_SESSION_TOKEN,
                   size_t segment1_sz = sizeof(::capnp::word) * ::capnp::SUGGESTED_FIRST_SEGMENT_WORDS);

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
   * @param segment1_sz
   *        See non-log-in ctor.
   */
  template<typename Session>
  explicit Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                   Serialize_via_app_shm tag, Session* session,
                   bool is_server,
                   size_t segment1_sz = sizeof(::capnp::word) * ::capnp::SUGGESTED_FIRST_SEGMENT_WORDS);

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
   * start_and_poll() (and subsequent in-message-related work) and send() (and other out-message-related APIs)
   * will work (as opposed to no-op/return `false`).
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
   * @return See above.
   */
  const util::Process_credentials& remote_peer_process_credentials() const;

  /**
   * See #Async_io_obj counterpart.
   *
   * @param hndl_or_null
   *        See above.
   * @return See above.
   */
  Msg_out create_msg(Native_handle&& hndl_or_null = {}) const;

  /**
   * See #Async_io_obj counterpart.
   *
   * @param hndl_or_null
   *        See above.
   * @param msg_sz_cap_estimate_or_zero
   *        See above.
   * @return See above.
   */
  Msg_out create_msg(size_t msg_sz_cap_estimate_or_zero, Native_handle&& hndl_or_null = {}) const;

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
   *   -# `send(&X)`.  The latter automatically moves `*this` to logged-in phase locally: the bulk of the
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
  bool send(Msg_out* msg, const Msg_in* originating_msg_or_null = nullptr, Error_code* err_code = nullptr);

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
  bool async_request(Msg_out* msg, const Msg_in* originating_msg_or_null,
                     msg_id_out_t* id_unless_one_off, On_msg_handler&& on_rsp_func, Error_code* err_code = nullptr);

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
   * @tparam On_unexpected_response_func_t
   *         See above.
   * @param on_func
   *        See above.
   * @return See above.
   */
  template<typename On_unexpected_response_func_t>
  bool set_unexpected_response_handler(On_unexpected_response_func_t&& on_func);

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
   * See #Async_io_obj counterpart.
   * @param err_code
   *        See above.
   * @return See above.
   */
  bool remote_peer_process_liveness_check(Error_code* err_code = nullptr);

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

  /**
   * Returns cumulative structured-channel stats.  The returned object reflects
   * all activity from construction or last stats_reset() through the most recent completed operation.
   *
   * @return See above.
   */
  const stat::Channel_stats& stats() const;

  /**
   * Resets stats().  The formal meaning of a reset is discussed in
   * `flow::util::stat` doc header.  The effect of the last stats_configure_rcv_one_off_request_rtt_histogram(),
   * if any/or similar, is retained.
   */
  void stats_reset();

  /**
   * Replaces the default RTT histogram bucket configuration for one-off `async_request()` round-trip
   * tracking (stat::Channel_stats::Rcv::m_histo_one_off_request_rtt_usec).  Must be called before start_and_poll().
   *
   * The default is a log-scale 1-2-5 ladder (10us through 10s) suitable for most protocols without tuning;
   * this method replaces it with a linear-scale config -- e.g., if uniform resolution over a specific
   * range is desired.
   *
   * @param n_buckets
   *        Number of histogram buckets (at least 2).
   * @param bucket_sz
   *        Width of each bucket.
   */
  void stats_configure_rcv_one_off_request_rtt_histogram(size_t n_buckets, util::Fine_duration bucket_sz);

private:
  // Types.

  /// Clarifying short-hand for incoming-message IDs.
  using msg_id_in_t = msg_id_t;

  /// Ref-counted wrapper of type corresponding to `On_msg_handler` template param: in-message handler.
  using On_msg_handler_ptr = boost::shared_ptr<Function<void (Msg_in_ptr&& msg)>>;

  /**
   * Concrete type corresponding to `On_unexpected_response_func_t` template param
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
   * Policy for how to act upon receiving an expected message of one kind or another.  As of this writing
   * there are two applicable scenarios:
   *   - receiving a response in-message that indicates its originating out-message is
   *     the message associated with this object in #Expecting_response_map;
   *   - receiving an in-message whose top-level-union #Msg_which_in `enum` equals the
   *     #Msg_which_in associated with this object in #Expecting_msg_map.
   *
   * For the former see Expecting_response as well.
   */
  struct Expecting_msg
  {
    // Types.

    /// Short-hand to cheap handle type.
    using Ptr = boost::movelib::unique_ptr<Expecting_msg>;

    // Data.

    /**
     * `true` if receiving the expected in-message means the expectation is satisfied; `false` if an `undo_*()`
     * must be invoked to mark it satisfied.  This controls the mechanism by which an Expecting_msg is removed
     * from the expected-messages map (#Expecting_response_map, #Expecting_msg_map), freeing
     * the non-zero RAM (at least) resource involved.
     */
    bool m_one_expected;

    /**
     * The handler to invoke on receiving the expected in-message.
     *
     * The `shared_ptr` wrapper is because in case of #m_one_expected being `false` we have to copy it when
     * `handlers_post()`ing; `move()` could destroy it (leave it `.empty()`) instead.
     */
    On_msg_handler_ptr m_on_msg_func;
  }; // struct Expecting_msg

  /**
   * Policy for how to act upon receiving a response in-message that indicates its originating out-message is
   * the message associated with this object in #Expecting_response_map.
   *
   * @note Attention: It builds on Expecting_msg.
   */
  struct Expecting_response : public Expecting_msg
  {
    // Types.

    /// Short-hand to cheap handle type.
    using Ptr = boost::movelib::unique_ptr<Expecting_response>;

    // Data.

    /// Time point when this request was sent; used for one-off RTT histogram in stat::Channel_stats.
    util::Fine_time_pt m_sent_when;
  }; // struct Expecting_response

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
  using Msg_in_ptr_uniq = boost::movelib::unique_ptr<Msg_in>;

  /// Short-hand for queue (FIFO) of in-messages.
  using Msg_in_q = std::queue<Msg_in_ptr_uniq>;

  /**
   * Reassembly queue type: "queue" of all in-messages with #msg_id_in_t exceeding #m_rcv_msg_next_id, sorted
   * in increasing order by that #msg_id_in_t (sequence #).  Relevant only if `Owned_channel::S_HAS_2_PIPES == true`.
   */
  using Reassembly_q = std::map<msg_id_in_t, Msg_in_ptr_uniq>;

  /**
   * An `enum` with 3 values we use to encode lead-message-has-handle-or-not expectation policy:
   * `Tribools_vals::true_value` (lead message must have a native handle), `Tribool_vals::false_value`
   * (must have no native handle), `Tribool_vals::indeterminate_value` (either is allowed).
   *
   * ### Rationale ###
   * We do not use an actual `tribool`, because the tricky `==` and `!=` semantics (where it results in a
   * `tribool` whose conversion to `bool` has subtle semantics we do not want) invite bugs.  We just use
   * its `enum` whose meanings fit our desired ones nicely.
   */
  using Tribool = boost::logic::tribool::value_t;

  /// See #Tribool.
  using Tribool_vals = boost::logic::tribool;

  /**
   * Data and policy with respect to receipt of the next/currently-incomplete in-message.  Depending on whether
   * 1 or 2 in-pipes are enabled in #Owned_channel, and in the former case what kind of in-pipe it is
   * (see Channel::S_HAS_BLOB_PIPE, Channel::S_HAS_NATIVE_HANDLE_PIPE, and similar), there
   * may be 1 or 2 of these objects (sets of policy + state) in `*this`.  See #m_rcv_pipes
   * (null pointer being used to permanently disable up to 1 of them) of this type.
   *
   * As a general rule:
   *   - Information that never changes about a given Msg_in_pipe is determined at compile-time and available
   *     via `static constexpr` and alias members.  Do not use non-`constexpr` members (not even `const` ones).
   *   - The rest is (mutable) state and resides in normal data members.
   *
   * ### Algorithm/background ###
   * The #Owned_channel, potentially being a Native_handle_receiver,
   * is possibly capable of receiving unstructured messages, each
   * message bearing a blob and, optionally, a #Native_handle.  Suppose the opposing peer wants to send
   * a structured message M, plus #Native_handle S.  The latter can just be sent in a single message.
   * The former (M) is serialized into 1 or 2+ RAM blobs (a/k/a segments in capnp parlance), depending on its
   * size.  This shall *always* be 1 if we are SHM-backed.
   *
   * Then: the whole structured message M + #Native_handle S shall be sent as:
   *   -# 1 *lead* message containing:
   *      -# (as header) mandatory metadata (containing notably the message ID);
   *      -# (after header) 1st capnp-segment serializing M itself;
   *   -# 0+ further *continuation* messages serializing the rest of M (more capnp-segments as needed 1 seg = 1 msg).
   *      (Typically 0; always 0 if SHM-backed.)
   *
   * There are also internal messages -- presumed rare.  These are represented by the bullet point 1.1 only:
   * there is just the mdt header; nothing else.  The algorithm here operates, therefore, under the assumption
   * that each structured message = 1 lead message; plus 0+ continuation messages.  However in perf analysis
   * one should assume 0 continuation messages, as most messages are user messages, and those usually are serializable
   * as just 1 capnp-segment.
   *
   * `m_incomplete_msg` shall store that state across the 1+ async-reads.  Once all the segments (and leading mdt
   * header) of a given message have been received and loaded into `*m_incomplete_msg`, it is complete and is
   * `move()`d to the next layer, eventually hopefully emitted to the user.  So then `m_incomplete_msg` is
   * assigned a newly-constructed empty message, until the next async-read feeds the msdt/seg 1 into it -- and so on.
   *
   * Which pipe is used for these 1+ messages, including the 1st one that includes `Native_handle` S?
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
   * Now assume *no* #Native_handle S wants to be sent with M.  The serialization logic is all the same -- there's
   * just no S transmitted along with the lead message.  The transport media are different; as any pipe can
   * transmit these unstructured messages, not needing to transmit these strange native handle thingies.  So then
   * which pipe is used for these messages?
   *   - If Channel::S_HAS_BLOB_PIPE_ONLY is `true`, then naturally the blobs pipe is used -- being the only one
   *     available.
   *   - If Channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY is `true`, then the handles pipe is used -- being the only one
   *     available (and perfectly capable of transmitting just-blobs).
   *   - If Channel::S_HAS_2_PIPES is `true`, then: Although either pipe *could* be used, we use the
   *     blobs pipe; the idea being that the only reason to configure both pipes in a Channel is for performance
   *     (certainly it doesn't make anything *simpler* after all), and since the handles-pipe alone was deemed
   *     insufficient, we'll reserve it for handle-bearing structured user messages only.
   *     - Perf caveat: Isn't it "giving away" perf to (for the sake of simplicity) send messages
   *       pertaining to sans-handle lead messages along the handles pipe (and not the blobs pipe)?
   *       Well, yes, possibly a bit -- but only (1) if a message is handle-bearing in the first place; and (2)
   *       ifs serialization comprises 2+ segments, not 1 (SHM-backed message = always 1 segment in particular).
   *       That alone likely puts in mostly-negligible territory, as typically messages are 1-segment.
   *       Still: Here's a to-do:
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
   * #m_rcv_pipes, each one's type's #S_LEAD_HNDL_EXPECTATION (compile-time constant from t-param!)
   * determining (1) which `async_receive_*()` API will be called; and (2) how the async non-error
   * result will be handled.  (See start_and_poll() where this is kicked off for the first time,
   * meaning each of the 2 `Msg_in_pipe`s is cted, specifying #S_LEAD_HNDL_EXPECTATION forever via t-param.)
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
   * just blobs (#S_LEAD_HNDL_EXPECTATION is `false_value`), one for just blob-and-handle combos
   * (#S_LEAD_HNDL_EXPECTATION is `true_value`).  They can be interleaved.
   * Otherwise there's only 1 Msg_in_pipe, namely:
   *   - If the 1 pipe is blobs-only, `Owned_channel::S_HAS_BLOB_PIPE_ONLY`: it's, obviously, for just
   *     blobs (#S_LEAD_HNDL_EXPECTATION is `false_value`).
   *   - If the 1 pipe is blobs-and-handles, `Owned_channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY`: it's for
   *     both types of messages (#S_LEAD_HNDL_EXPECTATION is `indeterminate_value`).
   *
   * @note Detour/update: As noted in daddy Channel class doc header, we now typically make use of batch-receiving
   *       (unless relevant `Msg_in_pipe::S_RCV_BATCH_SIZE == 1`).  The above description essentially holds; just
   *       potentially many messages are received with one call (`.async_receive_*_batch()`) into the #Owned_channel
   *       layer.  For the purposes of the above discussion one can think of this as the equivalent number of
   *       `.async_receive_{blob|native_handle}()` calls succeeding synchronously one after the other.
   *       Anyway the algorithm above holds; just the actual async-read calls are expressed somewhat differently
   *       from the above text.
   *
   * @tparam LEAD_HNDL_EXPECTATION
   *         See #S_LEAD_HNDL_EXPECTATION.
   */
  template<Tribool LEAD_HNDL_EXPECTATION>
  struct Msg_in_pipe
  {
    // Types and constants.

    /**
     * A #Tribool specifying the requirement on whether the lead (often only) unstructured message of each
     * structured in-message must contain or not contain a native handle.  Specific meanings are documented
     * in #Tribool doc header.  (Also it explains why we avoid acutal `tribool` logic and just use it as an `enum`.)
     *
     * This also contains the information as to which of the two potential pipes of #Owned_channel a `*this` pertains
     * to: `false_value` <=> blobs-only pipe of #Owned_channel;
     * else (`true_value` or `indeterminate_value`) <=> handles-and-blobs pipe of #Owned_channel.
     * To wit see #S_BLOBS_ELSE_HNDLS_PIPE.
     */
    static constexpr Tribool S_LEAD_HNDL_EXPECTATION = LEAD_HNDL_EXPECTATION;

    /// If `true`, this pertains to the blobs-only pipe of #Owned_channel; else to its blobs-and-handles pipe.
    static constexpr bool S_BLOBS_ELSE_HNDLS_PIPE = S_LEAD_HNDL_EXPECTATION == Tribool_vals::false_value;

    /// Batch-size to use for this pipe (`Channel` carries the concept constant under per-pipe bifurcated names).
    static constexpr size_t S_RCV_BATCH_SIZE = S_BLOBS_ELSE_HNDLS_PIPE
                                                 ? Owned_channel::S_RCV_BLOB_BATCH_SZ_RECOMMENDATION
                                                 : Owned_channel::S_RCV_NATIVE_HANDLE_BATCH_SZ_RECOMMENDATION;
    static_assert(S_RCV_BATCH_SIZE >= 1,
                  "Batch must be able to hold at least one message.  `1` would bypass most batching code but work.");

    /**
     * The type of #m_target_batch: the target (blob(s), handle(s)) of the next unstructured-message async-read.
     *
     * Note that, as explained in daddy class Channel doc header, we use a simplified code-path if configured
     * to use "batches" that are sized 1; that is to not use batch-receiving.  So in that case
     * the actual #Batch type is Generic_msg_batch_in (which we always set to contain 1 slot at run-time),
     * and the async-receiving layer of `Channel` shall simply target slot 0's contained target blob (and possibly
     * `Native_handle`), and we don't even let its `.n_used()` change away from 0.  Basically it's just a
     * buffer/handle store, nothing more.
     *
     * ### Rationale ###
     * In that case why not just store a separate field pair in Msg_in_pipe, like `Segment_blob_in m_target_blob` +
     * `Native_handle m_target_hndl`?  Answer: Sure, that would've been fine too; then one would just not touch
     * `m_target_batch` (it is already `optional` even, for other reason(s)).  Doing it this way just seemed like a nice
     * compact way (both memory-wise and code-wise) to store it; that's all.  Either way is fine.  (To be clear
     * Generic_msg_batch_in is useful for other purposes; it doesn't just exist for this degenerate use.)
     */
    using Batch = std::conditional_t<S_RCV_BATCH_SIZE == 1,
                                     Generic_msg_batch_in<Segment_blob_in, S_BLOBS_ELSE_HNDLS_PIPE>,
                                     std::conditional_t
                                       <S_BLOBS_ELSE_HNDLS_PIPE,
                                        typename Owned_channel::template Blob_batch_in<Segment_blob_in>,
                                        typename Owned_channel::template Native_handle_batch_in<Segment_blob_in>>>;
    // Data.

    /**
     * As we are building the current structured #m_incomplete_msg, whether the the next unstructured in-message be the
     * lead/first (often only) one composing it (`true`); or one of the next ones a/k/a a continuation
     * message (`false`).  So it starts `false` whenever #m_incomplete_msg is cted; changed to `true` after the lead
     * message (if there are continuation messages forthcoming; otherwise #m_incomplete_msg is cted right then again).
     *
     * @note We could also, conceptually, get this state from #m_incomplete_msg itself.  As of this writing there's
     *       no accessor to the effect of "how many segments ya got?" or "have ya got at least one segment already?"
     *       in Msg_in_impl or Msg_in, and this flag here seemed like a simple way to keep track of it ourselves.
     */
    bool m_next_msg_lead_else_cont;

    /**
     * 0 when #m_next_msg_lead_else_cont is `true`; otherwise: The number of user-message-composing capnp-segments
     * still needed **including** the last message (segment) to have been received.  Hence once it reaches 1,
     * #m_incomplete_msg is complete and must be `move()`d away and cted anew.  So it goes like this:
     *   -# #m_incomplete_msg is cted; #m_next_msg_lead_else_cont set to `true`; we're set to 0.
     *   -# Read lead msg.  Scan the mdt header therein; its Msg_in_impl::deserialize_mdt() returns the initial
     *      value assigned to us.  If we're 0, it's an internal message.  If we're 1, it's a user message comprising
     *      1 segment (this is good and typical but not universal).  Either way #m_incomplete_msg is immediately
     *      complete: go back to step 1.  Otherwise it is 2 or higher.
     *   -# Read continuation msg.  Decrement us.  If we're now 1, #m_incomplete_msg is complete: go back to step 1.
     *      Otherwise it is still 2 or higher.  Repeat the present step.
     */
    size_t m_n_segs_left_pre_last_read;

    /**
     * See #Batch doc header.
     *
     * ### Rationale ###
     * This type is template-parameterized, so we use method templates of daddy `Channel` to operate on a `*this`;
     * and use auto-typed lambda captures of `this` when async-ops are involved.  See, relatedly, #m_rcv_pipes doc
     * header.
     *
     * It is `optional` as of this writing only because it is somewhat painful to direct-initialize a `*this`
     * otherwise.  For all intents and purposes this thing is never null except at the very start.
     */
    std::optional<Batch> m_target_batch;

    /**
     * Target (incomplete) in-message for the `async_receive_*()` operation in-progress; one is always in-progress
     * once `start_and_poll()` has been called, until the pipe is hosed.  Its lifecycle is described in
     * #m_n_segs_left_pre_last_read doc header.  Some added details as pertains to that:
     *   - Msg_in_impl::add_serialization_segment() is called to `move()` a #Segment_blob_in (blob) stored inside
     *     #m_target_batch into this `m_incomplete_msg`.  (That slot's blob is then re-allocated for the next time.)
     *     - The blob's `.capacity()` is unchanged; while `.size()` is adjusted via `.resize()` to reflect the
     *       actual # of bytes received which may be `<= .capacity()`.  We do this just before
     *       `m_incomplete_msg.add_serialization_segment()`.
     *   - Msg_in_impl::deserialize_mdt(), called upon receiving the lead message and doing the preceding step,
     *     returns the # of segments (at least 1).
     *   - Msg_in_impl::deserialize_body() is called once all segments have been added into this guy.
     *     As a result Msg_in::body_root() now provides access to the structured (capnp) accessor tree.  Done!
     *     Can emit to user, or at least save at the next layer of daddy `*this` to emit later.
     *
     * Subtlety: It's a `unique_ptr` (not #Msg_in_ptr `shared_ptr`) for anti-leak safety/perf.
     * It is moved and upgraded to a `shared_ptr` (#Msg_in_ptr) when message completed.
     */
    Msg_in_ptr_uniq m_incomplete_msg;

    /**
     * Used only for the first/second in-messages in the pipe, before any #m_incomplete_msg, when doing protocol
     * negotiation + channel-header grabbing:
     * this is the (small) target buffer to the simple `ProtocolNegotiation` capnp-`struct`; then again
     * the `ChannelHeader` capnp-`struct`.  It is emptied once this procedure finishes (i.e., once #m_incomplete_msg
     * is targeted with an async-read for the 1st time).
     */
    Segment_blob_in m_init_msg_blob;

    /**
     * Accompanying #m_init_msg_blob, this is the target `Native_handle` for those initial async-receives (if the
     * pipe supports such, else untouched).  However: such initial messages shall never include any
     * non-null `Native_handle`; but we still need a place to put it for the `.async_receive_native_handle()` API.
     *
     * @todo Msg_in_pipe::m_init_msg_blob and Msg_in_pipe::m_init_msg_hndl are only relevant for the initial
     * phase of daddy `Channel`'s read-chain; as such it might be stylistically nicer to keep them around as
     * `shared_ptr`-framed transient state that's destroyed upon completing that initial async-receive and
     * passed-around as needed exclusively via lambda captures.  It would hardly save any real RAM,
     * but less `struct` state is arguably nice for such short-term ops.
     */
    Native_handle m_init_msg_hndl;
  }; // struct Msg_in_pipe

  /**
   * Just groups the varities of handler to be saved and executed via handlers_post() and handlers_poll().
   *
   * @see Doc header for #m_sync_io_handlers which discusses the design+trade-offs here.
   *
   * To coders/maintainers: If you add/modify nested types inside this, please follow this rule of thumb:
   *   - If it fits in ~64 bytes (assuming x86-64 say), use a movable `struct`, like `struct Cool_variety {}`.
   *     (This is preferred.)
   *   - Otherwise make it a pointer-handle, like:
   *     `struct Cool_variety_body { ... }; using Cool_variety = unique_ptr<Cool_variety_body>;`.
   *
   * (Even if your `Cool_variety` will be rarely used, but it is large, it'll affect the size of `Handler` which
   * *might* still affect perf even when it's not used.  Maybe... maybe not... but let's stay on the safe side.)
   */
  struct Handlers
  {
    // Types.

    /**
     * Executor for handler indicating receipt of a message (an expected response as via `async_request()`,
     * an expected notification as via `expect_*()`, or a remote unexpected response as via
     * set_remote_unexpected_response_handler()).  This is by far the most popular expected variety in Handlers.
     *
     * @note Would've made it and the others `struct`s with list-initializable data members, but that
     *       doesn't play well with forwarding-things like `.emplace_back()`; hence forced to write ctor boiler-plate.
     */
    class Message
    {
    public:
      // Constructors/destructor.
      /**
       * Inits it.
       * @param msg_in
       *        Message that arrived.
       * @param on_msg_func_if_expected
       *        Handler to execute; or null if the message was an unexpected response.
       */
      Message(Msg_in_ptr_uniq&& msg_in, On_msg_handler_ptr&& on_msg_func_if_expected);
      // Methods.
      /**
       * Executes handler.
       * @param daddy
       *        The containing object.
       */
      void execute(Channel* daddy);
    private:
      // Data.
      /// See ctor.
      Msg_in_ptr_uniq m_msg_in;
      /// See ctor.
      On_msg_handler_ptr m_on_msg_func_if_expected;
    }; // class Message

    /**
     * Executor for handler indicating being informed by opposing side that *it* received a response to a message
     * that was not issued as a request receving a message (as via set_unexpected_response_handler()).
     */
    class Remote_unexpected_response
    {
    public:
      // Constructors/destructor.
      /**
       * Inits it.
       * @param originating_msg_id
       *        As indicated by opposing side, the ID of the message that allegedly requested a response (but
       *        didn't in fact) according to the metadata of the incoming (to the opposing side) problematic in-message.
       * @param mdt_text
       *        Explanatory text indicated by the opposing side ostensibly describing various things about
       *        the problematic in-message (for logging/reporting).
       */
      Remote_unexpected_response(msg_id_t originating_msg_id, std::string&& mdt_text);
      // Methods.
      /**
       * Executes handler.
       * @param daddy
       *        The containing object.
       */
      void execute(Channel* daddy);
    private:
      // Data.
      /// See ctor.
      msg_id_t m_originating_msg_id;
      /// See ctor.
      std::string m_mdt_text;
    }; // class Remote_unexpected_response

    /// Executor for handler indicating `Channel` error (as via start_and_poll() arg).  Empty (`daddy` arg is enough).
    class Error
    {
    public:
      // Methods.
      /**
       * Executes handler.
       * @param daddy
       *        The containing object.
       */
      void execute(Channel* daddy);
    }; // class Error

    /**
     * Thing that stores a handler of one of the peer types, determined at run-time (in union-y fashion).
     *
     * To coder/maintainer: Try to keep the variant types sorted from most to least frequent in occurrence (perf).
     */
    using Handler = std::variant<Message, Remote_unexpected_response, Error>;
  }; // struct Handlers

  // Constants.

  /**
   * If `true` handlers_poll() is a no-op, and handlers_post() will simply synchronously run the "posted" body;
   * if `false` any posted handlers will execute the next time the code does handlers_poll().
   *
   * @see m_sync_io_handlers doc header which gets into all this.
   * @todo Perhaps this should be overridable via a `#define/#if` controllable via build script(s).
   */
  static constexpr bool S_AGGRESSIVE_HANDLER_EXECUTION = true;

  /// Convenience magic number thing.  It's best to see how exactly is used; the situation might be non-trivial.
  static constexpr Protocol_negotiator::proto_ver_t S_PROTOCOL_VERSION_MIN = 2;
  /// Similarly to #S_PROTOCOL_VERSION_MIN....
  static constexpr Protocol_negotiator::proto_ver_t S_PROTOCOL_VERSION_MAX = S_PROTOCOL_VERSION_MIN;

  // Methods.

  // start_and_poll() pipe: Protocol negotiation/header phase.

  /**
   * Acts on the pre-condition that the given in-pipe has just started (thus not known to be in would-block state);
   * and therefore we should read (and process) as many unstructured in-messages as synchronously possible
   * -- starting with the protocol-negotiation in-message -- until reaching would-block state, at which point we
   * should `return` with a pending async-wait/read on that in-pipe.  Either synchronously or asynchronously
   * (across an async-wait) this will ultimately invoke rcv_on_async_read_proto_neg_msg() which, in the best case
   * scenario, will move on to the next step: `rcv_async_read_channel_header(pipe)`.
   *
   * This shall be called in exactly the following situation: start_and_poll().
   *
   * @param pipe
   *        Pointer into #m_rcv_pipes array.
   */
  template<typename Msg_in_pipe_t>
  void rcv_async_read_proto_neg_msg(Msg_in_pipe_t* pipe);

  /**
   * To execute upon completing an `m_channel.async_receive_*()` of the expected protocol-negotiation message along
   * the given in-pipe, this processes the result (message or error) and continues the read chain (on negotiation
   * success) or ends it (on any failure).  It *will* invoke handlers_poll() if appropriate (namely on error).
   *
   * If continuing the read chan: the next link is rcv_async_read_channel_header().
   *
   * @param pipe
   *        See rcv_async_read_proto_neg_msg().
   *        `pipe->m_init_msg_blob` and `pipe->m_init_msg_hndl` may have been received-into.
   * @param err_code
   *        From `Channel::async_receive_*()`.
   * @param sz
   *        From `Channel::async_receive_*()`.
   */
  template<typename Msg_in_pipe_t>
  void rcv_on_async_read_proto_neg_msg(Msg_in_pipe_t* pipe, Error_code err_code, size_t sz);

  /**
   * Next in the read chain following rcv_on_async_read_proto_neg_msg(), this is similar to
   * rcv_async_read_proto_neg_msg() but on success invokes rcv_on_async_read_channel_header() which
   * processes the `ChannelHeader` message as opposed to protocol-negotiation message (the preceding thing).
   *
   * This shall be called in exactly the following situation: rcv_on_async_read_proto_neg_msg().
   *
   * @param pipe
   *        Pointer into #m_rcv_pipes array.
   */
  template<typename Msg_in_pipe_t>
  void rcv_async_read_channel_header(Msg_in_pipe_t* pipe);

  /**
   * This is to rcv_async_read_channel_header() what rcv_on_async_read_proto_neg_msg() is to
   * rcv_async_read_proto_neg_msg():
   *
   * Processes the result (message or error) and continues the read chain (if no error) or ends it (on error).
   * It *will* invoke handlers_poll() if appropriate (namely on error).  Processing the result successfully means
   * at least saving the relevant `ChannelHeader` info; as of this writing #m_peer_process_creds.
   *
   * If continuing the read chan: the next link is rcv_async_read().
   *
   * @param pipe
   *        See rcv_async_read_channel_header().
   *        `pipe->m_init_msg_blob` and `pipe->m_init_msg_hndl` may have been received-into.
   * @param err_code
   *        From `Channel::async_receive_*()`.
   * @param sz
   *        From `Channel::async_receive_*()`.
   */
  template<typename Msg_in_pipe_t>
  void rcv_on_async_read_channel_header(Msg_in_pipe_t* pipe, Error_code err_code, size_t sz);

  // start_and_poll() pipe: Main phase, unstructured-message layer.

  /**
   * On rcv_on_async_read_channel_header() having received the pipe-leading protocol-negotiation bytes and
   * passed #m_protocol_negotiator negotiation and obtained `ChannelHeader`-stored info, this continues the
   * read chain by starting the bulk of it, wherein synchronously or asynchronously we are always reading
   * the next unstructured in-message -- until error or `*this` destruction.
   *
   * Compile-time-depending on certain compile-time config, we equal either rcv_async_read_batched() or
   * rcv_async_read_unbatched().  The two code paths are mutually exclusive (have conceptual similarities but
   * as of this writing ~no common code in `*this` itself) but share the same code at the layer wherein
   * a given unstructured in-message (in blob form) is ready for interpretation (complete
   * Msg_in_pipe::m_incomplete_msg, send it to user-facing layer... and so on).
   *
   * @param pipe
   *        Pointer into #m_rcv_pipes array.
   */
  template<typename Msg_in_pipe_t>
  void rcv_async_read(Msg_in_pipe_t* pipe);

  /**
   * To be invoked either *as* rcv_async_read() (if we are compile-time-configured to receive unstructured in-messages
   * in *batches*) or later in the read-chain upon having obtained (and processed) an
   * in-batch (1+ unstructured in-messages).
   *
   * @param pipe
   *        Pointer into #m_rcv_pipes array.
   * @param pessimistic
   *        A/k/a `assume_would_block` elsewhere, this shall be `true` if the just-received-and-processed
   *        in-batch implied would-block -- got 1+ in-messages but fewer than its capacity; or `false` if
   *        it did not imply it -- got a totally-full in-batch (could mean there's even more to read; could
   *        mean that's exactly how many were pending, a coincidence).
   */
  template<typename Msg_in_pipe_t>
  void rcv_async_read_batched(Msg_in_pipe_t* pipe, bool pessimistic = false);

  /**
   * To be invoked either *as* rcv_async_read() (if we are compile-time-configured to not receive unstructured
   * in-messages in *batches* but rather one async-read at a time) or later in the read-chain upon having obtained
   * (and processed) an unstructured in-message.
   *
   * @note In particular, it should reduce to *exactly* the same logic as rcv_async_read_batched() if
   *       the batch-capacity (Msg_in_pipe::S_RCV_BATCH_SIZE) were `1`.  Incidentally it lacks `bool pessimistic`,
   *       because when the capacity is 1, receiving an in-batch (1+ in-messages... so 1 in-message) makes it unclear
   *       whether the pipe is now in would-block or not; hence `pessimistic = false` always.
   *
   * @param pipe
   *        Pointer into #m_rcv_pipes array.
   */
  template<typename Msg_in_pipe_t>
  void rcv_async_read_unbatched(Msg_in_pipe_t* pipe);

  /**
   * The part of rcv_async_read_batched() algorithm that is "between" any possible async-waits; in other words
   * processes an in-batch or an error (but not would-block!) in trying to read such -- but stops before
   * perpetuating the read chain even in the former (no-error) case.
   *
   * If and only #m_channel_err_code_or_ok is truthy on return, stop read chain; else continue it.
   *
   * @param pipe
   *        Pointer into #m_rcv_pipes array.
   *        `*pipe->m_target_batch` contains the updated Msg_batch_in (including Msg_batch_in::n_used() `>= 1`
   *        indicating how many in-messages were obtained) unless `err_code` truthy.
   * @param err_code
   *        Result of batch read; either falsy (success) or truthy (error but *not* would-block).
   * @return If `m_channel_err_code_or_ok` is truthy on return, return `false`, but basically the return value
   *         should be ignored (channel-hosing error should be checked-for first).  Otherwise:
   *         `true` means no implied would-block; please do another *synchronous* read.
   *         `false` (with `!m_channel_err_code_or_ok`) means implied would-block;
   *         do a definitely-asynchronous (for perf) read (since pipe is in would-block).
   */
  template<typename Msg_in_pipe_t>
  bool rcv_on_async_read_batch(Msg_in_pipe_t* pipe, const Error_code& err_code);

  /**
   * The part of rcv_async_read_unbatched() algorithm that is "between" any possible async-waits; in other words
   * processes an in-message or an error (but not would-block!) in trying to read such -- but stops before
   * perpetuating the read chain even in the former (no-error) case.
   *
   * If and only #m_channel_err_code_or_ok is truthy on return, stop read chain; else continue it.
   *
   * @note In particular, it should reduce to *exactly* the same logic as rcv_on_async_read_batch() if
   *       the batch-capacity (Msg_in_pipe::S_RCV_BATCH_SIZE) were `1`, and `batch->n_used() == 1`, and
   *       `batch->n_used()->result_payload_blob() == sz`.  Incidentally it takes `sz`, because
   *       we do not use Msg_in_pipe::m_target_batch as anything more but a place to store bytes + handle, in its
   *       slot 0.  `.n_used()` would go from 0 to 1 by definiton of single-message receive (so no info needed);
   *       and bytes-received is given to us explicitly by Blob_receiver::async_receive_blob() or
   *       Native_handle_receiver::async_receive_native_handle() (as `sz`) as opposed to modifying that part of
   *       slot 0, so we can just pass that to this method.  Lastly we return `void`, unlike
   *       rcv_on_async_read_batch(), as with a 1-batch that guy would always return `true` anyway.
   *
   * @param pipe
   *        Pointer into #m_rcv_pipes array.
   *        `*pipe->m_target_batch` (capacity 1!) slot 0 contains the new in-message unless `err_code` truthy.
   * @param err_code
   *        Result of single-message read; either falsy (success) or truthy (error but *not* would-block).
   * @param sz
   *        If `err_code` falsy, this value (which must be positive) is how many bytes were obtained.
   */
  template<typename Msg_in_pipe_t>
  void rcv_on_async_read_one(Msg_in_pipe_t* pipe, const Error_code& err_code, size_t sz);

  /**
   * When `pipe->m_next_msg_lead_else_cont == true`:
   * Where the rcv_async_read_batched() and rcv_async_read_unbatched() mutually-exclusive code-paths merge again,
   * this is where we bridge to the structured-message layer, updating everything in Msg_in_pipe
   * except Msg_in_pipe::m_target_batch (the preceding layer/the caller's responsibility still).
   *
   * More specifically, then: updates `pipe->m_incomplete_message` with the new unstructured in-message; and
   * if that completes the structured in-message, feeds it to the next layer: rcv_struct_new_msg_in().
   *
   * If and only #m_channel_err_code_or_ok is truthy on return, stop read chain; else continue it.
   * The former would occur if `pipe->m_incomplete_msg` is completed by the processed in-message, but
   * it is broken somehow (other side didn't follow protocol/bug).
   *
   * @param pipe
   *        Pointer into #m_rcv_pipes array.
   * @param target_blob_ptr
   *        New unstructured in-message's blob content, with `->size() == ->capacity()`.  We take ownership
   *        of it (move it into the next layer); it shall be as-if default-cted on return.
   * @param sz
   *        Number of bytes at `target_blob_ptr->begin()` that were read; the rest of `->size()` is garbage.
   * @param target_hndl
   *        Native handle, if any, in the new unstructured in-message.
   *        For certain compile-time properties of `Msg_in_pipe_t`, this must be `.null()`; for others
   *        the reverse; and for others still it can be either.  See Msg_in_pipe doc header.
   */
  template<typename Msg_in_pipe_t>
  void rcv_on_async_read_lead_msg(Msg_in_pipe_t* pipe, Segment_blob_in* target_blob_ptr, size_t sz,
                                  Native_handle target_hndl);

  /**
   * Same as rcv_on_async_read_lead_msg() but for when `pipe->m_next_msg_lead_else_cont == false`.
   *
   * @note If we've tuned things appropriately, this should be invoked not too often, mostly
   *       rcv_on_async_read_lead_msg().  With SHM-backing, this is never called.
   *
   * @param pipe
   *        See rcv_on_async_read_lead_msg().
   * @param target_blob_ptr
   *        See rcv_on_async_read_lead_msg().
   * @param sz
   *        See rcv_on_async_read_lead_msg().
   * @param target_hndl
   *        See rcv_on_async_read_lead_msg().
   */
  template<typename Msg_in_pipe_t>
  void rcv_on_async_read_continuation_msg(Msg_in_pipe_t* pipe, Segment_blob_in* target_blob_ptr, size_t sz,
                                          Native_handle target_hndl);


  // start_and_poll() pipe: Main phase, structured-message layer.

  /**
   * Once a structured in-message is completed, it is fed to this which processes it (the structured-message
   * layer proper).
   *
   * If and only #m_channel_err_code_or_ok is truthy on return, stop read chain; else continue it.
   * The former would occur if `msg_in` is broken somehow (other side didn't follow protocol/bug).
   *
   * @param msg_in
   *        The completed structured in-message: after Msg_in::deserialize_mdt() but before
   *        Msg_in::deserialize_body() (which may not be involved at all, if it's an internal message).
   */
  void rcv_struct_new_msg_in(Msg_in_ptr_uniq&& msg_in);

  /**
   * Helper from `rcv_struct_new_msg_in()`: the case where the in-message has a sentinel message ID value, indicating
   * an internal message rather that one from a user send() (et al).
   *
   * If and only #m_channel_err_code_or_ok is truthy on return, stop read chain; else continue it.
   * The former would occur if `msg_in` is broken somehow (other side didn't follow protocol/bug).
   *
   * @param msg_in
   *        See rcv_struct_new_msg_in().  Must have sentinel message ID.
   */
  void rcv_struct_new_internal_msg_in(const Msg_in_impl<Msg_in>& msg_in);

  /**
   * Helper from `rcv_struct_new_msg_in()`: the case where #m_phase is Phase::S_CLI_LOG_IN, and the in-message has
   * the log-in-response value 1 + session-token=nil as required: Processes the in-message according to the rigid
   * log-in phase algorithm.
   *
   * If and only #m_channel_err_code_or_ok is truthy on return, stop read chain; else continue it.
   * The former would occur if `msg_in` is broken somehow (other side didn't follow protocol/bug).
   *
   * @param msg_in
   *        See rcv_struct_new_msg_in().  Must have message ID 1 and session-token=nil.
   */
  void rcv_struct_new_msg_in_during_log_in_as_cli(Msg_in_ptr_uniq&& msg_in);

  /**
   * Like rcv_struct_new_msg_in_during_log_in_as_cli() but for Phase::S_SRV_LOG_IN instead of CLI_LOG_IN.
   *
   * If and only #m_channel_err_code_or_ok is truthy on return, stop read chain; else continue it.
   * The former would occur if `msg_in` is broken somehow (other side didn't follow protocol/bug).
   *
   * @param msg_in
   *        See rcv_struct_new_msg_in_during_log_in_as_cli().
   */
  void rcv_struct_new_msg_in_during_log_in_as_srv(Msg_in_ptr_uniq&& msg_in);

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
   * If and only #m_channel_err_code_or_ok is truthy on return, stop read chain; else continue it.
   * The former would occur if `msg_in` is broken somehow (other side didn't follow protocol/bug).
   *
   * @param msg_in
   *        See rcv_struct_new_msg_in().
   */
  void rcv_struct_new_msg_in_is_next_expected(Msg_in_ptr_uniq&& msg_in);

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

  // Registration.

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
  bool expect_msgs_impl(Msgs_in* qd_msgs, bool one_off, Msg_which_in which, On_msg_handler_ptr&& on_msg_func);

  // Sending.

  /**
   * send() and async_request() implementation.
   * Note this is still the higher-level, public send() or async_request() really;
   * it handles *user* messages; and once it has decided to indeed attempt the actual sending over
   * #Owned_channel, it synchronously invokes send_core().  To send an internal message use send_core()
   * directly.
   *
   * `check_unsendable(msg)` and `check_prior_error()` must have returned `true`.
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
  bool send_impl(Msg_out* msg, const Msg_in* originating_msg_or_null, Error_code* err_code,
                 const On_msg_handler_ptr& on_rsp_func_or_null, msg_id_out_t* id_unless_one_off);

  /**
   * Core of send() or internal-message-send: Given a list of raw blobs (intended as one per structured out-message)
   * sends them (first byte accompanies by the `Native_handle` supplied if any) out over the proper pipe.
   *
   * @see send_init_msgs(), the alternative to send_core() briefly used before the main phase (which uses send_core()).
   *
   * This is the last step of send() and async_request() (for user out-messages) or an internal attempt to send
   * internal out-message.  As such (being the last step):
   * If user message (`msg` not null), and send_core() succeeds in Phase::S_SRV_LOG_IN, #m_phase moves to
   * Phase::S_LOGGED_IN within this method.
   *
   * @param blobs_out
   *        1+ messages, each as a blob, to send.
   * @param hndl
   *        If not null, handle to send with `blobs_out.front()`.
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
  bool send_core(const Segment_bufs& blobs_out, Native_handle hndl, Error_code* err_code_or_ignore);

  /**
   * Essentially a much-cut-down version of send_core() that specifically sends, along all #Owned_channel pipes,
   * the `ProtocolNegotiation` and `ChannelHeader` messages -- the first payload required along each pipe.  As of
   * this writing invoked from start_ops(), meaning before start_and_poll() or any send() or async_request().
   *
   * Post-condition: #m_init_msg_err_code_or_ok is falsy if there was no problem sending; else the error to emit
   * from subsequent start_and_poll() or send() or async_request() (whichever is called first).
   */
  void send_init_msgs();

  // Utilities.

  /**
   * Record handler to invoke in handlers_poll() soon (in rolled-back mode `S_AGGRESSIVE_HANDLER_EXECUTION == false`);
   * or simply invokes that handler synchronously (if `S_AGGRESSIVE_HANDLER_EXECUTION == true`).
   *
   * The context for this is: we're scanning an unstructured (low-level) message received along a pipe
   * of #Owned_channel; and it completed something such that the user
   * shall be informed of this via a completion handler they had registered.  The relevant completion handler
   * varieties are 1-1 with the nested classes within our nested type Handlers.
   *
   * (async_end_sending() completion handler is a separate, lower-level thing.)
   *
   * @see Doc header for #m_sync_io_handlers which discusses the design+trade-offs here.
   *
   * @tparam Handler_variety
   *         Specifies the variety of handler to invoke: a nested class of `struct` Handlers.
   * @param context
   *        Brief context string for (immediate) logging.
   * @param members
   *        Values for the `Handler_variety` object, describing the needed info.  These are perfect-forwarded
   *        into ctor of `Handler_variety`.
   */
  template<typename Handler_variety, typename... Members>
  void handlers_post(util::String_view context, Members&&... members);

  /**
   * Executes what was recorded recently in handlers_post(); or no-op if `S_AGGRESSIVE_HANDLER_EXECUTION == true`.
   *
   * If Channel::S_HAS_2_PIPES is `true`, then 1+ handlers may execute here; otherwise at most 1 can execute here.
   *
   * @see Doc header for #m_sync_io_handlers which discusses the design+trade-offs here.
   *
   * @param context
   *        Brief context string for logging.
   */
  void handlers_poll(util::String_view context);

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
   * @return `false` if `err_code` or a prior condition indicate the channel is hosed; `true` otherwise.
   */
  bool handle_async_err_code(const Error_code& err_code, util::String_view context);

  /**
   * Helper that handles the situation where #m_channel_err_code_or_ok is falsy, and processing has
   * found a new error condition.  Routes through hose() to set #m_channel_err_code_or_ok; then emits to
   * on-error handler.
   *
   * Do *not* use when synchronously emitting an error (send_core() only as of this writing).
   *
   * @param err_code_not_ok
   *        Truthy code (or assertion may trip).
   * @param context
   *        Brief context string for logging.
   * @param lower_layer_originating
   *        Forwarded to hose(); see its doc header.  Default `false` covers nearly all call sites
   *        (struc-protocol-layer detections); pass `true` from sites that promote a transport-layer
   *        (Owned_channel) failure into a channel-hosing error.
   */
  void handle_new_error(const Error_code& err_code_not_ok, util::String_view context,
                        bool lower_layer_originating = false);

  /**
   * The single funnel for transitioning #m_channel_err_code_or_ok from falsy to truthy: assigns
   * `err_code_not_ok` to that member and logs final stats via log_stats().  All sites that hose the channel
   * must go through here.
   *
   * Caller is still responsible for any caller-specific WARNING and any subsequent on-error handler
   * work (e.g., handle_new_error() does the latter via handlers_post()).
   *
   * @param err_code_not_ok
   *        Truthy code (or assertion may trip); the channel-hosing reason.  Will be saved into
   *        #m_channel_err_code_or_ok and never change thereafter.
   * @param context
   *        For logging: the algorithmic context (function name or whatever).
   * @param lower_layer_originating
   *        `true` if the cause is a transport-layer (#Owned_channel) failure -- in which case the
   *        relevant sub-object will have hosed/logged its own stats already, so we do not re-log them.
   *        `false` otherwise (out own layer's failures such as encountering wrong capnp protocol):
   *        log_stats() additionally prints (still-healthy) Owned_channel sub-objects' stats as a snapshot
   *        before they sit unused.
   */
  void hose(const Error_code& err_code_not_ok, util::String_view context, bool lower_layer_originating);

  /**
   * INFO-logs current stats: the structured-channel-level #m_stats; and (conditionally) the
   * Owned_channel sub-objects' stats.
   *
   * Designed to be invoked in two situations: (1) clean dtor; (2) immediately upon hosing (via hose()).
   * In both cases there is a small budget (~1-2) of such prints per `*this` lifetime, so the INFO
   * log level is appropriate.
   *
   * Subtlety: Where called from a synchronous code path that may invoke a user handler immediately
   * thereafter, log this *before* invoking the handler -- to avoid the user re-entering us and
   * confusing the stats picture.  hose() already does this.
   *
   * @param context
   *        For logging: the algorithmic context (function name or whatever).
   * @param log_owned_channel_stats
   *        If `true`, additionally print stats of the (1-2) Owned_channel sub-objects (snapshot at
   *        struc-layer hosing time, when those sub-objects are still healthy).  Pass `false` at
   *        clean-dtor time (sub-objects will print themselves moments later via their own dtors)
   *        and at lower-layer-originating hosing time (sub-objects already printed).
   */
  void log_stats(util::String_view context, bool log_owned_channel_stats) const;

  /**
   * Helper that returns `false` if and only if start_ops() has not yet been called.  This guard is required for
   * APIs that trigger #m_channel transmission API calls.  Outgoing-direction such calls would be, e.g.,
   * send() and async_end_sending().  Incoming-direction transmission is all triggered by start_and_poll() (which
   * begins the async-read chain(s) indefinitely).
   *
   * @param context
   *        Brief context string for logging.
   * @return `false` if not started (do not proceed); else `true`.
   */
  bool check_not_started_ops(util::String_view context) const;

  /**
   * Helper for some public APIs to use at the top: ensures that no prior error has been detected
   * (by incoming-direction processing or send()); returns `false` if not; `true` if so.
   * I.e., please no-op if returns `false`.
   *
   * @param context
   *        Brief context string for logging.
   * @return See above.  `false` bad.  `true` good.
   */
  bool check_prior_error(util::String_view context) const;

  /**
   * Helper for most public APIs to use at the top: ensures check_prior_error() passes, and that #m_phase equals
   * `required_phase`; returns `false` if not; `true` if so.  I.e., please no-op if returns `false`.
   *
   * @param required_phase
   *        #m_phase must equal this to return `true`.
   * @param context
   *        Brief context string for logging.
   * @return See above.  `false` bad.  `true` good.
   */
  bool check_phase_and_prior_error(Phase required_phase, util::String_view context) const;

  /**
   * send() and async_request() helper that returns `false` if and only if `msg` contains a native handle, but
   * #Owned_channel is compile-time-incapable of transporting them.  No `*this` state other than
   * logging context is accessed.
   *
   * @param msg
   *        See, e.g., send().
   * @return `false` if unsendable; else `true`.
   */
  bool check_unsendable(const Msg_out& msg) const;

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
   * Every user out-message created via create_msg() is supplied by the latter with #m_struct_builder_config
   * as the builder config.
   *   - So the schema template param is `<Msg_body_t>`, the `Channel` template param from the user.
   *   - The user is allowed to simply direct-construct the #Msg_out without any create_msg().  They have to
   *     supply the builder config themselves again, though.  It has to be the same type as
   *     #m_struct_builder_config, or it won't compile; but in theory it could have different knob values.
   *     E.g., it could have a null `Logger`, while `*this` one logs-galore.
   *
   * So it's an important member, but arguably its type is more important than its contents (knobs).
   */
  const Builder_config m_struct_builder_config;

  /**
   * Value (possibly of size 0 depending on this type) to pass to Msg_out_impl::emit_serialization()
   * to indicate the opposing side of the #Owned_channel.  If the type is Null_session (empty type), then
   * no information is necessary to indicate this.
   */
  const typename Builder_config::Builder::Session m_struct_lender_session;

  /// Analogous to #m_struct_builder_config but for deserialization.
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
   * Error, or lack thereof, recorded by start_ops() (having returned `true`) when synchronously sending out
   * protocol-negotiation and channel-header message(s).  More specifically send_init_msgs() sets it.
   * If truthy, subsequent start_and_poll(), send(), async_request(), or contextualy similar (whichever happens
   * first) will promote it to #m_channel_err_code_or_ok and emit it in the normal fashion.
   */
  Error_code m_init_msg_err_code_or_ok;

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
   * 1 or 2 active `struct`s containing policy (via their types, passed around as template args to methods) and
   * state (as data members) w/r/t receipt of low-level (unstructured) messages with the aim to complete the
   * next structured in-message(s).  Either only `.front()` or both elements are non-null.
   *
   * @see Msg_in_pipe doc header for detailed algorithm overview.
   *
   * ### Rationale: General ###
   * This is mostly explained in the aforementioned doc header, but as for as a couple technicalities:
   *
   * This thing is nothing more than a way for each Msg_in_pipe to keep existing starting with start_and_poll().
   * Nothing even accesses `m_rcv_pipes` by name after it is loaded; the pointer to each of the 1-2 relevant
   * pipe objects is passed continuously through each pipe's chain of methods/async-ops, as method args or
   * lambda captures (the latter when an async-op is involved; in fact specificaly an async-receive op on
   * #m_channel).  In fact the actual type of each non-null element is erased; note it is `shared_ptr<void>`; so
   * that information is passed around *only* as template parameters to the various methods.
   *
   * It would have been pretty easy to even get rid of #m_rcv_pipes entirely: just make a
   * `shared_ptr<Msg_in_pipe<...>>` and pass that thing around instead of a raw `Msg_in_pipe*`.  It would get freed
   * on error or `*this` destruction via `shared_ptr` ref-counting.  The reason we do it the current way is (1) it is
   * a bit faster (no ref-counting to track when a raw pointer is passed-around), and (2) it's no less
   * straightforward stylistically.
   *
   * ### Rationale: Why `shared_ptr<void>`? ###
   * ...as opposed to `unique_ptr` or `optional` or something?  Well, (1) it doesn't matter
   * perf-wise or style-wise much (it only "matters" in start_and_poll(), once, and then ~at destruction), and (2)
   * `unique_ptr` cannot do type erasure in a natural way; while `optional` cannot at all.  Though actually
   * see the following to-do regarding `any`.
   *
   * @todo Changing Channel::m_rcv_pipes to store `any` instead of `shared_ptr<void>` might be *slightly* prettier.
   * As noted ahead of this to-do, though, the only places that would actually change at all would be
   * at init (start_and_poll()) and ~at destruction.  Perf-wise that means it doesn't matter.  Style-wise it would
   * only matter in start_and_poll() in ~3 lines and here at the declaration site.  Arguably `any` (all else being
   * equal, which it is here) is simply a more expressive way to represent a type-erased value.  It's just slow
   * (in certain contexts -- but not ours).
   *
   * But I (ygoldfel) digress.  It's fine!
   *
   * ### Rationale: Why `array`? ###
   * ...Whatever.  `vector<>` would have been fine too... or two members.  `array<2>` just seemed a nice way
   * to express there can be only up to 2 pipes; that's all.
   */
  boost::array<boost::shared_ptr<void>, 2> m_rcv_pipes;

  /// See set_unexpected_response_handler().
  On_unexpected_response_func m_on_unexpected_response_func_or_empty;

  /// See set_remote_unexpected_response_handler().
  On_remote_unexpected_response_func m_on_remote_unexpected_response_func_or_empty;

  /**
   * The opposing peer's self-reported credentials (such as process ID); unknown/null until
   * the read chain started in start_and_poll() passes protocol negotiation and immediately following that
   * receives the `ChannelHeader` containing that info; unchanged after that point.
   *
   * As of this writing it is used in accessor remote_peer_process_credentials().
   */
  std::optional<util::Process_credentials> m_peer_process_creds;

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
   * Map storing current policy for expecting non-response messages.  See #Expecting_msg_map and Expecting_msg
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
   *   - We could hose the whole channel (fatal #Error_code emitted to user).
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

  /// Cumulative structured-channel stats, updated incrementally on send/receive/expect operations.
  stat::Channel_stats m_stats;

  /**
   * The handlers as pushed by handlers_post() to be flushed via handlers_poll() (internally by `*this`),
   * in rolled-back mode `S_AGGRESSIVE_HANDLER_EXECUTION == false`; unused if that constant is `true`.
   *
   * @note handlers_poll() is only called as a result of the user reporting an active event on an async-wait,
   *       meaning via the `sync_io`-pattern `(*on_active_ev_func)()` mechanism.  So anything stored in here
   *       (via handlers_post()) will only be called (and removed) as a result of that, not any actual API
   *       (with one exception; read on).  To be somewhat more specific, we'll ask to
   *       be informed when some socket/handle/thing is readable/whatever,
   *       and as a result we might receive some traffic, and as a result we might need to invoke a handler
   *       (most commonly "you wanted to know about an in-message due to this expectation/request, here it is"
   *       but also "this channel is hosed" -- and so on).  As of this writing some APIs can yield
   *       immediate results are; in all cases they report their results via out-args, not via invoking handlers:
   *       `expect_*()`, `async_end_sending()`.  Exception: start_and_poll() (one-time thing) may synchronously
   *       invoke handler(s).  Reason: this begins the read chain, which immediately reads any messages that have
   *       already arrived; by that point user may have (e.g.) set things up via expect_msgs().
   *
   * ### Rationale: Why do this? ###
   * That is, why have one part of our internal code handlers_post(), essentially, info on what to do a tiny bit
   * later and then read that info and do it?  Why not simply... do it right then?  In fact let's use a specific
   * example in this discussion from now on: `expect_msgs(X, F)` sets up the expectation of messages of type `X`,
   * instructing us to invoke `F(M)`, where `M` is the in-message of type `X`, whenever such a message arrives.
   * So, upon receiving some bytes and realizing that we now have a complete message `M` of type `X`, we could just
   * call the saved `F` on it.  (Specifically, #m_rcv_expecting_msg_map stores the `F`s; a relevant message is
   * detected in rcv_struct_new_msg_in_is_next_expected().  So why not just call `F(M)` right in there?)
   *
   * The truth is it's not a slam dunk that it's necessary.  The intuition is we're invoking user code, and to
   * keep entropy minimal that should be done *not* in the middle of our processing where we are changing `*this`
   * state; so should be ideally at the end of (whatever call they're making; namely as of this writing either
   * start_and_poll() or `(*on_active_ev_func)()`).  In reality, as of this writing anyway, they wouldn't call
   * anything dangerous that would again risk modifying *that* state... namely `(*on_active_ev_func())` again
   * (start_and_poll() is one-time), so perhaps the whole thing is wrong-headed (see to-do just below).  For now,
   * though, we go with the avoiding-entropy rationale anyway.
   *
   * @todo Possibly the `handlers_post()/handlers_poll()` algorithm (as described in
   * sync_io::Channel::m_sync_io_handlers doc header) is unnecessarily tricky, with too many words talking about it
   * in said doc header; we could simply fire any relevant handler wherever today we, instead, issue `handlers_post()`
   * thus delaying it until `handlers_poll()`.  The entropy argument intuitively makes sense, but hard logic seems
   * to negate this intuition.  Look into it. / Update: The internal constant `S_AGGRESSIVE_HANDLER_EXECUTION` being
   * `true` (as of this writing that is the default) will indeed do what we described in this to-do, more or less.
   * A "full" conversion would skip the whole Handlers boiler-plate and the `S_AGGRESSIVE_HANDLER_EXECUTION` and
   * simply *do it right then and there* instead of a `handler_post()` call.  For now we're hedging a bit, so it's
   * easy to revert if desired.  Until then the optimizer at, like, -O3 levels should generate the same machine code,
   * when `S_AGGRESSIVE_HANDLER_EXECUTION == true`.
   *
   * @note Digression: Okay, so then should we/do we do the "ideal" thing, placing all the handler execution at those
   *       two places?  Well, no; we know in practice it isn't great, as in the case where a single on-active-event-func
   *       invocation yields (e.g.) 50 in-messages, it's better that the 50 handler invocations are distributed
   *       across the processing procedure as opposed to burstily invoked at the end.  Instead we picked a few
   *       places judiciously.  More or less it's: (1) on error just do it ASAP, as error stops the read chain
   *       entirely; (2) on message receipt do it after processing a newly completed in-order in-message (or
   *       several if reassembly occurred; see below).  Now, without digressing further, the bottom line is it's
   *       not simply "right there" but somewhat delayed.  So, we need to store the info somehow, so that we can
   *       then a bit later execute each handler in the appropriate way.  Hence back to it.
   *
   * ### Rationale: Why implement it this way? ###
   * So the task is, there are a few different handler varieties we can handlers_post() to as to execute in
   * handlers_poll().  For each variety this means simply:
   *   -# Save a few bits of info at handlers_post() time.  (E.g., for the on-response handler we need the response
   *      message and the user-supplied-to-`async_request()` callback that shall be informed.)
   *   -# Retrieve those bits of info at handlers_poll() time and use them to run appropriate handler-invoking code.
   *
   * The most obvious and straightforward-to-code way of doing this is, simply, store a lambda whose body
   * is bullet point 2, and whose captures are bullet point 1.  Pack that in a `Function<void ()>`; save that
   * in a container; cool.  If we suspect the `Function<>()` itself is too fat to be stored in a maybe-realloc-ing
   * `vector` then wrap it in a `unique_ptr<>`.  Cool.
   *
   * An earlier version did just that.  However this particular area of the code -- not necessarily the
   * function-storing and -invoking but the general inner-loop part of the on-active-event-func callback -- has
   * shown up in load tests/profiling.  So we decided to hand-optimize this aspect of it (after we banked some
   * other things in that area).  Can this be improved-upon?
   *
   * The answer is yes, probably.  `Function<>` is great, but it's a general solution for a general problem, with
   * hairy STL code doing the hard work; visibility into how good that code is isn't perfect (depends on the
   * build environment at the very least).  What good impls do is quite smart, but since we have a more specific
   * situation, we can write a custom solution that is guaranteed to be essentially as quick/minimal as it an be.
   * Then we needn't rely on `Function<>` giving this to us.  (An arguably similar reasoning led to, e.g.,
   * developing `flow::util::Basic_blob` over `vector<uint8_t>`.)
   *
   * The custom solution is straightforward.  We have, as of this writing, 3-5 handler varieties, depending on
   * whether we collate several similar ones into 1 or not; each requires capturing things like 1-2 smart pointers
   * or an ID and a `string`.  The low number means `variant<>` (safe union) and `visit(variant)` (essentially a
   * `switch()` on the union-selector) will be extremely quick while requiring only limited boiler-plate (versus
   * `Function<>` approach's ~zero boiler-plate).  The small size means we can store the captures directly, sans
   * smart-pointer handle.  (If larger captures become needed, then we can easily -- for that particular variety --
   * indeed wrap in `unique_ptr`.  See Channel::Handlers doc header.)  So we relace type erasure with an explicit
   * type-safe union (`variant`) and polymorphism with a `switch()`-like polymorphism (`visit()`).
   *
   * The downside is just having the boiler-plate that are the `class`es nested inside Handlers: e.g.,
   * Handlers::Message, with its full `{ body }` and explicit ctor.  At this time it's about 120 extra lines.
   * At least it is nice and organized:
   *
   * @see Channel::Handlers; the nested `class`es therein (like Handlers::Message) enumerate the handler varieties.
   */
  std::vector<typename Handlers::Handler> m_sync_io_handlers;
}; // class Channel

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in ../structured_channel.hpp).
#define TEMPLATE_SIO_STRUCT_CHANNEL \
  template<typename Owned_channel_t, typename Msg_body_t, \
           typename Builder_config_t, typename Reader_config_t>
/// Internally used macro; public API users should disregard (same deal as in ../structured_channel.hpp).
#define CLASS_SIO_STRUCT_CHANNEL \
  Channel<Owned_channel_t, Msg_body_t, Builder_config_t, Reader_config_t>

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Owned_channel_t&& channel,
                                  const Builder_config& struct_builder_config,
                                  const typename Builder_config::Builder::Session& struct_lender_session,
                                  const Reader_config& struct_reader_config,
                                  const Session_token& session_token_non_nil) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_struct_builder_config(struct_builder_config),
  m_struct_lender_session(struct_lender_session),
  m_struct_reader_config(struct_reader_config),

  /* This all will get quite a bit more complex, especially for m_protocol_negotiator_aux,
   * once at least one relevant protocol decides to both gain a version and still support an older version,
   * For now though there's just one version for both thingies, and only that version is supported (MIN == MAX).
   * See class doc header for discussion.  Note for now the related static_assert() below. */
  m_protocol_negotiator(get_logger(), std::string{"struc-"} + channel.nickname(),
                        S_PROTOCOL_VERSION_MIN, S_PROTOCOL_VERSION_MAX),
  m_protocol_negotiator_aux(get_logger(), std::string{"struc-aux-"} + channel.nickname(),
                        S_PROTOCOL_VERSION_MIN, S_PROTOCOL_VERSION_MAX),

  m_started_ops(false),

  // Skip log-in phase.  Ready to rock immediately.
  m_session_token(session_token_non_nil),
  m_snd_msg_next_id(1),

  m_phase(Phase::S_LOGGED_IN),

  m_rcv_msg_next_id(1),

  m_channel(std::move(channel))
{
  static_assert(S_PROTOCOL_VERSION_MIN == S_PROTOCOL_VERSION_MAX,
                "Various code assumes we don't do backward-compatible protocols; if this fails please "
                  "ensure the code all-over is thoughtfully handling this topic and delete this static-assert.");

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
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Owned_channel_t&& channel,
                                  const Builder_config& struct_builder_config,
                                  const typename Builder_config::Builder::Session& struct_lender_session,
                                  const Reader_config& struct_reader_config,
                                  bool is_server) :
  flow::log::Log_context(logger_ptr, Log_component::S_TRANSPORT),
  m_struct_builder_config(struct_builder_config),
  m_struct_lender_session(struct_lender_session),
  m_struct_reader_config(struct_reader_config),

  // See comment in earlier Channel::Channel().  @todo More code reuse?
  m_protocol_negotiator(get_logger(), std::string{"struc-"} + channel.nickname(),
                        S_PROTOCOL_VERSION_MIN, S_PROTOCOL_VERSION_MAX),
  m_protocol_negotiator_aux(get_logger(), std::string{"struc-aux-"} + channel.nickname(),
                        S_PROTOCOL_VERSION_MIN, S_PROTOCOL_VERSION_MAX),

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
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                                  Serialize_via_heap,
                                  const Session_token& session_token_non_nil,
                                  size_t segment1_sz) :
  Channel(logger_ptr, std::move(channel),
          Async_io_obj::heap_fixed_builder_config(channel, segment1_sz), NULL_SESSION,
          Async_io_obj::heap_reader_config(channel),
          session_token_non_nil)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                                  Serialize_via_heap,
                                  bool is_server,
                                  size_t segment1_sz) :
  Channel(logger_ptr, std::move(channel),
          Async_io_obj::heap_fixed_builder_config(channel, segment1_sz), NULL_SESSION,
          Async_io_obj::heap_reader_config(channel),
          is_server)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Session>
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                                  Serialize_via_session_shm, Session* session,
                                  const Session_token& session_token_explicit,
                                  size_t segment1_sz) :
  Channel(logger_ptr, std::move(channel),
          session->session_shm_builder_config(segment1_sz), session->session_shm_lender_session(),
          session->session_shm_reader_config(),
          (session_token_explicit == NULL_SESSION_TOKEN)
            ? session->session_token()
            : session_token_explicit)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Session>
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                                  Serialize_via_session_shm, Session* session,
                                  bool is_server,
                                  size_t segment1_sz) :
  Channel(logger_ptr, std::move(channel),
          session->session_shm_builder_config(segment1_sz), session->session_shm_lender_session(),
          session->session_shm_reader_config(),
          is_server)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Session>
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                                  Serialize_via_app_shm, Session* session,
                                  const Session_token& session_token_explicit,
                                  size_t segment1_sz) :
  Channel(logger_ptr, std::move(channel),
          session->app_shm_builder_config(segment1_sz), session->app_shm_lender_session(),
          session->app_shm_reader_config(),
          (session_token_explicit == NULL_SESSION_TOKEN)
            ? session->session_token()
            : session_token_explicit)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Session>
CLASS_SIO_STRUCT_CHANNEL::Channel(flow::log::Logger* logger_ptr, Owned_channel&& channel,
                                  Serialize_via_app_shm, Session* session,
                                  bool is_server, size_t segment1_sz) :
  Channel(logger_ptr, std::move(channel),
          session->app_shm_builder_config(segment1_sz), session->app_shm_lender_session(),
          session->app_shm_reader_config(),
          is_server)
{
  // Delegated.
}

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::~Channel()
{
  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Shutting down.  "
                "Owned-channel and async-worker shall shut down presently.");

  /* See log_stats() doc header for basic background behind the logic here.
   * If we are hosed, hose() already logged final stats at that moment; don't repeat at dtor.
   * Otherwise (clean shutdown) log now.  We do not log Owned_channel sub-objects' stats from here:
   * they will print themselves moments later via their own dtors. */
  if (!m_channel_err_code_or_ok)
  {
    log_stats("dtor/healthy", false);
  }
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

  /* We can, and must (see "Protocol negotiation" in class doc header), now send out the outgoing protocol-negotiation;
   * plus the channel-header info. */
  send_init_msgs();
  /* That may have set truthy m_init_msg_err_code_or_ok; in which case
   * start_and_poll()/send()/async_request()/etc. will pick it up (whichever happens first). */

  return true;
} // Channel::start_ops()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::check_not_started_ops(util::String_view context) const
{
  if (!m_started_ops)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: This operation (context [" << context << "]) requires "
                     "start-*-ops which has not been invoked.  Ignoring.");
    return false; // [sic]
  }
  return true;
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Task_err>
bool CLASS_SIO_STRUCT_CHANNEL::start_and_poll(Task_err&& on_err_func)
{
  using flow::util::Blob;

  if (!check_not_started_ops("start_and_poll()"))
  {
    return false;
  }
  // else

  if (!m_on_err_func.empty())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Start requested, but we have already started.  Ignoring.");
    return false;
  }
  // else

  if (m_channel_err_code_or_ok)
  {
    // As promised do not re-emit: just follow the no-op contingency.
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Start requested, but the error "
                     "[" << m_channel_err_code_or_ok << "] [" << m_channel_err_code_or_ok.message() << "] "
                     "was previously emitted by another send()/similar.  Ignoring.");
    return false;
  } // else

  m_on_err_func = std::move(on_err_func);
  assert(!m_on_err_func.empty());

  FLOW_LOG_INFO("struc::Channel [" << *this << "]: Start requested.  Proceeding to start "
                "async-receive chain(s).  On-receive handlers may now execute.");

  // send_init_msgs() explains this.  Essentially think of it as a channel-hosing condition that was *just* discovered.
  if (m_init_msg_err_code_or_ok)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Start requested, but the error "
                     "[" << m_init_msg_err_code_or_ok << "] [" << m_init_msg_err_code_or_ok.message() << "] "
                     "was previously detected when trying to internally send protocol-negotiation + channel-header "
                     "messages over a pipe (details likely logged earlier).  Emitting channel-hosing error.");
    handle_new_error(m_init_msg_err_code_or_ok, "start_and_poll()", true);
    handlers_poll("start_and_poll()"); // !!!
    /* (On-receive code below for anything synchronously read will fire any resulting handlers.  We didn't get to
     * that phase so had to do it specially here to satisfy contract, namely the `_and_poll` part of the name.) */
    return true;
  }
  // else

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
    using Msg_in_pipe_t // Always use m_channel.async_receive_blob*(); sender sends handle => Channel fatal error.
      = Msg_in_pipe<Tribool_vals::false_value>; // The Blob_receiver_obj pipe.
    const auto sz = m_channel.receive_blob_max_size();
    m_rcv_pipes.front().reset
      (new Msg_in_pipe_t
           {
             true, // First message is lead.
             0, // Segment count unknown until lead message received.
             {}, // We .emplace() just below.
             Msg_in_impl<Msg_in>::ct_base(m_struct_reader_config), // Always start with empty message.
             // Prepare the simple one-time protocol-negotiation read/serialize op for this pipe.
             Segment_blob_in{sz}, {}
           });
    const auto pipe = static_cast<Msg_in_pipe_t*>(m_rcv_pipes.front().get());
    auto& batch = pipe->m_target_batch;
    batch.emplace(Msg_in_pipe_t::S_RCV_BATCH_SIZE);
    do { Segment_blob_in blob{sz}; batch->prepare_target_payload(blob.mutable_buffer(), std::move(blob)); }
      while (!batch->initialized());
    rcv_async_read_proto_neg_msg(pipe);

    assert(!m_rcv_pipes.back());
  }
  else if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    // As above; except Batch is Native_handle_receiver instead of Blob_receiver, and it's OK to receive hndl or not.
    using Msg_in_pipe_t = Msg_in_pipe<Tribool_vals::indeterminate_value>; // The Native_handle_receiver_obj pipe.
    const auto sz = m_channel.receive_meta_blob_max_size();
    m_rcv_pipes.front().reset
      (new Msg_in_pipe_t
           {
             true, 0, {}, Msg_in_impl<Msg_in>::ct_base(m_struct_reader_config), // As above.
             Segment_blob_in{sz}, {} // As above.
           });
    // @todo Code reuse in the following snippet?  Problem is it might be harder to understand/more lines.
    const auto pipe = static_cast<Msg_in_pipe_t*>(m_rcv_pipes.front().get());
    auto& batch = pipe->m_target_batch;
    batch.emplace(Msg_in_pipe_t::S_RCV_BATCH_SIZE);
    do { Segment_blob_in blob{sz}; batch->prepare_target_payload(blob.mutable_buffer(), std::move(blob)); }
      while (!batch->initialized());
    rcv_async_read_proto_neg_msg(pipe);

    assert(!m_rcv_pipes.back());
  }
  else // Reminder: The following is only compiled if the above two compile-time conditions are false.
  {
    static_assert(Owned_channel::S_HAS_2_PIPES, "Maintenance bug?!  Channel must have 1 blobs, 1 handles pipe/both.");

    // As above, the two combined.

    /* Always use m_channel.async_receive_native_handle*(); accept only with-handle lead messages.
     * Then m_rcv_pipes.back() below will in-parallel accept only sans-handle lead messages. */
    using Msg_in_pipe1_t = Msg_in_pipe<Tribool_vals::true_value>; // The Native_handle_receiver_obj pipe.
    using Msg_in_pipe2_t = Msg_in_pipe<Tribool_vals::false_value>; // The Blob_receiver_obj pipe.
    const auto sz1 = m_channel.receive_meta_blob_max_size();
    const auto sz2 = m_channel.receive_blob_max_size();
    m_rcv_pipes.front().reset
      (new Msg_in_pipe1_t
           {
             true, 0, {}, Msg_in_impl<Msg_in>::ct_base(m_struct_reader_config), // As above.
             Segment_blob_in{sz1}, {} // As above.
           });
    m_rcv_pipes.back().reset
      (new Msg_in_pipe2_t
           {
             true, 0, {}, Msg_in_impl<Msg_in>::ct_base(m_struct_reader_config), // As above.
             Segment_blob_in{sz2}, {} // As above.
           });
    /* Combine the two above in one loop.
     * @todo Code reuse for the following snippet?  Problem is it might be harder to understand/more lines. */
    const auto pipe1 = static_cast<Msg_in_pipe1_t*>(m_rcv_pipes.front().get());
    const auto pipe2 = static_cast<Msg_in_pipe2_t*>(m_rcv_pipes.back().get());
    auto& batch1 = pipe1->m_target_batch;
    auto& batch2 = pipe2->m_target_batch;
    batch1.emplace(Msg_in_pipe1_t::S_RCV_BATCH_SIZE);
    batch2.emplace(Msg_in_pipe2_t::S_RCV_BATCH_SIZE);
    do { Segment_blob_in blob{sz1}; batch1->prepare_target_payload(blob.mutable_buffer(), std::move(blob)); }
      while (!batch1->initialized());
    do { Segment_blob_in blob{sz2}; batch2->prepare_target_payload(blob.mutable_buffer(), std::move(blob)); }
      while (!batch2->initialized());

    rcv_async_read_proto_neg_msg(pipe1);
    if (!m_channel_err_code_or_ok) // ^-- That could've hosed the Channel; e.g. a transport error immediately seen.
    {
      rcv_async_read_proto_neg_msg(pipe2);
    }
  } // else if (Owned_channel::S_HAS_2_PIPES)

  return true;
} // Channel::start_and_poll()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_async_read(Msg_in_pipe_t* pipe)
{
  if constexpr(Msg_in_pipe_t::S_RCV_BATCH_SIZE == 1)
  {
    rcv_async_read_unbatched(pipe);
  }
  else
  {
    rcv_async_read_batched(pipe);
  }
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_async_read_batched(Msg_in_pipe_t* pipe, bool pessimistic)
{
  /* The pre-condition is, we are either starting from (basically) start_and_poll(); or we have just read and fully
   * processed a batch (of 1+ unstructured in-messages); and now it is time to read the next.
   * (If `pessimistic`, then that in-batch was !full(), meaning the pipe is now in would-block, so we simply
   * tell the unstructured Owned_channel async-read-batch API that fact, so it can for perf skip an actual
   * read attempt and just async-wait right away.  Otherwise it may or not be in would-block, so we tell it *that*,
   * and it'll try to nb-read first.)
   *
   * The algorithm below is the usual sync_io-pattern thing, where the goal is to read as much in-data as possible
   * until would-block (or error), processing the data fully (which also could lead to error).  If no error before we
   * hit the would-block, then continue the read-chain by doing an async-wait.  In other words it's basically (ignoring
   * possibility of error and `pessimistic` details):
   *   do
   *   {
   *     // Either this outputs immediately into *batch, or async-waits, invoking F() eventually once in-batch received.
   *     async_read(batch, F);
   *     if (!<it got would-block>) { process the in-batch in *batch; }
   *   }
   *   while (!<it got would-block>)
   *   void F()
   *   {
   *     process the in-batch in *batch;
   *     rcv_async_read_batched(); // Continue read-chain by calling *us*.
   *   }
   * That's what the below does.  It's only somewhat different-looking, because (1) it deals with possibility of
   * channel-hosing error at various places; and (2) the batch->full() checking is in helper
   * rcv_on_async_read_batch(), which is where <process in-batch in *batch> code is.
   *
   * As long as you grok the above algorithm, it should all be reasonably clear from there. */

  constexpr auto LEAD_HNDL_EXPECTATION = Msg_in_pipe_t::S_LEAD_HNDL_EXPECTATION;

  auto& batch = *pipe->m_target_batch;
  bool would_block = false;
  do // while (!would_block && !m_channel_err_code_or_ok)
  {
    // This is invoked asynchronously, if the below async_read_*() yields would-block.  Else it is ignored.
    auto on_recv_func = [this, pipe](const Error_code& err_code) mutable
    {
      // Got 1+ messages or error asynchronously.

      const bool pessimistic = !rcv_on_async_read_batch(pipe, err_code);
      if (m_channel_err_code_or_ok)
      {
        return; // Just end read chain, as channel is hosed.
      }
      // else

      // Slide right back to the start of the algorithm; iff [sic] pipe in would-block then `pessimistic` is true.
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: The above on-received-message-batch logic was invoked "
                     "asynchronously.  Continue read-chain; `pessimistic`? = [" << pessimistic << "]; "
                     "if true batch not totally-full => pipe in would-block => skip nb-read; "
                     "else pipe may or may not be in would-block => do nb-read first.");
      rcv_async_read_batched(pipe, pessimistic);
    }; // auto on_recv_func =

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-batch-read starting; message 1 of next in-batch shall be "
                   "[" << (pipe->m_next_msg_lead_else_cont ? "lead" : "continuation") << "]; "
                   "in-pipe lead-message handle-bearing mode is "
                   "[" << int(LEAD_HNDL_EXPECTATION) << "].  "
                   "Pessimistic mode (pre-assumed would-block)? = [" << pessimistic << "].");

    assert((batch.n_used() == 0) && "*this Channel should've processed all received messages, creating clean slate.");

    Error_code sync_err_code;

    if constexpr(LEAD_HNDL_EXPECTATION == Tribool_vals::false_value)
    {
      m_channel.template async_receive_blob_batch<Segment_blob_in>(&batch, pessimistic, &sync_err_code,
                                                                   std::move(on_recv_func));
    }
    else
    {
      m_channel.template async_receive_native_handle_batch<Segment_blob_in>(&batch, pessimistic, &sync_err_code,
                                                                            std::move(on_recv_func));
    }
    assert(((!pessimistic) || sync_err_code)
           && "`pessimistic` => async-read must yield would-block (at best) or other error (otherwise).");

    if (sync_err_code == transport::error::Code::S_SYNC_IO_WOULD_BLOCK)
    {
      would_block = true;
      continue; // Loop will end.  Live to fight another day: async-wait outstanding.
    }
    // else: Got 1+ messages or error synchronously.  Handle it right here (a-la on_recv_func() which won't run).

    pessimistic = !rcv_on_async_read_batch(pipe, sync_err_code);

    if (!m_channel_err_code_or_ok)
    {
      continue; // Loop will end.  As in on_recv_func(): Just end read chain, as channel is hosed.
    }
    // else

    /* As in on_recv_func(): iff [sic] pipe in would-block then `pessimistic` is true.
     * Unlike there, we just continue our loop, having updated `pessimistic` to true.
     * (If `pessimistic` was already true, then we can't be here, as that would've caused would-block above.) */

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: The above on-received-message-batch logic was invoked "
                   "synchronously (message-batch was immediately pending).  "
                   "Continue read-chain; `pessimistic`? = [" << pessimistic << "]; "
                   "if true batch not totally-full => pipe in would-block => skip nb-read; "
                   "else pipe may or may not be in would-block => do nb-read first.");
  }
  while ((!would_block) && (!m_channel_err_code_or_ok));
} // Channel::rcv_async_read_batched()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_async_read_unbatched(Msg_in_pipe_t* pipe)
{
  /* We are the exact equivalent of rcv_async_read_batched() but executed only if, basically, batching is disabled;
   * meaning we are only allowed to execute Owned_channel (Blob_receiver + Native_handle_receiver concepts)
   * single-message-read APIs rather than the batchy ones: .async_receive_{blob|native_handle}() rather than
   * <same>_batch().  (The batch API is always available, and even if we have RCV_BATCH_SIZE=1 and therefore
   * use that API with 1-batches only, it'll work perfectly well; and that layer has its own measures it takes
   * to avoid batch-related overhead when the batches are "batches" (contain up to only 1 message).  So why does
   * this code-path of ours -- which avoids the batch API of Owned_channel entirely -- still exist?  To repeat
   * the same explanation elsewhere for your convenience; 1, it was available; and 2, it *is* simpler and might
   * avoid certain bits of overhead.  It can also be thought of as a way to roll-back more fully to the
   * state of the code before the batch-receiving feature existed.  @todo With time we might get rid of it.
   * After all Blob_receiver/Native_handle_receiver impls are required to use batch emulation if no native
   * OS batch capabilities exist... so our having our own form thereof might be overkill at this point.  Again, though,
   * it was available for use, so for now we have kept it.)
   *
   * We will keep ~full inline comments despite the similarity to rcv_async_read_batched().  It'll help us
   * sanity-check both algorithms; plus obviously it'll make the code easier to understand if somewhat lengthier.
   *
   * So: The pre-condition is, we are either starting from (basically) start_and_poll(); or we have just read and fully
   * processed 1 unstructured in-message; and now it is time to read the next.  (Since we received, last time,
   * by definition of single-message async-receive, exactly 1 in-message -- equivalent to a 1-batch -- the "batch"
   * was indeed "full"; hence what would have been the `pessimistic` arg for us would have always been `false.
   * So we lack that arg, and the code below reduces to the !pessimistic-branch equivalent of what occurs
   * in _batched().
   *
   * The algorithm below is the usual sync_io-pattern thing, where the goal is to read as much in-data as possible
   * until would-block (or error), processing the data fully (which also could lead to error).  If no error before we
   * hit the would-block, then continue the read-chain by doing an async-wait.  In other words it's basically (ignoring
   * possibility of error):
   *   do
   *   {
   *     // Either this outputs immediately into *batch, or async-waits, invoking F() eventually once in-batch received.
   *     // target_slot is slot[0] of 1-batch in *pipe.  target_slot comprises simply a Blob [and a Native_handle].
   *     async_read(target_slot, F);
   *     if (!<it got would-block>) { process the in-message in target_slot; }
   *   }
   *   while (!<it got would-block>)
   *   void F()
   *   {
   *     process the in-message in target_slot;
   *     rcv_async_read_unbatched(); // Continue read-chain by calling *us*.
   *   }
   * That's what the below does.  It's only somewhat different-looking, because it deals with possibility of
   * channel-hosing error at various places. (The equivalent of batch->full() is always true for us, so
   * rcv_on_async_read_one() -- our version of rcv_on_async_read_batch() -- is void instead of returning bool;
   * and we act as-if it returned true in the _batched() equivalent of us.)
   *
   * As long as you grok the above algorithm, it should all be reasonably clear from there. */

  constexpr auto LEAD_HNDL_EXPECTATION = Msg_in_pipe_t::S_LEAD_HNDL_EXPECTATION;

  auto& batch = *pipe->m_target_batch;
  assert((batch.n_used() == 0) && "*this Channel shouldn't use batch facilities except as a simple target store.");

  [[maybe_unused]] Native_handle* target_hndl;
  if constexpr(LEAD_HNDL_EXPECTATION != Tribool_vals::false_value)
  {
    target_hndl = pipe->m_target_batch->next_target_hndl(); // Always the same.
  }

  bool would_block = false;
  do // while (!would_block && !m_channel_err_code_or_ok)
  {
    // This is invoked asynchronously, if the below async_read_*() yields would-block.  Else it is ignored.
    auto on_recv_func = [this, pipe](const Error_code& err_code, size_t sz) mutable
    {
      // Got 1 message or error asynchronously.

      rcv_on_async_read_one(pipe, err_code, sz);
      if (!m_channel_err_code_or_ok)
      {
        // Slide right back to the start of the algorithm.
        FLOW_LOG_TRACE("struc::Channel [" << *this << "]: The above on-received-message logic was invoked "
                       "asynchronously.  Continue read-chain.");
        rcv_async_read_unbatched(pipe);
      }
      // else { Just end read chain, as channel is hosed. }
    }; // auto on_recv_func =

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-read starting; next message shall be "
                   "[" << (pipe->m_next_msg_lead_else_cont ? "lead" : "continuation") << "]; "
                   "in-pipe lead-message handle-bearing mode is "
                   "[" << int(LEAD_HNDL_EXPECTATION) << "].");

    Error_code sync_err_code;
    size_t sync_sz;

    if constexpr(LEAD_HNDL_EXPECTATION == Tribool_vals::false_value)
    {
      m_channel.async_receive_blob(batch.next_target_blob(), &sync_err_code, &sync_sz,
                                   std::move(on_recv_func));
    }
    else
    {
      m_channel.async_receive_native_handle(target_hndl, batch.next_target_blob(), &sync_err_code, &sync_sz,
                                            std::move(on_recv_func));
    }

    if (sync_err_code == transport::error::Code::S_SYNC_IO_WOULD_BLOCK)
    {
      would_block = true;
      continue; // Loop will end.  Live to fight another day: async-wait outstanding.
    }
    // else: Got 1 message or error synchronously.  Handle it right here (a-la on_recv_func() which won't run).

    rcv_on_async_read_one(pipe, sync_err_code, sync_sz);

    if (m_channel_err_code_or_ok)
    {
      continue; // Loop will end.  As in on_recv_func(): Just end read chain, as channel is hosed.
    }
    // else

    /* As in on_recv_func() log a thing but unlike there, we just continue our loop to enter another async-read.
     * That is we enter another async-read, same as there, just no call required to do it. */

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: The above on-received-message logic was invoked "
                   "synchronously (message was immediately pending).  Continue read-chain.");
  }
  while ((!would_block) && (!m_channel_err_code_or_ok));
} // Channel::rcv_async_read_unbatched()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
bool CLASS_SIO_STRUCT_CHANNEL::rcv_on_async_read_batch(Msg_in_pipe_t* pipe, const Error_code& err_code)
{
  constexpr auto LEAD_HNDL_EXPECTATION = Msg_in_pipe_t::S_LEAD_HNDL_EXPECTATION;

  /* Per our doc header: Our job is to fully read to one of the following having just occurred as a result of
   * an async-read-batch call, synchronously or otherwise:
   *   - got an in-batch (1+ unstructured in-messages); or
   *   - got a hosing error (not would-block).
   *
   * We are to fully handle that but *not* perpetuating the read-chain in the former case, as that's the caller's
   * responsibility.  Also, in this and all our helpers, the only way we communicate to the caller that the
   * read chain must end is by setting m_channel_err_code_or_ok to truthy. */

  assert(err_code != transport::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER); // sync_io must not do this.
  assert(err_code != boost::asio::error::operation_aborted); // Or definitely this.

  if (!handle_async_err_code(err_code, "rcv_on_async_read_batch()"))
  {
    // If that detected a *new* error, it posted a handler.
    handlers_poll("rcv_on_async_read_batch(1)"); // !!!

    // It's over -- the entire read chain must stop -- either because of us or an earlier pipe hosing.
    return false; // As advertised we return false in this case, but really they shouldn't care what we return then.
  }
  // else if (all good (including !err_code)):

  auto& batch = *pipe->m_target_batch;

  const auto n_rcvd = batch.n_used();

  assert((n_rcvd != 0) && "async_receive_..._batch() must yield either an error or 1+ in-messages.");
  assert((!m_channel_err_code_or_ok) && "We should have returned above already.");

  for (size_t idx = 0; (idx != n_rcvd) && (!m_channel_err_code_or_ok); // @todo Always false first time.  do/while?
       ++idx)
  {
    Segment_blob_in* blob_in_batch;
    const auto sz = batch.result_payload_blob(idx, &blob_in_batch);

    const auto max_sz = blob_in_batch->size();
    assert(max_sz == blob_in_batch->capacity());

    Native_handle hndl;
    if constexpr(LEAD_HNDL_EXPECTATION != Tribool_vals::false_value)
    {
      hndl = batch.result_payload_hndl(idx); // (This method does not exist if `LEAD_HNDL_EXPECTATION == false_value`.)
    }
    // else { We used .async_receive_blob_batch(), meaning handles are not accepted at the lower layer. }

    pipe->m_next_msg_lead_else_cont
      ? rcv_on_async_read_lead_msg(pipe, blob_in_batch, sz, hndl)
      : rcv_on_async_read_continuation_msg(pipe, blob_in_batch, sz, hndl);

    // Run the handler(s) (if any) accumulated by that call.  (In normal operation this is the main place it happens.)
    handlers_poll("rcv_on_async_read_batch(2)"); // !!!

    if (m_channel_err_code_or_ok)
    {
      continue; // Channel-hosing error occurred; stop entire read chain; so no need to even reset this `batch` slot.
    }
    // else: Reset this `batch` slot for the next async-receive.  So do just like in start_and_poll().

    Segment_blob_in new_blob{max_sz};
    batch.prepare_target_payload(new_blob.mutable_buffer(), std::move(new_blob), idx);
  } // for (idx in [0, n_rcvd) && <channel not hosed>)

  if (m_channel_err_code_or_ok)
  {
    return false; // As advertised we return false in this case, but really they shouldn't care what we return then.
  }
  // else

  /* It's possible (actually fairly probable) there is
   * implied would-block, meaning that we got some messages but not enough to fill all available slots; this
   * implies no more messages are available right now, so indeed we should return true in that case.
   * Otherwise it's indeterminate: could be a coincidence we got exactly enough to fill slots; or could be
   * because there's more to read. */
  const bool would_block_indeterminate = batch.full();

  // Lastly: we've reset all the received-into slots to accept more in-messages; so reset ->n_used() == 0.
  batch.clear_used();
  // Now `batch` is a clean slate again as needed for the next read (whether it's now or later).

  return would_block_indeterminate;
} // Channel::rcv_on_async_read_batch()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_on_async_read_one(Msg_in_pipe_t* pipe, const Error_code& err_code, size_t sz)
{
  /* We are the exact equivalent of rcv_on_async_read_batch() but executed only if, basically, batching is disabled;
   * meaning we are only allowed to execute Owned_channel (Blob_receiver + Native_handle_receiver concepts)
   * single-message-read APIs rather than the batchy ones: .async_receive_{blob|native_handle}() rather than
   * <same>_batch().
   *
   * We will keep ~full inline comments despite the similarity to rcv_async_read_batched().  It'll help us
   * sanity-check both algorithms; plus obviously it'll make the code easier to understand if somewhat lengthier.
   * ...But really, though, it's the same thing -- just instead of a loop, it handles slot 0 of the "batch"
   * which is where we keep the target blob [and target Native_handle].
   *
   * Per our doc header: Our job is to fully read to one of the following having just occurred as a result of
   * an async-read-message call, synchronously or otherwise:
   *   - got an in-message; or
   *   - got a hosing error (not would-block).
   *
   * We are to fully handle that but *not* perpetuating the read-chain in the former case, as that's the caller's
   * responsibility.  Also, in this and all our helpers, the only way we communicate to the caller that the
   * read chain must end is by setting m_channel_err_code_or_ok to truthy. */

  constexpr auto LEAD_HNDL_EXPECTATION = Msg_in_pipe_t::S_LEAD_HNDL_EXPECTATION;

  assert(err_code != transport::error::Code::S_OBJECT_SHUTDOWN_ABORTED_COMPLETION_HANDLER); // sync_io must not do this.
  assert(err_code != boost::asio::error::operation_aborted); // Or definitely this.

  if (!handle_async_err_code(err_code, "rcv_on_async_read_one()"))
  {
    // If that detected a *new* error, it posted a handler.
    handlers_poll("rcv_on_async_read_one(1)"); // !!!
    return; // It's over -- the entire read chain must stop -- either because of us or an earlier pipe hosing.
  }
  // else if (all good (including !err_code)):

  auto& batch = *pipe->m_target_batch;

  Segment_blob_in* blob_in_batch;
  batch.result_payload_blob(0, &blob_in_batch);

  const auto max_sz = blob_in_batch->size();
  assert(max_sz == blob_in_batch->capacity());

  Native_handle hndl;
  if constexpr(LEAD_HNDL_EXPECTATION != Tribool_vals::false_value)
  {
    hndl = batch.result_payload_hndl(0); // (This method does not exist if `LEAD_HNDL_EXPECTATION == false_value`.)
  }
  // else { We used .async_receive_blob(), meaning handles are not accepted at the lower layer. }

  pipe->m_next_msg_lead_else_cont
    ? rcv_on_async_read_lead_msg(pipe, blob_in_batch, sz, hndl)
    : rcv_on_async_read_continuation_msg(pipe, blob_in_batch, sz, hndl);

  // Run the handler(s) (if any) accumulated by that call.  (In normal operation this is the main place it happens.)
  handlers_poll("rcv_on_async_read_one(2)"); // !!!

  if (!m_channel_err_code_or_ok)
  {
    // Reset this `batch` slot 0 for the next async-receive.  So do just like in start_and_poll().
    Segment_blob_in new_blob{max_sz};
    const auto new_blob_buf = new_blob.mutable_buffer();
    batch.prepare_target_payload(new_blob_buf, std::move(new_blob), 0);

    /* Now `batch` (really its first and only slot) is a clean slate again as needed for the next read (whether it's
     * now or later). */
  }
  // else { Channel-hosing error occurred; stop entire read chain.  No need to even reset `batch` slot 0. }
} // Channel::rcv_on_async_read_one()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_on_async_read_lead_msg(Msg_in_pipe_t* pipe,
                                                          Segment_blob_in* target_blob_ptr, size_t sz,
                                                          Native_handle target_hndl)
{
  constexpr auto LEAD_HNDL_EXPECTATION = Msg_in_pipe_t::S_LEAD_HNDL_EXPECTATION;

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Handler for would-be lead unstructured "
                 "in-message invoked; size received = [" << sz << "]; native handle if any = [" << target_hndl << "].");

  if constexpr(LEAD_HNDL_EXPECTATION == Tribool_vals::true_value)
  {
    if (target_hndl.null())
    {
      /* This is possible but only due to remote-peer misbehavior (sending us weird stuff/over wrong channel).
       * So it's not an assert() situation firstly.  What it's similar-ish to is when, say, a lower-level
       * Native_socket_stream (not that it has to be in Owned_channel -- just using it as an example/foil)
       * was invoked with async_receive_blob() but internally detected a Native_handle arrived too.
       * It's a protocol misbehavior on the part of the user of the low-level/unstructured Channel.
       * In our case it is also that (we are the user of said Channel); it's just that Channel cannot detect that
       * for us by itself (it simply lacks the API for "require Native_handle!").  If it had that capability,
       * it would've emitted the err_code stating that (as it does in the analogous scenario:
       * error::Code::S_BLOB_RECEIVER_GOT_NON_BLOB).  So: we'll do just that ourselves. */
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Handler for would-be lead unstructured "
                       "in-message invoked sans low-level error; but received sans-handle low-level message "
                       "along the handles-only pipe (of 2 pipes).  Other side misbehaved?");
      handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_HNDL,
                       "rcv_on_async_read_lead_msg(1)");
      return;
    }
  }
  else if constexpr(LEAD_HNDL_EXPECTATION == Tribool_vals::false_value)
  {
    assert(target_hndl.null()
           && "Bug?  Somehow we have truthy target_hndl yet should've invoked async_receive_blob() "
                "which can't do that.");
  }
  // else if (LEAD_HNDL_EXPECTATION == indeterminate_value) { All good; target_hndl can be truthy or falsy. }

  /* Another corner case is a message with an empty blob.  At the transport::Channel (unstructured, low) level it's
   * allowed.  We (struc::Channel) never use this ability however.  It would be a protocol error on the remote peer's
   * part. */
  if (sz == 0)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Handler for would-be lead unstructured "
                     "in-message invoked sans low-level error; but got no (meta-)blob at all.  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_BLOB,
                     "rcv_on_async_read_lead_msg(2)");
    return;
  }
  // else

  auto& target_blob = *target_blob_ptr;

  // Ignore garbage after the received bytes (this is permanent).
  target_blob.resize(sz);

  // OK, target_blob inside *pipe is finalized; feed it into m_incomplete_msg, so that that part is also finalized.
  Msg_in_impl<Msg_in> incomplete_msg{ *pipe->m_incomplete_msg }; // Gain access to back-end API.
  incomplete_msg.add_serialization_segment(std::move(target_blob));
  // target_blob is now nullified, and *pipe->m_incomplete_msg owns what target_blob had just before.

  // Finalize the Native_handle (if any) in m_incomplete_msg.
  incomplete_msg.store_native_handle_or_null(std::move(target_hndl));

  // Because segment 1 inside m_incomplete_msg is now finalized, we can now access:
  {
    Error_code err_code;

    /* Note this will update stats .m_rcv.m_msg by contract, but only if the mdt indicates it is an internal body.
     * Otherwise .deserialize_body() will do it (that could happen just below in rcv_struct_new_msg_in(); or
     * in that same method but after 1+ more blobs arrive). */
    pipe->m_n_segs_left_pre_last_read
      = incomplete_msg.deserialize_mdt(get_logger(), &err_code, &m_stats.m_rcv.m_msg);
    if (err_code) // It logged on error.
    {
      handle_new_error(err_code, "rcv_on_async_read_lead_msg(3)");
      return;
    }
    // else

    // (W/r/t to the pretty-print -- see any comments near the somewhat-mirrored call in send_*().  May apply here.)
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Deserialized (zero-copy) new structured in-message: "
                   "mandatory metadata portion; further segment count (including one, if any, in this msg): "
                   "[" << pipe->m_n_segs_left_pre_last_read << "]; "
                   "here is the metadata header: "
                   "\n" << ostreamable_capnp_full(incomplete_msg.mdt_root()));
  }

  // Now to finalize dealing with the in-message.

  if (pipe->m_n_segs_left_pre_last_read <= 1)
  {
    /* Sweet: Lead unstructured message encoded entire structured message.
     * (Context: Well, that's true, but if n_segs_left_including_this_read is 0, then it is an internal message;
     * if it is 1, then it is a user message that only required 1 capnp segment to be fully serialized -- it is shared
     * between the mdt header parsed by .deserialize_mdt() above and user message seg1.  In fact for efficiency we
     * would hope that most/all user messages are like that, not requiring continuation messages/more OS-read calls.
     * rcv_struct_new_msg_in() will figure out which one is the case efficiently, so we don't need to tell it via
     * arg.) */

    rcv_struct_new_msg_in(std::move(pipe->m_incomplete_msg));
    if (m_channel_err_code_or_ok)
    {
      return; // Channel-hosing deserialization error (it logged).
    }
    // else

    // So get the next one (async)!  Re-init these similarly to start_and_poll().
    pipe->m_incomplete_msg = Msg_in_impl<Msg_in>::ct_base(m_struct_reader_config);
    pipe->m_n_segs_left_pre_last_read = 0; // 1 -> 0 (or 0 -> 0, no-op).
    // Note: Resetting, like, target_blob and pipe->m_target_batch of which it is part = not our responsibility.

    // And now indeed recommend reading the next lead message.
    assert(pipe->m_next_msg_lead_else_cont && "We are lead message... so this should not need to change.");
  } // if (pipe->m_n_segs_left_pre_last_read <= 1)
  else // if (pipe->m_n_segs_left_pre_last_read > 1)
  {
    // Need more segments to come in.  Keep m_incomplete_msg, as it's still incomplete.  Get next blob to async-read-to.
    --pipe->m_n_segs_left_pre_last_read;

    // And now recommend reading the 1st continuation message.
    assert(pipe->m_next_msg_lead_else_cont);
    pipe->m_next_msg_lead_else_cont = false;
  }
} // Channel::rcv_on_async_read_lead_msg()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_on_async_read_continuation_msg(Msg_in_pipe_t* pipe,
                                                                  Segment_blob_in* target_blob_ptr, size_t sz,
                                                                  Native_handle target_hndl)
{
  constexpr auto LEAD_HNDL_EXPECTATION = Msg_in_pipe_t::S_LEAD_HNDL_EXPECTATION;

  // In its initial basics this method is much like rcv_on_async_read_lead_msg().  Consider reading that one first.

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Handler for would-be continuation unstructured "
                 "in-message invoked; size received = [" << sz << "]; native handle if any = [" << target_hndl << "].");

  if constexpr(LEAD_HNDL_EXPECTATION == Tribool_vals::false_value)
  {
    // Same check as in other branch, but along this no-handles channel it implies our bug as opposed to *their* bug.
    assert(target_hndl.null()
           && "Bug?  Somehow we have truthy target_hndl yet should've invoked async_receive_blob() "
                "which can't do that.");
  }
  else // if constexpr(LEAD_HNDL_EXPECTATION == true_value or indeterminate_value)
  {
    if (!target_hndl.null())
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Handler for would-be continuation "
                       "unstructured in-message invoked sans low-level error; but it contains native handle; "
                       "this is not allowed for continuation messages by definition.  Other side misbehaved?");
      handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_HNDL,
                       "rcv_on_async_read_continuation_msg(1)");
      return;
    }
    // else if (target_hndl.null()) { All good; continuation messages never have handles. }
  }

  // Continuation message is just the blob (we've now ensured this), and all of it is simply the next segment/chunk.

  if (sz == 0)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Handler for would-be continuation "
                     "unstructured in-message invoked sans low-level error; but got no (meta-)blob at all.  "
                     "Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_BLOB,
                     "rcv_on_async_read_continuation_msg(2)");
    return;
  }
  // else

  auto& target_blob = *target_blob_ptr;

  // Ignore garbage after the received bytes (this is permanent).
  target_blob.resize(sz);

  // OK, target_blob inside *pipe is finalized; feed it into m_incomplete_msg, so that that part is also finalized.
  Msg_in_impl<Msg_in> incomplete_msg{ *pipe->m_incomplete_msg }; // Gain access to back-end API.
  incomplete_msg.add_serialization_segment(std::move(target_blob));
  // target_blob is now nullified, and *pipe->m_incomplete_msg owns what target_blob had just before.

  // Now to finalize dealing with the in-message.
  if (pipe->m_n_segs_left_pre_last_read == 1)
  {
    // Sweet: This is the last expected continuation message.
    rcv_struct_new_msg_in(std::move(pipe->m_incomplete_msg));
    if (m_channel_err_code_or_ok)
    {
      return; // Channel-hosing deserialization error (it logged).
    }
    // else

    // So recommeng getting the next structured in-message!  Re-init these similarly to start_and_poll().
    pipe->m_incomplete_msg = Msg_in_impl<Msg_in>::ct_base(m_struct_reader_config);
    pipe->m_n_segs_left_pre_last_read = 0; // 1 -> 0.
    // Note: Resetting, like, target_blob and pipe->m_target_batch of which it is part = not our responsibility.

    assert(!pipe->m_next_msg_lead_else_cont);
    pipe->m_next_msg_lead_else_cont = true;
  } // if (pipe->m_n_segs_left_pre_last_read == 1)
  else // if (pipe->m_n_segs_left_pre_last_read > 1)
  {
    // Need more segments to come in.  Keep m_incomplete_msg, as it's still incomplete.
    --pipe->m_n_segs_left_pre_last_read;

    assert(!pipe->m_next_msg_lead_else_cont);
  }
} // Channel::rcv_on_async_read_continuation_msg()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_async_read_proto_neg_msg(Msg_in_pipe_t* pipe)
{
  constexpr auto LEAD_HNDL_EXPECTATION = Msg_in_pipe_t::S_LEAD_HNDL_EXPECTATION;

  /* The code here is reasonably easy to follow.  We want to read exactly 1 protocol-negotiation message
   * (see "Protocol negotiation" in class doc header), verify it, and then rcv_async_read(pipe)
   * which does the real work along this pipe, async-reading the first real message (etc.).  If this fails
   * (a read fails, or protocol negotiation fails), then stop the read chain and report error via handlers_poll().
   *
   * That said, it may be interesting to note that this is a much simpler/cut-down version of
   * rcv_async_read_unbatched(); the main source of simplicity is we want up-to-1-message-or-error;
   * whereas they have an endless loop (conceptually speaking).  Nevertheless, modulo those differences, this code
   * is based on that code. */

  Error_code sync_err_code;
  size_t sync_sz;

  // What happens if (and only if) our synchronous/non-blocking async_receive_*() yields message/error, not would-block.
  auto on_recv_func = [this, pipe](const Error_code& err_code, size_t sz) mutable
  {
    rcv_on_async_read_proto_neg_msg(pipe, err_code, sz);
  };

  // With that in mind, here in sync-land: Read unstructured message.

  if constexpr(LEAD_HNDL_EXPECTATION == Tribool_vals::false_value)
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-read starting for protocol-negotiation message "
                   "along blobs-only pipe.");
    m_channel.async_receive_blob(pipe->m_init_msg_blob.mutable_buffer(), &sync_err_code, &sync_sz,
                                 std::move(on_recv_func));
  }
  else
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-read starting for protocol-negotiation message "
                   "along blobs-and-handles pipe.");
    m_channel.async_receive_native_handle(&pipe->m_init_msg_hndl, pipe->m_init_msg_blob.mutable_buffer(),
                                          &sync_err_code, &sync_sz,
                                          std::move(on_recv_func));
  }

  if (sync_err_code == transport::error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    return; // Live to fight another day: async-wait outstanding.
  }
  // else: Got a message or error synchronously.  Handle it right here (a-la on_recv_func() which won't run).

  rcv_on_async_read_proto_neg_msg(pipe, sync_err_code, sync_sz);
} // Channel::rcv_async_read_proto_neg_msg()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_on_async_read_proto_neg_msg(Msg_in_pipe_t* pipe, Error_code err_code, size_t sz)
{
  using flow::util::Blob;
  using std::exception;
  auto& blob = pipe->m_init_msg_blob;

  if (!handle_async_err_code(err_code, "rcv_on_async_read_proto_neg_msg()"))
  {
    handlers_poll("rcv_on_async_read_proto_neg_msg(1)"); // !!!
    blob.make_zero(); // Might as well free the memory.

    return; // It's over, either because of us or an earlier pipe hosing.
  }
  // else if (all good (including !err_code)):

  if (m_protocol_negotiator.negotiated_proto_ver() != Protocol_negotiator::S_VER_UNKNOWN)
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-receive handler for would-be protocol-negotiation "
                   "in-message invoked; size received = [" << sz << "].  However, the "
                   "negotiation already succeeded along the other pipe; assuming "
                   "this is the same information; ignoring; will continue read chain "
                   "(grab channel-header info, then do real work along this pipe).");
    // (Do not `blob.make_zero();`!  The following call will reuse `blob`... same as the other `if` path below.)
    rcv_async_read_channel_header(pipe);
    return;
  }
  // else

  Protocol_negotiator::proto_ver_t proto_ver = Protocol_negotiator::S_VER_UNKNOWN;
  Protocol_negotiator::proto_ver_t proto_ver_aux = Protocol_negotiator::S_VER_UNKNOWN;

  if (!pipe->m_init_msg_hndl.null())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Async-receive handler for would-be protocol-negotiation "
                     "in-message invoked; size received = [" << sz << "].  The message contains a native handle; "
                     "this is wrong in this context -- bug somewhere, possibly in opposing process?  "
                     "Protocol negotiation will fail.");
  }
  else
  {
    // Interpret the received protocol-negotiation in-message.

    /* Ignore garbage after the received bytes (this is permanent).  No need to `blob.resize(sz)`; capnp will
     * just ignore the garbage trailing area.  This way rcv_async_read_channel_header() can just reuse `blob`
     * without having to futz with .resize()ing it yet again. */

    try
    {
      Capped_sz_capnp_message_reader proto_neg_reader{blob.const_buffer()};
      const auto root = proto_neg_reader.getRoot<schema::detail::ProtocolNegotiation>(); // Can throw.
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Would-be protocol-negotiation in-message contents:"
                     "\n" << ostreamable_capnp_full(root));

      proto_ver = root.getMaxProtoVer();
      proto_ver_aux = root.getMaxProtoVerAux();
    }
    catch (const exception& exc)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Async-receive handler for would-be protocol-negotiation "
                       "in-message invoked; size received = [" << sz << "].  Could not capnp-deserialize the "
                       "contents; protocol negotiation will fail.  Exception = [" << exc.what() << "].");
      assert(proto_ver == Protocol_negotiator::S_VER_UNKNOWN);
    }
  }

  /* Now to negotiate (or intentionally fail, if that weird WARNING-inducing thing above happened).
   * Protocol_negotiator handles everything (invalid value, incompatible range...). */
#ifndef NDEBUG
  bool ok =
#endif
  m_protocol_negotiator.compute_negotiated_proto_ver(proto_ver, &err_code);
  assert(ok && "Protocol_negotiator breaking contract?  Bug?");
  if (!err_code)
  {
#ifndef NDEBUG
    ok =
#endif
    m_protocol_negotiator_aux.compute_negotiated_proto_ver(proto_ver_aux, &err_code);
    assert(ok && "Protocol_negotiator breaking contract?  Bug?");
  }

  if (err_code)
  {
    handle_new_error(err_code, "rcv_on_async_read_proto_neg_msg()");
    handlers_poll("rcv_on_async_read_proto_neg_msg(2)"); // !!!
    blob.make_zero(); // As above.
    return; // It's over.
  }
  // else

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-receive handler for would-be protocol-negotiation "
                 "in-message invoked; size received = [" << sz << "].  Negotiation passed.  "
                 "Will continue read chain (grab channel-header info, then do real work along this pipe).");
  rcv_async_read_channel_header(pipe);
} // Channel::rcv_on_async_read_proto_neg_msg()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_async_read_channel_header(Msg_in_pipe_t* pipe)
{
  constexpr auto LEAD_HNDL_EXPECTATION = Msg_in_pipe_t::S_LEAD_HNDL_EXPECTATION;

  /* Negotiation passed; but we can't quite begin normal operation yet; must grab the opposing peer's
   * channel-header info.  The code is very similar: read a single message expected to be a capnp-struct X,
   * where X is ChannelHeader this time.  Though, as of this writing there's nothing to verify in it; it's just info
   * like the peer's process ID; so in that sense it is simpler.  Anyway keeping comments light this time.
   * @todo Can we make this more compact via code reuse versus rcv_[on_]async_read_proto_neg_msg()?
   *
   * Really we would've placed the ProtocolNegotiation item as field @0 of ChannelHeader (and verified it before
   * touching anything past that); it's just that protocol v1 lacked any ChannelHeader, so now it's too late to
   * do it compatibly (probably... if we are reading https://capnproto.org/language.html rules on protocol-evolving
   * correctly... better safe than sorry).  Perf impact should be negligible. */

  Error_code sync_err_code;
  size_t sync_sz;

  auto on_recv_func = [this, pipe](const Error_code& err_code, size_t sz) mutable
  {
    rcv_on_async_read_channel_header(pipe, err_code, sz);
  };

  if constexpr(LEAD_HNDL_EXPECTATION == Tribool_vals::false_value)
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-read starting for channel-header message "
                   "along blobs-only pipe.");
    m_channel.async_receive_blob(pipe->m_init_msg_blob.mutable_buffer(), &sync_err_code, &sync_sz,
                                 std::move(on_recv_func));
  }
  else
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-read starting for channel-header message "
                   "along blobs-and-handles pipe.");
    m_channel.async_receive_native_handle(&pipe->m_init_msg_hndl, pipe->m_init_msg_blob.mutable_buffer(),
                                          &sync_err_code, &sync_sz, std::move(on_recv_func));
  }

  if (sync_err_code != transport::error::Code::S_SYNC_IO_WOULD_BLOCK)
  {
    rcv_on_async_read_channel_header(pipe, sync_err_code, sync_sz);
  }
} // Channel::rcv_async_read_channel_header()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Msg_in_pipe_t>
void CLASS_SIO_STRUCT_CHANNEL::rcv_on_async_read_channel_header(Msg_in_pipe_t* pipe, Error_code err_code, size_t sz)
{
  using flow::util::Blob;
  using util::Process_credentials;
  using std::exception;
  auto& blob = pipe->m_init_msg_blob;

  // Again keeping comments light; similar stuff to rcv_on_async_read_proto_neg_msg() (but simpler still).

  if (!handle_async_err_code(err_code, "rcv_on_async_read_channel_header()"))
  {
    handlers_poll("rcv_on_async_read_channel_header(1)"); // !!!
    blob.make_zero();
    return;
  }
  // else if (all good (including !err_code)):

  if (m_peer_process_creds)
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-receive handler for would-be channel-header "
                   "in-message invoked; size received = [" << sz << "].  However "
                   "we got the info along the other pipe; assuming "
                   "this is the same information; ignoring; will continue read chain "
                   "(do real work along this pipe).");
    blob.make_zero();
    rcv_async_read(pipe);
    return;
  }
  // else

  try
  {
    Capped_sz_capnp_message_reader hdr_reader{blob.const_buffer()};
    const auto root = hdr_reader.getRoot<schema::detail::ChannelHeader>(); // Can throw.
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Channel-header in-message contents:"
                   "\n" << ostreamable_capnp_full(root));

    assert(root.hasClaimedOwnProcessCredentials()); // See @todo below; applies here.
    auto claimed_proc_creds_root = root.getClaimedOwnProcessCredentials();
    m_peer_process_creds.emplace(claimed_proc_creds_root.getProcessId(),
                                 claimed_proc_creds_root.getUserId(),
                                 claimed_proc_creds_root.getGroupId());
  }
  catch (const exception& exc)
  {
    FLOW_LOG_FATAL("struc::Channel [" << *this << "]: Async-receive handler for would-be channel-header "
                   "in-message invoked; size received = [" << sz << "].  Could not capnp-deserialize the "
                   "contents; protocol violation despite passing protocol-negotiation earlier; aborting.  "
                   "Exception = [" << exc.what() << "].");
    assert(false
             && "Async-receive handler for would-be channel-header "
                "in-message invoked; could not capnp-deserialize the contents; "
                "protocol violation despite passing protocol-negotiation earlier; aborting.");
    std::abort();
    /* @todo Could emit civilized error instead; we do so in hairier situations later.  Same near the assert() above.
     * We don't have to do so either there or here, though; there we do so due to higher likelihood of bugs.
     * At this stage, it should really be fine, this close to protocol negotiation that just passed. */
  }
  blob.make_zero();

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Async-receive handler for channel-header "
                 "in-message invoked; size received = [" << sz << "].  Got the info:  "
                 "peer-process credentials [" << *m_peer_process_creds << "].  "
                 "Will continue read chain (do real work along this pipe).");
  rcv_async_read(pipe);
} // Channel::rcv_on_async_read_channel_header()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_msg_in(Msg_in_ptr_uniq&& msg_in_moved)
{
  Msg_in_ptr_uniq msg_in{std::move(msg_in_moved)};
  // msg_in_moved is now nullified as promised.  We own the Msg_in.

  Msg_in_impl<Msg_in> msg_in_privileged{ *msg_in }; // Gain access to back-end API.

  /* Pre-condition: msg_in is with-handle or sans-handle; and has all the serialization segments inside it that
   * the remote peer alleged are needed for deserialization to work; and msg_in.deserialize_mdt() has already
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

  const auto id = msg_in_privileged.id_or_none();
  const bool id_0 = id == 0;

  if (!id_0)
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: About to deserialize (zero-copy) new structured in-message: "
                   "optional user-message-body portion.");

    /* Note this will update all relevant stats .m_rcv.m_msg.
     * If it is an internal message, that already occurred earlier during .deserialize_mdt().
     * Don't worry -- that guy's contract is specifically to only do that for internal messages/let
     * .deserialize_body() do it otherwise. */
    Error_code err_code;
    msg_in_privileged.deserialize_body(&err_code, &m_stats.m_rcv.m_msg);
    if (err_code)
    {
      handle_new_error(err_code, "rcv_struct_new_msg_in(1)");
      return;
    }
    // else

    // (W/r/t to the pretty-print -- see any comments near the somewhat-mirrored calls in send_core().  May apply here.)
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Deserialized user message [" << msg_in_privileged.m_base << "].");
    FLOW_LOG_DATA("struc::Channel [" << *this << "]: The complete user message: "
                  "\n" << ostreamable_capnp_full(msg_in_privileged.m_base.body_root()));
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
                       "rcv_struct_new_msg_in(2)");
      return;
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
                       "rcv_struct_new_msg_in(3)");
      return;
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
                       "rcv_struct_new_msg_in(4)");
      return;
    }
  }
  // Got here: session_token is OK.  On to ID/seq #.

  if (id_0)
  {
    // Internal message.  We'll deal with the various possibilities in here.
    rcv_struct_new_internal_msg_in(msg_in_privileged);
    return; // Note m_channel_err_code_or_ok may have just become truthy.
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
                       "rcv_struct_new_msg_in(5)");
      return;
    }
    // else

    if constexpr(!Owned_channel::S_HAS_2_PIPES) // C++17: #if-like compile-time check.
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured in-message, but its ID "
                       "[" << id << "] is not sequential to next expected ID [" << m_rcv_msg_next_id << "], even "
                       "though the channel has only 1 in-pipe.  Other side misbehaved?");
      handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA,
                       "rcv_struct_new_msg_in(6)");
      return;
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
                         "rcv_struct_new_msg_in(7)");
        return;
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
                         "rcv_struct_new_msg_in(8)");
        return;
      }
      // else: inserted OK.  msg_in is now null.

      const auto q_sz = m_rcv_reassembly_q->size();

      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Deserialized new structured in-message, but its ID "
                     "[" << id << "] is not sequential to next expected ID [" << m_rcv_msg_next_id << "]; "
                     "inserted it into the reassembly queue which now has size "
                     "[" << q_sz << "].");

      // Stats: reassembly queue insertion + depth gauge + depth high-water mark.
      {
        ++m_stats.m_rcv.m_reassembly_q_insertions;
        m_stats.m_rcv.m_reassembly_q_depth = q_sz;
        flow::util::stat::update_hi_wmark(&m_stats.m_rcv.m_reassembly_q_hi_wmark, q_sz);
      }

      return; // That's it for now.  Await the seq # gap to be filled by future in-message(s).
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
                     "rcv_struct_new_msg_in(9)");
    return;
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
    rcv_struct_new_msg_in_during_log_in_as_cli(std::move(msg_in)); return; // m_channel_err_code_or_ok may be truthy.
  case Phase::S_SRV_LOG_IN:
    rcv_struct_new_msg_in_during_log_in_as_srv(std::move(msg_in)); return; // Ditto.
  case Phase::S_LOGGED_IN:
    // Fall through:
    break;
  } // Compiler should warn if we missed an enum value.

  // Yay, this is the mainstream (logged-in) case.

  // This handles the next layers (see summary above) for a given in-order message.
  rcv_struct_new_msg_in_is_next_expected(std::move(msg_in));
  if (m_channel_err_code_or_ok)
  {
    return;
  }
  // else

  // As noted in the summary we may have bridged gap to m_rcv_reassembly_q.begin() seq #.  Let's handle those too.
  if constexpr(Owned_channel::S_HAS_2_PIPES)
  {
    typename Reassembly_q::iterator first_reassembly_q_it;
    while ((!m_rcv_reassembly_q->empty())
           && (first_reassembly_q_it = m_rcv_reassembly_q->begin())->first == m_rcv_msg_next_id)
    {
      Msg_in_ptr_uniq msg_in{std::move(first_reassembly_q_it->second)};
      m_rcv_reassembly_q->erase(first_reassembly_q_it);

      ++m_rcv_msg_next_id;
      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: ID (sequence #) gap bridged to "
                     "[" << m_rcv_msg_next_id << "].  Handling in-order in-message now.");

      // As above for the new message:
      rcv_struct_new_msg_in_is_next_expected(std::move(msg_in));
      if (m_channel_err_code_or_ok)
      {
        return;
      }
      // else: continue.
    } // while ((!m_rcv_reassembly_q->empty()) && (m_rcv_reassembly_q lowest seq # == m_rcv_msg_next_id))

    // Stats (if no -- catastrophic, as opposed to normal channel-hosing -- error):
    m_stats.m_rcv.m_reassembly_q_depth = m_rcv_reassembly_q->size();
  } // if constexpr(Owned_channel::S_HAS_2_PIPES)
} // Channel::rcv_struct_new_msg_in()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_internal_msg_in(const Msg_in_impl<Msg_in>& msg_in_privileged)
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
                     "rcv_struct_new_internal_msg_in(1)");
    return;
  }
  // else

  const auto originating_msg_id = msg_in_privileged.originating_msg_id_or_none();
  const auto rsp_root = root.getUnexpectedResponse();
  string mdt_text = rsp_root.getOriginatingMessageMetadataText();
  FLOW_LOG_TRACE("Unexpected-response message regarding earlier out-message with ID [" << originating_msg_id << "]; "
                 "metadata text = [" << mdt_text << "]; will inform user via special handler if registered.");

  if (originating_msg_id == 0)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Deserialized new structured in-message: it is an internal "
                     "message indicating unexpected response out-message, but the ID of that out-message is 0 which "
                     "is a violation of our internal protocol.  Other side misbehaved?");
    handle_new_error(error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA,
                     "rcv_struct_new_internal_msg_in(2)");
    return;
  }
  // else

  handlers_post<typename Handlers::Remote_unexpected_response>("rcv_struct_new_internal_msg_in()",
                                                               originating_msg_id, std::move(mdt_text));
  // See Message::execute() for body. --^
} // Channel::rcv_struct_new_internal_msg_in()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_msg_in_during_log_in_as_cli(Msg_in_ptr_uniq&& msg_in_moved)
{
  Msg_in_ptr_uniq msg_in{std::move(msg_in_moved)};
  // msg_in_moved is now nullified as promised.  We own the Msg_in.

  Msg_in_impl<Msg_in> msg_in_privileged{ *msg_in }; // Gain access to back-end API.

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
                     "rcv_struct_new_msg_in_during_log_in_as_cli(1)");
    return;
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
                     "rcv_struct_new_msg_in_during_log_in_as_cli(2)");
    return;
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
                     "rcv_struct_new_msg_in_during_log_in_as_cli(3)");
    return;
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
  rcv_struct_new_msg_in_is_next_expected(std::move(msg_in));
  // m_channel_err_code_or_ok may have just become truthy.
} // Channel::rcv_struct_new_msg_in_during_log_in_as_cli()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_msg_in_during_log_in_as_srv(Msg_in_ptr_uniq&& msg_in_moved)
{
  Msg_in_ptr_uniq msg_in{std::move(msg_in_moved)};
  // msg_in_moved is now nullified as promised.  We own the Msg_in.

  const auto which = msg_in->body_root().which();
  Msg_in_impl<Msg_in> msg_in_privileged{ *msg_in }; // Gain access to back-end API.

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
                     "rcv_struct_new_msg_in_during_log_in_as_srv(4)");
    return;
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
                       "rcv_struct_new_msg_in_during_log_in_as_srv(5)");
      return;
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
                       "rcv_struct_new_msg_in_during_log_in_as_srv(6)");
      return;
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
  rcv_struct_new_msg_in_is_next_expected(std::move(msg_in));
  // m_channel_err_code_or_ok may have just become truthy.
} // Channel::rcv_struct_new_msg_in_during_log_in_as_srv()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::rcv_struct_new_msg_in_is_next_expected(Msg_in_ptr_uniq&& msg_in_moved)
{
  using flow::util::stat::Histogram_counter;
  using flow::Fine_clock;
  using boost::promise;
  using boost::chrono::round;
  using boost::chrono::microseconds;
  using std::holds_alternative;
  using std::monostate;

  Msg_in_ptr_uniq msg_in{std::move(msg_in_moved)};
  // msg_in_moved is now nullified as promised.  We own the Msg_in.

  Msg_in_impl<Msg_in> msg_in_privileged{ *msg_in }; // Gain access to back-end API.

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

      ++m_stats.m_rcv.m_unexpected_responses;

      rcv_struct_inform_of_unexpected_response(std::move(msg_in));
      assert((!m_channel_err_code_or_ok) && "That guy is best-effort and should not be hosing channel.");
      return;
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

    if (one_off)
    {
      // One-off request RTT histogram (microseconds).
      const auto rtt_usec
        = round<microseconds>(Fine_clock::now() - exp_rsp.m_sent_when).count();
      m_stats.m_rcv.m_histo_one_off_request_rtt_usec.record_value
        (static_cast<Histogram_counter::value_t>(rtt_usec));

      --m_stats.m_rcv.m_expect_response_one_off_active;
      m_rcv_expecting_response_map.erase(exp_rsp_it);
    }

    handlers_post<typename Handlers::Message>("rcv_struct_new_msg_in_is_next_expected(on-rsp)",
                                              std::move(msg_in), std::move(on_msg_func));
                                              // See Message::execute() for body.
    return;
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

    // Stats.
    {
      auto& rcv = m_stats.m_rcv;
      ++rcv.m_unsolicited_msgs_cached;
      ++rcv.m_pending_msgs_depth;
      flow::util::stat::update_hi_wmark(&rcv.m_pending_msgs_hi_wmark, rcv.m_pending_msgs_depth);
    }
    return;
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

  if (one_off)
  {
    m_rcv_expecting_msg_map.erase(exp_msg_it);

    --m_stats.m_rcv.m_expect_msg_active; // Attn: Stats (also just below).
  }
  ++m_stats.m_rcv.m_unsolicited_msgs_routed;

  handlers_post<typename Handlers::Message>("rcv_struct_new_msg_in_is_next_expected(on-msg)",
                                            std::move(msg_in), std::move(on_msg_func));
                                            // See Message::execute() for body.
} // Channel::rcv_struct_new_msg_in_is_next_expected()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::rcv_struct_inform_of_unexpected_response(Msg_in_ptr_uniq&& msg_in)
{
  using schema::detail::StructuredMessage;
  using util::Blob_mutable;
  using util::Blob_const;
  using flow::util::ostream_op_string;
  using flow::util::ceil_div;
  using boost::array;
  using Word = ::capnp::word;

  // Needs to be enough to store the mdt header below; be fairly careful in limiting any strings below and such.
  constexpr size_t INTERNAL_MSG_MAX_SZ = 512;

  assert((m_phase == Phase::S_LOGGED_IN)
         && "We do not mess with internal messages until log-in has finished.  Bug?");

  /* There are 2 actions below: send_core() an internal message about the unexpected response, for the other
   * side's benefit (it can fire its on-*remote*-unexpected-response handler if applicable); and -- separately --
   * fire our on-*local*-unexpected-response handler.  The latter definitely should be handlers_post()ed to
   * avoid recursive mayhem as usual.  Should the send_core() action also be handlers_post()ed, or could we just
   * do it synchronously?  Back when Channel was async-I/O all the way (before sync_io pattern existed)
   * I had post()ed it onto thread W, which at the time was used exclusively for firing user handlers; the idea
   * was to emulate the user themselves send()ing a thing.  Now, though, that appears unnecessary: it's an internal
   * best-effort message; we don't even care about any error, and it cannot be replied-to. */

  Msg_in_impl<Msg_in> msg_in_privileged{ *msg_in }; // Gain access to back-end API.

  // Send the internal message.  Forego some of the vanilla-send() steps and ignore errors (spirit = do our best).
  if (!m_channel_err_code_or_ok)
  {
    /* What we are about to is exceedingly simple: make a small buffer (on the stack even) sufficient
     * to store the internal-message mdt header (a StructuredMessage that specifies an internal msg as opposed to user
     * msg); load that header in there (the rudimentary info we want to convey about the problem);
     * and send_core() the buffer.  For context though:
     *
     * The receiver, when receiving the first (of 1+) low-level blob comprised by the structured-message, doesn't
     * pre-know if it's gonna be an internal message or user message.  It has to be able to figure it out first-thing
     * (upon receiving that blob), and that's done simply by looking inside the mdt header (StructuredMessage)
     * which in civilized fashion states whether it's a user message or internal message; and in the former case
     * how many more per-segment blobs to read to get the whole thing.  For user messages this is slightly tricky;
     * first it treats the entire blob as an encoding of the mdt header (any non-header -- user message payload -- in
     * the rest of that blob is ignored by capnp at that stage as trailing junk); then upon realizing it *is* a
     * user message it will take the post-header part of blob 1; and that's seg 1 of the user message.  See
     * send_impl() where this blob 1 (with mdt header and seg 1 of user message) is composed, if you want.
     *
     * For us, though, it is much simpler: There *is* nothing but the mdt header.  So that's all we send!  The only
     * trick is to make it so that the receiver can handle either type (internal versus user).  Happily, there is
     * no trick: either way, the first thing in blob 1 is the mdt header, of whichever type (StructuredMessage
     * schema either way).  Then in our case it'll just say, "OK, got the internal message.  Let's wait for/read the
     * next in-message of whatever type."  In the other case it'll interpret the rest of blob 1 and so on, until
     * all segments are received; and then wait for/read the next in-message of whatever type.
     *
     * Anyway.  Do that simple thing then. */

    // Array-of-words, not bytes: capnp requires word-aligned segments; a stack byte-array guarantees nothing.
    array<Word, ceil_div(INTERNAL_MSG_MAX_SZ, sizeof(Word))> blob_out;
    Capped_sz_capnp_message_builder int_msg_builder{Blob_mutable{blob_out.data(), INTERNAL_MSG_MAX_SZ},
                                                    true}; // Gotta zero it for capnp; array<> lacks ctor and does not.

    StructuredMessage::InternalMessageBody::Builder int_msg_root{nullptr};
#ifndef NDEBUG
    const bool ok =
#endif
    load_mdt(&int_msg_builder, &int_msg_root, &m_channel_err_code_or_ok,
             m_session_token, msg_in_privileged.id_or_none());
    // Indicate we're referencing offending msg *msg_in. -------^
    assert(ok && "It should only fail if we try to reuse an existing int_msg_builder.");
    assert((!m_channel_err_code_or_ok)
           && "No mdt-loading issues should be possible when accompanying internal-messages.");

    /* For the metadata-text, shove in a pretty-printing of the metadata header with lots of goodies in there --
     * but reasonably capped in length (and in compute used, though certainly not super-quick either) and
     * *not* including the user message body itself.  However do add the top-level union-which as well.
     *
     * Again: Careful to make this fit into INTERNAL_MSG_MAX_SZ (see above) or change it if needed (but small
     * is good). */
    auto rsp_root = int_msg_root.initUnexpectedResponse();
    rsp_root.setOriginatingMessageMetadataText
               (ostream_op_string("user-msg-union-which = ", int(msg_in_privileged.m_base.body_root().which()),
                                  ", metadata-header =\n",
                                  ostreamable_capnp_full(msg_in_privileged.mdt_root())));

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Sending internal-message: "
                   "[" << ostreamable_capnp_full(int_msg_builder.getRoot<StructuredMessage>()
                                                   .asReader()) << "].");

    const auto int_msg_sz = int_msg_builder.getSegmentsForOutput()[0].asBytes().size();
    send_core({ Blob_const{blob_out.data(), int_msg_sz} },
              {}, nullptr); // Last nullptr => ignore error.

    // Stats: internal msg is always single-seg, single-blob.
    {
      auto& msg = m_stats.m_snd.m_msg;
      ++msg.m_internal_msgs;
      ++msg.m_single_segment_msgs;
      ++msg.m_total_segments;
      ++msg.m_total_low_lvl_blobs;
      msg.m_histo_msg_sz.record_value(int_msg_sz);
    }
  } // if (!m_channel_err_code_or_ok)

  handlers_post<typename Handlers::Message>("rcv_struct_inform_of_unexpected_response()",
                                            std::move(msg_in), On_msg_handler_ptr{});
                                            // See Message::execute() for body.
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
      return false;
    }
    // else if (!overall_hosed):
    return true;
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
    /* err_code came from an Owned_channel async-completion (transport-layer failure); the relevant
     * sub-object will have hosed/logged its own stats already. */
    handle_new_error(err_code, context, true);
  }

  return false;
} // Channel::handle_async_err_code()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::handle_new_error(const Error_code& err_code_not_ok, util::String_view context,
                                                bool lower_layer_originating)
{
  const auto& err_code = err_code_not_ok; // For brevity.

  FLOW_LOG_WARNING("struc::Channel [" << *this << "]: An operation [" << context << "] yielded failure "
                   "[" << err_code << "] [" << err_code.message() << "] thus hosing the channel; "
                   "saving that fail-reason and potentially ending the calling async chain.");
  hose(err_code, context, lower_layer_originating); // Sets m_channel_err_code_or_ok; logs final stats.

  handlers_post<typename Handlers::Error>("handle_new_error()"); // See Message::execute() for body.
} // Channel::handle_new_error()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Handler_variety, typename... Members>
void CLASS_SIO_STRUCT_CHANNEL::handlers_post(util::String_view context, Members&&... members)
{
  using std::in_place_type;

  // You'll need to read doc header of m_sync_io_handlers to make sense of this.

  if constexpr(S_AGGRESSIVE_HANDLER_EXECUTION)
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Handler with context "
                   "[" << context << "] shall synchronously execute right now.");

    /* (A decent optimizer should generate pretty-much the body of Handler_variety::execute(this) here, inlined.
     * The constructor, even if really executed, just copied over a scalar or two-ish onto some stack variable(s).
     * .execute() then did the actual "stuff," using those value(s) if any.) */
    Handler_variety handler_of_known_variety{std::forward<Members>(members)...};
    handler_of_known_variety.execute(this);
  }
  else // if constexpr(!S_AGGRESSIVE_HANDLER_EXECUTION)
  {
    auto& handlers = m_sync_io_handlers;

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Handler added in context "
                   "[" << context << "]; handlers count will rise to [" << (handlers.size() + 1) << "].");

    if constexpr(!Owned_channel::S_HAS_2_PIPES)
    {
      assert(handlers.empty()
             && "Each low-level in-message can result in at most one handler due to lacking reassembly-queue "
                "in one-pipe Owned_channel; and we poll handlers after each in-message; "
                "and no handler can add another handler; so how did we end up with 2+ pending handlers?  Bug?");
    }
    /* else { In rcv_struct_new_msg_in() the `if constexpr(Owned_channel::S_HAS_2_PIPES)` clause can cause multiple
     *        handlers to be posted before getting flushed in here. } */

    handlers.emplace_back(in_place_type<Handler_variety>, std::forward<Members>(members)...);
  }
} // Channel::handlers_post()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::handlers_poll([[maybe_unused]] util::String_view context)
{
  using std::visit;

  // You'll need to read doc header of m_sync_io_handlers to make sense of this.

  if constexpr(!S_AGGRESSIVE_HANDLER_EXECUTION)
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
      auto& handler = handlers[idx];

      /* In short, `handler` is really one of concrete types, like, Handler_type1, Handler_type2, Handler_type3...
       * and this visit() "just" does handler.execute(this), regardless of which one it really is.  So it is
       * polymorphism-by-switch-statement essentially. */
      visit([this](auto&& handler_of_known_variety) { handler_of_known_variety.execute(this); },
            handler);

      FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Handler [" << idx << "] (0-based) of "
                     "[" << handlers.size() << "] (1-based) ready handlers: completed.");
    } // for (idx in [0, handlers.size()))

    assert(handlers_ref.empty() && "Did handler re-push a handler?  Should not be possible with struc::Channel.");
  }
  // else { No-op.  Each handlers_post() already executed its thing synchronously. }
} // Channel::handlers_poll()

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
      return false;
    }
    // else
  }
  return true;
} // Channel::check_unsendable()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::send(Msg_out* msg_ptr, const Msg_in* originating_msg_or_null, Error_code* err_code)
{
  return check_not_started_ops("send()")
         && check_unsendable(*msg_ptr)
         && check_prior_error("send()")
         && send_impl(msg_ptr, originating_msg_or_null, err_code, On_msg_handler_ptr{}, nullptr);
} // Channel::send()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename On_msg_handler>
bool CLASS_SIO_STRUCT_CHANNEL::async_request(Msg_out* msg_ptr, const Msg_in* originating_msg_or_null,
                                             msg_id_out_t* id_unless_one_off, On_msg_handler&& on_rsp_func,
                                             Error_code* err_code)
{
  using boost::make_shared;

  return check_not_started_ops("async_request()")
         && check_unsendable(*msg_ptr)
         && check_prior_error("async_request()")
         && send_impl(msg_ptr, originating_msg_or_null, err_code,
                      make_shared<typename On_msg_handler_ptr::element_type>(std::move(on_rsp_func)),
                      id_unless_one_off);
} // Channel::async_request()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::send_impl(Msg_out* msg_public_ptr, const Msg_in* originating_msg_or_null,
                                         Error_code* err_code,
                                         // @todo Make it && for a little perf boost.
                                         const On_msg_handler_ptr& on_rsp_func_or_null, msg_id_out_t* id_unless_one_off)
{
  using util::Blob_const;
  using flow::Fine_clock;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, send_impl,
                                     msg_public_ptr, originating_msg_or_null, _1,
                                     on_rsp_func_or_null, id_unless_one_off);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  Msg_out_impl<Msg_out> msg{ *msg_public_ptr }; // Gain access to internal (back end) API of the Msg_out.
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
                                         new Expecting_response{ { one_off, on_rsp_func_or_null },
                                                                 Fine_clock::now() });
    assert(result.second && "IDs do not repeat, so dupe-insertion should not be possible.");

    // Stats: response expectation gauges.
    ++(one_off ? m_stats.m_rcv.m_expect_response_one_off_active
               : m_stats.m_rcv.m_expect_response_sticky_active);

    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Registered a (one-off? = [" << one_off << "]) "
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

  /* Let's prep the binaries to send!  This is the other side of the logic on the receive side.  See start_and_poll()
   * where that's kicked off.  It will refer you to Msg_in_pipe doc header and so on.  The below should follow
   * from that understanding.  So we'll keep inline comments fairly svelte, unless there's new info. */

  msg_id_t originating_msg_id_or_none = 0;
  if (originating_msg_or_null)
  {
    const Msg_in_impl<Msg_in> originating_msg_privileged{ *const_cast<Msg_in*>(originating_msg_or_null) };
    /* (The const_cast<> works around the deficiency of the facade technique Msg_in_impl uses; it cannot wrap
     * a const object, as it stores a ref to non-const Msg_in.  We even made the object const to guarantee we won't
     * do anything untoward with it.  @todo Nevertheless try to think of a nicer way to do this.) */
    originating_msg_id_or_none = originating_msg_privileged.id_or_none();
  }

  Segment_bufs buffers_out; // Place buffer ptrs/sizes into this.
  /* Plumbed out of emit_serialization() only for seg-related stats and/or logs below.
   * Perf cost is low, as it just move()s a thing it already prepared internally, according to its contract. */
  Split_segments split_segs_out;
  msg.emit_serialization(&buffers_out, &split_segs_out, m_struct_lender_session, err_code,
                         m_session_token, originating_msg_id_or_none, id);
  if (*err_code)
  {
    // Try to be helpful with logging depending on which error occurred, for things we specifically know about.
    if ((*err_code == error::Code::S_SERIALIZE_FAILED_SPLIT_ENCODING_TOO_BIG)
        || (*err_code == error::Code::S_SERIALIZE_FAILED_SPLIT_SEGS_TOO_MANY))
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send out-message; "
                       "mdt header in seg1 would contain session-token [" << m_session_token << "], "
                       "replying-to msg ID [" << originating_msg_id_or_none << "], our msg ID [" << id << "].  "
                       "However an error occurred ([" << *err_code << "] [" << err_code->message() << "]).  "
                       "Split-segments-related error (which one: see code above): additional relevant info "
                       "follows in logs below.  "
                       "For reference: Message itself (may be truncated): [" << msg.m_base << "].  "
                       "This is a channel-hosing error; emitting it.");

      /* By contract, on these particular errors, out-arg split_segs_out shall be valid.  Printing it will
       * show the general situation w/r/t seg-splitting; e.g., one can see which entry or entries would trigger
       * the error. */
      size_t idx = 0;
      for (const auto& split_seg : split_segs_out)
      {
        FLOW_LOG_WARNING("struc::Channel [" << *this << "]: "
                         "Split-segments entry [" << (++idx) << '/' << split_segs_out.size() << "]: "
                         "start_idx = [" << split_seg.m_start_idx << "], "
                         "n_cont_subsegs = [" << split_seg.m_n_cont_subsegs << "].");
      }
      /* (Incidentally: if no error then this info is probably still TRACE-logged and/or DATA-logged one way or
       * another.  Here we want to make pretty sure it is visible, as if this does happen it'll probably feel
       * quite annoying, and it's nice to have some debug info immediately ready in such cases.  Hence WARNING.)
       * @todo Maybe do some join()/transform() type stuff => log 1 msg?  The above is fine too though. */
    }
    else if (*err_code == error::Code::S_INVALID_ARGUMENT)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send out-message; "
                       "mdt header in seg1 would contain session-token [" << m_session_token << "], "
                       "replying-to msg ID [" << originating_msg_id_or_none << "], our msg ID [" << id << "].  "
                       "However an error occurred ([" << *err_code << "] [" << err_code->message() << "]).  "
                       "INVALID_ARGUMENT: there was insufficient or no space for this mdt header; "
                       "if you use a custom Builder_config when forming your Msg_out, please be sure "
                       "to set its .m_frame_prefix_sz to at least "
                       "[" << BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL << "].  "
                       "For reference: Message itself (may be truncated): [" << msg.m_base << "].  "
                       "This is a channel-hosing error; emitting it.");
    }
    else
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send out-message; "
                       "mdt header in seg1 would contain session-token [" << m_session_token << "], "
                       "replying-to msg ID [" << originating_msg_id_or_none << "], our msg ID [" << id << "].  "
                       "However a misc error occurred ([" << *err_code << "] [" << err_code->message() << "]).  "
                       "For reference: Message itself (may be truncated): [" << msg.m_base << "].  "
                       "This is a channel-hosing error; emitting it.");
    }

    /* @todo Does it need to be channel-hosing really? INVALID_ARGUMENT and SERIALIZE_FAILED_SPLIT_ENCODING_TOO_BIG
     * at least are per-message problems; it is conceivable one could recover, continuing to use *this Channel
     * for other things.  The trade-off is versus a simpler contract and a higher chance user notices the error,
     * if it hoses the whole Channel just by coming in contact with this message. */
    hose(*err_code, "send_core()/emit_serialization", false);

    return true; // This is similar to send_core() failing below, as far as the user is concerned.
  } // if (*err_code) (from emit_serialization())
  // else:

  // Stats: send-side user-message classification + seg/split stats.
  {
    /* @todo (Maybe/revisit) The symmetrical receive-side logic sits in Msg_in[_impl]::deserialize_body(),
     * so for consistency it might be nice to put the below nearby in msg.hpp in Msg_out[_impl]::emit_serialization().
     * It might also be a bit more efficient about the split-seg stuff, if it can load it up in the same loop
     * as the actual splitting (if any).  On the other hand we'd have to pass some info that only we have,
     * like at least bool(on_rsp_func_or_null) and one_off.  It's more or less fine this way too.  As for perf
     * specifically: it's probably all right, particularly since the splitting should be a rare (with SHM backing,
     * non-existent) event. */

    auto& snd_msg = m_stats.m_snd.m_msg;
    ++snd_msg.m_user_msgs;
    if (on_rsp_func_or_null) // Request path (async_request()).
    {
      ++snd_msg.m_requests;
      one_off && (++snd_msg.m_requests_one_off);
      originating_msg_or_null && (++snd_msg.m_request_responses);
    }
    else // Notification path (send()).
    {
      ++snd_msg.m_notifications;
      originating_msg_or_null && (++snd_msg.m_notification_responses);
    }

    msg.m_base.native_handle_or_null().null() || (++snd_msg.m_handle_bearing_msgs);

    // Seg/split stats from emit_serialization() output.
    auto n_segs = buffers_out.size(); // # of blobs now; we'll "correct" it to # of segs below.
    snd_msg.m_total_low_lvl_blobs += n_segs;

    // Original (pre-split) segment count: subtract split overhead.
    for (const auto& split_seg : split_segs_out)
    {
      n_segs -= split_seg.m_n_cont_subsegs; // m_n_cont_subsegs = # of blobs excluding the first one.
      snd_msg.m_histo_split_blobs_per_seg.record_value(split_seg.m_n_cont_subsegs + 1);
    }

    snd_msg.m_total_segments += n_segs;
    ++((n_segs == 1) ? snd_msg.m_single_segment_msgs : snd_msg.m_multi_segment_msgs);
    split_segs_out.empty() || (++snd_msg.m_msgs_with_split_segments);

    /* Total serialization size (sum of all blobs).
     * Perf: @todo It would be nice to do that in an existing loop in emit_serialization() somewhere. */
    size_t total_sz = 0;
    for (const auto& blob : buffers_out)
    {
      total_sz += blob.size();
    }
    snd_msg.m_histo_msg_sz.record_value(total_sz);
  } // Stats block.

  /* `msg` ostream printout has some other interesting info plus a (possibly truncated) one-line representation of
   * .body_root() contents. */
  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Send request wants to send (user) out-message; "
                 "mdt header in seg1 contains session-token [" << m_session_token << "], "
                 "replying-to msg ID [" << originating_msg_id_or_none << "], our msg ID [" << id << "], "
                 "segment-count [" << buffers_out.size() << "].  Message itself (may be truncated): "
                 "[" << msg.m_base << "].");

  /* Print the entire contents with indentation/newlines.
   * The user message could be giant.  By definition that is for DATA verbosity only.
   * In addition even the mere computation of what to print (e.g., if we wanted to truncate it before printing
   * at TRACE level) is potentially cripplingly slow; so absolutely do not do it outside the log macro. */
  FLOW_LOG_DATA("struc::Channel [" << *this << "]: Here is the complete user "
                "message:\n" << ostreamable_capnp_full(msg.m_base.body_root()->asReader()));

  // OK!  Send it/them out.
  return send_core(buffers_out, msg.m_base.native_handle_or_null(), err_code);
} // Channel::send_impl()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::send_core(const Segment_bufs& blobs_out,
                                         Native_handle hndl, Error_code* err_code_or_ignore)
{
  /* `sink` used only if err_code_or_ignore is null -- we are to ignore any error and let the next send*() catch it.
   * As of this writing used only for internal messages which are best-effort. */
  Error_code sink;
  Error_code* const err_code = err_code_or_ignore ? err_code_or_ignore : &sink;

  // See send() doc header which summarizes our course of action.  See also m_channel doc header for context.

  /* One more thing before we really do stuff -- as discussed in send_init_msgs() -- if protocol-negotiation
   * message send failed, and we got here (meaning check_prior_error() is false, meaning !m_channel_err_code_or_ok,
   * meaning no prior send_core()/remote_peer_process_liveness_check()/.../start_and_poll() got to it first), then
   * promote that saved protocol-negotiation-send error to m_channel_err_code_or_ok and emit it (the latter done one
   * time per *this, as usual). */
  if (m_init_msg_err_code_or_ok)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send out-message, "
                     "but earlier protocol-negotiation message send along a pipe failed (error "
                     "[" << m_init_msg_err_code_or_ok << "] [" << m_init_msg_err_code_or_ok.message() << "]).  "
                     "We now promote this to a channel-hosing error; emitting it.");

    assert(err_code_or_ignore
           && "No internal messages shall be sent before start_and_poll() begins read chain.  Did this change?  "
                "Just did not want to deal with the subtleties of that + m_init_msg_err_code_or_ok "
                "until really necessary, if ever.");
    *err_code = m_init_msg_err_code_or_ok;
    hose(*err_code, "send_core()/init-msg promotion", true);
    return true; // This is similar to send_*() failing below, as far as the user is concerned.
  } // if (m_init_msg_err_code_or_ok)

  // That's it.  Now it's just blobs to send out, regardless of what they represent.

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
        m_channel.send_blob(blob, err_code);
        assert(ok); // Only false if not PEER state; we promised undefined behavior in that case.

        if (*err_code)
        {
          return;
        }
      }
    }
    else
    {
      assert(false && "We shouldn't be called; see code below.");
    }
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
        m_channel.send_native_handle(first ? (first = false, hndl) : Native_handle{},
                                     blob, err_code);
        assert(ok); // Same as above.

        if (*err_code)
        {
          return;
        }
      }
    }
    else
    {
      assert(false && "We shouldn't be called; see code below.");
    }
  }; // const auto send_meta_blobs =

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Send request wants to send out-message, "
                 "and the serialization shall now be sent: total of [" << blobs_out.size() << "] segments, 1 per blob; "
                 "1st blob contains (ahead of potential user message payload) metadata header "
                 "including ID and the further-segments count, plus native handle (unless null) = [" << hndl << "].");

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
  }

  // `msg` may now be safely destroyed.

  if (*err_code)
  {
    // A send_*() failed.  There is one special case however which we promised to emit in a special way.
    if (*err_code == transport::error::Code::S_SENDS_FINISHED_CANNOT_SEND)
    {
      FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Send request wants to send out-message, "
                       "and we tried to send the serialization: total of [" << blobs_out.size() << "] segs, 1/blob; "
                       "1st blob contains (ahead of potential user message payload) metadata header "
                       "including ID and the further-segments count, plus native handle (unless null) = "
                       "[" << hndl << "].  However Channel reports "
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
                     "(ahead of potential user message payload) metadata header "
                     "including ID and the further-segments count, plus native handle (unless null) = "
                     "[" << hndl << "].  However Channel reports "
                     "a send failed (error [" << *err_code << "] [" << err_code->message() << "]).  "
                     "This is a channel-hosing error; emitting it "
                     "unless internally-ignoring (are we? = [" << (!err_code_or_ignore) << "]).");
    if (err_code_or_ignore)
    {
      // Owned_channel sender pipe just hosed at the transport layer; it has hosed/logged itself already.
      hose(*err_code, "send_core()/Owned_channel send fail", true);
    }
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
void CLASS_SIO_STRUCT_CHANNEL::send_init_msgs()
{
  using flow::util::ceil_div;
  using util::Blob_mutable;
  using util::Blob_const;
  using boost::array;
  using Word = ::capnp::word;

  constexpr size_t SEG_SZ = 256; // Enough to serialize a ProtocolNegotiation or ChannelHeader capnp-struct in 1 seg.

  /* See "Protocol negotiation" in class doc header for background.
   * Our job here is straightforward enough (like a much-simpler send_core(), albeit along both pipes if relevant, with
   * a really simple payload contained in 1 blob-message), and the code below is easy enough to follow.
   *
   * (Update: Now, also, we send the ChannelHeader struct immediately following that.)
   *
   * The only subtlety has to do with error handling.  Recall we are called at the end of successful start_ops(), hence
   * before any user send()/async_request() and before start_and_poll() (which is when we begin the read chain, and
   * error handler is registered and can be subsequently invoked).  What we do below is synchronous.
   * Vast majority of the time all will work below, so nothing to worry about; now suppose an m_channel.send_*()
   * below fails (into err_code).  Now what?
   *
   * Short-circuiting discussion of various other possibilities, basically, we treat this as the whole channel being
   * hosed; so we save error into m_init_msg_err_code_or_ok (ref = err_code).  Only question is when to emit it.
   * We execute before start_and_poll() (through which incoming-direction work becomes possible) and before
   * and send()/async_request() (through which outgoing-direction work happens).  (The two interact fine in either
   * order; a given new error is emitted to the first such API call.)  So what we do is check m_init_msg_err_code_or_ok
   * here and check it in start_and_poll(); if truthy then immediately promote it to m_channel_err_code_or_ok
   * and emit via handle_new_error() as usual.  What if send()/async_request() happens first though?  Similarly
   * there. */

  auto& err_code = m_init_msg_err_code_or_ok;
  assert(!err_code);

  const auto protocol_ver_to_send = m_protocol_negotiator.local_max_proto_ver_for_sending();
  const auto protocol_ver_to_send_aux = m_protocol_negotiator_aux.local_max_proto_ver_for_sending();
  assert((protocol_ver_to_send != Protocol_negotiator::S_VER_ALREADY_SENT)
         && (protocol_ver_to_send_aux != Protocol_negotiator::S_VER_ALREADY_SENT)
         && "How'd we get to this line twice?  Or Protocol_negotiator bug?");
  assert((m_protocol_negotiator.local_max_proto_ver_for_sending() == Protocol_negotiator::S_VER_ALREADY_SENT)
         && (m_protocol_negotiator_aux.local_max_proto_ver_for_sending() == Protocol_negotiator::S_VER_ALREADY_SENT)
         && "Protocol_negotiator not properly marking the once-only sending-out of protocol version?");

  // Array-of-words, not bytes: capnp requires word-aligned segments; a stack byte-array guarantees nothing.
  array<Word, ceil_div(SEG_SZ, sizeof(Word))> blob_out1;
  array<Word, ceil_div(SEG_SZ, sizeof(Word))> blob_out2;
  Capped_sz_capnp_message_builder builder1{Blob_mutable{blob_out1.data(), SEG_SZ},
                                           true}; // Gotta zero it for capnp; array<> lacks ctor and does not.
  Capped_sz_capnp_message_builder builder2{Blob_mutable{blob_out2.data(), SEG_SZ},
                                           true};
  /* The two headers are expected in this particular order.  Must pass protocol-negotiation first before even looking
   * at anything past that, including ChannelHeader.  This is a formal requirement of the Protocol_negotiator
   * pattern. */
  {
    auto root = builder1.initRoot<schema::detail::ProtocolNegotiation>();
    root.setMaxProtoVer(protocol_ver_to_send);
    root.setMaxProtoVerAux(protocol_ver_to_send_aux);
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Here is the prepended protocol-negotiation header we shall send:"
                   "\n" << ostreamable_capnp_full(root.asReader()));
  }
  {
    auto root = builder2.initRoot<schema::detail::ChannelHeader>();
    auto claimed_own_proc_creds = root.initClaimedOwnProcessCredentials();
    claimed_own_proc_creds.setProcessId(util::Process_credentials::own_process_id());
    claimed_own_proc_creds.setUserId(util::Process_credentials::own_user_id());
    claimed_own_proc_creds.setGroupId(util::Process_credentials::own_group_id());
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Here is the prepended channel-header we shall send:"
                   "\n" << ostreamable_capnp_full(root.asReader()));
  }

  const Blob_const seg_blob_out1{blob_out1.data(),
                                 builder1.getSegmentsForOutput()[0].asBytes().size()};
  const Blob_const seg_blob_out2{blob_out2.data(),
                                 builder2.getSegmentsForOutput()[0].asBytes().size()};

  // That's it.  Now it's just <blob>x2 to send out.

  // Synchronous helpers.
  [[maybe_unused]] const auto send_blobs = [&]()
  {
    if constexpr(Owned_channel::S_HAS_BLOB_PIPE)
    {
      m_channel.send_blob(seg_blob_out1, &err_code);
      err_code || m_channel.send_blob(seg_blob_out2, &err_code);
    }
    // else if constexpr(true) { No-op: We won't be called; see below. }
  };
  [[maybe_unused]] const auto send_meta_blobs = [&]()
  {
    if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE)
    {
      m_channel.send_native_handle(Native_handle{}, seg_blob_out1, &err_code);
      err_code || m_channel.send_native_handle(Native_handle{}, seg_blob_out2, &err_code);
    }
    // else if constexpr(true) { No-op: We won't be called; see below. }
  };

  if constexpr(Owned_channel::S_HAS_BLOB_PIPE_ONLY)
  {
    send_blobs();
  }
  else if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE_ONLY)
  {
    send_meta_blobs();
  }
  else
  {
    static_assert(Owned_channel::S_HAS_2_PIPES, "This code assumes 1 blobs pipe, 1 handles pipe, or both exactly.");
    send_meta_blobs();
    if (!err_code)
    {
      send_blobs();
    }
  }
} // Channel::send_init_msgs()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::remote_peer_process_liveness_check(Error_code* err_code)
{
  using util::process_running;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(bool, remote_peer_process_liveness_check, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  if (!check_prior_error("remote_peer_process_liveness_check()"))
  {
    return false;
  }
  // else

  ++m_stats.m_rcv.m_liveness_checks;

  /* One more thing before we really do stuff -- as discussed in send_init_msgs() -- perform the same check as
   * in send_core() at this stage.  Please read that comment; then come back here.
   *
   * If this is truthy, this is our doc header's "channel is not healthy for reason E, and this is our first chance
   * to emit this" outcome. */
  if (m_init_msg_err_code_or_ok)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: They want to check whether opposing process is alive "
                     "and hose channel/emit error if not; "
                     "but earlier protocol-negotiation message send along a pipe failed (error "
                     "[" << m_init_msg_err_code_or_ok << "] [" << m_init_msg_err_code_or_ok.message() << "]).  "
                     "We now promote this to a channel-hosing error; emitting it.  Note that this does not "
                     "mean the opposing process is dead (though it might be); but that the channel is hosed, as we "
                     "are emitting this for the first and only time now, opportunistically.");

    assert((m_init_msg_err_code_or_ok != transport::error::Code::S_PEER_PROCESS_NO_LONGER_EXISTS)
           && "At any rate we promised this!  Indeed any init-msg sends don't do liveness checking.");
    *err_code = m_init_msg_err_code_or_ok;
    hose(*err_code, "remote_peer_process_liveness_check()/init-msg promotion", true);
  } // if (m_init_msg_err_code_or_ok)
  else if (!m_peer_process_creds) // && (!m_init_msg_err_code_or_ok)
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: They want to check whether opposing process is alive "
                   "and hose channel/emit error if not; but we do not know its PID yet; must assume it is alive for "
                   "now.  This can only occur if (1) they have not yet invoked start_and_poll(); or (2) they have, but "
                   "we have not yet received the mandatory init-messages (protocol-negotiation + channel-header).");
    err_code->clear();
  }
  else if (process_running(m_peer_process_creds->process_id()))
  {
    FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Opposing process with creds [" << *m_peer_process_creds << "] "
                   "is still alive according to OS.");
    err_code->clear();
  }
  else // if (!process_running(opposing PID))
  {
    *err_code = transport::error::Code::S_PEER_PROCESS_NO_LONGER_EXISTS;
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Opposing process with creds [" << *m_peer_process_creds << "] "
                     "is (no longer) alive according to OS; emitting dedicated peer-dead channel-hosing error "
                     "[" << *err_code << "] [" << err_code->message() << "].");
    hose(*err_code, "remote_peer_process_liveness_check()/peer-not-running", false);
  }

  return true;
} // Channel::remote_peer_process_liveness_check()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename Task_err>
bool CLASS_SIO_STRUCT_CHANNEL::async_end_sending(Error_code* sync_err_code, Task_err&& on_done_func)
{
  return check_not_started_ops("async_end_sending()")
         && m_channel.async_end_sending(sync_err_code, on_done_func);
}

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::check_prior_error(util::String_view context) const
{
  if (m_channel_err_code_or_ok)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: In context [" << context << "]: A prior error "
                     "[" << m_channel_err_code_or_ok << "] [" << m_channel_err_code_or_ok.message() << "] shall "
                     "prevent further action in this context: bailing out of this operation.");
    return false;
  }
  // else
  return true;
}

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::check_phase_and_prior_error(Phase required_phase, util::String_view context) const
{
  if (!check_prior_error(context))
  {
    return false;
  }
  // else

  if (m_phase != required_phase)
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: In context [" << context << "]: The required log-in "
                     "phase for this context is not in effect: bailing out of this operation.");
    return false;
  }
  // else

  return true;
} // Channel::check_phase_and_prior_error()

TEMPLATE_SIO_STRUCT_CHANNEL
typename CLASS_SIO_STRUCT_CHANNEL::Msg_out CLASS_SIO_STRUCT_CHANNEL::create_msg(Native_handle&& hndl_or_null) const
{
  // We are just a convenience helper really; all we do is pass-in m_struct_builder_config memorized for convenience.

  Msg_out msg{m_struct_builder_config};
  if (!hndl_or_null.null()) // Tiny optimization to skip past some boring checks inside store_...().
  {
    msg.store_native_handle_or_null(std::move(hndl_or_null));
  }
  return msg;
}

TEMPLATE_SIO_STRUCT_CHANNEL
typename CLASS_SIO_STRUCT_CHANNEL::Msg_out CLASS_SIO_STRUCT_CHANNEL::create_msg
                                             (size_t msg_sz_cap_estimate_or_zero, Native_handle&& hndl_or_null) const
{
  // We are just a convenience helper really; all we do is pass-in m_struct_builder_config memorized for convenience.

  if (msg_sz_cap_estimate_or_zero == 0)
  {
    return create_msg(std::move(hndl_or_null));
  }
  // else

  auto& cfg_ref = m_struct_builder_config;
  if (cfg_ref.m_segment_sz_init == msg_sz_cap_estimate_or_zero)
  {
    return create_msg(std::move(hndl_or_null));
  }
  // else

  auto cfg_copy = cfg_ref;

  // It'll be rounded to an aligned-width thing, so don't waste time doing it ourselves.
  cfg_copy.m_segment_sz_init = msg_sz_cap_estimate_or_zero;

  Msg_out msg{cfg_copy};
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
  if (!check_phase_and_prior_error(Phase::S_LOGGED_IN, "expect_msg()"))
  {
    return false;
  }
  // else

  Msgs_in qd_msgs;
  const bool ok = expect_msgs_impl(&qd_msgs,
                                   true, // One-off expectation (one message expected).
                                   which,
                                   make_shared<typename On_msg_handler_ptr::element_type>(std::move(on_msg_func)));

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
  if (!check_phase_and_prior_error(Phase::S_LOGGED_IN, "expect_msgs()"))
  {
    return false;
  }
  // else

  return expect_msgs_impl(qd_msgs,
                          false, // Open-ended expectation (0+ messages expected).
                          which,
                          make_shared<typename On_msg_handler_ptr::element_type>(std::move(on_msg_func)));
}

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename On_msg_handler>
bool CLASS_SIO_STRUCT_CHANNEL::expect_log_in_request(Msg_which_in which, Msg_in_ptr* qd_msg,
                                                     On_msg_handler&& on_log_in_req_func)
{
  using boost::make_shared;

  assert(qd_msg);
  if (!check_phase_and_prior_error(Phase::S_SRV_LOG_IN, "expect_log_in_request()"))
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
                                   make_shared<typename On_msg_handler_ptr::element_type>
                                     (std::move(on_log_in_req_func)));
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
bool CLASS_SIO_STRUCT_CHANNEL::expect_msgs_impl(Msgs_in* qd_msgs, bool one_off, Msg_which_in which,
                                                On_msg_handler_ptr&& on_msg_func_moved)
{
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

  // Stats: total registration count + active gauge.
  if (one_off)
  {
    ++m_stats.m_rcv.m_expect_msg_count;
    ++m_stats.m_rcv.m_expect_msg_active;
  }
  else
  {
    ++m_stats.m_rcv.m_expect_msgs_count;
    ++m_stats.m_rcv.m_expect_msgs_active;
  }

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

      /* @todo This looks like a bug: This can only happen from expect_log_in_request(), but if it does happen then
       * there's no handlers_poll() here or up the call stack; so that error is not emitted.  If it were emitted,
       * as of this writing it would formally break contract (expect_*() should not sync-call error callback; only
       * start_and_poll() or on_active_ev_func() APIs should).  So we should fix this; while keeping in mind
       * the obscurity of this situation: ~100% of user-wranged `Channel`s are pre-logged-in; the session-master-channel
       * is really the reason log-in exists; and that thing is handled internally in ipc::session and simply won't
       * have the above silly bug.  Formally though a user has access to log-in functionality in Channel if they wish
       * to use it; so it is a bug.  (As of this writing a ticket is filed in official open-source project at least.) */

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

    Msg_in_ptr_uniq msg_in{std::move(msgs_in_q.front())};
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

    qd_msgs->emplace_back(Msg_in_ptr{std::move(msg_in)}); // Upgrade to shared_ptr<> by the way.

    // ...and un-insert the registered expectation, since it was immediately met.  @todo Can perf this up probably.
    m_rcv_expecting_msg_map.erase(exp_msg_it);

    // Stats.
    {
      ++m_stats.m_rcv.m_expect_q_immediate_pops; // Popped cached msg on registration.
      --m_stats.m_rcv.m_pending_msgs_depth;
      --m_stats.m_rcv.m_expect_msg_active; // One-off expectation immediately satisfied.
    }
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

  // Stats.
  {
    ++m_stats.m_rcv.m_expect_q_immediate_pops; // Popped (all) cached msgs on registration.  Still counts for 1 here.
    m_stats.m_rcv.m_pending_msgs_depth -= qd_msgs->size();
  }

  FLOW_LOG_TRACE("Popped all [" << qd_msgs->size() << "] cached in-message from queue.");

  // And leave m_rcv_expecting_msg_map alone: new element will stay there until undo_expect_msgs().

  return true;
} // Channel::expect_msgs_impl()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::undo_expect_msgs(Msg_which_in which)
{
  if (!check_phase_and_prior_error(Phase::S_LOGGED_IN, "undo_expect_msgs()"))
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

  --m_stats.m_rcv.m_expect_msgs_active;

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Attempt to erase in-message expectation "
                 "[" << int(which) << "]: Success.  Their total number is now "
                 "[" << m_rcv_expecting_msg_map.size() << "].");
  return true;
} // Channel::undo_expect_msgs()

TEMPLATE_SIO_STRUCT_CHANNEL
bool CLASS_SIO_STRUCT_CHANNEL::undo_expect_responses(msg_id_out_t originating_msg_id)
{
  assert((originating_msg_id != 0) && "send() would never set *id_unless_one_off = 0.");

  if (!check_phase_and_prior_error(Phase::S_LOGGED_IN, "undo_expect_responses()"))
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

  --m_stats.m_rcv.m_expect_response_sticky_active; // Only sticky (non-one-off) responses use undo.

  FLOW_LOG_TRACE("struc::Channel [" << *this << "]: Attempt to erase response expectation for sent "
                 "message with ID [" << originating_msg_id << "]: Success.  Their total number is now "
                 "[" << m_rcv_expecting_response_map.size() << "].");
  return true;
} // Channel::undo_expect_responses()

TEMPLATE_SIO_STRUCT_CHANNEL
template<typename On_unexpected_response_func_t>
bool CLASS_SIO_STRUCT_CHANNEL::set_unexpected_response_handler(On_unexpected_response_func_t&& on_func)
{
  if (!m_on_unexpected_response_func_or_empty.empty())
  {
    FLOW_LOG_WARNING("struc::Channel [" << *this << "]: Attempt to register unexpected-response handler, "
                     "when one is already set.");
    return false;
  }
  // else

  m_on_unexpected_response_func_or_empty = On_unexpected_response_func{std::move(on_func)};
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

  m_on_remote_unexpected_response_func_or_empty = On_remote_unexpected_response_func{std::move(on_func)};
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
  return check_phase_and_prior_error(Phase::S_LOGGED_IN, "session_token()") ? m_session_token
                                                                            : NULL_SESSION_TOKEN;
}

TEMPLATE_SIO_STRUCT_CHANNEL
const util::Process_credentials& CLASS_SIO_STRUCT_CHANNEL::remote_peer_process_credentials() const
{
  return (check_prior_error("remote_peer_process_credentials()") && m_peer_process_creds)
           ? *m_peer_process_creds
           : util::NULL_PROCESS_CREDENTIALS;
}

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::Handlers::Message::Message(Msg_in_ptr_uniq&& msg_in,
                                                     On_msg_handler_ptr&& on_msg_func_if_expected) :
  m_msg_in(std::move(msg_in)),
  m_on_msg_func_if_expected(std::move(on_msg_func_if_expected))
{}

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::Handlers::Message::execute(Channel* daddy)
{
  if (m_on_msg_func_if_expected)
  {
    /* Fast path: Report message via saved handler (response to request/expected notification).
     * Don't even TRACE-log (avoid log setup).
     *
     * Upgrade to shared_ptr when giving to user. */
    (*m_on_msg_func_if_expected)(Msg_in_ptr{std::move(m_msg_in)});
  }
  else // if (!m_on_msg_func_if_expected)
  {
    // Unexpected response: this is rare.  So setting up logging (and logging if it comes to it) is fine.
    FLOW_LOG_SET_CONTEXT(daddy->get_logger(), daddy->get_log_component());

    if (daddy->m_on_unexpected_response_func_or_empty.empty())
    {
      FLOW_LOG_TRACE("struc::Channel [" << *daddy << "]: Would invoke on-unexpected-rsp user handler; "
                     "but none is configured.  Restating some details: "
                     "Structured in-message ID "
                     "[" << Msg_in_impl<Msg_in>{ *m_msg_in }.id_or_none() << "] is a response to "
                     "alleged out-message ID "
                     "[" << Msg_in_impl<Msg_in>{ *m_msg_in }.originating_msg_id_or_none() << "] "
                     "which either never existed or is not registered as awaiting a response.");
    }
    else
    {
      FLOW_LOG_TRACE("struc::Channel [" << *daddy << "]: Invoking on-unexpected-rsp user handler.");
      daddy->m_on_unexpected_response_func_or_empty(std::move(m_msg_in));
    }
  }
} // Channel::Handlers::Message::execute()

TEMPLATE_SIO_STRUCT_CHANNEL
CLASS_SIO_STRUCT_CHANNEL::Handlers::Remote_unexpected_response::Remote_unexpected_response
  (msg_id_t originating_msg_id, std::string&& mdt_text) :
  m_originating_msg_id(originating_msg_id),
  m_mdt_text(std::move(mdt_text))
{}

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::Handlers::Remote_unexpected_response::execute(Channel* daddy)
{
  using std::string;

  // Unexpected response: this is rare.  So setting up logging (and logging if it comes to it) is fine.
  FLOW_LOG_SET_CONTEXT(daddy->get_logger(), daddy->get_log_component());

  string mdt_text{std::move(m_mdt_text)}; // For ultra-cleanliness (so both paths act the same w/r/t m_mdt_text).
  if (daddy->m_on_remote_unexpected_response_func_or_empty.empty())
  {
    FLOW_LOG_TRACE("struc::Channel [" << *daddy << "]: Would invoke on-remote-unexpected-rsp user handler; "
                   "but none is configured.  Restating the details: "
                   "out-message with ID [" << m_originating_msg_id << "]; metadata text = [" << mdt_text << "].");
  }
  else
  {
    FLOW_LOG_TRACE("struc::Channel [" << *daddy << "]: Invoking on-remote-unexpected-rsp user handler.");
    daddy->m_on_remote_unexpected_response_func_or_empty(m_originating_msg_id, std::move(mdt_text));
  }
} // Channel::Handlers::Remote_unexpected_response::execute()

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::Handlers::Error::execute(Channel* daddy)
{
  // Error: this is rare.  So setting up logging (and logging if it comes to it) is fine.
  FLOW_LOG_SET_CONTEXT(daddy->get_logger(), daddy->get_log_component());
  auto& err_func = daddy->m_on_err_func;

  FLOW_LOG_TRACE("struc::Channel [" << *daddy << "]: Invoking on-error user handler.");
  assert((!err_func.empty()) && "Bug?  Error shall be reported at most once.");
  err_func(daddy->m_channel_err_code_or_ok);

  err_func.clear(); // Save RAM.  Also help detect any breaking of promise that we'd invoke it at most once.
}

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::hose(const Error_code& err_code_not_ok, util::String_view context,
                                    bool lower_layer_originating)
{
  assert(err_code_not_ok && "hose() must be called with a truthy error code.");
  assert((!m_channel_err_code_or_ok)
         && "hose() must be called only when channel is not already hosed.  This is the single funnel "
              "for transitioning m_channel_err_code_or_ok from falsy to truthy.");

  m_channel_err_code_or_ok = err_code_not_ok; // It will now never change.

  /* We want to do this pretty much as soon as m_channel_err_code_or_ok has become truthy; anything -- including
   * but not necessarily limited to handler-posting/execution -- that could happen in-between = entropy.
   *
   * On lower-layer-originating hosing the relevant Owned_channel per-pipe sub-object has hosed/logged itself
   * already; otherwise the per-pipe sub-objects are still healthy and may not hose for a while, so snapshot
   * their stats too.
   *
   * @todo Technically, in the HAS_2_PIPES setup, if lower_layer_originating=true, then the other pipe would
   * still be healthy; yet in this logic we aren't going to print its stats -- though they're going to freeze
   * at this point, as we either operate all 1-2 pipes we own or none at any given time point -- until
   * dtor.  So for the cleanest possible behavior: `lower_layer_originating` and its forwardee should
   * really be about *which* pipe(s) (if any) should stat-log (maybe 2 bools to log_stats() and a 3-way
   * enum to us and handle_new_error().  There's definitely an elegant way to propagate that info, but it's
   * also definitely non-trivial effort for only a bit of added visibility. */
  log_stats(context, !lower_layer_originating);
}

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::log_stats(util::String_view context, bool log_owned_channel_stats) const
{
  using flow::util::stat::print;

  FLOW_LOG_INFO("struc::Channel [" << *this << "]: In context [" << context << "]: Stats: "
                "[" << print(m_stats) << "].");

  if (log_owned_channel_stats)
  {
    if constexpr(Owned_channel::S_HAS_BLOB_PIPE)
    {
      FLOW_LOG_INFO("struc::Channel [" << *this << "]: In context [" << context << "]: "
                    "Owned_channel blob-stats: snd[" << print(m_channel.blob_send_stats()) << "] "
                    "rcv[" << print(m_channel.blob_receive_stats()) << "].");
    }
    if constexpr(Owned_channel::S_HAS_NATIVE_HANDLE_PIPE)
    {
      FLOW_LOG_INFO("struc::Channel [" << *this << "]: In context [" << context << "]: "
                    "Owned_channel hndl-stats: snd[" << print(m_channel.native_handle_send_stats()) << "] "
                    "rcv[" << print(m_channel.native_handle_receive_stats()) << "].");
    }
  }
}

TEMPLATE_SIO_STRUCT_CHANNEL
const stat::Channel_stats& CLASS_SIO_STRUCT_CHANNEL::stats() const
{
  return m_stats;
}

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::stats_reset()
{
  flow::util::stat::stats_reset(&m_stats, stat::Channel_stats{});
}

TEMPLATE_SIO_STRUCT_CHANNEL
void CLASS_SIO_STRUCT_CHANNEL::stats_configure_rcv_one_off_request_rtt_histogram(size_t n_buckets,
                                                                                 util::Fine_duration bucket_sz)
{
  using flow::util::stat::Histogram_counter;
  using boost::chrono::round;
  using boost::chrono::microseconds;

  assert((!m_started_ops)
         && "stats_configure_rcv_one_off_request_rtt_histogram() must be called before start_and_poll().");
  const auto usec = round<microseconds>(bucket_sz).count();
  m_stats.m_rcv.m_histo_one_off_request_rtt_usec = Histogram_counter{n_buckets, usec, usec, 0};
}

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
