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
#include <boost/move/unique_ptr.hpp>

namespace ipc::transport::struc
{

// Types.

/**
 * This is the `friend` facade of Msg_out that exposes `private` APIs hidden from public user by providing
 * public access to them; this is used internally by struc::sync_io::Channel at least.  The background is
 * briefly explained in the impl section of Msg_out doc header.
 *
 * @tparam Base_t
 *         The type of object whose specific `private` API to expose.
 */
template<typename Base_t>
struct Msg_out_impl
{
  // Types.

  /// Short-hand for wrapped class.
  using Base = Base_t;

  // Data.

  /// Direct-initializable wrapped object.  Access `public` API through this reference; `private` API via `*this`.
  Base& m_base;

  // Methods.

  /**
   * See #Base counterpart.
   * @param args
   *        See #Base counterpart.
   */
  template<typename... Args>
  void emit_serialization(Args&&... args);
}; // struct Msg_out_impl

/**
 * This is the `friend` facade of Msg_in that exposes `private` APIs hidden from public user by providing
 * public access to them; this is used internally by struc::sync_io::Channel at least.  The background is
 * briefly explained in the impl section of Msg_in doc header.
 *
 * @todo Some of the methods in Msg_in_impl forward an arbitrary args list while others match the Msg_in
 * signature exactly; probably we should do the former in all of them.  At least if there are 1+ args for a given
 * method, might as well.  (When there are none... more of a toss-up... it's more code now but better for
 * ease of potential maintenance.)
 *
 * @tparam Base_t
 *         The type of object whose specific `private` API to expose.
 */
template<typename Base_t>
struct Msg_in_impl
{
  // Types.

  /// Short-hand for wrapped class.
  using Base = Base_t;

  /// See #Base counterpart.
  using Internal_msg_body_reader = typename Base::Internal_msg_body_reader;

  /// See #Base counterpart.
  using Mdt_reader = typename Base::Mdt_reader;

  // Data.

  /// Direct-initializable wrapped object.  Access `public` API through this reference; `private` API via `*this`.
  Base& m_base;

  // Methods.

  /**
   * Forwards to #Base ctor(s).
   * @param ctor_args
   *        See above.
   * @return New #Base.
   */
  template<typename... Ctor_args>
  static auto ct_base(Ctor_args&&... ctor_args) -> boost::movelib::unique_ptr<Base>;
  // @todo See definition for reason for the odd signature form (Doxygen). --^

  /**
   * See #Base counterpart.
   * @param native_handle_or_null
   *        See #Base counterpart.
   */
  void store_native_handle_or_null(Native_handle&& native_handle_or_null);

  /**
   * See #Base counterpart.
   * @param blob
   *        See #Base counterpart.
   */
  void add_serialization_segment(Segment_blob_in&& blob);

  /**
   * See #Base counterpart.
   * @param args
   *        See #Base counterpart.
   * @return See #Base counterpart.
   */
  template<typename... Args>
  size_t deserialize_mdt(Args&&... args);

  /**
   * See #Base counterpart.
   * @param args
   *        See #Base counterpart.
   */
  template<typename... Args>
  void deserialize_body(Args&&... args);

  /**
   * See #Base counterpart.
   * @return See #Base counterpart.
   */
  msg_id_t id_or_none() const;

  /**
   * See #Base counterpart.
   * @return See #Base counterpart.
   */
  msg_id_t originating_msg_id_or_none() const;

  /**
   * See #Base counterpart.
   * @return See #Base counterpart.
   */
  Internal_msg_body_reader internal_msg_body_root() const;

  /**
   * See #Base counterpart.
   * @return See #Base counterpart.
   */
  const Session_token& session_token() const;

  /**
   * See #Base counterpart.
   * @return See #Base counterpart.
   */
  const Mdt_reader& mdt_root() const;
}; // struct Msg_in_impl

// Template implementations.

template<typename Base_t>
template<typename... Args>
void Msg_out_impl<Base_t>::emit_serialization(Args&&... args)
{
  m_base.emit_serialization(std::forward<Args>(args)...);
}

template<typename Base_t>
template<typename... Ctor_args>
auto Msg_in_impl<Base_t>::ct_base(Ctor_args&&... ctor_args) -> boost::movelib::unique_ptr<Base_t>
// Doxygen 1.9.4 gets confused here otherwise; the `->` form is a work-around for that.  @todo Revisit with later ver.
{
  using boost::movelib::unique_ptr;

  // Out of abundance of caution: Avoid initializer-list pitfalls in this generic code: (), not {} here.
  return unique_ptr<Base>(new Base(std::forward<Ctor_args>(ctor_args)...));
}

template<typename Base_t>
void Msg_in_impl<Base_t>::store_native_handle_or_null(Native_handle&& native_handle_or_null)
{
  m_base.store_native_handle_or_null(std::move(native_handle_or_null));
}

template<typename Base_t>
void Msg_in_impl<Base_t>::add_serialization_segment(Segment_blob_in&& blob)
{
  return m_base.add_serialization_segment(std::move(blob));
}

template<typename Base_t>
template<typename... Args>
size_t Msg_in_impl<Base_t>::deserialize_mdt(Args&&... args)
{
  return m_base.deserialize_mdt(std::forward<Args>(args)...);
}

template<typename Base_t>
template<typename... Args>
void Msg_in_impl<Base_t>::deserialize_body(Args&&... args)
{
  m_base.deserialize_body(std::forward<Args>(args)...);
}

template<typename Base_t>
msg_id_t Msg_in_impl<Base_t>::id_or_none() const
{
  return m_base.id_or_none();
}

template<typename Base_t>
msg_id_t Msg_in_impl<Base_t>::originating_msg_id_or_none() const
{
  return m_base.originating_msg_id_or_none();
}

template<typename Base_t>
typename Msg_in_impl<Base_t>::Internal_msg_body_reader Msg_in_impl<Base_t>::internal_msg_body_root() const
{
  return m_base.internal_msg_body_root();
}

template<typename Base_t>
const Session_token& Msg_in_impl<Base_t>::session_token() const
{
  return m_base.session_token();
}

template<typename Base_t>
const typename Msg_in_impl<Base_t>::Mdt_reader& Msg_in_impl<Base_t>::mdt_root() const
{
  return m_base.mdt_root();
}

} // namespace ipc::transport::struc
