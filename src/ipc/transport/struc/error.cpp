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
#include "ipc/transport/struc/error.hpp"
#include "ipc/util/util_fwd.hpp"

namespace ipc::transport::struc::error
{

// Types.

/**
 * The boost.system category for errors returned by the ipc::transport::struc module.  Analogous to
 * transport::error::Category.  All notes therein apply.
 */
class Category :
  public boost::system::error_category
{
public:
  // Constants.

  /// The one Category.
  static const Category S_CATEGORY;

  // Methods.

  /**
   * Analogous to transport::error::Category::name().
   *
   * @return See above.
   */
  const char* name() const noexcept override;

  /**
   * Analogous to transport::error::Category::name().
   *
   * @param val
   *        See above.
   * @return See above.
   */
  std::string message(int val) const override;

  /**
   * Analogous to transport::error::Category::name().
   * @param code
   *        See above.
   * @return See above.
   */
  static util::String_view code_symbol(Code code);

private:
  // Constructors.

  /// Boring constructor.
  explicit Category();
}; // class Category

// Static initializations.

const Category Category::S_CATEGORY;

// Implementations.

Error_code make_error_code(Code err_code)
{
  /* Assign Category as the category for flow::error::Code-cast error_codes;
   * this basically glues together Category::name()/message() with the Code enum. */
  return Error_code(static_cast<int>(err_code), Category::S_CATEGORY);
}

Category::Category() = default;

const char* Category::name() const noexcept // Virtual.
{
  return "ipc/transport/struc";
}

std::string Category::message(int val) const // Virtual.
{
  using std::string;

  // KEEP THESE STRINGS IN SYNC WITH COMMENT IN error.hpp ON THE INDIVIDUAL ENUM MEMBERS!

  /* Just create a string (unless compiler is smart enough to do this only once and reuse later)
   * based on a static char* which is rapidly indexed from val by the switch() statement.
   * Since the error_category interface requires that message() return a string by value, this
   * is the best we can do speed-wise... but it should be fine.
   *
   * Some error messages can be fancier by specifying outside values (e.g., see
   * net_flow's S_INVALID_SERVICE_PORT_NUMBER). */
  switch (static_cast<Code>(val))
  {
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_HNDL:
    return "Structured channel: received unexpected payload: message sans native handle along handles-only pipe.";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_BLOB:
    return "Structured channel: received unexpected payload: message with empty blob (not even an encoded length).";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_HNDL:
    return "Structured channel: received unexpected payload: continuation message contains native handle.";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA:
    return "Structured channel: received structured message with invalid internally-set/used fields.";
  case Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG:
    return "Structured message serialization (e.g., when sending over channel): A user-mutated datum (e.g., "
           "blob or string) is so large as to require a too-large segment to fit into the prescribed max size (such "
           "as max blob size in an IPC channel, when using direct serialization sans SHM).  Consider using a "
           "different serialization mechanism.";
  case Code::S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED:
    return "Structured message deserialization (e.g., when receiving from channel): Reader engine, when asked "
           "to supply a target memory area for a serialization-storing segment, reports it is unable to obtain an "
           "area of sufficient size, such as due to running out of mem-pool space.  Consider using a different "
           "deserialization mechanism.";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA:
    return "Structured channel: log-in phase: received structured message with invalid internally-set/used fields.";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_BAD_AUTH:
    return "Structured channel: "
           "auth session token in in-message does not match expected value established during log-in.";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_INTERNAL_MSG_TYPE_UNKNOWN:
    return "Structured channel: in internally-generated message the internal message type is unknown.";
  case Code::S_STRUCT_CHANNEL_GOT_UNEXPECTED_LOG_IN_REQUEST:
    return "Structured channel: "
           "log-in phase as server: actual log-in request contents differ from local user expectation.";
  case Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS:
    return "Structured message deserialization: Tried to deserialize without enough input segments (e.g., none).";
  case Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED:
    return "Structured message deserialization: An input segment is not aligned as required.";

  case Code::S_END_SENTINEL:
    assert(false && "SENTINEL: Not an error.  "
                    "This Code must never be issued by an error/success-emitting API; I/O use only.");
  }
  assert(false);
  return "";
} // Category::message()

util::String_view Category::code_symbol(Code code) // Static.
{
  // Note: Must satisfy istream_to_enum() requirements.

  switch (code)
  {
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_HNDL:
    return "STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_HNDL";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_BLOB:
    return "STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_NO_BLOB";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_HNDL:
    return "STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISMATCH_GOT_HNDL";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA:
    return "STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA";
  case Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG:
    return "INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG";
  case Code::S_INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED:
    return "INTERNAL_ERROR_DESERIALIZE_TARGET_ALLOC_FAILED";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA:
    return "STRUCT_CHANNEL_INTERNAL_PROTOCOL_LOG_IN_MISUSED_SCHEMA";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_BAD_AUTH:
    return "STRUCT_CHANNEL_INTERNAL_PROTOCOL_BAD_AUTH";
  case Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_INTERNAL_MSG_TYPE_UNKNOWN:
    return "STRUCT_CHANNEL_INTERNAL_PROTOCOL_INTERNAL_MSG_TYPE_UNKNOWN";
  case Code::S_STRUCT_CHANNEL_GOT_UNEXPECTED_LOG_IN_REQUEST:
    return "STRUCT_CHANNEL_GOT_UNEXPECTED_LOG_IN_REQUEST";
  case Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS:
    return "DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS";
  case Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED:
    return "DESERIALIZE_FAILED_SEGMENT_MISALIGNED";

  case Code::S_END_SENTINEL:
    return "END_SENTINEL";
  }
  assert(false);
  return "";
}

std::ostream& operator<<(std::ostream& os, Code val)
{
  // Note: Must satisfy istream_to_enum() requirements.
  return os << Category::code_symbol(val);
}

std::istream& operator>>(std::istream& is, Code& val)
{
  /* Range [<1st Code>, END_SENTINEL); no match => END_SENTINEL;
   * allow for number instead of ostream<< string; case-insensitive. */
  val = flow::util::istream_to_enum(&is, Code::S_END_SENTINEL, Code::S_END_SENTINEL, true, false,
                                    Code(S_CODE_LOWEST_INT_VALUE));
  return is;
}

} // namespace ipc::transport::struc::error
