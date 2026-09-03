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
#include "ipc/transport/struc/schema/detail/structured_msg.capnp.h"
#include "ipc/common.hpp"

namespace ipc::transport::struc
{

// Types.

template<typename Base_t>
struct Msg_out_impl;
template<typename Base_t>
struct Msg_in_impl;

// Free functions.

/**
 * Given user- or internal-message-describing metadata values as arguments, loads (via the provided
 * `capnp::MessageBuilder`) a `schema::detail::StructuredMessage` capnp-`struct` so as to contain those values.
 * The resulting `MessageBuilder` a/k/a #Capnp_msg_builder_interface can then be serialized as an mdt header
 * for an out-message.
 *
 * The out-arg `*builder` may be reused for performance; meaning if it encoded mdt for out-message X earlier,
 * it can be reused to encode mdt for out-message Y now with higher perf than if a fresh `*builder` had to be
 * created.  There are however the following limitations.
 *   - If you're loading mdt for an *internal* message (hence `internal_msg_builder_or_null != null`, and
 *     `id_or_none == 0`), then `*builder` must be fresh.  Else behavior is undefined.
 *   - Else (it's a user message), and indeed `*builder` is previously-`load_mdt()`ed:
 *     - `*builder` must have been used to also load a user message's MDT.  Else behavior is undefined.
 *     - Typically reuse is possible.  If so load_mdt() shall return `true` indicating success.
 *     - In some rare cases it is not possible.  If so load_mdt() shall return `false` indicating reuse failure.
 *       You should in that case try again, passing-in a fresh `*builder`.  We'll return `true` that time.
 *
 * In other words:
 *   - If `*builder` is fresh, all is cool; we'll return `true`.
 *   - If `*builder` is being reused:
 *     - You must be trying to load a user message's mdt, not an internal message.
 *     - `*builder` must have been `load_mdt()`ed for the same purpose (user message, not internal message).
 *     - If reuse is possible (typical), we'll return `true`.
 *     - If reuse is not possible (atypical), we'll return `false`.  Try again with fresh `*builder`.
 *
 * ### Fatal error possibility ###
 * The above ignores the possibility of fatal error, meaning an input such that for internal reasons load_mdt()
 * cannot work, regardless of `*builder` freshness.  So far we've described these:
 *   - If `false` returned: no fatal error; try with fresh `*builder` as explained.
 *   - If `true` returned, and `*err_code` is falsy post-call: no fatal error; all good as explained.
 *
 * That leaves this one:
 *   - If `true` returned, and `*err_code` is truthy post-call: fatal error; recommend emitting to user.
 *     - This shall *not* occur if `id_or_none == 0` (for internal messages).  Only user messages have enough
 *       degrees of freedom as to overload the mdt conventions we've established internally.
 *
 * @param builder
 *        A `MessageBuilder`.  If `id_or_none == 0`, it must be fresh (not previously `load_mdt()`ed).
 *        Else it may be fresh or previously `load_mdt()`ed; in the latter case `id_or_none != 0` had to have been
 *        the case as well. / If we return `true`, `*builder` will have been loaded with a filled-out
 *        `schema::detail::StructuredMessage`.
 * @param internal_msg_builder_or_null
 *        Must be null if and only if `id_or_none != 0`, indicating we're loading a user message's mdt.  Else
 *        we're loading an internal message.  In that case the post-condition is: we return `true` (always succeed); and
 *        `*internal_msg_builder_or_null` is a capnp-`Builder` for the
 *        `schema::detail::StructuredMessage::InternalMessageBody` node inside `*builder`.  It is the return value of
 *        `.initInternalMessageBody()`, but that node's insides have not been filled further.  The caller should
 *        use the `Builder` to set the particulars of the internal message per schema (see structured_msg.capnp).
 * @param err_code
 *        To set, if and only if returning `true` (see above).  If null behavior is undefined (assertion may trip).
 *        As of this writing generated codes:
 *        error::Code::S_SERIALIZE_FAILED_SPLIT_ENCODING_TOO_BIG (a Split_segment record's field's value does not
 *        fit the chosen capnp-encoding type's bit-width);
 *        error::Code::S_SERIALIZE_FAILED_SPLIT_SEGS_TOO_MANY (the mdt in total, due to its variable-length
 *        parts -- in practice the `*split_segs`-encoding list as of this writing -- does not fit `*builder`'s
 *        fixed-size area; a 2nd segment would be needed => this error).
 * @param session_token
 *        The token to load into `*builder`.
 * @param originating_msg_id_or_none
 *        Message ID of the in-message to which this one responds or which this one otherwise references; or 0
 *        if none.
 * @param id_or_none
 *        Message ID of the user message whose mdt we're loading; or 0 to indicate we're loading an internal message.
 * @param n_serialization_segments_or_none
 *        Must be 0 if and only if `id_or_none == 0`, indicating we're loading an internal message.  Else we're
 *        loading a user message.  In that case this is the number of (sub-)segments composing the user message's
 *        payload (this is 1 or higher).  Some of these (sub-)segments may need to be re-joined by the receiver before
 *        being used as actual capnp-serialization segments; in which case this info is in `split_segs`.
 * @param split_segs
 *        Must be null and is otherwise ignored if `id_or_none == 0` (internal message case).  Else it's a
 *        user message's mdt; in which case: If `split_segs` is null, then no splitting of segments was needed, hence
 *        no re-joining is needed on the opposing side; otherwise it is a list (by convention of size at least 1)
 *        describing the split structure.  See #Split_segments doc header for details.
 * @return See above: `true` means success, `false` means retry sans reuse.
 */
bool load_mdt(Capped_sz_capnp_message_builder* builder,
              schema::detail::StructuredMessage::InternalMessageBody::Builder* internal_msg_builder_or_null,
              Error_code* err_code,
              const Session_token& session_token,
              msg_id_t originating_msg_id_or_none,
              msg_id_t id_or_none = 0, size_t n_serialization_segments_or_none = 0,
              const Split_segments* split_segs = nullptr);

} // namespace ipc::transport::struc
