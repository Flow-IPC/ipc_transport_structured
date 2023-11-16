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

#include "ipc/common.hpp"

/**
 * Analogous to ipc::transport::error but applies to the sub-namespace ipc::transport::struc -- errors having
 * to do with structured messaging.  All notes from that doc header apply analogously within reason.
 */
namespace ipc::transport::struc::error
{

// Types.

/// Numeric value of the lowest Code.
constexpr int S_CODE_LOWEST_INT_VALUE = 1;

/// Analogous to ipc::transport::error::Code.  All notes from that doc header apply analogously within reason.
enum class Code
{
  /// Structured channel: received unexpected payload: message sans native handle along handles-only pipe.
  S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_HNDL,

  /// Structured channel: received unexpected payload: message with empty blob (not even an encoded length).
  S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_BLOB,

  /// Structured channel: received unexpected payload: continuation message contains native handle.
  S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_HNDL,

  /// Structured channel: received structured message with invalid internally-set/used fields.
  S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA,

  /**
   * Structured message serialization (e.g., when sending over channel): A user-mutated datum (e.g., blob or string)
   * is so large as to require a too-large segment to fit into the prescribed max size (such as max blob size in
   * an IPC channel, when using direct serialization sans SHM).  Consider using a different serialization mechanism.
   */
  S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG,

  /**
   * Structured message deserialization (e.g., when receiving from channel): Reader engine, when asked to
   * supply a target memory area for a serialization-storing segment, reports it is unable to obtain an area of
   * sufficient size, such as due to running out of mem-pool space.  Consider using a different
   * deserialization mechanism.
   */
  S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED,

  /// Structured channel: log-in phase: received structured message with invalid internally-set/used fields.
  S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA,

  /// Structured channel: auth session token in in-message does not match expected value established during log-in.
  S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_BAD_AUTH,

  /// Structured channel: in internally-generated message the internal message type is unknown.
  S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_INTERNAL_MSG_TYPE_UNKNOWN,

  /// Structured channel: log-in phase as server: actual log-in request contents differ from local user expectation.
  S_STRUCT_CHANNEL_GOT_UNEXPECTED_LOG_IN_REQUEST,

  /// Structured message deserialization: Tried to deserialize without enough input segments (e.g., none).
  S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS,

  /// Structured message deserialization: An input segment is not aligned as required.
  S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED,

  /// SENTINEL: Not an error.  This Code must never be issued by an error/success-emitting API; I/O use only.
  S_END_SENTINEL
}; // enum class Code

// Free functions.

/**
 * Analogous to transport::error::make_error_code().
 *
 * @param err_code
 *        See above.
 * @return See above.
 */
Error_code make_error_code(Code err_code);

/**
 * Analogous to transport::error::operator>>().
 *
 * @param is
 *        See above.
 * @param val
 *        See above.
 * @return See above.
 */
std::istream& operator>>(std::istream& is, Code& val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

/**
 * Analogous to transport::error::operator<<().
 *
 * @param os
 *        See above.
 * @param val
 *        See above.
 * @return See above.
 */
std::ostream& operator<<(std::ostream& os, Code val);
// @todo - `@relatesalso Code` makes Doxygen complain; maybe it doesn't work with `enum class`es like Code.

} // namespace ipc::transport::struc::error

namespace boost::system
{

// Types.

/**
 * Ummm -- it specializes this `struct` to -- look -- the end result is boost.system uses this as
 * authorization to make `enum` `Code` convertible to `Error_code`.  The non-specialized
 * version of this sets `value` to `false`, so that random arbitary `enum`s can't just be used as
 * `Error_code`s.  Note that this is the offical way to accomplish that, as (confusingly but
 * formally) documented in boost.system docs.
 */
template<>
struct is_error_code_enum<::ipc::transport::struc::error::Code>
{
  /// Means `Code` `enum` values can be used for `Error_code`.
  static const bool value = true;
  // See note in similar place near transport::error.
};

} // namespace boost::system
