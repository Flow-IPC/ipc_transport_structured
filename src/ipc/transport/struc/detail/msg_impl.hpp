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

namespace ipc::transport::struc
{

// Types.

/**
 * Internally used (data-free) addendum on-top of Msg_out which makes the `protected` API public instead.
 * See the implementation notes in Msg_out doc header regarding this design.
 */
template<typename Message_body, typename Struct_builder>
class Msg_out_impl : public Msg_out<Message_body, Struct_builder>
{
public:
  // Types.

  /// Short-hand for base class.
  using Base = Msg_out<Message_body, Struct_builder>;

  // Methods.

  /**
   * See super-class.
   *
   * @param target_blobs
   *        See super-class.
   * @param session
   *        See super-class.
   * @param err_code
   *        See super-class.
   */
  void emit_serialization(Segment_ptrs* target_blobs, const typename Base::Builder::Session& session,
                          Error_code* err_code) const;

  /**
   * See super-class.
   * @return See super-class.
   */
  size_t n_serialization_segments() const;
}; // class Msg_out_impl

/**
 * Internally used (data-free) addendum on-top of Msg_in which makes the `protected` API public instead.
 * See the implementation notes in Msg_in doc header regarding this design.
 *
 * @todo Msg_in_impl is pretty wordy; maybe `friend` would have been stylistically acceptable after all?
 * It's so much briefer, and we could simply resolve to only access the `protected` APIs and not `private` stuff....
 */
template<typename Message_body, typename Struct_reader_config>
class Msg_in_impl : public Msg_in<Message_body, Struct_reader_config>
{
public:
  // Types.

  /// Short-hand for base class.
  using Base = Msg_in<Message_body, Struct_reader_config>;

  /// See super-class.
  using Internal_msg_body_reader = typename Base::Internal_msg_body_reader;

  /// See super-class.
  using Mdt_reader = typename Base::Mdt_reader;

  // Constructors.

  /**
   * See super-class.
   *
   * @param struct_reader_config
   *        See super-class.
   */
  explicit Msg_in_impl(const typename Base::Reader_config& struct_reader_config);

  /**
   * See super-class.
   *
   * @param native_handle_or_null
   *        See super-class.
   */
  void store_native_handle_or_null(Native_handle&& native_handle_or_null);

  /**
   * See super-class.
   *
   * @param max_sz
   *        See super-class.
   * @return See super-class.
   */
  flow::util::Blob* add_serialization_segment(size_t max_sz);

  /**
   * See super-class.
   *
   * @param logger_ptr
   *        See super-class.
   * @param err_code
   *        See super-class.
   * @return See super-class.
   */
  size_t deserialize_mdt(flow::log::Logger* logger_ptr, Error_code* err_code);

  /**
   * See super-class.
   * @param err_code
   *        See super-class.
   */
  void deserialize_body(Error_code* err_code);

  /**
   * See super-class.
   * @return See super-class.
   */
  msg_id_t id_or_none() const;

  /**
   * See super-class.
   * @return See super-class.
   */
  msg_id_t originating_msg_id_or_none() const;

  /**
   * See super-class.
   * @return See super-class.
   */
  Internal_msg_body_reader internal_msg_body_root() const;

  /**
   * See super-class.
   * @return See super-class.
   */
  const Session_token& session_token() const;

  /**
   * See super-class.
   * @return See super-class.
   */
  const Mdt_reader& mdt_root() const;
}; // class Msg_in_impl

// Msg_out_impl template implementations.

template<typename Message_body, typename Struct_builder_config>
void
  Msg_out_impl<Message_body, Struct_builder_config>::emit_serialization(Segment_ptrs* target_blobs,
                                                                        const typename Base::Builder::Session& session,
                                                                        Error_code* err_code) const
{
  Base::emit_serialization(target_blobs, session, err_code);
}

template<typename Message_body, typename Struct_builder_config>
size_t Msg_out_impl<Message_body, Struct_builder_config>::n_serialization_segments() const
{
  return Base::n_serialization_segments();
}

// Msg_in_impl template implementations.

template<typename Message_body, typename Struct_reader_config>
Msg_in_impl<Message_body, Struct_reader_config>::Msg_in_impl
  (const typename Base::Reader_config& struct_reader_config) :
  Base(struct_reader_config)
{
  // Yeah.
}

template<typename Message_body, typename Struct_reader_config>
void Msg_in_impl<Message_body, Struct_reader_config>::store_native_handle_or_null
       (Native_handle&& native_handle_or_null)
{
  Base::store_native_handle_or_null(std::move(native_handle_or_null));
}

template<typename Message_body, typename Struct_reader_config>
flow::util::Blob* Msg_in_impl<Message_body, Struct_reader_config>::add_serialization_segment(size_t max_sz)
{
  return Base::add_serialization_segment(max_sz);
}

template<typename Message_body, typename Struct_reader_config>
size_t Msg_in_impl<Message_body, Struct_reader_config>::deserialize_mdt(flow::log::Logger* logger_ptr,
                                                                        Error_code* err_code)
{
  return Base::deserialize_mdt(logger_ptr, err_code);
}

template<typename Message_body, typename Struct_reader_config>
void Msg_in_impl<Message_body, Struct_reader_config>::deserialize_body(Error_code* err_code)
{
  Base::deserialize_body(err_code);
}

template<typename Message_body, typename Struct_reader_config>
msg_id_t Msg_in_impl<Message_body, Struct_reader_config>::id_or_none() const
{
  return Base::id_or_none();
}

template<typename Message_body, typename Struct_reader_config>
msg_id_t Msg_in_impl<Message_body, Struct_reader_config>::originating_msg_id_or_none() const
{
  return Base::originating_msg_id_or_none();
}

template<typename Message_body, typename Struct_reader_config>
typename Msg_in_impl<Message_body, Struct_reader_config>::Internal_msg_body_reader
  Msg_in_impl<Message_body, Struct_reader_config>::internal_msg_body_root() const
{
  return Base::internal_msg_body_root();
}

template<typename Message_body, typename Struct_reader_config>
const Session_token& Msg_in_impl<Message_body, Struct_reader_config>::session_token() const
{
  return Base::session_token();
}

template<typename Message_body, typename Struct_reader_config>
const typename Msg_in_impl<Message_body, Struct_reader_config>::Mdt_reader&
  Msg_in_impl<Message_body, Struct_reader_config>::mdt_root() const
{
  return Base::mdt_root();
}

} // namespace ipc::transport::struc
