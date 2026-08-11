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

#include "ipc/transport/struc/struc_fwd.hpp"
#include "ipc/transport/protocol_negotiator.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::transport::struc
{

// Types.

/// `Channel` base that contains non-parameterized items such as tag types and constants.
class Channel_base
{
public:
  // Types.

  /**
   * Clarifying short-hand for outgoing-message IDs.  This is public for logging/reporting use only
   * (see set_remote_unexpected_response_handler()) as of this writing.
   */
  using msg_id_out_t = msg_id_t;

  /**
   * Tag type for ctor selection: Backing memory for serialization comes from fixed-size segment allocation(s) from
   * regular heap (`malloc()`); and similarly for deserialization.  Therefore the serialization is copied
   * into the Channel::Owned_channel transport in the Channel::send() invoker process;
   * then copied out of there by the opposing (receiving) process.  I.e., this mode is *not* zero-copy, in
   * both directions.
   *
   * More specifically: Heap_fixed_builder and Heap_reader are the serialization engines used.
   *
   * For concision, and to compile successfully, use the struc::Channel alias class template:
   * #Channel_via_heap.
   *
   * ### Relative benefits/limitations (versus SHM-based `Serializa_via_*_shm`s) ###
   * These are discussed in some detail in Heap_fixed_builder doc header.  Short version:
   * Pros:
   *   - It's simple internally and externally, and cleanup is 100% guaranteed (no kernel-persistent resources
   *     are involved in serialization of structured messages).
   *     - In terms of simplicity specifically: it is not required to operate within ipc::session at all;
   *       whereas `Serializa_via_*_shm` are so required.
   *       - Subtlety: One *can* operate the SHM-based builders without ipc::session.  To do so you'll have to
   *         use the struc::Channel ctor form wherein you provide your own builder and reader engines
   *         which you'll need to first create manually.  One cannot do so with Serialize_via_heap ctor form,
   *         however, which is intended for easiest possible struc::Channel setup.
   *
   * Cons:
   *   - It's not zero-copy.  A linear-time copy is involved on each side of a transmitted message's processing.
   *   - If a leaf is too large to fit into the underlying `Owned_channel` pipe, the transmission will fail,
   *     and the struc::Channel will be hosed.  I.e., it places limits on what can be transmitted, and the
   *     exact nature of these limits is not 100% exact/predictable.
   */
  struct Serialize_via_heap {};

  /**
   * Tag type for ctor selection: Backing RAM for serialization comes from a given session::Session's SHM arena
   * (of per-session scope) via exponential-growing-size segment allocation(s).  Subsequent deserialization in the
   * opposing process occurs from directly within the same backing RAM area(s).  Therefore the serialization
   * is *never* copied, period; only a short handle is internally copied into and out of the `Owned_channel` transport.
   * Conversely, the opposing process does the same as the sender, and we do the converse as the receiver.
   * I.e., this mode *is* zero-copy, in both directions.  Serialize_via_session_shm is usable only within the
   * ipc::session framework; if you are operating outside it then use the struc::Channel ctor form wherein the
   * builder/reader engines are supplied as args.
   *
   * More specifically, as of this writing: If the `Session` type in tag-form struc::Channel ctor is
   * session::shm::classic::Server_session or session::shm::classic::Client_session,
   * then shm::classic::Builder and shm::classic::Reader are the [de]serialization engines used.  If the
   * `Session` type in tag-form struc::Channel ctor is session::shm::arena_lend::jemalloc::Server_session
   * or session::shm::arena_lend::jemalloc::Client_session, then shm::arena_lend::jemalloc::Builder and
   * shm::arena_lend::jemalloc::Reader are the [de]serialization engines used.  The source arena for our serialization
   * (message building) comes from the `Session` impl supplied nearby in the ctor call; specifically its
   * `.session_shm()` arena.
   *
   * For concision, and to compile successfully, use the struc::Channel alias class template:
   * shm::classic::Channel or shm::arena_lend::jemalloc::Channel.
   *
   * Relative benefits/limitations
   * -----------------------------
   * ### As compared to Serialize_via_heap ###
   * Essentially they are the inverse of those of Serialize_via_heap above.
   *
   * ### shm::classic::Channel versus shm::arena_lend::jemalloc::Channel ###
   * The relative merits of the two SHM providers (SHM-classic versus SHM-jemalloc) in ipc::shm are discussed within
   * that namespace's docs.  Short version:
   *   - SHM-jemalloc is intended for safety (due to its asymmetrical internally-IPC-using nature) and efficiency of
   *     allocation (jemalloc is an industry-strength `malloc()` impl, which we've adapted to SHM use; whereas
   *     SHM-classic uses boost.ipc's relatively rudimentary default allocation algorithm (e.g., it does not do thread
   *     caching)).
   */
  struct Serialize_via_session_shm {};

  /**
   * Similar to Serialize_via_session_shm but assumes per-app-scope SHM-arena (as opposed to
   * per-session-scope) at compile time.  More specifically: The source arena for our serialization comes from the
   * `Session` impl supplied nearby in the ctor call; specifically its `.app_shm()` arena.  As for deserialization
   * on our end: we will be able to handle any objects sent from the other side, period.
   *
   * `Session` must be specifically *not* of the `"ipc::session::shm::arena_lend::*::Server_session"` variety;
   * `"ipc::session::shm::arena_lend::*::Client_session"` cannot be used (any attempt
   * will not compile).  This is because the latter lacks `.app_shm()` due to the nature of arena-lending
   * SHM providers, where each process allocates from an arena it maintains for itself (and from which
   * the other process can only borrow objects).  Some more notes on this are below in this doc header.
   *
   * For concision, and to compile successfully, use the struc::Channel alias class template:
   * shm::classic::Channel or shm::arena_lend::jemalloc::Channel.
   *
   * Relative benefits/limitations
   * -----------------------------
   * ### As compared to Serialize_via_heap ###
   * Essentially they are the inverse of those of Serialize_via_heap above.
   *
   * ### shm::classic::Channel versus shm::arena_lend::jemalloc::Channel ###
   * See Serialize_via_session_shm.
   *
   * In addition there is a *possibly* crucial difference in capabilities, depending on one's app needs
   * (it is mentioned above in this doc header):
   *   - With SHM-jemalloc (via shm::arena_lend::jemalloc::Channel), it is *not* possible
   *     to use this per-app-scope mode on the client side; only per-session scope is available (this is
   *     a consequence of its asymmetrical, safety-oriented design as of this writing).
   *     On the `Server_session` end it is possible to create out-messages in either scope however.
   *     - An attempt to use this per-app-scope mode with a `session::shm::arena_lend::jemalloc::Client_session`
   *       will be caught at compile time (it... won't compile).
   *     - Specifically, then, with SHM-jemalloc, in session-server use either Serialize_via_app_shm or
   *       Serialize_via_session_shm; in the opposing session-client use the latter only.  Deserialization will work
   *       on both sides; but serialization (building/sending) will work only for session-scope resources
   *       (again, attempt to do otherwise will not compile).
   */
  struct Serialize_via_app_shm {};

  // Constants.

  /// The sole value of the tag type Serialize_via_heap.
  static constexpr Serialize_via_heap S_SERIALIZE_VIA_HEAP = {};

  /// The sole value of the tag type Serialize_via_session_shm.
  static constexpr Serialize_via_session_shm S_SERIALIZE_VIA_SESSION_SHM = {};

  /// The sole value of the tag type Serialize_via_app_shm.
  static constexpr Serialize_via_app_shm S_SERIALIZE_VIA_APP_SHM = {};

  /**
   * Minimum protocol version supported internally by struc::sync_io::Channel and derivative(s).
   * Public for logging/reporting only; really it is an internally used only value.
   */
  static constexpr Protocol_negotiator::proto_ver_t S_PROTOCOL_MIN_VERSION = 2;

  /**
   * Maximum protocol version supported internally by struc::sync_io::Channel and derivative(s).
   * Public for logging/reporting only; really it is an internally used only value.
   *
   * It is currently the same as #S_PROTOCOL_MIN_VERSION, as so far we've kept it intentionally simple to not
   * deal with backward-compatibility.
   *
   * @internal
   * See sync_io::Channel doc header impl section "Protocol negotiation" for our strategy around this.
   */
  static constexpr Protocol_negotiator::proto_ver_t S_PROTOCOL_MAX_VERSION = S_PROTOCOL_MIN_VERSION;

  /**
   * For struc::Channel::sync_request() (and any other `sync_*()` APIs, that is potentially-blocking ops),
   * how often the op shall, while awaiting completion or channel error, internally check that the opposing process
   * still exists (and if not, exit and emit transport::error::Code::S_PEER_PROCESS_NO_LONGER_EXISTS).
   *
   * Public for logging/reporting only; really it is an internally used only value.
   *
   * ### Rationale / Explanation ###
   * The mechanism tuned by this knob is a back-stop/backup mechanism employed by each `struc::Channel::sync_...()`
   * operation; every effort is made to not need to use it and to return an appropriate channel-hosed error
   * (such as error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE) ~insantly upon the hosing of the channel by the
   * peer process.
   *
   * To wit:
   *   - If you use a with-timeout overload of the `sync_...()` op in question, and that timeout is exceeded,
   *     the `sync_...()` shall always return appropriately.  The present mechanism is in that situation irrelevant.
   *     From this point on, let's assume either one used a no-timeout overload; or a long-enough timeout to where
   *     it is desirable to return from `sync_...()` closer chronologically to a problem occurring in the opposing
   *     process than simply awaiting said long timeout.
   *   - A properly written (with Channel::async_end_sending() and Channel dtor both used per their docs)
   *     use of `Channel` on the opposing side, without the opposing process crashing (aborting),
   *     will make the present mechanism irrelevant.  error::Code::S_RECEIVES_FINISHED_CANNOT_RECEIVE shall be emitted
   *     responsively.
   *   - Even without `async_end_sending()`, if the opposing process does not crash (abort), the present
   *     mechanism will still be irrelevant.  `boost::asio::error::eof` or equivalent shall be emitted responsively.
   *   - In a properly functioning system, if the opposing side does crash, at least for some Channel::Owned_channel
   *     types -- including ones with at least a Native_socket_stream pipe -- an error shall ~instantly propagate
   *     to our process; thus the present mechanism will again be irrelevant.  `eof` or equivalent shall again be
   *     emitted responsively.
   *
   * Unfortunately:
   *   - Sometimes an opposing process shall indeed crash (abort).
   *   - When this happens, and there is no Native_socket_stream in the relevant Channel::Owned_channel, and
   *     there is no conceptually similar pipe -- one such that the opposing crash would reponsively propagate
   *     an error along the now-defunct `Owned_channel` -- available in `Owned_channel`...
   *     - ...then we `sync_...()` op will hang, either indefinitely or until the (earlier-assumed undesirable large)
   *       timeout given as arg to `sync_...()`.
   *   - We have also observed, in the field in production, that certain mysterious (as of this writing) circumstances
   *     can cause even `Native_socket_stream` to not detect an error, if the opposing process crashes.  An `eof`
   *     should be emitted responsively, but we have seen this not happen at times, much to our frustration...
   *     - ...in which case, again, `sync_...()` will hang indefinitely or too-long.
   *
   * For that reason, we internally use this back-stop/backup mechanism to still detect the death (crash) of
   * the opposing process.  Specifically:
   *   - Instead of, internally, issuing an indefinite-or-up-to-long-user-specified-timeout wait...
   *   - ...we break it up into successive waits up to the period #S_SYNC_OP_PEER_PROCESS_LIVENESS_CHECK_PERIOD.
   *     - This period shall be sufficiently long so as to make the periodic wakeups negligible in terms of compute
   *       used.
   *     - It will be sufficiently short for a human to perceive the detection of opposing-peer-death as
   *       reasonably responsive.  So, it would be a fraction of a second.
   *   - Therefore, after the first and each successive such wait, we check whether the opposing process is alive
   *     according to the OS.  (As of this writing the mechanism used for this is util::process_running().)  If so,
   *     `sync_...()` shall immediately emit transport::error::Code::S_PEER_PROCESS_NO_LONGER_EXISTS (hence return or
   *     throw).
   *
   * This mechanism is not perfect:
   *   - It is not maximally responsive; a value such as 500msec is quick-enough but not sub-millisecond as with
   *     proper OS-driven error propagation.
   *   - It will not (as of this writing) detect a zombified or otherwise stalled opposing process
   *     (unlike official zombification combined with OS-driven error propagation).  util::process_running() does not
   *     detect those cases.
   *   - Upon detecting opposing process death, the emitted error -- unlike the OS-propagated ones -- does not
   *     "stick" to the `Channel` object.  One *should* stop using that `Channel`; but if one does not, further attempts
   *     to use it may not obviously and quickly fail.
   *     - (That said, in our experience, ipc::session will always report any opposing process death/zombification/stall
   *       appropriately quickly.  The problem, typically, is to get out of the hanging `sync_...()` first so as to
   *       be able to react to such a report.  The present mechanism will do that.)
   *
   * However, it can help significantly when needed; and when not needed, it does no harm (basically a periodic
   * internal no-op inside the given `sync_...()`).
   */
  static constexpr util::Fine_duration S_SYNC_OP_PEER_PROCESS_LIVENESS_CHECK_PERIOD = boost::chrono::milliseconds(500);
}; // class Channel_base

} // namespace ipc::transport::struc
