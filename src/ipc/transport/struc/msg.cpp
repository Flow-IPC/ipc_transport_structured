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

#include "ipc/transport/struc/msg.hpp"
#include "ipc/transport/struc/error.hpp"
#include <flow/error/error.hpp>
#include <cassert>
#include <cstring>

namespace ipc::transport::struc
{

// Capped_sz_capnp_message_*der implementations.

Capped_sz_capnp_message_builder::Capped_sz_capnp_message_builder(const util::Blob_mutable& seg, bool zero_it_please) :
  m_seg(seg.data(),
        (seg.size() / sizeof(::capnp::word)) * sizeof(::capnp::word)), // Round down to nearest word.
  m_returned_seg(false)
{
  if (zero_it_please)
  {
    std::memset(m_seg.data(), 0, m_seg.size());
  }
  assert(m_seg.size() != 0);
}

kj::ArrayPtr<::capnp::word> Capped_sz_capnp_message_builder::allocateSegment(unsigned int min_sz_words) // Virtual.
{
  using flow::error::Runtime_error;
  using Word = capnp::word;
  using Capnp_word_buf = kj::ArrayPtr<Word>;

  if (m_returned_seg)
  {
    // Segment 1 was not enough to store the needed data; so seg.size() (size cap) was insufficient.
    throw Runtime_error{error::Code::S_INVALID_ARGUMENT,
                        "Capped_sz_capnp_message_builder::allocateSegment()[bad size cap]"};
  }
  m_returned_seg = true;

  assert(((min_sz_words * sizeof(Word)) <= m_seg.size())
         && "Should not be possible: First allocateSegment() always takes `1`; second would have thrown above.");

  return Capnp_word_buf{reinterpret_cast<Word*>(m_seg.data()),
                        reinterpret_cast<Word*>(static_cast<uint8_t*>(m_seg.data()) + m_seg.size())};
}

Capped_sz_capnp_message_reader::Capped_sz_capnp_message_reader(const util::Blob_const& seg) :
  // Init this private base (glorified data member) first.  See class doc header Impl section for brief explanation.
  Word_array_ptr_base(static_cast<const ::capnp::word*>(seg.data()),
                      seg.size() / sizeof(::capnp::word)),
  Seg_array_msg_reader_base
    (kj::ArrayPtr<const Word_array_ptr_base>
       {static_cast<const Word_array_ptr_base*>(this), 1})
{
  // Eep.
}

} // namespace ipc::transport::struc
