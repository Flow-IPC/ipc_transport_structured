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

namespace ipc::transport::struc
{

// Static initializers.

const Null_session NULL_SESSION;

// Heap_fixed_builder implementations.

Heap_fixed_builder::Heap_fixed_builder() = default;

Heap_fixed_builder::Heap_fixed_builder(const Config& config) :
  flow::log::Log_context(config.m_logger_ptr, Log_component::S_TRANSPORT),

  m_segment_sz(config.m_segment_sz),
  // The builder engine launched here.  In the future mutators will cause it to allocate in heap on-demand.
  m_engine(boost::movelib::make_unique<Capnp_heap_engine>
             (get_logger(),
              config.m_segment_sz, config.m_frame_prefix_sz, config.m_frame_postfix_sz))
{
  FLOW_LOG_TRACE("Heap_fixed_builder [" << *this << "]: Fixed-size builder started: "
                 "segment size limit [" << m_segment_sz << "].");
  assert(m_segment_sz != 0);
}

Heap_fixed_builder::Heap_fixed_builder(Heap_fixed_builder&&) = default;

Heap_fixed_builder::~Heap_fixed_builder()
{
  FLOW_LOG_TRACE("Heap_fixed_builder [" << *this << "]: Fixed-size builder being destroyed.");
  // @todo Maybe have Heap_fixed_builder_capnp_message_builder log something useful in its dtor?
}

Heap_fixed_builder& Heap_fixed_builder::operator=(Heap_fixed_builder&&) = default;

Capnp_msg_builder_interface* Heap_fixed_builder::payload_msg_builder()
{
  assert(m_engine && "Are you operating on a moved-from `*this`?");

  return m_engine.get();
}

void Heap_fixed_builder::emit_serialization(Segment_ptrs* target_blobs, [[maybe_unused]] const Session& session,
                                            Error_code* err_code) const
{
  using util::Blob_const;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code) { emit_serialization(target_blobs, session, actual_err_code); },
         err_code, "Heap_fixed_builder::emit_serialization()"))
  {
    return;
  }
  // If got here: err_code is not null.

  assert(m_engine && "Are you operating on a moved-from `*this`?");

  /* Reminder: concept API says emit_serialization() can be called >1 times.  We do allow that in the below.
   * Keep in mind that mutations can be executed in-between, so whatever caching for perf one imagines has
   * to not mess up in the face of that. */

  Segment_ptrs& blob_ptrs = *target_blobs;

  size_t n_blobs = blob_ptrs.size();
  m_engine->emit_segment_blobs(&blob_ptrs); // It appends, like we must.  Just be careful in remembering that below.
  n_blobs = blob_ptrs.size() - n_blobs;
  assert((n_blobs > 0) && "Empty serialization?  What went wrong?");

  for (size_t abs_idx = blob_ptrs.size() - n_blobs, idx = 0;
       idx != n_blobs;
       ++abs_idx, ++idx)
  {
    const auto blob = blob_ptrs[abs_idx];
    const auto seg_size = blob->size();

    FLOW_LOG_TRACE("Heap_fixed_builder [" << *this << "]: "
                   "Serialization segment [" << idx << "] (0 based, of [" << n_blobs << "], 1-based): "
                   "Heap buffer @[" << static_cast<const void*>(blob->begin()) << "] sized [" << seg_size << "].");

    // Check for the one error condition: a too-large segment.
    if (seg_size > m_segment_sz)
    {
      FLOW_LOG_WARNING("Heap_fixed_builder [" << *this << "]: "
                       "Serialization segment [" << idx << "] (0 based, of [" << n_blobs << "], 1-based): "
                       "Heap buffer @[" << static_cast<const void*>(blob->begin()) << "] "
                       "sized [" << seg_size << "]: exceeded limit "
                       "[" << m_segment_sz << "].  Emitting error; will not return serialization.");
      *err_code = error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG;
      return;
    }
    // else
  } // for (blob : blob_ptrs, starting from first appended one)
  // Got here: no error.  Done and done.
} // Heap_fixed_builder::emit_serialization()

size_t Heap_fixed_builder::n_serialization_segments() const
{
  assert(m_engine && "Are you operating on a moved-from `*this`?");

  return m_engine->n_segments();
}

std::ostream& operator<<(std::ostream& os, const Heap_fixed_builder& val)
{
  return os << '@' << &val;
}

// Heap_reader implementations.

Heap_reader::Heap_reader(const Config& config) :
  flow::log::Log_context(config.m_logger_ptr, Log_component::S_TRANSPORT)
{
  FLOW_LOG_TRACE("Heap_reader [" << *this << "]: Heap reader started: "
                 "expecting [" << config.m_n_segments_est << "] segments (0 if unknown).");
  m_serialization_segments.reserve(config.m_n_segments_est);
}

Heap_reader::~Heap_reader()
{
  if (!m_engine)
  {
    FLOW_LOG_TRACE("Heap_reader [" << *this << "]: Reader being destroyed including: "
                   "serialization containing [" << m_serialization_segments.size() << "] segments; "
                   "no deserialization yet attempted.");
  }
  else
  {
    // This *might* be perf-pricey, to some extent, so definitely be careful with the log level.
    FLOW_LOG_TRACE("Heap_reader [" << *this << "]: Reader being destroyed including: "
                   "serialization containing [" << m_serialization_segments.size() << "] segments; "
                   "last deserialization reports [" << (m_engine->sizeInWords() * sizeof(::capnp::word)) << "] "
                   "input heap space total.");
  }
}

flow::util::Blob* Heap_reader::add_serialization_segment(size_t max_sz)
{
  using flow::util::Blob;

  const auto blob = new Blob(get_logger(), max_sz);
  m_serialization_segments.emplace_back(blob);

  FLOW_LOG_TRACE("Heap_reader [" << *this << "]: "
                 "Serialization segment [" << (m_serialization_segments.size() - 1) << "] (0-based) added/owned: "
                 "Heap buffer @[" << static_cast<const void*>(m_serialization_segments.back()->const_data()) << "] "
                 "max-sized [" << max_sz << "].");

  return blob;
}

std::ostream& operator<<(std::ostream& os, const Heap_reader& val)
{
  return os << '@' << &val;
}

} // namespace ipc::transport::struc
