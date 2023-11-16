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

namespace ipc::transport::struc
{

// Types.

/// `Channel` base that contains non-parameterized `public` items such as tag types and constants.
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
}; // class Channel_base

} // namespace ipc::transport::struc
