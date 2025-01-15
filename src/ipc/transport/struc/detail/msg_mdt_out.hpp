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

#include "ipc/transport/struc/msg.hpp"
#include "ipc/transport/struc/schema/detail/structured_msg.capnp.h"
#include <boost/endian.hpp>

namespace ipc::transport::struc
{

// Types.

/**
 * Internally used (data-free) addendum on-top of Msg_out; really an alias
 * to `Msg_out<schema::detail::StructuredMessage>`, where the latter is the internal-use
 * set of metadata schema, with a convenience public mutating API wrapping around capnp-generated mutator API
 * for things like message ID.
 *
 * @see Msg_in whose `public` + `protected` API = essentially the accessor counterpart of
 *      Msg_mdt_out and Msg_out<Message_body> mutator APIs.
 *
 * @tparam Struct_builder_config
 *         See struc::Channel.
 */
template<typename Struct_builder_config>
class Msg_mdt_out :
  private Msg_out<schema::detail::StructuredMessage, typename Struct_builder_config::Builder>
{
private:
  // Types.

  /// Our base class.
  using Base = Msg_out<schema::detail::StructuredMessage, typename Struct_builder_config::Builder>;

  /// Short-hand for capnp-generated mutating `Builder` nested class of #Body.  See body_root().
  using Body_builder = typename Base::Body_builder;

public:
  // Types.

  /// Short-hand for user-specified builder engine.
  using Builder_config = Struct_builder_config;

  /**
   * In the event `*this` describes an internally-created message -- for internal purposes, not by the user --
   * this is the equivalent of `Base::Body_builder` for the internal-message schema.
   * Refer to structured_msg.capnp for clarity; you'll see a field there that stores either
   * an `InternalMessageBody` or null.
   */
  using Internal_msg_body_builder = typename Base::Body::InternalMessageBody::Builder;

  // Constructors/destructor.

  /**
   * Constructs out-metadata-message describing a user-created message (not an internal message);
   * or else both describing and containing an internal message (no user-created message associated).
   *
   * @param struct_builder_config
   *        See other ctor.
   * @param session_token
   *        See other ctor.
   * @param originating_msg_id_or_none
   *        0 if the message is unsolicited; else the message ID of the out-message to which this
   *        is responding.
   * @param id_or_none
   *        Out-message ID; 0 if internal message.
   * @param n_serialization_segments_or_none
   *        # of segments serializing the user-created message described by `*this`; ignored
   *        if `id_or_none == 0`.
   */
  explicit Msg_mdt_out(const Builder_config& struct_builder_config,
                       const Session_token& session_token, msg_id_t originating_msg_id_or_none, msg_id_t id_or_none,
                       size_t n_serialization_segments_or_none);

  // Methods.

  /**
   * Returns a builder for the internal-message root; to be called only if `id_or_none == 0` in ctor.
   * (Behavior undefined/assertion may trip otherwise.)  See #Internal_msg_body_builder doc header.
   *
   * We could return a pointer to something stored, but as of this writing such an optimization would
   * be pointless, as it's really invoked just once in practice.
   *
   * @return See above.
   */
  Internal_msg_body_builder internal_msg_body_root();

  /**
   * Forwards to Msg_out::body_root() (`const` overload).  May be useful for, say, pretty-printing it
   * to log (e.g.: `capnp::prettyPrint(M.body_root()->asReader()).flatten().cStr()`).
   *
   * @return See above.
   */
  const Body_builder* body_root() const;

  /**
   * Returns the serialization in the form of a sequence of 1 `Blob`.  Behavior is undefined if it is called
   * more than once.  (In reality it's probably fine, but this internal type and its API aren't meant for that,
   * so it's easier to just say that and not worry about it.)
   *
   * ### Wait, why 1 Blob? ###
   * This is an internal-use structure, and we've ensured "heuristically" it'll fit in any reasonable segment.
   *
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  See Msg_out::emit_serialization()
   *        which is the core of this guy here.
   * @param session
   *        Specifies the opposing recipient for which the serialization is intended.
   *        See Msg_out::emit_serialization() which is the core of this guy here.
   * @return What Msg_out::emit_serialization() would have placed in the target list container; or null if
   *         it would have failed.  In case of success:
   *         In our case that container must have size 1, so we just return that first element.
   */
  flow::util::Blob* emit_serialization(const typename Base::Builder::Session& session, Error_code* err_code) const;
}; // class Msg_mdt_out

// Template implementations.

template<typename Struct_builder_config>
Msg_mdt_out<Struct_builder_config>::Msg_mdt_out
  (const Builder_config& struct_builder_config, const Session_token& session_token,
   msg_id_t originating_msg_id_or_none, msg_id_t id_or_none, size_t n_serialization_segments_or_none) :

  Base(struct_builder_config)
{
  using boost::endian::native_to_little;

  const auto msg_root = Base::body_root();

  /* Refer to structured_msg.capnp StructuredMessage.  Base has initialized it for us.  Now to fill it in:
   * It is *now* serialization/send time, so we fill in everything.
   * Let's go over it:
   *   - authHeader.sessionToken: We set it here.
   *   - id: It is used as a sequence #.  We set it here; it may be zero indicating internal message.
   *   - originatingMessageOrNull: Used if and only if this is a response to a past incoming message.
   *     We set it here if so directed.
   *   - internalMessageBody: Similarly we initialize it if so directed.  It's not dependent on any template params
   *     BTW. */

  // Deal with sessionToken.  Encode as mandated in .capnp Uuid doc header.  @todo Factor this out into a util method.
  auto capnp_uuid = msg_root->initAuthHeader().initSessionToken();
  static_assert(std::remove_reference_t<decltype(session_token)>::static_size() == 2 * sizeof(uint64_t),
                "World is broken: UUIDs expected to be 16 bytes!");
  auto& first8 = *(reinterpret_cast<const uint64_t*>(session_token.data())); // capnp_uuid is aligned, so this is too.
  auto& last8 = *(reinterpret_cast<const uint64_t*>(session_token.data() + sizeof(uint64_t))); // As is this.
  capnp_uuid.setFirst8(native_to_little(first8)); // Reminder: Likely no-op + copy of uint64_t.
  capnp_uuid.setLast8(native_to_little(last8)); // Ditto.

  msg_root->setId(id_or_none);

  if (originating_msg_id_or_none != 0)
  {
    msg_root->initOriginatingMessageOrNull().setId(originating_msg_id_or_none);
  }

  if (id_or_none == 0)
  {
    msg_root->initInternalMessageBody();
  }
  else
  {
    assert((n_serialization_segments_or_none != 0)
            && "If serializing a user message, you have to have some segs for it.");
    msg_root->setNumBodySerializationSegments(n_serialization_segments_or_none);
  }
} // Msg_mdt_out::Msg_mdt_out()

template<typename Struct_builder_config>
typename Msg_mdt_out<Struct_builder_config>::Internal_msg_body_builder
  Msg_mdt_out<Struct_builder_config>::internal_msg_body_root()
{
  return Base::body_root()->getInternalMessageBody();
}

template<typename Struct_builder_config>
const typename Msg_mdt_out<Struct_builder_config>::Body_builder*
  Msg_mdt_out<Struct_builder_config>::body_root() const
{
  return Base::body_root();
}

template<typename Struct_builder_config>
flow::util::Blob* Msg_mdt_out<Struct_builder_config>::emit_serialization(const typename Base::Builder::Session& session,
                                                                         Error_code* err_code) const
{
  Segment_ptrs target_blobs;

  Base::emit_serialization(&target_blobs, session, err_code); // Throws <=> err_code is null *and* serialization failed.

  // If got here, either no error; or there is an error, and err_code is not null, and *err_code is truthy (the error).
  if (err_code && *err_code)
  {
    return nullptr;
  }
  // No error.  err_code is null, or *err_code is falsy.

  assert((!target_blobs.empty()) && "That is just weird.");
  assert((target_blobs.size() == 1) && "As described in our contract we should have ensured this internal-use "
                                         "structure shall fit in one segment no matter the builder.  Bug?");

  return target_blobs.front();
}

} // namespace ipc::transport::struc
