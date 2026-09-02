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

#include "ipc/transport/struc/heap_serializer.hpp"
#include "ipc/transport/struc/detail/struc_fwd.hpp"
#include "ipc/transport/struc/error.hpp"
#include <boost/endian.hpp>
#include <boost/integer.hpp>
#include <cstring>
#include <limits>
#include <utility>

namespace ipc::transport::struc
{

// Free function implementations.

bool load_mdt(Capnp_msg_builder_interface* builder,
              schema::detail::StructuredMessage::InternalMessageBody::Builder* internal_msg_builder_or_null,
              Error_code* err_code,
              const Session_token& session_token, msg_id_t originating_msg_id_or_none, msg_id_t id_or_none,
              size_t n_serialization_segments_or_none, const Split_segments* split_segs)
{
  using boost::endian::native_to_little;
  /* capnp doesn't put capnp-`using` aliases into generated code, so to get at SplitSegment (a scalar type)
   * grab the element type of the splitSegmentsOrNull list via its getter's return.
   * Careful: This is the thing storing *both* m_start_idx and m_n_cont_subsegs, so each of those has
   * 1/2 the bit width of split_seg_t. */
  using split_seg_t
    = decltype(std::declval<schema::detail::StructuredMessage::BodySerializationInfo::Reader>()
                 .getSplitSegmentsOrNull()[0]);
  /* This, then, is the C type corresponding to the capnp-type we use to store each of the aforementioned m_*.
   *
   * Please read the .capnp doc comment for `using SplitSegment`; it explains the strategy and tactics behind
   * (1) storing the two m_ in a single UInt??? and (2) the width ??? chosen for that type. */
  using split_seg_sz_t = boost::uint_t<std::numeric_limits<split_seg_t>::digits / 2>::exact;
  static_assert((sizeof(split_seg_sz_t) * 2) == sizeof(split_seg_t), "Gotta be a bug in the last 2 statements.");

  assert(err_code && "We are not public and are free to decide (and decided) not to have a throwing mode.");

  /* Typically one uses initRoot<>(), but here it would be wrong.  getRoot<X>() will be equivalent to initRoot<X>(),
   * if X has not yet been init-ed; or return the existing X, if it has.  We promised we would try to work in either
   * case.
   * (Though in that case it's their responsibility to ensure `id_or_none != 0`; and to be ready if we return `false`
   * indicating reuse is impossible in a certain other situation.) */
  auto msg_root = builder->getRoot<schema::detail::StructuredMessage>();

  /* Refer to structured_msg.capnp StructuredMessage.  It is *now* send time, so we fill in all the metadata needed.
   * Let's go over it:
   *   - authHeader.sessionToken: We set it here.
   *   - id: It is used as a sequence #.  We set it here; it may be zero indicating internal message.
   *   - originatingMessageIdOrNone: We set it here; it is non-zero if and only if this is a response to a past
   *     incoming message.
   *   - bodySerializationInfo: We initialize it and fill it out if so directed.
   *     - numBodySerializationSegments: Comes from n_serialization_segments_or_none.
   *     - splitSegments: Shall be left null if !split_segs; else contains a direct transcription of
   *       *split_segs, which is a sequence of integer-bearing simple structs.  (Spoiler: They describe which
   *       of the following numBodySerializationSegments must be joined together to obtain segment(s) that are
   *       each too large to fit into an IPC message.)
   *   - internalMessageBody: Similarly we initialize it (and return via out-arg its builder) if so directed.
   *
   * As noted in structured_msg.capnp docs and our own doc header, splitSegments is touchy in that *if* reusing
   * a previously load_mdt()ed *builder, *and* splitSegments size must change (including going to or from 0, or more
   * accurately splitSegments but go non-null to null or vice versa), then we are unable to reuse *builder and
   * shall return `false` ASAP, probably causing caller to re-create a fresh *builder and call us again.  Reminder:
   * This is assumed (and measures are taken elsewhere) to be rare, which is why we don't balk at doing this.
   *
   * Note: changing size from non-zero to <other non-zero>, or from non-zero to zero, leaks serialization space
   * (capnp does not reclaim the orphaned list); hence those cases return `false`.  Going from zero (null node)
   * to non-zero would be capnp-legal (a fresh allocation; nothing orphaned) -- but the mdt header is a fixed-size
   * area, so we conservatively treat any size change alike.  It's a corner case of a corner case; requiring
   * a re-call with fresh *builder is not too bad.
   *
   * Similarly if internalMessageBody is relevant (this describes an internal message), it'll need to be
   * re-.init...()ed, but we classify this as a pre-condition by contract (assert() on their trying to
   * reuse *builder). */

  /* Since caller may be relying on our diagnostic of needing to re-create *builder, check for those conditions first
   * so as to make it snappy. */
  const bool inited_already = msg_root.hasAuthHeader(); // Telltale sign that .getRoot() returned existing root.
  const bool want_internal_else_user_msg = id_or_none == 0;
  if (inited_already)
  {
    assert((!want_internal_else_user_msg) && "Contract is not to try to load internal msg into reused *builder.");

    auto cur_body_serialization_info = msg_root.getBodySerializationInfo();
    const size_t cur_n_split_segs = cur_body_serialization_info.hasSplitSegmentsOrNull()
                                      ? cur_body_serialization_info.getSplitSegmentsOrNull().size()
                                      : 0;
    if (cur_n_split_segs != (split_segs ? split_segs->size() : size_t{0}))
    {
      return false;
    }
    /* else { Happens to be the same number of split_segs before and after (possibly 0).
              So can no-op or modify list elements in-place (see below).  OK to reuse *builder. } */
  }
  // else { *builder is fresh anyway; we will set everything up below. }

  // Deal with sessionToken.  Encode as mandated in .capnp Uuid doc header.  @todo Factor this out into a util method.

  auto capnp_uuid = inited_already ? msg_root.getAuthHeader().getSessionToken()
                                   : msg_root.initAuthHeader().initSessionToken();
  static_assert(std::remove_reference_t<decltype(session_token)>::static_size() == 2 * sizeof(uint64_t),
                "World is broken: UUIDs expected to be 16 bytes!");
  uint64_t first8;
  uint64_t last8;
  std::memcpy(&first8, session_token.data(), sizeof(first8));
  std::memcpy(&last8, session_token.data() + sizeof(first8), sizeof(last8));
  capnp_uuid.setFirst8(native_to_little(first8)); // Reminder: Likely no-op + copy of uint64_t.
  capnp_uuid.setLast8(native_to_little(last8)); // Ditto.

  msg_root.setId(id_or_none);
  msg_root.setOriginatingMessageIdOrNone(originating_msg_id_or_none);

  if (want_internal_else_user_msg)
  {
    assert(internal_msg_builder_or_null && "Did not follow contract.");
    assert((n_serialization_segments_or_none == 0) && "Did not follow contract.");
    assert((!split_segs) && "Did not follow contract.");

    *internal_msg_builder_or_null = msg_root.initInternalMessageBody(); // OK to .init...() because fresh *builder.
  }
  else // if (user message)
  {
    /* Little subtlety: If `inited_already`, contract requires that existing *builder already describes a user message,
     * not an internal message.  We could probably assert() on this but choose not to.  Let's trust 'em this time. */

    assert((!internal_msg_builder_or_null) && "Did not follow contract.");
    assert((n_serialization_segments_or_none != 0)
           && "Did not follow contract: If serializing a user message, you have to have some segs for it.");

    auto body_serialization_info = inited_already ? msg_root.getBodySerializationInfo()
                                                  : msg_root.initBodySerializationInfo();
    body_serialization_info.setNumBodySerializationSegments(n_serialization_segments_or_none);
    if (split_segs)
    {
      const auto n_split_segs = split_segs->size(); // We've ensured this equals existing list's (if any) size.
      assert((n_split_segs != 0) && "By convention if no split_segs then pass-in null.");

      auto split_segs_list = inited_already ? body_serialization_info.getSplitSegmentsOrNull()
                                            : body_serialization_info.initSplitSegmentsOrNull(n_split_segs);
      for (size_t idx = 0; idx != n_split_segs; ++idx)
      {
        const auto& split_seg = (*split_segs)[idx];

        if ((split_seg.m_start_idx > std::numeric_limits<split_seg_sz_t>::max())
            ||
            (split_seg.m_n_cont_subsegs > std::numeric_limits<split_seg_sz_t>::max()))
        {
          // Split-segment record does not fit the wire type; a message this fragmented is not supported.
          *err_code = error::Code::S_SERIALIZE_FAILED_SPLIT_ENCODING_TOO_BIG;
          return true;
        }
        // else

        /* Again: See doc header for `using SplitSegment` in .capnp for full strategy/tactics here.
         * Bottom line is we encode it not as a struct but as a single double-width integer containing the two
         * equally-sized integers within it. */
        split_segs_list.set(idx,
                            static_cast<split_seg_t>(split_seg.m_start_idx)
                              | (static_cast<split_seg_t>(split_seg.m_n_cont_subsegs)
                                 << (4 * sizeof(split_seg_t)))); // Or: 8 * sizeof(split_seg_sz_t).
      }
    }
    // else if (!split_segs): We've guaranteed .getSplitSegmentsOrNull() is null already, and so it shall remain.
  } // else // if (user message)

  err_code->clear();
  return true;
} // load_mdt()

} // namespace ipc::transport::struc
