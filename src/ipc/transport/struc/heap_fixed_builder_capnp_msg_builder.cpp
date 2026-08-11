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

#include "ipc/transport/struc/heap_fixed_builder_capnp_msg_builder.hpp"
#include "ipc/transport/struc/serializer_stats.hpp"

namespace ipc::transport::struc
{

// Implementations.

Heap_fixed_builder_capnp_message_builder::Heap_fixed_builder_capnp_message_builder
  (size_t seg_and_frame_sz_cap, size_t frame_prefix_sz1, size_t frame_prefix_sz_cont,
   bool grow_seg_sz_else_constant, size_t segment_sz_init_pre_growth,
   stat::Serializer_stats::Snd* stats) :
  /* This is a hard cap for a thing that must be a word-multiple; so we want it to be a word-multiple;
   * but also this is an attempt to fit every segment in this many bytes at most *including* the frame prefix.
   * Point is, round down: do not use round_to_multiple() (which rounds up). */
  m_seg_and_frame_sz_cap((seg_and_frame_sz_cap / sizeof(::capnp::word)) * sizeof(::capnp::word)),
  m_frame_prefix_sz1(frame_prefix_sz1),
  m_frame_prefix_sz_cont(frame_prefix_sz_cont),
  m_grow_seg_sz_else_constant(grow_seg_sz_else_constant),
  // Borrow MallocMessageBuilder's heuristic when grow_seg_sz_else_constant (and don't forget to apply the cap).
  m_segment_sz(grow_seg_sz_else_constant
                 ? std::min(flow::util::round_to_multiple(segment_sz_init_pre_growth, sizeof(::capnp::word)),
                            m_seg_and_frame_sz_cap - m_frame_prefix_sz1)
                 : (m_seg_and_frame_sz_cap - m_frame_prefix_sz1)),
  m_stats(stats),
  m_alloc_sz(0),
  m_saw_big_leaf(false)
{
  // By contract we can't let the prefix frame change the start of the area given to capnp to be non-aligned.
  assert(((m_frame_prefix_sz1 % sizeof(::capnp::word)) == 0)
         && "N=frame_prefix_sz1 passed to ctor, by contract, must be such that (aligned_ptr + N) is also aligned.");
  assert(((m_frame_prefix_sz_cont % sizeof(::capnp::word)) == 0)
         && "N=frame_prefix_sz_cont passed to ctor, by contract, must be such that (aligned_ptr + N) is also aligned.");

  /* allocateSegment() relies on the pre-condition that m_segment_sz is capped (and perpetuates this); we (or the user,
   * if m_grow_seg_sz_else_constant) should have ensured this pre-condition initially. */
  assert((m_segment_sz <= (m_seg_and_frame_sz_cap - m_frame_prefix_sz1))
         && "We should have assigned this to the cap or capped it at the cap, above.");
}

Heap_fixed_builder_capnp_message_builder::~Heap_fixed_builder_capnp_message_builder()
{
  using flow::util::stat::fetch_sub;
  using flow::util::stat::fetch_add;

  /* Stats: end-of-life sampling.  No-op if no stats configured, or if `*this` was never used to
   * actually allocate (e.g., a Msg_out was constructed but never serialized). */
  if ((!m_stats) || m_serialization_segments_plus_frame_space.empty())
  {
    return;
  }
  // else

#ifndef NDEBUG
  const auto prev_alloc_outstanding_sz =
#endif
  fetch_sub(&m_stats->m_alloc_outstanding_sz, m_alloc_sz);
  assert((prev_alloc_outstanding_sz >= m_alloc_sz)
         && "GAUGE underflow: dtor's per-msg subtraction exceeds the cumulative outstanding.");

#ifndef NDEBUG
  const auto prev_msgs_outstanding =
#endif
  fetch_sub(&m_stats->m_msgs_outstanding, 1);
  assert((prev_msgs_outstanding >= 1)
         && "GAUGE underflow: dtor decrement on already-zero msgs-outstanding.");

  /* Allocated-bytes; we've summed this opportunistically.  As promised we sample this at the end, once it can
   * no longer grow through further capnp-mutations and therefore allocateSegment()s (potentially). */
  m_stats->m_histo_msg_alloc_sz.record_value(m_alloc_sz);

  /* Used-bytes: sum of bytes capnp actually ended up using by the end.  Note: if
   * lend() ran, the SHM-stored Basic_blobs were also resize()d to match these sizes; if lend() did
   * not run (e.g., Msg_out built but never sent), getSegmentsForOutput() still reflects capnp's last
   * write -- so this is the right source either way.  Using a Basic_blob::size() here would be a (subtle, often
   * hard-to-notice) bug. */
  uint64_t used_bytes = 0;
  for (const auto& capnp_seg : getSegmentsForOutput())
  {
    used_bytes += capnp_seg.asBytes().size();
  }
  m_stats->m_histo_msg_used_sz.record_value(used_bytes);

  m_stats->m_histo_segs_per_msg.record_value(m_serialization_segments_plus_frame_space.size());
} // Heap_fixed_builder_capnp_message_builder::~Heap_fixed_builder_capnp_message_builder()

void Heap_fixed_builder_capnp_message_builder::emit_segment_blobs(Segment_ptrs* target_blob_ptrs_ptr)
{
  // We'll need to adjust it a bit below.
  auto& blobs = m_serialization_segments_plus_frame_space;
  auto& target_blob_ptrs = *target_blob_ptrs_ptr;

  assert((!blobs.empty()) && "Should not be possible for serialization to be empty with our use cases.  Investigate.");
  target_blob_ptrs.reserve(target_blob_ptrs.size() + blobs.size()); // Remember: we are appending; not replacing.

  const auto capnp_segs = getSegmentsForOutput();
  assert((capnp_segs.size() == n_segments())
         && "Somehow our MessageBuilder created fewer or more segments than allocateSegment() was called?!");

  for (size_t idx = 0; idx != capnp_segs.size(); ++idx)
  {
    // Pointer-to-word + size in words => pointer-to-byte + size in bytes.
    const auto capnp_seg = capnp_segs[idx].asBytes();
    const auto seg_sz = capnp_seg.size();

    auto& blob = blobs[idx];

    assert((capnp_seg.begin() == blob.begin())
           && "Somehow capnp-returned segments are out of order to allocateSegment() calls; or something....");
    assert((seg_sz != 0)
           && "capnp shouldn't be generating zero-sized segments.");

    blob.resize(seg_sz); // Just pull up end() to immediately follow the serialization.

    /* @todo Maybe output some stuff, like the segment dump, here, like
     * shm::capnp_message_builder::lend() does?  Though, in practice, this is always the
     * top serialization and hence is printed out in pretty good detail as the actual blob is transmitted over
     * ipc::transport core layer.  So maybe skip it to avoid unneeded verbosity. */

    /* Lastly output a *pointer*, as promised, to the Basic_blob we hold.  See our doc header re. what they're formally
     * allowed to do to the pointee.  Basically though: all bets are off upon the next capnp-mutation; until
     * then any read-only access is fine; as for writing they can only write into the header area
     * (from `.begin() - .start()` and up-to `.begin()`) and can modify nothing else (no other memory area nor
     * the Basic_blob itself).  Though, as for the latter, we basically just don't want .begin() to change. */
    target_blob_ptrs.push_back(&blob);
  } // for (idx in [0, capnp_segs.size()))
} // Heap_fixed_builder_capnp_message_builder::emit_segment_blobs()

kj::ArrayPtr<capnp::word>
  Heap_fixed_builder_capnp_message_builder::allocateSegment(unsigned int min_sz_words) // Virtual.
{
  using Word = capnp::word;
  using Capnp_word_buf = kj::ArrayPtr<Word>;
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;
  constexpr size_t WORD_SZ = sizeof(Word);
  const size_t min_sz = size_t(min_sz_words) * WORD_SZ; // Don't forget: in their API min_sz is in `word`s.

  /* It'd be nice not to realloc m_serialization_segments_plus_frame_space's internal buffer, as that involves
   * moving `unique_ptr<Blob>`s around: constant-time/cheap though it is, if we can avoid it, good.
   * This value is pretty decent; 1 is really most typical, while 4 shallow `Blob`s is not a ton of RAM. */
  constexpr size_t N_SEGS_GUESS = 4;
  m_serialization_segments_plus_frame_space.reserve(N_SEGS_GUESS); // No-op after 1st time.

  /* Background from capnp: They're saying they need the allocated space for serialization to store at least min_sz:
   * probably they're going to store some object that needs at least this much space.  So typically it's some
   * scalar leaf thing, like 4 bytes or whatever; but it could be larger -- or even huge (e.g., a Data or List
   * of huge size, because the user mutated it so via a ::Builder).  Oh, and it has to be zeroed, as by calloc().
   *
   * So all we *have* to allocate is min_sz exactly in that sense.  But the idea is to try to allocate more, so that
   * capnp can efficiently shove more objects in there too without calling allocateSegment() for each one.
   * Namely:
   *   - If !m_grow_seg_sz_else_constant:
   *     As a fixed-heap builder we promised to make that the fixed size m_segment_sz.
   *   - Else:
   *     We're supposed to grow exponentially each time, so we keep track of the next size in m_segment_sz, same
   *     as capnp::MallocMessageBuilder internally does (check its source code).  Plus, we apply the cap
   *     m_seg_and_frame_sz_cap.
   * Of course, if min_sz exceeds that, then we have no choice but to allocate the larger amount min_sz. */

  const bool big_leaf = min_sz > m_segment_sz;
  const size_t seg_sz
    = big_leaf ? min_sz
               /* m_segment_sz is maintained to be a word-multiple.
                * Its init value is pre-capped; and subsequently if we grow it, we cap it then (below). */
               : m_segment_sz;

  // Now just follow the logic we expounded on in the various doc headers.  Keeping explanations light here.

  m_serialization_segments_plus_frame_space.emplace_back();
  auto& blob = m_serialization_segments_plus_frame_space.back();

  const bool first_alloc = m_serialization_segments_plus_frame_space.size() == 1;
  const size_t frame_prefix_sz = first_alloc ? m_frame_prefix_sz1 : m_frame_prefix_sz_cont;

  blob.reserve(frame_prefix_sz + seg_sz, flow::util::CLEAR_ON_ALLOC);
  /* Attn!  capnp requires: it must be zeroed.  And Blob::reserve(N, CLEAR_ON_ALLOC) we used *does* zero it.
   * So do not memset() it.  This is, at worst, syntactic sugar; at best it can bring significant perf improvements
   * (e.g., calloc(64Ki) we've seen be ~20% faster than malloc() + memset()). */

  blob.resize(seg_sz, frame_prefix_sz); // Set: size(), start(), respectively.
  assert(blob.size() != 0);

  /* Since we are supposed to grow exponentially, increase this for next time (if any), if so configured (and cap not
   * yet reached).  So then next time (if any) m_segment_sz can be used as-is (see above), no cap check. */
  const bool prev_grow_seg_sz_else_constant = m_grow_seg_sz_else_constant;
  if (prev_grow_seg_sz_else_constant)
  {
    m_segment_sz += seg_sz; // Both values are word-multiples.

    // Compute the *next* cap (hence, always use m_frame_prefix_sz_cont); then apply it for next time (if any).

    const auto hard_sz_cap = m_seg_and_frame_sz_cap - m_frame_prefix_sz_cont; // Values are word-multiples.
    if (m_segment_sz >= hard_sz_cap)
    {
      m_segment_sz = hard_sz_cap; // (No-op if they're equal... but the next line applies too in that case.)
      m_grow_seg_sz_else_constant = false; // Reached the cap; so next time, if any, don't worry about it.
    }
  }
  // else { Configured to not grow from the start; or reached the cap in a preceding iteration. }

  /* @todo MallocMessageBuilder does some bounding according to some absolute maximum.  Probably we should do the same.
   * Get back to this and follow capnp-interface reqs and/or follow what their internal logic does.
   * Just, m_seg_and_frame_sz_cap is already a cap... but what if it is itself too huge?  It's a corner case for sure.
   * If we do handle it, it's probably easiest to just cap m_seg_and_frame_sz_cap when first setting it in ctor. */

  // Stats.
  if (m_stats)
  {
    if (first_alloc)
    {
      // First alloc for this message: update message counter, outstanding-gauge, high-water mark thereof.
      fetch_add(&m_stats->m_msgs, 1);
      update_hi_wmark(&m_stats->m_msgs_outstanding_hi_wmark,
                      fetch_add(&m_stats->m_msgs_outstanding, 1) + 1);
    }

    /* Capacity-lock transition: at-most-once per *this -- so this is also the count of `*this`es
     * that hit the cap. */
    if (prev_grow_seg_sz_else_constant != m_grow_seg_sz_else_constant)
    {
      fetch_add(&m_stats->m_seg_grow_cap_lock_count, 1);
    }

    // Big-leaf event: capnp asked for more than our preferred (based on capped-growth algorithm) next-segment size.
    if (big_leaf)
    {
      fetch_add(&m_stats->m_big_leaf_alloc_count, 1);
      m_stats->m_histo_big_leaf_sz.record_value(min_sz);
      if (!m_saw_big_leaf)
      {
        m_saw_big_leaf = true;
        fetch_add(&m_stats->m_msgs_with_big_leaf, 1);
      }
    }

    /* Lifetime + outstanding alloc-byte counters (every call).  Excludes frame prefix; that goes into
     * m_frame_lifetime_sz separately.  Total physical heap alloc = m_alloc_lifetime_sz + m_frame_lifetime_sz. */
    fetch_add(&m_stats->m_alloc_lifetime_sz, seg_sz);
    update_hi_wmark(&m_stats->m_alloc_outstanding_sz_hi_wmark,
                    fetch_add(&m_stats->m_alloc_outstanding_sz, seg_sz) + seg_sz);
    m_alloc_sz += seg_sz; // For dtor.

    // Frame-prefix bytes as mentioned above.
    fetch_add(&m_stats->m_frame_lifetime_sz, frame_prefix_sz);
  } // if (m_stats)

  return Capnp_word_buf{reinterpret_cast<Word*>(blob.begin()),
                        reinterpret_cast<Word*>(blob.end())};
} // Heap_fixed_builder_capnp_message_builder::allocateSegment()

size_t Heap_fixed_builder_capnp_message_builder::n_segments() const
{
  return m_serialization_segments_plus_frame_space.size();
  // Note this equals getSegmentsForOutput().size().  emit_segment_blobs() as of this writing assert()s as much.
}

} // namespace ipc::transport::struc
