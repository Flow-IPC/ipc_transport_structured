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

#include "ipc/transport/struc/serializer_stats.hpp"

namespace ipc::transport::struc
{

// Types.

/**
 * A `capnp::MessageBuilder` used by Heap_fixed_builder: similar to a `capnp::MallocMessageBuilder` (albeit with
 * somewhat different accounting/knobs) and with framing/header space around the allocated segment(s).
 *
 * It can also be used as a #Capnp_msg_builder_interface (`capnp::MessageBuilder`) independently of the rest
 * of ipc::transport or even ::ipc, although that was not the impetus for its development originally.
 *
 * ### Move-ctible and move-assignable? ###
 * As of capnp-1.0.0 at the latest, but sometime after capnp-0.10, the super-class #Capnp_msg_builder_interface
 * a/k/a/ `capnp::MessageBuilder` -- which is not a pure interface but includes some impl -- is no longer
 * move-ctible or move-assignable.  Therefore we aren't either.
 *
 * (Historically: When it was movable, we were too, but we still advised that the super-class size is such that
 * using its move facilities instead of e.g. `unique_ptr` wrapping might have been a bit slow.)
 *
 * ### Rationale ###
 * Why we'd use a `MallocMessageBuilder`-like approach is explained within Heap_fixed_builder doc header.
 * So why not just use that actual class?  Answer: We send the resulting serialization segments over IPC pipes
 * (Blob_sender, Native_handle_sender), and in some cases struc::Channel needs to surround the serialization
 * with a bit of metadata.  `MallocMessageBuilder` will, for each segment, allocate X bytes -- and then potentially
 * fill all of them.  The only way for struc::Channel to work with this is to allocate another buffer of
 * X + (frame size) bytes and then copy the X bytes into it -- losing the adherence to the zero-copy-when-possible
 * paradigm.  Hence this class which will use `flow::util::Basic_blob` machinery to maintain the frame space.
 *
 * Note that in order to make use of the added feature (frame space) one should *not* access the serialization result
 * via the usual `MessageBuilder::getSegmentsForOutput()`.  That still works, and that method is essential in
 * the impl of this class; but it returns only the actual serialization segments (each one's start and size,
 * not including for example the allocated-but-unused data probably at the tail of the allocated buffer).
 * Technically it would indeed be correct/safe to simply write frame prefix data by subtracting `frame_prefix_sz`
 * from the segment's start pointer and writing there.  However that is error-prone and inconvenient.  Call
 * emit_segment_blobs() instead: it will return the `flow::util::Basic_blob`s which will have set `start()` and `size()`
 * appropriately (see emit_segment_blobs() doc header).
 */
class Heap_fixed_builder_capnp_message_builder :
  public Capnp_msg_builder_interface
{
public:
  // Types.

  // Constructors/destructor.

  /**
   * Constructs the message-builder, with the policy to allocate the segments in a particular way.
   * See the doc headers for invidiuals args for details.
   *
   * For a given segment, emit_segment_blobs() shall emit it in the form of a sequence
   * of `Basic_blob` pointers, 1-1 with the segments, where for a given `Basic_blob B` that capnp-serialization segment
   * itself is in range [`B.begin()`, `B.end()`) (indices within internal buffer [`B.start()`, `B.start() + B.size()`));
   * and the header if any lives in area [`B.begin() - B.start()`, `B.begin()`).
   *
   * Also/instead `getSegmentsForOutput()` can be used to obtain the same (well, very-related) information.  The form
   * is somewhat different; as only memory area descriptions are returned; are in `word`s instead of bytes; and
   * any header location/size would have to be calculated by the caller.  E.g., no equivalent of `B.start()` is
   * communicated, so the caller would have to have memorized it.
   *
   * @param seg_and_frame_sz_cap
   *        Say `allocateSegment(M)` is being called; we shall now select the size N of the buffer to allocate;
   *        though if `M * sizeof(word)` exceeds this size, then N shall be ignored in favor of the `M`-derived size.
   *        N shall be chosen based on `grow_seg_sz_else_constant` and `segment_sz_init_pre_growth` as well as
   *        the value(s) chosen in preceding `allocateSegment()` calls if any; and then it shall be capped at
   *        `seg_and_frame_sz_cap`.  Note that the buffer being described shall hold both the segment being requested;
   *        and header space just ahead of it (if so configured).
   * @param frame_prefix_sz1
   *        For the first allocateSegment(): The allocated buffer shall have `.start() ==` this value.  Hence
   *        the reserved header area for seg 1 is of this size and is immediately followed by the segment itself.
   *        Can be zero.  Must be a multiple of `sizeof(capnp::word)`.
   * @param frame_prefix_sz_cont
   *        Same as `frame_prefix_sz1` but applied to each subsequent allocateSegment() if any.
   * @param grow_seg_sz_else_constant
   *        `true` means mimic capnp's `GROW_HEURISTICALLY` (preferred seg sizes grow ~exponentially) but capping
   *        preferred seg-size (including header if any) to `seg_and_frame_sz_cap` and starting at
   *        `segment_sz_init_pre_growth` (plus header if any).
   *        `false` means mimic `FIXED_SIZE` with preferred seg-size (including header if any) equal to
   *        `seg_and_frame_sz_cap`.  (In the latter case `segment_sz_init_pre_growth` is ignored.)
   *        Note: "Preferred" here communicates the capnp mechanics described in the doc header for
   *        arg `seg_and_frame_sz_cap` (where if `M` is large enough, it overrides our calculated "preferred" seg-size).
   * @param segment_sz_init_pre_growth
   *        See above.  Relevant only if `grow_seg_sz_else_constant == true`, this is chosen
   *        as the size of the segment (not counting header if any) for the first allocateSegment().
   *        (Due to capnp mechanics, `allocateSegment()` 1 is always called with arg equal to ~0; hence seg 1's
   *        size, unlike the others', is simply known ahead of time.)  If `grow_seg_sz_else_constant == false`, that
   *        seg 1 size (sans header) is instead chosen as `seg_and_frame_sz_cap - frame_prefix_sz1`.
   *        If it can be estimated, reasonably tightly, to be large enough for the entire message, then
   *        this can lead to significant perf wins: fewer zero-filling cycles spent and the entire message
   *        being in 1 segment.
   * @param stats
   *        Optional pointer to the cumulative stats to update.  If not null, pointee must outlive `*this`.
   */
  explicit Heap_fixed_builder_capnp_message_builder
             (size_t seg_and_frame_sz_cap, size_t frame_prefix_sz1, size_t frame_prefix_sz_cont,
              bool grow_seg_sz_else_constant,
              size_t segment_sz_init_pre_growth = sizeof(::capnp::word) * ::capnp::SUGGESTED_FIRST_SEGMENT_WORDS,
              stat::Serializer_stats::Snd* stats = nullptr);

  /**
   * Destructor: frees segments-related state held inside the base `MessageBuilder` and within `*this`; and
   * updates certain stats (if so configured in ctor) that can only be by definition sampled at a builder/message's
   * end of life.
   */
  ~Heap_fixed_builder_capnp_message_builder() override;

  // Methods.

  /**
   * Returns pointers to `Basic_blob`s which are the serialization segments at this time.  They are as follows:
   *   - Each `Basic_blob` holds a serialization segment, in order.  There shall be at least 1.
   *   - For each `Basic_blob B` (see ctor):
   *     - `B.start() == frame_prefix_sz`.
   *     - [`B.begin()`, `B.end()`) is the serialization segment.  Recall that `B.begin()` starts at position
   *       `B.start()`, while `B.end()` is at position `B.start() + B.size()`.
   *   - See ctor doc header for how `frame_prefix_sz` is determined.
   *
   * This method can be invoked more than once; however the emitted result is meaningless once any mutation occurs
   * subsequently.
   *
   * @warning For a given emitted blob `B`: You'll note it is mutable.  However, behavior is undefined if you
   *          modify anything about it or it-described memory area except the contents of the header memory area
   *          [`B.begin() - B.start()`, `B.begin()`).  Hence, things you are disallowed to do include (but may not
   *          be limited to): modifying any other `B`-owned memory area (anything in
   *          [`B.begin()`, `B.begin() - B.start() + B.capacity()`); modifying any metadata of `B`
   *          itself (no `B.resize()` or derivatives thereof; no `B.make_zero()` or derivatives thereof).
   *
   * It is mutable expressly for the purpose of potentially modifying the header area -- that is it!
   *
   * ### Rationale ###
   * This API is, on reflection, a bit funky.  And, really, one could get by with just `getSegmentsForOutput()`
   * (and memorizing the header policy, as passed to our ctor).  It evolved this way, and to be fair it's nice
   * to emit information that's stored anyway, namely `flow::util::Basic_blob::start()` being the header size (et al).
   * On the other hand it's somewhat odd to emit mutable `Basic_blob`s but only formally allow modifying the header
   * areas they describe.  It's not language-enforced which is... well... it's okay but not ideal.
   *
   * At this time I (ygoldfel) think it's fine in the context of how Flow-IPC uses it but would've been probably
   * too funky if its main or only use-case were direct calls by the user.  So maintainers should keep that in mind.
   *
   * @param target_blob_ptrs
   *        It is appended-to.  Recall these are mere pointers (into `*this`-managed heap memory, with each pointee
   *        `Blob_sans_log_context` describing a header + segment memory area).
   */
  void emit_segment_blobs(Segment_ptrs* target_blob_ptrs);

  /**
   * Returns what `target_blob_ptrs.size()` would return after calling `emit_segment_blobs(&target_blob_ptrs)` (with
   * an empty `target_blob_ptrs` going-in), right now.  In other words it is the number of times capnp has invoked
   * allocateSegment().
   *
   * @return See above.
   */
  size_t n_segments() const;

  /**
   * Implements `MessageBuilder` API.  Invoked by capnp, as the user mutates via `Builder`s.  Do not invoke directly.
   *
   * @note The strange capitalization (that goes against standard Flow-IPC style) is because we are implementing
   *       a capnp API.
   *
   * For details as to how this will behave in context of `*this` see Heap_fixed_builder_capnp_message_builder()
   * ctor doc header.
   *
   * @param min_sz_words
   *        See `MessageBuilder` API.
   *        The allocated segment will allow for a serialization of at most
   *        `min(min_sz_words * sizeof(word), segment_sz)` bytes, where `segment_sz` is determined according to the
   *        algorithm described in our ctor doc header (plus adjusted to the ceiling-nearest word).
   * @return See `MessageBuilder` API.
   *         The ptr and size of the area for capnp to serialize-to.
   *         (As noted in ctor doc header: `frame_prefix_sz` bytes will precede this area; the value is determined as
   *         described in our ctor doc header.)
   */
  kj::ArrayPtr<::capnp::word> allocateSegment(unsigned int min_sz_words) override;

private:
  // Data.

  /// See ctor.  A multiple of capnp `word`.
  const size_t m_seg_and_frame_sz_cap;
  /// See ctor.
  const size_t m_frame_prefix_sz1;
  /// See ctor.
  const size_t m_frame_prefix_sz_cont;
  /**
   * See ctor; this is set to `true` if we will grow #m_segment_sz; otherwise keep it constant.
   * In addition we set it to `false` upon reaching #m_seg_and_frame_sz_cap -- no need to keep increasing it, once
   * we have hit the limit.
   */
  bool m_grow_seg_sz_else_constant;

  /**
   * Preferred, capped size of the next segment returned (and allocated) by allocateSegment() -- if indeed
   * `allocateSegment()` will again be called; a word-multiple.  `allocateSegment()` will be called at least 1x;
   * so before that happens the initial value is either `segment_sz_init_pre_growth` (if
   * #m_grow_seg_sz_else_constant is `true`) or `m_seg_and_frame_sz_cap - m_frame_prefix_sz1` (otherwise).
   * In the latter case it is also the final/forever value.  Also, once the cap is reached in the former case,
   * `m_segment_sz` similarly stops changing (in fact we then modify `m_grow_seg_sz_else_constant = false`).
   *
   * Explanation of "capped": `m_segment_sz` shall not exceed `m_seg_and_frame_sz_cap - F`, where `F` is one
   * of `m_frame_prefix_sz1` (before first allocateSegment() call) or `m_frame_prefix_sz_cont` (after it).
   * This is guaranteed initially in ctor; and subsequently in allocateSegment(), when (and if) it grows
   * `m_segment_sz` therein.
   *
   * Explanation of "preferred": When selecting the actual segment size to return from allocateSegment(),
   * the value chosen will be the *higher* of `min_sz` words or `m_segment_sz` (with cap already applied); where
   * `min_sz` is the value passed by capnp into allocateSegment().  In other words `m_segment_sz` is
   * what we'd "prefer" to size the segment, but if user demands more space, we will follow that demand.
   * That typically occurs when the user tries to create a big leaf (list/blob/string).
   *
   * Explanation of how `m_segment_sz` grows while `m_grow_seg_sz_else_constant == true`:
   * Vaguely speaking, it follows exponential growth with each allocateSegment() call.  More specifically,
   * we rip off `MallocMessageBuilder`: `m_segment_sz` is increased by the size of the segment actually chosen.
   * So for example, supposing allocateSegment() is called always with `min_sz == 1` (not realistic... but it models
   * well-enough needing more space for a small leaf, once the existing segment is too full, so the "preferred" size
   * is never exceeded by `min_sz`), and the init size is 8Ki, with a huge cap and no headers for simplicity:
   * `m_segment_sz` shall start at 8Ki; seg 1 shall be 8Ki, `m_segment_sz` shall increase to 16Ki; seg 2 shall be
   * 16Ki, `m_segment_sz` shall increase to 32Ki (and so on, until no `allocateSegment()` is forthcoming, and
   * the last-computed `m_segment_sz` is left unused).  (Now, if a too-big `min_sz` is involved, then `m_segment_sz`
   * wouldn't double but grow even more than that, that time.  The header and cap if any affect the growth a
   * bit... etc.  The general pattern is roughly as shown though.)
   */
  size_t m_segment_sz;

  /**
   * Before emit_segment_blobs() emits a given segment, stores each segment allocated as requested by capnp via
   * allocateSegment() `virtual` API, with `.size()` equal to `.capacity()` and >= `min_sz` words (arg to that method);
   * immediately after emit_segment_blobs() each `.size()` has been adjusted to indicate the # of bytes actually used in
   * that segment in the final serialization (so far).  emit_segment_blobs() yields an equally sized list of pointers
   * to the `.size()`-adjusted per-segment `Basic_blob`s.
   *
   * Note that `*this` itself does not rely on each guy's `.size()` which depends on whether allocateSegment() of that
   * guy, or emit_segment_blobs() of that guy, was last called; it is used purely as an out-arg (of sorts) via
   * emit_segment_blobs().  In *that* case `.size()` is set (by emit_segment_blobs()) to the above specific meaning.
   *
   * How does `*this` know the size of the serialization then?  Answer: via `getSegmentsForOutput()`.  How does
   * capnp know the size of the allocated buffer then?  Answer: we give it to it via allocateSegment() return value,
   * and it memorizes it in `MessageBuilder` super-object somewhere (and `getSegmentsForOutput()` grabs that).
   */
  std::vector<flow::util::Blob_sans_log_context> m_serialization_segments_plus_frame_space;

  /// Cumulative stats target, or null if no stats recording is configured.
  stat::Serializer_stats::Snd* const m_stats;

  /**
   * Running tally: sum of `seg_sz` chosen for each allocateSegment() call.  Sampled
   * into #m_stats histo at dtor; also used to decrement the alloc-bytes-outstanding gauge.
   * It is only used for stats, so it's untouched if null #m_stats.
   */
  uint64_t m_alloc_sz;

  /**
   * Gate: `false` until the first big-leaf event happens; then flipped to `true`, and `m_msgs_with_big_leaf` stat
   * goes up.  It is only used for stats, so it's untouched if null #m_stats.
   */
  bool m_saw_big_leaf;
}; // class Heap_fixed_builder_capnp_message_builder

} // namespace ipc::transport::struc
