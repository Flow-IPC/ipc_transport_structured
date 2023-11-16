# Flow-IPC: Structured Transport
# Copyright (c) 2023 Akamai Technologies, Inc.; and other contributors.
# Each commit is copyright by its respective author or author's employer.
#
# Licensed under the MIT License:
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

@0xf58127c9945da5ce; # Manually generated by `capnp id` and saved here for all time.

using Cxx = import "/capnp/c++.capnp";

# The following section is, informally, this schema file's *header*.
# Recommendation: copy-paste relevant pieces into other schema files + follow similar conventions in this
# and other library+API combos.
#
# Header BEGIN ---

# Quick namespace reference:
# - ipc::transport::struc: Globally used for all IPC transport structured layer-related matters in
#   Flow-IPC library+API.
#   - schema: Indicates the symbols therein are auto-generated by capnp from a schema such as in this file.
$Cxx.namespace("ipc::transport::struc::schema");

# --- common.capnp ---
# This schema defines generally applicable types, including structs and aliases, for Flow-IPC users of ipc::transport.

# --- END Header.

struct Uuid
{
  # UUID (RFC 4122 Universally Unique IDentifier) encoded in fixed-size terms (no blobs) for, we think, perf:
  # ipc::transport structured layer internally sends one of these in essentially every message involved in IPC.
  #
  # UUID is 16 bytes by definition.  A given non-nil UUID is *never* generated more than once (exceeds # of atoms
  # in the universe... etc).
  #
  # Suppose a UUID is written in big-endian (i.e., logical) order in a `uint8_t uuid[16]` in RAM.
  # Suppose it's aligned properly (e.g., it's on the stack).
  # Then:
  #   - first8 encodes the overall value in uuid[0], [1], ..., [7].
  #   - last8 encodes the overall value in uuid[8], [9], ..., [15].
  #
  # To encode {first|last}8 from the corresponding half-array uuid[A0], uuid[A1], ..., uuid[A7]:
  #   - let uint64_t& X = *(reinterpret_cast<uint64_t*>(&uuid[A0]))
  #   - let X' = endian::native_to_little(X) (in x86 and most arch: this is a no-op/copy)
  #   - set{First|Last}8(X')
  # To decode {first|last}8 into the corresponding half-array uuid[A0], uuid[A1], ..., uuid[A7]:
  #   - let X' = get{First|Last}8()
  #   - let uint64_t& X = *(reinterpret_cast<uint64_t*>(&uuid[A0]))
  #   - X = endian::little_to_native(X') (again: usually a no-op/copy)
  # Rationale: This defines a portable meaning, even when builder and reader are in
  # mutually-conflicting-endianness architectures: the same uuid[0..15] bytes in the same order will result.
  # Yet, in most architectures, no byte flipping will be necessary to achieve it.

  first8 @0 :UInt64 = 0; # Default to nil.  Specified explicitly for exposition (would be 0 anyway).
  last8 @1 :UInt64 = 0; # Ditto like first8.
}

using Size = UInt64; # Should match size_t.
using Offset = Int64; # Should match ptrdiff_t.

struct Metadata(Payload)
{
  # Metadata holder for use when opening channels.  It is a struct, instead of simply placing a
  # `payload @...:Payload` directly, wherever, so that capnp-genrated C++ types
  # `class Metadata<Payload>:{Builder|Reader}` are available in ipc::sesson::{Client|Server}_session
  # APIs for opening channels, including open_channel().

  payload @0 :Payload;
}
