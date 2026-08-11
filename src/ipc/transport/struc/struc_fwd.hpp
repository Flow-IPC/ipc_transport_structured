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
#include "ipc/util/util_fwd.hpp"
#include <flow/util/blob.hpp>
#include <flow/util/stat/stat_set_list.hpp>
#include <capnp/message.h>
#include <boost/uuid/uuid.hpp>
#include <string>
#include <vector>

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

template<typename Body_t, typename Builder_config_t>
class Msg_out;
template<typename Body_t, typename Reader_config_t>
class Msg_in;
class Capped_sz_capnp_message_builder;
class Capped_sz_capnp_message_reader;
class Channel_base;
template<typename Owned_channel_t, typename Msg_body_t,
         typename Builder_config_t, typename Reader_config_t>
class Channel;
class Heap_fixed_builder;
class Heap_reader;
struct Split_segment;
struct Null_session;

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
 * @todo Look into whether something smaller than RFC 4122 UUIDs can and should be used for #Session_token.
 * This would be for perf but may well be unnecessary.  See discussion near this to-do.
 */
using Session_token = boost::uuids::uuid;

/**
 * Sequence of 1+ segment descriptions, each representing a live writable segment (of a capnp serialization of a
 * capnp-`struct`) and optional header/prefix area immediately preceding it.  As of this writing this is specifically
 * 1+ `Basic_blob` *pointers* to blobs that directly represent those segments+header(s) which must stay alive while
 * these pointers may be dereferenced.  In each `Basic_blob` [`begin()`, `end()`)
 * is the serialization segment itself; and space before `begin()` (of size `start()`) may
 * be reserved for framing prefix/header to preserve zero-copy when transmitting such serializations over
 * an IPC "wire."  (Area after `end()` up to `capacity()` could represent a framing postfix, though as of this writing
 * we assume only a header might exist, for simplicity.  As long as possible we shall avoid adding a postfix into the
 * design.)
 *
 * ### Rationale ###
 * Why represent the segment+header as `Basic_blob` specifically?  Answer: The relevant serialization engine(s) could
 * internally represent data structures (namely segments and frame-prefix and such) however they want, and indeed by
 * choosing `Basic_blob*` here essentially we force that impl to be specifically that.  That is a very reasonable
 * structure (and less-feature-filled, slightly-highly-performing than the derivative `Blob`),
 * but why force it?  Indeed, as regards a public API, something merely describing locations and sizes *inside*
 * the data structure of choice would be stylistically more direct and therefore more flexible/better.  To wit
 * #Segment_bufs (sequence of pointer + size pairs essentially) is almost such a thing; just it's not *quite* enough,
 * because it does not directly/intuitively represent the header/prefix areas that might precede segments (but
 * instead merely the segments themselves).  So we choose `Basic_blob*` simply
 * because it is (1) a decent choice for all impls in practice and (2) sufficiently-well describes both
 * segments and frame headers nearby.  It is slightly inflexible but acceptable overall.
 *
 * @todo As per nearby Rationale comment, `Segment_ptrs` element type would ideally be a view *into* `Blob`-or-similar
 * instead of actual `Basic_blob*`, to decouple impl from API more cleanly.  One approach might be to develop a
 * `flow::util` class/template, a peer of the `flow::util::Blob` hierarchy, that specifically represents a view into a
 * `Blob`-type-thing (perhaps 1 variant for mutable, 1 variant for immutable), perhaps analogously to how
 * `span<uint8_t>` + `span<const uint8_t>` is each a view into `vector<uint8_t>` (minus the latter's capacity).
 * It would therefore store a pointer, a size, a capacity, and a start-index -- representing a main buffer (`size()`
 * in size), N-byte header/prefix (N = `start()`), and M-byte postfix (M = `capacity() - start() - size()`).
 * (A more economical buffer+header version -- directly applicable here -- might store all of those minus the
 * capacity, meaning there is no postfix area.)  Possible names might be `Blob_view_const` (read-only view
 * into buffer plus header plus postfix), `Blob_view_mutable` (same plus writable access),
 * `Prefixed_byte_span_const` (`Blob_view_const` minus postfix area), `Prefixed_byte_span_mutable` (`Blob_view_mutable`
 * minus postfix area).  #Segment_ptrs would likely be `vector<Prefixed_byte_span_mutable>`.
 */
using Segment_ptrs = std::vector<flow::util::Blob_sans_log_context*>;

/**
 * Simpler cousin of #Segment_ptrs, this represents the IPC-transmissible representation of a capnp serialization
 * of a capnp-`struct` including any framing header(s), expressed as simply a sequence of N read-only views, each
 * containing a pointer to header-if-any-followed-by-serialization-segment and the size in bytes thereof.
 *
 * Note that while #Segment_ptrs contains info on header size, this `Segment_bufs` does not: So to properly interpret
 * the given `Blob_const` element, one must know the size of the header therein (if any; by convention we can say that
 * no header => header size is 0 => segment = entire `Blob_const`).
 */
using Segment_bufs = std::vector<util::Blob_const>;

/**
 * A simple data structure describing the segments (if any) in a #Segment_bufs (or similar) structure which had to
 * be split (presumably due to size constraints); by following the indices/sizes into a post-split #Segment_bufs one
 * can join the appropriate elements so as to obtain the original pre-split segments.
 *
 * The order of `Split_segment`s is strictly from left to right.  A reasonable join algorithm, therefore, would
 * iterate over a `Split_segments` in *reverse* order; for each Split_segment join the 2+ segments in the post-split
 * #Segment_bufs (or similar) described therein, in-place.  By doing this in reverse order, handling Split_segment
 * A before B will not invalidate the start-index member in B.
 *
 * By convention #Split_segments shall include a Split_segment only if a segment actually needed to be split.
 */
using Split_segments = std::vector<Split_segment>;

/**
 * Short-hand for a high-performance blob structure that represents an IPC-incoming (sub-)segment 1+ of which
 * make up the outer serialization of a given Msg_in.  Its *exact* semantics are context-dependent, so it's best
 * to read the API or context to determine such details.
 */
using Segment_blob_in = flow::util::Blob_sans_log_context;

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

/// Alias for capnp's MessageBuilder interface.  Rationale: as part of our API, we use our identifier style.
using Capnp_msg_builder_interface = ::capnp::MessageBuilder;

// Constants.

/// A value for which `.is_nil()` is true.
extern const Session_token NULL_SESSION_TOKEN;

/// The only necessary value of empty-type Null_session.
extern const Null_session NULL_SESSION;

/**
 * struc::Channel (and struc::sync_io::Channel) requires, for internal purposes, that any Msg_out transmitted over it
 * was provided with a Struct_builder::Config that has `m_frame_prefix_sz` member set to this value (or larger if
 * desired for some purpose).  As of this writing that includes Heap_fixed_builder::Config::m_frame_prefix_sz
 * (for heap-backed messages) and shm::Builder::Config::m_frame_prefix_sz (for SHM-backed ones).
 *
 * ### Rationale ###
 * In typical usage patterns one need not assemble a Struct_builder::Config explicitly, but nothing forbids it,
 * hence it seemed prudent to expose this publicly.
 *
 * Note that `Struct_builder`s and hence their `Config`s (formally speaking) may be used independently of
 * struc::Channel, so it *is* conceivable to not need this value set (e.g., `0` would be quite conceivable).
 * (In fact internally Flow-IPC might use these guys in various capacities and may well set `.m_frame_prefix_sz = 0`
 * in some contexts.)  That is to say, `m_frame_prefix_sz` should *not* simply be removed from our APIs and replaced by
 * a constant all-over.
 */
static constexpr size_t BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL = 128;

// Free functions.

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
template<typename Owned_channel_t, typename Msg_body_t,
         typename Builder_config_t, typename Reader_config_t>
std::ostream& operator<<(std::ostream& os,
                         const Channel<Owned_channel_t, Msg_body_t,
                                       Builder_config_t, Reader_config_t>& val);

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
template<typename Body_t, typename Struct_builder_t>
std::ostream& operator<<(std::ostream& os, const Msg_out<Body_t, Struct_builder_t>& val);

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
template<typename Body_t, typename Reader_config_t>
std::ostream& operator<<(std::ostream& os, const Msg_in<Body_t, Reader_config_t>& val);

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

/// Stats-related sub-namespace, for ADL segregation and general organization.
namespace ipc::transport::struc::stat
{

// Types.

// Find doc headers near the bodies of these compound types.

struct Channel_sync_req_stats;
struct Channel_stats;
struct Histo_cfg;
struct Pure_heap_tag;
struct Serializer_stats;
template<typename Cfg>
struct Serializer_stats_p;
struct Heap_serializer_stats_cfg;

/// Default-constructible Serializer_stats variant for pure-heap user messages.  See Serializer_stats doc header.
using Heap_serializer_stats = Serializer_stats_p<Heap_serializer_stats_cfg>;

/**
 * Pure-heap (non-SHM) cumulative-#Heap_serializer_stats singleton: convenience alias for the default
 * `flow::util::stat::Global_stats` instantiated with Pure_heap_tag.
 *
 * @todo Consider changing definition of alias struc::stat::Heap_serializer_global_stats from using
 * `N == 1` to something larger, so that the user can optionally -- while still using this global stats-holder --
 * further categorize stats according to their custom needs.  Also consider similar steps in
 * such alias(es) in struc::shm::stat.  (Note various relevant APIs allow one to simply pass-in a
 * `Serializer_stats*` (et al) directly -- and the user can maintain their own `Global_stats` or
 * `static Stat_set_list` and lightning-quickly obtain the aforementioned `*` therefrom.  So flexibility already
 * exists on that front; but here we use a `Global_stats` for a reason already; can make use of it more fully
 * this way.)
 * One tactical idea is that factories like struc::Channel::heap_fixed_builder_config() can take an optional
 * arg, defaulting to 0, which would cause such a factory to select the `Global_stats` slot.  Slot 0 would
 * continue to, by convention be the default/catch-all one; and the user can designate other slots for their own
 * purposes.
 */
using Heap_serializer_global_stats = flow::util::stat::Global_stats<Pure_heap_tag, Heap_serializer_stats, 1>;

// Free functions.

/**
 * Grabs a snapshot of the process-global pure-heap serializer stats -- the #Heap_serializer_global_stats
 * singleton -- into `*target_stats` which can then be read/printed/aggregated at leisure.
 *
 * This covers all of serializer global-land for a *pure-heap* (non-SHM) application.  A SHM-backed application
 * has 2 more such global stat-sets; grab all 3 in one call via struc::shm::stat::serializer_info_dump() instead.
 *
 * (This does not affect any serializer object that has been redirected -- via the relevant builder/reader config
 * knobs -- to accumulate into a custom Serializer_stats instead of the default global.)
 *
 * @param target_stats
 *        The stats are assigned here (via `flow::util::stat::stats_assign()`).  Must not be null.
 */
void heap_serializer_info_dump(Heap_serializer_stats* target_stats);

/**
 * Prints string representation -- `flow::util::stat::print()` output -- of the given Serializer_stats
 * (or variant thereof, e.g. #Heap_serializer_stats) to the given `ostream`.
 *
 * @note To be honest `os << print(val)` is equally available, as for any `Stat_set`; we add this direct
 *       `operator<<` merely so that the heap_serializer_info_dump() experience is symmetrical with "true"
 *       info-dumping a-la struc::shm::stat::Serializer_info_dump (grab everything with one free function;
 *       print with `<<`) -- there is just one `struct` here, so it is simpler.
 *
 * @relatesalso Serializer_stats
 *
 * @param os
 *        Stream to which to write.
 * @param val
 *        Object to serialize.
 * @return `os`.
 */
std::ostream& operator<<(std::ostream& os, const Serializer_stats& val);

/**
 * Declares the stats for Channel_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix, const Channel_stats* src_stats, Channel_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Channel_sync_req_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Channel_sync_req_stats* src_stats, Channel_sync_req_stats* target_stats,
                   Visitor&& visitor);

/**
 * Declares the stats for Serializer_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Serializer_stats* src_stats, Serializer_stats* target_stats,
                   Visitor&& visitor);

} // namespace ipc::transport::struc::stat

/**
 * `sync_io`-pattern counterparts to async-I/O-pattern object types in parent namespace ipc::transport::struc.
 * Namely transport::struc::sync_io::Channel <=> transport::struc::Channel.
 *
 * @see util::sync_io doc header -- describes the general `sync_io` pattern we are following.
 */
namespace ipc::transport::struc::sync_io
{

// Types.

// Find doc headers near the bodies of these compound types.

template<typename Owned_channel_t, typename Msg_body_t,
         typename Builder_config_t, typename Reader_config_t>
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
template<typename Owned_channel_t, typename Msg_body_t,
         typename Builder_config_t, typename Reader_config_t>
std::ostream& operator<<(std::ostream& os,
                         const Channel<Owned_channel_t, Msg_body_t,
                                       Builder_config_t, Reader_config_t>& val);

} // namespace ipc::transport::struc::sync_io

/// Stats-related sub-namespace, for ADL segregation and general organization.
namespace ipc::transport::struc::sync_io::stat
{

// Types.

// Find doc headers near the bodies of these compound types.

struct Channel_stats;

// Free functions.

/**
 * Declares the stats for Channel_stats.  Not invoked directly except by `flow::util::stat` internals,
 * or when composing this stat-set into another.
 * @see `flow::util::stat` namespace doc header for background on the declare/visit mechanism.
 *
 * @tparam Visitor
 *         See above.
 * @param name_prefix
 *        See above.
 * @param src_stats
 *        See above.
 * @param target_stats
 *        See above.
 * @param visitor
 *        See above.
 */
template<typename Visitor>
void declare_stats(std::string name_prefix, const Channel_stats* src_stats, Channel_stats* target_stats,
                   Visitor&& visitor);

} // namespace ipc::transport::struc::sync_io::stat

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
