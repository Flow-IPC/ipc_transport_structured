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

#include "ipc/transport/transport_fwd.hpp"
#include <flow/util/blob.hpp>
#include <capnp/message.h>
#include <boost/uuid/uuid.hpp>

/**
 * Sub-module of Flow-IPC module ipc::transport providing transmission of structured messages specifically.
 * See ipc::transport doc header.  As that notes, the big daddy here is struc::Channel.
 *
 * Be aware of sub-namespace ipc::transport::struc::shm which concerns itself with end-to-end-zero-copyable
 * messages leveraging in-SHM storage.  Normally there's no need for the user to know or worry about it,
 * but for advanced applications, particularly extensions and customizations, one might delve into this area of
 * the code.  Otherwise it'll be used silently as important glue between various systems.  Your journey would
 * like start with the struc::Builder concept doc header which would then lead you to
 * struc::shm::Builder impl.  That might lead you to specific SHM-providers like ipc::shm::classic.
 */
namespace ipc::transport::struc
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Message_body, typename Struct_builder_config>
class Msg_out;
template<typename Message_body, typename Struct_reader_config>
class Msg_in;

class Channel_base;
template<typename Channel_obj, typename Message_body,
         typename Struct_builder_config, typename Struct_reader_config>
class Channel;

class Heap_fixed_builder;
class Heap_reader;

struct Null_session;

template<typename Capnp_reader>
struct Ostreamable_capnp_brief;
template<typename Capnp_reader>
struct Ostreamable_capnp_full;

/// Alias for capnp's MessageBuilder interface.  Rationale: as part of our API, we use our identifier style.
using Capnp_msg_builder_interface = ::capnp::MessageBuilder;

/// Alias for capnp's MessageReader interface.  Rationale: as part of our API, we use our identifier style.
using Capnp_msg_reader_interface = ::capnp::MessageReader;

/**
 * A type used by struc::Channel for internal safety/security/auth needs.  See in particular
 * struc::Channel constructors, both the regular-channel one and the session-master-channel one.
 * That said: The user of ipc::transport (aided by ipc::session) structured layer shall generally be unconcerned
 * with this, in practice, as ipc::session machinery will take care of:
 *   - setting up the session master struc::Channel which will generate this token during initial setup (log-in);
 *     and
 *   - setting up further `struc::Channel`s as requested by the user (the preceding bullet's token shall be
 *     passed to each new struc::Channel and expected internally in all messages).
 *
 * However, if one plans to *create* a struc::Channel directly (as ipc::session does), then one must have some
 * limited understanding of this guy's existence (if not the internal details).
 *
 * @internal
 * As you can see: internally it is a UUID (RFC 4122 Universally Unique IDentifier) which is 128 bits (16 bytes)
 * in size.  It's also a POD (Plain Old Data object), meaning it's ctor-free (direct-initialized) and can be
 * `memcpy()`d around and such.  See boost.uuid docs (which are simple).
 *
 * ### Use and rationale thereof ###
 * A session master channel struc::Channel, operating in server mode during the log-in phase, will (as of
 * this writing anyway) generate this using the boost.uuid random UUID generator.  From that point on all
 * channels spawned through that guy (by ipc::session) will be cted with that token value; and all messages both
 * in those channels and the original master channel (after the log-in message exchange) will send this value
 * and ensure any in-message has that value (or immediately hose the channel -- auth failure).  This is a
 * safety (not really security, due to the lack of token invalidation at least) measure.  It will reduce chances
 * that "wires get crossed," and a message goes into the wrong channel somehow.
 *
 * That's how it's used.  Is the random-generator method of UUID generation acceptable for this use?
 *
 * A bit of research (starting in the RFC perhaps; or Wikipedia) shows that collisions are so cosmically rare as to
 * make it, quite officially, a non-concern.  Even if one does not accept that, however, suppose a collision does
 * occur: two channels happen to generate the same UUID on the same machine around the same time.  This (essentially
 * impossible) scenario would not cause either channel to break; it would only allow (in theory) a token check
 * to succeed where it shouldn't, if the fabled wires-crossed scenario occurred.  This is an acceptable level of risk.
 *
 * Conversely, perhaps using UUIDs for this is overkill.  128 bits isn't heavy, but perhaps something smaller --
 * maybe even much smaller, like 32 bits or even 16 -- would improve perf without really reducing effectiveness
 * given the above rationale.  Indeed that's worth considering but only in the phase where perf optimization is
 * the explicit goal.  Until then (if that even happens) the ability to use the nice boost.uuid library instead
 * of rolling our own jankier/smaller UUID-like thing (or whatever) is well worth the alleged perf cost.
 *
 * @todo Look into whether something smaller that RFC 4122 UUIDs can and should be used for #Session_token.
 * This would be for perf but may well be unnecessary.  See discussion near this to-do.
 */
using Session_token = boost::uuids::uuid;

/**
 * Sequence of 1+ `Blob` *pointers* to blobs which must stay alive while these pointers may be dereferenced,
 * intended here to refer to a capnp serialization of a capnp-`struct`.  In each `Blob` [`begin()`, `end()`)
 * is the serialization itself; and space before `begin()` and starting with `end()` may
 * be reserved for framing prefix/postfix to preserve zero-copy when transmitting such serializations over
 * an IPC "wire."
 */
using Segment_ptrs = std::vector<flow::util::Blob*>;

/**
 * Message ID uniquely identifying outgoing message (Msg_out, among all other `Msg_out`s), per channel; and
 * similarly incoming message (Msg_in, among `Msg_in`s), per channel.  0 is a sentinel value and not a valid
 * user message ID.  A message ID pertains to a *sent* or *received* *instance* of a user-created Msg_out
 * (and its in-message countrepart Msg_in).  A given Msg_out can be sent 2+ times through a given
 * struc::Channel and even a *different* struc::Channel; a different message ID will pertain to each
 * of those times for a given struc::Channel.  Therefore there is no #msg_id_t *inside* a user Msg_out.
 *
 * This type is in the public API, as the message ID is made available in certain contexts for:
 *   - referring to an earlier-sent message, such as in struc::Channel::undo_expect_responses(); and
 *   - logging/reporting.
 *
 * @internal
 * It can also be used as a sequence number and is therefore assigned from sequence 1, 2, ... (0 is sentinel).
 *
 * ### Rationale for type used ###
 * Needs to be big enough to where there's no chance it overflows in a given channel for a given direction.
 * Assume 1 billion messages per second, or about 2^30 msg/s.  That would take 2^(64-30), or 2^34, seconds,
 * or over 500 years, to overflow.  So this should be fine.  Moreover overflow is fine in that case, in practice,
 * if this is used only as a unique ID; it would however in theory present problems if used as a sequence number.
 */
using msg_id_t = uint64_t;

// Constants.

/// A value for which `.is_nil()` is true.
extern const Session_token NULL_SESSION_TOKEN;

/// The only necessary value of empty-type Null_session.
extern const Null_session NULL_SESSION;

// Free functions.

/**
 * Convenience function that returns an object passable to `ostream<<` to print a
 * one-line/potentially-truncated representation to that stream, given an arbitrary capnp `Reader`.
 *
 * @param capnp_reader
 *        The `Reader` to presumably print.  Tip: if you have a `Builder builder` to print then pass
 *        `builder.asReader()` here.
 * @return See above.
 */
template<typename Capnp_reader>
Ostreamable_capnp_brief<Capnp_reader> ostreamable_capnp_brief(const Capnp_reader& capnp_reader);

/**
 * Convenience function that returns an object passable to `ostream<<` to print a
 * multi-line/pretty-printed/indented/full-length representation to that stream, given an arbitrary capnp `Reader`.
 *
 * @param capnp_reader
 *        The `Reader` to presumably print.  Tip: if you have a `Builder builder` to print then pass
 *        `builder.asReader()` here.
 * @return See above.
 */
template<typename Capnp_reader>
Ostreamable_capnp_full<Capnp_reader> ostreamable_capnp_full(const Capnp_reader& capnp_reader);

/**
 * Prints string representation (one-line/potentially-truncated form) of the given capnp `Reader`, via proxy object,
 * to the given `ostream`.
 *
 * @warning Potentially the entire underlying capnp tree shall be traversed to make the output work
 *          (even if ultimately the output is truncated for length).  This can be quite slow.  Do not use in
 *          perf-critical paths, unless verbose-logging (etc.) is enabled.
 *
 * @see ostreamable_capnp_brief(): Wrap your `Reader capnp_reader` (or `builder.asReader()`) in this call
 *      to make use of this output operator conveniently.
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Capnp_reader>
std::ostream& operator<<(std::ostream& os, const Ostreamable_capnp_brief<Capnp_reader>& val);

/**
 * Prints string representation (multi-line/pretty-printed/indented/full-length form) of the given capnp `Reader`,
 * via proxy object, to the given `ostream`.
 *
 * @warning Potentially the entire underlying capnp tree shall be traversed to make the output work.
 *          This can be quite slow.  Do not use in perf-critical paths, unless verbose-logging (etc.) is enabled.
 *
 * @see ostreamable_capnp_full(): Wrap your `Reader capnp_reader` (or `builder.asReader()`) in this call
 *      to make use of this output operator conveniently.
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Capnp_reader>
std::ostream& operator<<(std::ostream& os, const Ostreamable_capnp_full<Capnp_reader>& val);

/**
 * Prints string representation of the given struc::Channel to the given `ostream`.
 *
 * @relatesalso struc::Channel
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Channel_obj, typename Message_body,
         typename Struct_builder_config, typename Struct_reader_config>
std::ostream& operator<<(std::ostream& os,
                         const Channel<Channel_obj, Message_body,
                                       Struct_builder_config, Struct_reader_config>& val);

/**
 * Prints string representation of the given `Msg_out` to the given `ostream`.
 * Namely it prints via Msg_out::to_ostream()`; be sure to read perf notes on that!
 *
 * @relatesalso Msg_out
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Message_body, typename Struct_builder_t>
std::ostream& operator<<(std::ostream& os, const Msg_out<Message_body, Struct_builder_t>& val);

/**
 * Prints string representation of the given `Msg_in` to the given `ostream`.
 *
 * @relatesalso Msg_in
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Message_body, typename Struct_reader_config>
std::ostream& operator<<(std::ostream& os, const Msg_in<Message_body, Struct_reader_config>& val);

/**
 * Prints string representation of the given `Heap_fixed_builder` to the given `ostream`.
 *
 * @relatesalso Heap_fixed_builder
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Heap_fixed_builder& val);

/**
 * Prints string representation of the given `Heap_reader` to the given `ostream`.
 *
 * @relatesalso Heap_reader
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Heap_reader& val);

} // namespace ipc::transport::struc

/**
 * `sync_io`-pattern counterparts to async-I/O-pattern object types in parent namespace ipc::transport::struc.
 * Namely transport::struc::sync_io::Channel <=> transport::struc::Channel.
 *
 * @see util::sync_io doc header -- describes the general `sync_io` pattern we are following.
 */
namespace ipc::transport::struc::sync_io
{

// Types.

template<typename Channel_obj, typename Message_body,
         typename Struct_builder_config, typename Struct_reader_config>
class Channel;

// Free functions.

/**
 * Prints string representation of the given struc::sync_io::Channel to the given `ostream`.
 *
 * @relatesalso struc::sync_io::Channel
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
template<typename Channel_obj, typename Message_body,
         typename Struct_builder_config, typename Struct_reader_config>
std::ostream& operator<<(std::ostream& os,
                         const Channel<Channel_obj, Message_body,
                                       Struct_builder_config, Struct_reader_config>& val);

} // namespace ipc::transport::struc::sync_io

/**
 * Small group of miscellaneous utilities to ease work with capnp (Cap'n Proto), joining its `capnp` namespace.
 * As of this writing circumstances have to very particular and rare indeed for something to actually be added
 * by us to this namespace.  As I write this, it contains only `oparator<<(std::ostream&, capnp::Text::Reader&)`.
 */
namespace capnp
{

/**
 * Prints string representation of the given `capnp::Text::Reader` to the given `ostream`.
 *
 * ### Rationale for existence ###
 * Well, capnp does not provide it, even though it provides a conversion operator to `string` and such; and
 * it's nice to be able to print these (particularly when logging with `FLOW_LOG_*()`).
 * For whatever reason the auto-conversion operator does not "kick in" when placed in a
 * an `ostream<<` context; so such printing does not compile (nor does `lexical_cast<string>`).
 *
 * It won't be forced on anyone that doesn't include the present detail/ header.
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Text::Reader& val);

} // namespace capnp
