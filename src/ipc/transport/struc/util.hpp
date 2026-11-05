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

#include <capnp/pretty-print.h>
#include <kj/string.h>
#include <flow/util/string_view.hpp>
#include <ostream>

namespace ipc::transport::struc
{

// Types.

/**
 * Proxy object referring to a capnp `Reader` for straightforward output via overloaded `ostream<<`:
 * one-line/potentially-truncated form.
 * To construct one pithily please use ostreamable_capnp_brief() free function.
 * If you have a `Builder` to print use `.asReader()` on it inside parentheses of that call.
 */
template<typename Capnp_reader>
struct Ostreamable_capnp_brief
{
  // Data.
  /// The encapsulated `Reader`.
  const Capnp_reader& m_capnp_reader;
};

/**
 * Proxy object referring to a capnp `Reader` for straightforward output via overloaded `ostream<<`:
 * multi-line/pretty-printed/indented/full-length form.
 * To construct one pithily please use ostreamable_capnp_full() free function.
 * If you have a `Builder` to print use `.asReader()` on it inside parentheses of that call.
 */
template<typename Capnp_reader>
struct Ostreamable_capnp_full
{
  // Data.
  /// The encapsulated `Reader`.
  const Capnp_reader& m_capnp_reader;
};

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Capnp_reader>
Ostreamable_capnp_brief<Capnp_reader> ostreamable_capnp_brief(const Capnp_reader& capnp_reader)
{
  return Ostreamable_capnp_brief<Capnp_reader>{ capnp_reader };
}

template<typename Capnp_reader>
Ostreamable_capnp_full<Capnp_reader> ostreamable_capnp_full(const Capnp_reader& capnp_reader)
{
  return Ostreamable_capnp_full<Capnp_reader>{ capnp_reader };
}

template<typename Capnp_reader>
std::ostream& operator<<(std::ostream& os, const Ostreamable_capnp_brief<Capnp_reader>& val)
{
  using kj::str;
  using kj::String;
  using util::String_view;

  constexpr size_t MAX_SZ = 256;
  constexpr String_view TRUNC_SUFFIX = "... )"; // Fake the end to look like the end of the real pretty-print.
  const String capnp_str = str(val.m_capnp_reader);
  if (capnp_str.size() > MAX_SZ)
  {
    return os << String_view(capnp_str.begin(), MAX_SZ - TRUNC_SUFFIX.size()) << TRUNC_SUFFIX;
  }
  // else
  return os << capnp_str.cStr();
}

template<typename Capnp_reader>
std::ostream& operator<<(std::ostream& os, const Ostreamable_capnp_full<Capnp_reader>& val)
{
  return os << ::capnp::prettyPrint(val.m_capnp_reader).flatten().cStr();
}

} // namespace ipc::transport::struc
