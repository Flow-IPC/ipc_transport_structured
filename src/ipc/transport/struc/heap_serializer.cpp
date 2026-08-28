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
#include <boost/move/make_unique.hpp>
#include <type_traits>

namespace ipc::transport::struc
{

// Static initializers.

const Null_session NULL_SESSION;

// Heap_fixed_builder implementations.

Heap_fixed_builder::Heap_fixed_builder() = default;

Heap_fixed_builder::Heap_fixed_builder(const Config& config) :
  flow::log::Log_context(config.m_logger_ptr, Log_component::S_TRANSPORT),
  // (The rounding-down-multiple thing is specified in Heap_fixed_builder_capnp_message_builder ctor doc header.)
  m_seg_and_frame_sz_cap((config.m_seg_and_frame_sz_cap / sizeof(::capnp::word)) * sizeof(::capnp::word)),
  // The builder engine launched here.  In the future mutators will cause it to allocate in heap on-demand.
  m_engine(boost::movelib::make_unique<Capnp_heap_engine>
             (m_seg_and_frame_sz_cap,
              config.m_frame_prefix_sz, // Do possibly reserve header space before seg 1.
              0, // Don't reserve headers before each of seg 2, 3, ....
              true, // Always use the exponential seg size growth, technically...
              config.m_segment_sz_init, // ...but if this ~equals m_seg_and_frame_sz_cap then effectively don't.
              config.m_stats)) // Snd-stats target chosen by config (typically by factory); null = no stats.
{
  FLOW_LOG_TRACE("Heap_fixed_builder [" << *this << "]: Fixed-size builder started: "
                 "segment (plus framing) size limit [" << m_seg_and_frame_sz_cap << "], "
                 "seg1 min size [" << config.m_segment_sz_init << "]; seg1 frame header "
                 "[" << config.m_frame_prefix_sz << "].");
  assert(m_seg_and_frame_sz_cap != 0);
  assert(config.m_segment_sz_init != 0);
  /* Note: config.m_segment_sz_init may exceed m_seg_and_frame_sz_cap - config.m_frame_prefix_sz; per Config
   * contract the engine clamps it to that.  (E.g., struc::Channel::heap_fixed_builder_config() counts on this:
   * it forwards the user's segment1_sz estimate as-is, with the cap set to the channel's max-blob-size.) */
}

Heap_fixed_builder::Heap_fixed_builder(Heap_fixed_builder&&) = default;

Heap_fixed_builder::~Heap_fixed_builder()
{
  FLOW_LOG_TRACE("Heap_fixed_builder [" << *this << "]: Fixed-size builder being destroyed.");
  /* @todo Maybe have Heap_fixed_builder_capnp_message_builder log something useful in its dtor?
   * It was more or less intentional for that capnp::MessageBuilder to not log/even take a log-context, so
   * this is not a slam-dunk.  Revisit.
   *
   * @todo Possibly TRACE-log final stats set by m_engine (either here + move-assign -- if preceding to-do not done;
   * or else in aforementioned dtor).  Also not a slam-dunk, as in the mainstream the same stats are being
   * touched by others concurrently (typically -- definitely not necessarily -- via some globals).  Still it could
   * help debugging/testing.
   *
   * @todo The preceding to-do (with not-a-slam-dunk caveat) would also apply at least to shm::Builder's
   * m_btm_engine type's dtor.
   *
   * @todo The preceding to-do (with not-a-slam-dunk caveat) would also apply to Heap_reader dtor. */
}

Heap_fixed_builder& Heap_fixed_builder::operator=(Heap_fixed_builder&& src)
{
  /* This exists only to log; otherwise it would be `= default`.  Replacing #m_engine destroys the
   * underlying capnp `MessageBuilder`, where snd-stats finalize -- so mirror dtor's TRACE here so
   * the audit trail is honest on this path too. */
  if (&src != this)
  {
    if (m_engine)
    {
      FLOW_LOG_TRACE("Heap_fixed_builder [" << *this << "]: Fixed-size builder being destroyed "
                     "(move-assigned-over).");
    }

    // The rest is what `= default` would have done.
    static_cast<flow::log::Log_context&>(*this) = std::move(src);
    m_seg_and_frame_sz_cap = src.m_seg_and_frame_sz_cap;
    m_engine = std::move(src.m_engine);
  }

  return *this;
}

Capnp_msg_builder_interface* Heap_fixed_builder::payload_msg_builder()
{
  assert(m_engine && "Are you operating on a moved-from `*this`?");

  return m_engine.get();
}

void Heap_fixed_builder::emit_serialization(Segment_bufs* target_blobs, util::Blob_mutable* hdr_blob,
                                            Split_segments* split_segs,
                                            const Session& session, Error_code* err_code) const
{
  using util::Blob_const;
  using util::Blob_mutable;

  if (flow::error::exec_void_and_throw_on_error
        ([&](Error_code* actual_err_code)
           { emit_serialization(target_blobs, hdr_blob, split_segs, session, actual_err_code); },
         err_code, "Heap_fixed_builder::emit_serialization()"))
  {
    return;
  }
  // If got here: err_code is not null.

  assert(m_engine && "Are you operating on a moved-from `*this`?");

  /* Reminder: concept API says emit_serialization() can be called >1 times.  We do allow that in the below.
   * Keep in mind that mutations can be executed in-between, so whatever caching for perf one imagines has
   * to not mess up in the face of that. */

  const auto& seg_and_hdr_sz_cap = m_seg_and_frame_sz_cap;
  target_blobs->clear();
  if (split_segs)
  {
    split_segs->clear();
  }

  Segment_ptrs blob_ptrs;
  m_engine->emit_segment_blobs(&blob_ptrs);

  const auto n_blobs = blob_ptrs.size();
  assert((n_blobs != 0) && "Empty serialization?  What went wrong?");
  auto n_blobs_with_subsegs = n_blobs; // Small optimization involves this....
  target_blobs->reserve(n_blobs); // No splits (which is typical) means this optimization is sufficient.

  for (size_t idx = 0; idx != n_blobs; ++idx)
  {
    auto& blob = *(blob_ptrs[idx]);
    const auto buf = blob.mutable_buffer();
    const auto seg_sz = buf.size();

    /* Header area precedes seg 1 (which must exist); no other headers.  Reminder: our workhorse m_engine is
     * required to allocate the seg and also enough header bytes preceding it.  So we just recognize the fruits of
     * its labor now; and do the post-processing splitting if needed/requested. */

    size_t hdr_sz_or_0;
    if (idx == 0)
    {
      hdr_sz_or_0 = blob.start();
      *hdr_blob = Blob_mutable{static_cast<void*>(blob.begin() - hdr_sz_or_0), hdr_sz_or_0};
    }
    else
    {
      hdr_sz_or_0 = 0;
    }

    FLOW_LOG_TRACE("Heap_fixed_builder [" << *this << "]: "
                   "Serialization segment [" << idx << "] (0-based, of [" << n_blobs << "], 1-based): "
                   "Heap buffer @[" << buf.data() << "] sized [" << seg_sz << "]; "
                   "with header/prefix (if any) sized [" << hdr_sz_or_0 << "].");

    target_blobs->emplace_back(buf); // In case of split below its .size() shall be "corrected."

    // Check for the one error (or special) condition: a too-large segment.
    if ((seg_sz + hdr_sz_or_0) > seg_and_hdr_sz_cap)
    {
      if (!split_segs)
      {
        FLOW_LOG_WARNING("Heap_fixed_builder [" << *this << "]: "
                         "Serialization segment [" << idx << "] (0-based, of [" << n_blobs << "], 1-based): "
                         "Heap buffer @[" << buf.data() << "] "
                         "sized [" << seg_sz << "] plus frame header [" << hdr_sz_or_0 << "]: exceeded limit "
                         "[" << seg_and_hdr_sz_cap << "].  Seg-split not requested (why not? bug?) so: "
                         "Emitting error; will not return serialization.");
        *err_code = error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG;
        return;
      }
      // else if (split_segs):

      /* If seg is too big, split it up into the following, in order:
       *   - seg itself: Set its size to <cap minus hdr_sz_or_0> instead of the too-big size.
       *   - [n_full_cont_subsegs times]: "Whole" (cap-sized) continuation sub-segments.
       *   - [Only if a "partial" (<cap-sized) # of bytes is left over after that]: Final sub-segment.
       * n_cont_subsegs = n_full_cont_subsegs from bullet two, plus 1 if bullet three applies.
       *
       * Note we aren't actually copying any of the contents of the segment, just figuring out the locations
       * and sizes of the sub-segments within it.  (After IPC-transmission, reassembly will involve copying, as
       * the sub-segments shall be scattered in receiver's RAM.) */

      // Finalize the original segment (now sub-seg 1).
      auto last_subseg_ptr = static_cast<uint8_t*>(buf.data());
      auto last_subseg_sz = static_cast<size_t>(seg_and_hdr_sz_cap - hdr_sz_or_0);
      target_blobs->back() = Blob_const{last_subseg_ptr, last_subseg_sz};
      const size_t seg_minus_subseg1_sz = seg_sz - last_subseg_sz;
      // seg_minus_subseg1_sz = remaining # of bytes from original segment (all but sub-seg 1).

      // Bullet two above: This many full sub-segments (possibly 0).
      size_t n_full_cont_subsegs = seg_minus_subseg1_sz / seg_and_hdr_sz_cap;
      size_t n_cont_subsegs = n_full_cont_subsegs;

      // Bullet three above: Possibly 1 more sub-segment.
      const size_t partial_cont_subseg_sz_or_0 = seg_minus_subseg1_sz % seg_and_hdr_sz_cap;
      (partial_cont_subseg_sz_or_0 != 0) && (++n_cont_subsegs);

      // Little additional optimization to try minimizing shifting/reallocs due to the "unexpected" split-related segs.
      target_blobs->reserve(n_blobs_with_subsegs += n_cont_subsegs);

      // Okay, it's planned-out/accounted; update the actual out-args target_blobs and split_segs.

      for (; n_full_cont_subsegs != 0; --n_full_cont_subsegs)
      {
        last_subseg_ptr += last_subseg_sz;
        last_subseg_sz = seg_and_hdr_sz_cap;
        target_blobs->emplace_back(last_subseg_ptr, last_subseg_sz);
      }

      if (partial_cont_subseg_sz_or_0 != 0)
      {
        last_subseg_ptr += last_subseg_sz;
        target_blobs->emplace_back(last_subseg_ptr, partial_cont_subseg_sz_or_0);
      }

      split_segs->emplace_back(Split_segment{ target_blobs->size() - n_cont_subsegs - 1,
                                              n_cont_subsegs });
      FLOW_LOG_TRACE("Heap_fixed_builder [" << *this << "]: "
                     "Serialization segment [" << idx << "] (0-based, of [" << n_blobs << "], 1-based): "
                     "Heap buffer @[" << buf.data() << "] "
                     "sized [" << seg_sz << "] plus frame header [" << hdr_sz_or_0 << "]: exceeded limit "
                     "[" << seg_and_hdr_sz_cap << "].  Seg-split requested; therefore split into "
                     "[" << (n_cont_subsegs + 1) << "] sub-segments sized "
                     "init[" << (seg_and_hdr_sz_cap - hdr_sz_or_0) << "], N x full[" << seg_and_hdr_sz_cap << "], "
                     "partial[" << partial_cont_subseg_sz_or_0 << "].");
    } // if ((seg_sz + hdr_sz_or_0) > seg_and_hdr_sz_cap)
    // else if ((seg_sz + hdr_sz_or_0) <= seg_and_hdr_sz_cap) { Segment need not be split. }
  } // for (blob : blob_ptrs)
  // Got here: no error.  Done and done.
  err_code->clear();
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
  flow::log::Log_context(config.m_logger_ptr, Log_component::S_TRANSPORT),
  m_stats(config.m_stats), // Rcv-stats target chosen by config (typically by factory); null = no stats.
  m_alloc_sz(0),
  m_used_sz(0)
{
  FLOW_LOG_TRACE("Heap_reader [" << *this << "]: Heap reader started: "
                 "expecting [" << config.m_n_segments_est << "] segments (0 if unknown).");
  m_serialization_segments.reserve(config.m_n_segments_est);
}

Heap_reader::~Heap_reader()
{
  using flow::util::stat::fetch_sub;

  if ((!m_stats) || (!m_engine)) // I.e., if (nothing to do except log):
  {
    FLOW_LOG_TRACE("Heap_reader [" << *this << "]: Reader being destroyed including: "
                   "serialization containing [" << m_serialization_segments.size() << "] segments; "
                   << (m_engine ? "no stats kept." : "no deserialization yet attempted."));
    /* If !m_engine: deserialization() never succeeded; nothing was incremented; nothing to decrement.
     * If !m_stats: stats not kept at all.  Either way, skip the decrements. */
    return;
  }
  // else

  // This *might* be perf-pricey, to some extent, so definitely be careful with the log level.
  FLOW_LOG_TRACE("Heap_reader [" << *this << "]: Reader being destroyed including: "
                 "serialization containing [" << m_serialization_segments.size() << "] segments; "
                 "last deserialization reports [" << (m_engine->sizeInWords() * sizeof(::capnp::word)) << "] "
                 "input heap space total.");

  /* Decrement gauges that deserialization() incremented.  Underflow assertions ride on the RMW's
   * pre-change return value. */
#ifndef NDEBUG
  const auto prev_msgs_outstanding =
#endif
  fetch_sub(&m_stats->m_msgs_outstanding, 1);
  assert((prev_msgs_outstanding >= 1)
         && "Bug: rcv-stats msgs_outstanding gauge underflow at Heap_reader dtor.");

#ifndef NDEBUG
  const auto prev_alloc_outstanding_sz =
#endif
  fetch_sub(&m_stats->m_alloc_outstanding_sz, m_alloc_sz);
  assert((prev_alloc_outstanding_sz >= m_alloc_sz)
         && "Bug: rcv-stats alloc-bytes_outstanding gauge underflow at Heap_reader dtor.");

#ifndef NDEBUG
  const auto prev_used_outstanding_sz =
#endif
  fetch_sub(&m_stats->m_used_outstanding_sz, m_used_sz);
  assert((prev_used_outstanding_sz >= m_used_sz)
         && "Bug: rcv-stats used-bytes_outstanding gauge underflow at Heap_reader dtor.");
} // Heap_reader::~Heap_reader()

void Heap_reader::add_serialization_segment(Segment_blob_in&& blob)
{
  using flow::util::Blob;
  using Ptr = const void*;

  static_assert(std::is_nothrow_move_constructible_v<Segment_blob_in>,
                "Segment_blob_in (as of this writing Basic_blob) move-ct must be noexcept for vector to move them "
                  "instead of copying on realloc; this is critical not just for performance, but because callers "
                  "may routinely rely on a given segment not moving in memory across add_serialization_segment()s.");

  FLOW_LOG_TRACE("Heap_reader [" << *this << "]: "
                 "Serialization segment [" << m_serialization_segments.size() << "] (0-based) added/owned: "
                 "Heap buffer @[" << static_cast<Ptr>(blob.const_data()) << "] "
                 "max-sized [" << blob.capacity() << "], "
                 "actual serialization sized [" << blob.size() << "].");

  /* Out of paranoia ensure seg1's memory never moves; the static_assert() above should ensure it already but still....
   * (None of the others should move either, but we'll just check it for that particular one.) */
#ifndef NDEBUG
  Ptr seg1_ptr = nullptr;
  m_serialization_segments.empty()
    || (seg1_ptr = static_cast<Ptr>(m_serialization_segments.front().const_data()));
#endif

  m_serialization_segments.emplace_back(std::move(blob));
  assert(((!seg1_ptr) || (seg1_ptr == static_cast<Ptr>(m_serialization_segments.front().const_data())))
         && "Bug: vector realloc moved a segment blob's buffer, presumably via unintended Basic_blob copy; "
              "make sure Basic_blob/Segment_blob_in is coded properly for by-value storage in containers.");
}

std::ostream& operator<<(std::ostream& os, const Heap_reader& val)
{
  return os << '@' << &val;
}

} // namespace ipc::transport::struc

namespace capnp
{

std::ostream& operator<<(std::ostream& os, const Text::Reader& val)
{
  return os << val.cStr();
}

} // namespace capnp
