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

#include "ipc/transport/struc/heap_fixed_builder_capnp_msg_builder.hpp"
#include "ipc/transport/struc/serializer_stats.hpp"
#include "ipc/transport/struc/error.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/error/error.hpp>
#include <boost/move/make_unique.hpp>
#include <utility>

namespace ipc::transport::struc
{

// Types.

/**
 * Value for Struct_builder::Session when no extra information is needed when serializing Struct_builder for
 * subsequent sending to another process.
 *
 * @see Struct_builder::Session concept.
 */
struct Null_session {};

/**
 * Simple structure describing how a pre-split single segment was turned into 2+ post-split component sub-segments.
 * @see #Split_segments for full context first.
 */
struct Split_segment
{
  // Data.

  /// The index into a fully-post-split #Segment_bufs (or similar) containing the first component of the split segment.
  size_t m_start_idx;

  /**
   * How many elements immediately following the one at #m_start_idx (but not including it!) compose the pre-split
   * segment.  This value shall be greater than or equal to 1. (0 would mean the segment was split into 1 components,
   * and by our convention in that degenerate case the Split_segment shall not be generated at all.)
   */
  size_t m_n_cont_subsegs;
};

/**
 * ~Simplest possible impl of Struct_builder concept that represents the capnp payload directly in heap memory
 * (outer serialization only), with a segment-allocation strategy similar to `capnp::MallocMessageBuilder`.  The
 * `Struct_builder`-required seg 1 header-space minutiae are also followed.
 *
 * That is, each mutation via payload_msg_builder() may, as needed, trigger a `malloc()` (or similar) of size N,
 * where N is (roughly speaking) either a fixed, configured value; or grows roughly exponentially each time up to
 * N.  (`capnp::MallocMessageBuilder` has similar knobs.)  Plus seg 1 shall be preceded by
 * a (zero-filled, untouched further by `*this`) header area of a specified size if so configured.
 *
 * @note This can/should be viewed as the core impl of Struct_builder concept.  Other impls thereof most likely
 *       should be built on top of/around a Heap_fixed_builder which would implement the outer serialization, an
 *       element in practice required in a uniform way by any Struct_builder.  For example shm::Builder does so: it
 *       maintains a Heap_fixed_builder for the outer serialization (even though in its case it's always one small
 *       segment, certainly without any splitting/joining needed) while itself handling the inner serialization
 *       in SHM.
 *
 * @internal
 * ### Implementation ###
 * If one is familiar with capnp, then one can probably quickly guess how this class template accomplishes
 * what it promises in just the brief summary at the top of this doc header:
 * Heap_fixed_builder_capnp_message_builder is what we use internally; it's like `MallocMessageBuilder`
 * but has support for a framing prefix coexisting within at least the first allocated buffer/segment.  As it must, that
 * builder (like `MallocMessageBuilder`) is documented to allocate a larger-than-N segment if necessary to encode a
 * particular too-large leaf.  We do not stop this in and of itself; but emit_serialization() -- if supplied
 * a split_segs out-arg -- will emit sub-segments in split fashion where needed.  (If `split_segs` is null, then
 * per concept requirements emit_serialization() will emit an error and barf, if a particular segment is too large.)
 * @endinternal
 *
 * @see Heap_reader
 *      Counterpart Struct_reader implementation that can deserialize data that this has serialized.
 * @see Struct_builder: implemented concept.
 */
class Heap_fixed_builder :
  public flow::log::Log_context
{
public:
  // Types.

  /**
   * Implements Struct_builder::Config sub-concept.  In particular, since this is the core outer-serialization
   * Struct_builder impl -- used either by itself or (realistically) in any other impl for its mandated
   * outer-serialization aspect -- it takes most of its members directly from the concept description.
   * Namely: #m_frame_prefix_sz; and (since indeed Heap_fixed_builder *can* produce non-small segments)
   * #m_seg_and_frame_sz_cap.  The latter is the max size of each segment (emit_serialization() will split any
   * segment if it was forced to make a larger one).  Lastly there is #m_segment_sz_init which is first (ideally
   * only) segment's size.
   *
   * @note If `m_segment_sz_init == m_seg_and_frame_sz_cap` (ignoring `m_frame_prefix_sz` for simplicity of exposition),
   *       then subsequent segments cannot grow any further (the cap is already reached).  This is in effect
   *       how `MallocMessageBuilder`'s `FIXED_SIZE` strategy acts, whereas if `m_segment_sz_init` is smaller, then
   *       we work like `GROW_HEURISTICALLY` strategy instead.
   *
   * @note Heap_fixed_builder_capnp_message_builder is internally used.  It implements `capnp::MessageBuilder` much as
   *       capnp's out-of-the-box `MallocMessageBuilder` does.  We mention this in case you needed a `MessageBuilder`
   *       directly for whatever reason.
   */
  struct Config
  {
    // Types.

    /// Implements concept API.
    using Builder = Heap_fixed_builder;

    // Data.

    /// Logger to use for logging subsequently.
    flow::log::Logger* m_logger_ptr;

    /**
     * Cap for how large a buffer size to choose for any header+segment buffer.  The header size is controlled
     * by #m_frame_prefix_sz; and is that value for seg 1 but zero for all subsequent ones.
     *
     * ### capnp behavior discussion ###
     * Let N be this value minus the header size for that segment (so `m_frame_prefix_sz` or 0).
     * Per capnp rules, a given segment may still exceed N, namely if capnp asks -- when calling
     * `capnp::MessageBuilder::allocateSegment()` API -- for a value exceeding N.  This occurs if and only if a
     * particular capnp-leaf, probably a list/blob/string, is by itself of that size; a leaf can't be split across
     * segments.
     */
    size_t m_seg_and_frame_sz_cap;

    /**
     * Seg 1 size; a value exceeding `m_seg_and_frame_sz_cap - m_frame_prefix_sz` is clamped to that.
     *
     * ### capnp behavior discussion ###
     * capnp will request a segment 1 of size ~0 regardless of how the user acts; as I (ygoldfel) recall before even any
     * capnp-generated mutators are called by the user.  Therefore seg 1 shall be sized according to this value
     * (as the larger of ~0 and X is X; in this case X being `m_seg_and_frame_sz_cap - m_frame_prefix_sz`);
     * it doesn't matter what the first leaf the user adds to the message.  Subsequent `allocateSegment()` calls,
     * however, will actually be based on what the capnp-user needs from that point on.
     *
     * The point here is that seg 1 has a known, certain size, which is a nice thing to be deterministic about;
     * after that it depends.  In particular if `m_seg_and_frame_sz_cap - m_frame_prefix_sz` is sufficient for
     * the entire message, then that's it.
     *
     * Therefore it is important for efficiency to select a nice `m_segment_sz_init` that's larger (ideally) than
     * what is ultimately necessary for the whole message but doesn't leave too much slack.  Zero-filling allocations
     * are fairly expensive (that is to say, `calloc(N)` is more expensive than `calloc(M)`, where N > M).
     */
    size_t m_segment_sz_init;

    /**
     * Size of header to precede seg 1 in the first buffer allocated by the Heap_fixed_builder.  The size must
     * be alignment-friendly (per align-size `sizeof(capnp::word)`).  Zero is allowed.
     *
     * No segment after seg 1 shall be preceded by such a header.
     */
    size_t m_frame_prefix_sz;

    /**
     * See Struct_builder::Config concept.  Pointee (null OK = no stats kept) for cumulative
     * snd-stats; must outlive each Heap_fixed_builder built with `*this`.
     */
    stat::Serializer_stats::Snd* m_stats;
  }; // class Config

  /**
   * Implements concept API.  This being a non-zero-copy Struct_builder, no information is needed for
   * emit_serialization() beyond the payload itself.
   */
  using Session = Null_session;

  // Constructors/destructor.

  /// Implements concept API.
  Heap_fixed_builder();

  /**
   * Implements concept API.  See Config doc header for information on how `*this` behavior is controlled by `config`.
   *
   * @param config
   *        See above.
   */
  explicit Heap_fixed_builder(const Config& config);

  /// Disallow copy construction.
  Heap_fixed_builder(const Heap_fixed_builder&) = delete;

  /**
   * Implements concept API.
   *
   * @param src
   *        See above.
   */
  Heap_fixed_builder(Heap_fixed_builder&& src);

  /// Implements concept API.  In this impl: frees all segments allocated on-demand so far.
  ~Heap_fixed_builder();

  // Methods.

  /// Disallow copy assignment.
  Heap_fixed_builder& operator=(const Heap_fixed_builder&) = delete;

  /**
   * Implements concept API.
   *
   * @param src
   *        See above.
   * @return See above.
   */
  Heap_fixed_builder& operator=(Heap_fixed_builder&& src);

  /**
   * Implements concept API.
   *
   * @return See above.
   */
  Capnp_msg_builder_interface* payload_msg_builder();

  /**
   * Implements concept API.
   *
   * @param target_blobs
   *        See above.
   * @param hdr_blob
   *        See above.
   * @param split_segs
   *        See above.
   * @param session
   *        See above.  In this impl: the proper value is `NULL_SESSION`.
   * @param err_code
   *        See above.  Generated codes:
   *        error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG (see above; reminder: possible only if
   *        `split_segs == nullptr`).
   */
  void emit_serialization(Segment_bufs* target_blobs, util::Blob_mutable* hdr_blob, Split_segments* split_segs,
                          const Session& session, Error_code* err_code = nullptr) const;

  /**
   * Implements concept API.
   *
   * @return See above.
   */
  size_t n_serialization_segments() const;

private:
  // Types.

  /// Alias for the capnp `MallocMessageBuilder`-like serialization engine of ours.
  using Capnp_heap_engine = Heap_fixed_builder_capnp_message_builder;

  // Data.  ATTN: If messing with this area: don't forget to check move-assignment or any other such list-of-members.

  /**
   * See ctor.  We only store it (as opposed to merely passing it to #m_engine) due to the need to
   * potentially split segments per `split_segs` in emit_serialization().  So if a certain segment had to
   * be sized by #m_engine larger than the request cap here, the split post-processing will still follow the cap,
   * albeit at an additional efficiency cost on the receiving side (the joining procedure will require copying).
   *
   * The `= 0` matters only in the default-cted case (which contractually allows only destruction or move-to);
   * it makes that state determinate on the cheap (the main ctor's member-init overrides it).  This might also
   * prevent sanitizer false warnings.
   */
  size_t m_seg_and_frame_sz_cap = 0;

  /**
   * The capnp builder engine which really does the work including owning the needed allocated segments so far.
   * payload_msg_builder() simply returns (up-cast of) `&m_engine`.
   *
   * ### Why the `unique_ptr` wrapper? ###
   * We need to be move-ctible and move-assignable.
   *
   * @note Somewhere after capnp-0.10 and before capnp-1.0.0 that became the simple situation.  However before that
   *       point #Capnp_msg_builder_interface was actually movable and therefore so was
   *       our #Capnp_heap_engine a/k/a Heap_fixed_builder_capnp_message_builder.  Yet we still used a
   *       wrapper here, because even then Heap_fixed_builder_capnp_message_builder doc header had a note
   *       that said it was movable, but the movability (due to `capnp::MessageBuilder` impl details) was somewhat
   *       slow.
   *
   * As a side benefit: it's an easy way to detect a moved-from `*this` and `assert()` in some situations.
   */
  boost::movelib::unique_ptr<Capnp_heap_engine> m_engine;
}; // class Heap_fixed_builder

/**
 * Implements Struct_reader concept by straightforwardly interpreting a serialization by Heap_fixed_builder
 * or any other builder that produces segments directly readable via `SegmentArrayMessageReader`.
 *
 * While written originally specifically as the counterpart to Heap_fixed_builder, it would work with any
 * builder that produces an equivalent serialization modulo using potentially different segment sizes.
 * That is why it is called `Heap_reader` -- not `Heap_fixed_reader` or some-such.
 *
 * @see Heap_fixed_builder
 *      A counterpart Struct_builder implementation that can create compatible serializations.
 * @see Struct_reader: implemented concept.
 */
class Heap_reader :
  public flow::log::Log_context,
  private boost::noncopyable
{
public:
  // Types.

  /// Implements Struct_reader::Config sub-concept.
  struct Config
  {
    // Types.

    /// Implements concept API.
    using Reader = Heap_reader;

    // Data.

    /// Logger to use for logging subsequently.
    flow::log::Logger* m_logger_ptr;

    /**
     * Best guess for how many times you expect to call add_serialization_segment(); or 0 if you have no guess.
     * This is a (fairly minor as of this writing) optimization opportunity; an internal container may be
     * `reserve()`d preventing future reallocs.
     */
    size_t m_n_segments_est;

    /**
     * See Struct_reader::Config concept.  Pointee (null OK = no stats kept) for cumulative
     * rcv-stats; must outlive each Heap_reader built with `*this` (the gauge decrements
     * happen in Heap_reader dtor).
     */
    stat::Serializer_stats::Rcv* m_stats;
  }; // class Config

  // Constructors/destructor.

  /**
   * Implements concept API.  See Config doc header for information on how `*this` behavior is controlled by `config`.
   *
   * @param config
   *        See above.
   */
  explicit Heap_reader(const Config& config);

  /// Implements concept API.  In this impl: frees all segments allocated via add_serialization_segment() so far.
  ~Heap_reader();

  // Methods.

  /**
   * Implements concept API.
   *
   * @param blob
   *        See above.
   */
  void add_serialization_segment(Segment_blob_in&& blob);

  /**
   * Implements concept API.
   *
   * @tparam Struct
   *         See above.
   * @param split_segs_or_null
   *        See above.
   * @param err_code
   *        See above.  #Error_code generated:
   *        error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS (add_serialization_segment() was never called),
   *        error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED (add_serialization_segment()-given segment
   *        was not properly aligned -- start address or size -- at this time),
   *        error::Code::S_DESERIALIZE_FAILED_REASSEMBLY_FAILED (something wrong with the contents of
   *        `*split_segs_or_null` including but not limited to it being `.empty()`).
   * @return See above.
   */
  template<typename Struct>
  typename Struct::Reader deserialization(const Split_segments* split_segs_or_null, Error_code* err_code = nullptr);

private:
  // Types.

  /// Alias for the capnp `SegmentArrayMessageReader` deserialization engine.
  using Capnp_heap_engine = ::capnp::SegmentArrayMessageReader;

  /// Alias to pointer to #Capnp_heap_engine.
  using Capnp_heap_engine_ptr = boost::movelib::unique_ptr<Capnp_heap_engine>;

  /**
   * Alias to capnp type that's similar to util::Blob_const but stores `word*` and `word` count
   * instead of `uint8_t*` and byte count.
   */
  using Capnp_word_array_ptr = kj::ArrayPtr<const ::capnp::word>;

  // Data.

  /**
   * Before deserialization(): the serialization payloads supplied so far, as divided up into (sub-)segments, each via
   * an add_serialization_segment() call; after it: the actual serialization consisting of the segments (after any
   * re-joining as needed).
   */
  std::vector<Segment_blob_in> m_serialization_segments;

  /**
   * Null/meaningless until deserialization() returns; once it does: if needed by the re-joining procedure
   * possibly performed by that method, this stores the moved contents of `m_serialization_segments[0]` which is
   * replaced with the finalized segment 1, as joined from 2+ sub-segments.  If `m_serialization_segments[0]` need not
   * be replaced (such as if it was a full-segment already, and/or no splitting had been done at all to any segments),
   * then this remains null.
   *
   * When non-null, the value itself is never checked/used by `*this` per se; it is just there to satisfy the
   * contract of add_serialization_segment() and deserialization(), which say that regardless of any re-joining
   * done by the latter, the first (sub-)segment supplied via add_serialization_segment() must remain in memory
   * until destruction or move-from.
   */
  Segment_blob_in m_seg1_hdr_backup;

  /**
   * After deserialization(), capnp-targeted data structure which is essentially `vector<>` of each
   * `Blob::const_buffer()` from #m_serialization_segments, in order, except instead of a given `const_buffer`
   * it's the capnp-style data structure that stores a `word*` and `word` count (instead of
   * `uint8_t*` and byte count).  This must be alive while #m_engine is non-null, so from deserialization() on;
   * capnp's #m_engine saves the pointer `&m_capnp_segments[0]` and `m_capnp_segments.size()`.
   */
  std::vector<Capnp_word_array_ptr> m_capnp_segments;

  /**
   * The capnp reader engine, as set in the deserialization() call and corresponding to the
   * `Struct::Reader` it returned; or null if deserialization() has not yet been called.  In terms of data this, really,
   * just stores pointers into the backing serialization -- the blobs #m_serialization_segments.
   * deserialization()-returned value then adds the capnp structured-data accessors which hop around those segments,
   * obtaining individual values directly from the segment blobs #m_serialization_segments.
   */
  Capnp_heap_engine_ptr m_engine;

  /**
   * See Config::m_stats: cumulative stat::Serializer_stats::Rcv pointee `*this` shall update; null OK
   * (no stats kept).  The pointee, if any, outlives `*this`.
   */
  stat::Serializer_stats::Rcv* const m_stats;

  /**
   * Cache: sum of `m_serialization_segments[].capacity()` (post-reassembly-if-needed inside
   * deserialization()); zero until then.  Used in dtor for outstanding-bytes gauge decrement.  Frozen
   * once deserialization() succeeds.  (Context: `*this`-storing Msg_in -- if any -- is read-only post-creation and
   * certainly on delivery to user from `Channel`).
   *
   * Ignored if #m_stats null (may or may not still accumulate; tactical decision).
   */
  uint64_t m_alloc_sz;

  /// Cache: analogously to #m_alloc_sz: sum of `m_serialization_segments[].size()`.
  uint64_t m_used_sz;
}; // class Heap_reader

/**
 * Convenience alias: Use this when constructing a struc::Channel with tag
 * Channel_base::S_SERIALIZE_VIA_HEAP.
 *
 * See Channel_base::Serialize_via_heap doc header for when/how to use.
 *
 * Tip: `Sync_io_obj` member alias will get you the `sync_io`-pattern counterpart.
 *
 * @internal
 * Unable to put it in ..._fwd.hpp, because it relies on nested class inside incomplete type.
 */
template<typename Channel_obj, typename Msg_body_t>
using Channel_via_heap
  = Channel<Channel_obj, Msg_body_t,
            Heap_fixed_builder::Config, Heap_reader::Config>;

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Struct>
typename Struct::Reader Heap_reader::deserialization(const Split_segments* split_segs_or_null, Error_code* err_code)
{
  using flow::error::Runtime_error;
  using flow::util::buffers_dump_string;
  using flow::util::stat::fetch_add;
  using flow::util::stat::update_hi_wmark;
  using boost::movelib::make_unique;
  using std::vector;
  using std::swap; // Required for correct ADL-semantics.
  using ::capnp::word;
  using Capnp_word_array_array_ptr = kj::ArrayPtr<const Capnp_word_array_ptr>;
  using Capnp_heap_engine_opts = ::capnp::ReaderOptions;

  FLOW_ERROR_EXEC_AND_THROW_ON_ERROR(typename Struct::Reader, deserialization<Struct>, split_segs_or_null, _1);
  // ^-- Call ourselves and return if err_code is null.  If got to present line, err_code is not null.

  // See explanation and associated to-do in shm::Reader::deserialization(); applies equally here.
  constexpr Capnp_heap_engine_opts RDR_OPTIONS = { std::numeric_limits<uint64_t>::max() / sizeof(word),
                                                   Capnp_heap_engine_opts{}.nestingLimit };

  if (m_serialization_segments.empty())
  {
    *err_code = error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS;
    return {};
  }
  // else: OK so far.

  assert((!m_engine) && "Do not call deserialization() more than once.");

  if (split_segs_or_null) // Note: This being true is rare, or should be; which is good, as reassembly is costly.
  {
    const auto& split_segs = *split_segs_or_null;
    if (split_segs.empty())
    {
      FLOW_LOG_WARNING("Heap_reader [" << *this << "]: "
                       "Split-segment records sequence is supplied but empty, against internal protocol; "
                       "it should have not been supplied (null).");
      *err_code = error::Code::S_DESERIALIZE_FAILED_REASSEMBLY_FAILED;
      return {};
    }
    // else

    /* Time to reassemble the segments, as needed, according to split_segs if not null.  split_segs semantics are
     * simple, but please refer to the doc header of Split_segments for a recap.  Per the algorithm explained there,
     * scan through split_segs in backwards order (so as to avoid making later-scanned indices in split_segs become
     * wrong), and join each sequence of 2+ elements of m_serialization_segments (as described in that split_segs[])
     * into 1, in-place.  A couple of subtleties to point out ahead of time:
     *   - It is possible (we won't discuss here how likely; it depends on stuff not in our purview, but trust me that
     *     it is at least conceivably possible) that when we must join 2+ segs, in order, [s1, ...], the area
     *     [s1.begin() + s1.size(), s1.begin() - s1.start() + s1.capacity()) (the unused, but allocated, area in the
     *     first of the to-be-joined sub-segments) is large enough to hold the combined `.size()`s of the remaining ...
     *     sub-segments.  In that case, for perf, we reuse s1 as the ultimately-joined segment Blob, copying each of ...
     *     onto it and adjusting its .size().  Then remove the ... from the vector.  (It's an opportunistic optimization
     *     that eliminates one allocation and one seg copy.)  Otherwise we must create a new large-enough seg and
     *     simply copy s1 and ... onto it; then replace s1 with the resulting segment and remove ... from the vector.
     *   - By our contract, we *are* allowed to invalidate m_serialization_segments elements -- both the `Blob`s
     *     themselves (essentially "handles" to data areas in memory) and the referred-to-thereby data areas themselves.
     *     However we are *not* allowed to invalidate the data area known as the header, which is
     *     the byte range [seg1.begin() - seg1.start(), seg1.begin()), where seg1 is m_serialization_segments[0].
     *     - Therefore: If seg1 is mentioned in split_segs, and its unused-capacity area is not large enough to
     *       enable the optimization in previous bullet point, then we must save seg1 somewhere -- to avoid invalidating
     *       the header area -- even as we replace it in m_serialization_segments with the new post-join segment Blob.
     *       For this purpose we use m_seg1_hdr_backup which shall simply keep the header (and remaining stuff, no
     *       longer used but cannot be deallocated due to being part of the header-containing buffer) until dtor
     *       destroys everything. */

    const auto split_segs_it_rend = split_segs.rend();
    for (auto split_segs_it = split_segs.rbegin();
         split_segs_it != split_segs_it_rend;
         ++split_segs_it)
    {
      const auto& split_seg = *split_segs_it;
      const size_t start_idx = split_seg.m_start_idx;
      const size_t n_cont_subsegs = split_seg.m_n_cont_subsegs;

      /* m_s_s.size() changes on-the-fly, but we go in decreasing order of start_idx values (no, we don't check it;
       * we trust the opposing side and check for correctness when we feel like it/decent effort), so the range
       * check here is correct.  The 1st check validates that a record is only included if a segment actually was
       * split; plus we need not think about degenerate cases below as a result.  Note the subtraction-form
       * bounds checks: the naive `start_idx + n_cont_subsegs + 1 > .size()` could wrap around given garbage
       * input values and pass. */
      if ((n_cont_subsegs == 0) || (start_idx >= m_serialization_segments.size())
          || (n_cont_subsegs > (m_serialization_segments.size() - 1 - start_idx)))
      {
        FLOW_LOG_WARNING("Heap_reader [" << *this << "]: "
                         "Split-segment record (start-index [" << start_idx << "] "
                         "(0-based, of [" << split_segs.size() << "], 1-based), "
                         "# continuation sub-segs [" << n_cont_subsegs << "]): "
                         "The latter value must be 1+ and not exceed (+ start_idx) # of input buffers "
                         "[" << m_serialization_segments.size() << "].");
        *err_code = error::Code::S_DESERIALIZE_FAILED_REASSEMBLY_FAILED;
        return {};
      }
      // else

      const size_t end_idx = start_idx + n_cont_subsegs + 1; // n_cont_subsegs does *not* include start_idx-th one.
      // (Cannot overflow: the range check above ensured end_idx <= m_serialization_segments.size().)

      size_t cont_total_sz = 0;
      for (size_t idx = start_idx + 1; idx != end_idx; ++idx)
      {
        cont_total_sz += m_serialization_segments[idx].size();
      }

      auto& subseg1 = m_serialization_segments[start_idx];
      const bool first_seg = start_idx == 0; // I.e., whether `&subseg1 == &m_serialization_segments.front()`.

      const auto unused_cap = static_cast<size_t>(subseg1.capacity() - subseg1.size() - subseg1.start());
      if (unused_cap >= cont_total_sz)
      {
        // This is the case where subseg1's unused capacity can fit the remaining sub-segs' total bytes.
        FLOW_LOG_TRACE("Heap_reader [" << *this << "]: "
                       "Split-segment record (start-index [" << start_idx << "] "
                       "(0-based, of [" << split_segs.size() << "], 1-based), "
                       "# continuation sub-segs [" << n_cont_subsegs << "]): "
                       "All [" << cont_total_sz << "] bytes across continuation sub-segs can fit into "
                       "unused capacity [" << unused_cap << "] of start-index-th sub-seg; copying their contents there "
                       "accordingly and deleting them.");

        auto dest_it = subseg1.end();
        subseg1.resize(subseg1.size() + cont_total_sz); // .start() left unchanged.
        const auto cont_subseg_begin_it = m_serialization_segments.begin() + start_idx + 1;
        const auto cont_subseg_end_it = m_serialization_segments.begin() + end_idx;
        for (auto cont_subseg_it = cont_subseg_begin_it;
             cont_subseg_it != cont_subseg_end_it;
             (dest_it += cont_subseg_it->size()), ++cont_subseg_it)
        {
          subseg1.emplace_copy(dest_it, cont_subseg_it->const_buffer());
        }
        assert(dest_it == subseg1.end());

        m_serialization_segments.erase(cont_subseg_begin_it, cont_subseg_end_it);

        /* Per algorithm above, reminder: subseg1's original contents sit undisturbed; no need to worry about
         * having to use m_seg1_hdr_backup, even if subseg1 is indeed m_serialization_segments.front().  It is
         * still there (and always will be), just more bytes are tacked on at the end. */
      } // if (unused_cap >= cont_total_sz) // subseg1 can fit all of subseg1 + continuation sub-segs.
      else // if (subseg1 cannot fit all of subseg1 + continuation sub-segs)
      {
        /* This is the (probably likelier) case where subseg1 is no different than the others.
         * Per algorithm above, reminders:
         *   - Mainly just make an all-new Blob combining subseg1 and the n_cont_subsegs other ones in that order.
         *     Then replace subseg1 with that resulting new Blob.
         *   - If subseg1 is .front() (<=> first_seg is true), then it must be saved in m_seg1_hdr_backup, so as to keep
         *     the header bytes alive where they are (and since buffer cannot be part-freed... all of it).
         * So tactically we just use m_seg1_hdr_backup as bullet 1 result Blob.  Then swap that guy with subseg1, and
         * .erase() the other sub-segs from m_serialization_segments.  If first_seg, we're done.  Otherwise, also
         * m_seg1_hdr_backup.make_zero(), since no need to keep ex-subseg1 bytes alive.  That completes bullet 2.
         *
         * Note that we're going backwards; so first_seg is true (if ever) only in our very last iteration.
         * Hence m_seg1_hdr_backup -- if it were ever non-.zero() in a previous iteration -- would have been
         * de-alloc-ed before we got here.  Hence these assert(). */
        assert(m_seg1_hdr_backup.zero() && "Original state should be unallocated.");

        m_seg1_hdr_backup.resize(subseg1.size() + cont_total_sz);
        auto dest_it = m_seg1_hdr_backup.begin();

        FLOW_LOG_TRACE("Heap_reader [" << *this << "]: "
                       "Split-segment record (start-index [" << start_idx << "] "
                       "(0-based, of [" << split_segs.size() << "], 1-based), "
                       "# continuation sub-segs [" << n_cont_subsegs << "]): "
                       "All [" << cont_total_sz << "] bytes across continuation sub-segs cannot fit into "
                       "unused capacity [" << unused_cap << "] of start-index-th sub-seg; "
                       "therefore creating new segment-blob of size [" << m_seg1_hdr_backup.size() << "], "
                       "copying sub-segs' contents there, and replacing sub-segments with the resulting blob.  "
                       "Start-index-th sub-seg shall be saved elsewhere so as to keep frame-header alive? = "
                       "[" << first_seg << "].");

        auto subseg_begin_it = m_serialization_segments.begin() + start_idx;
        const auto subseg_end_it = m_serialization_segments.begin() + end_idx;
        for (auto subseg_it = subseg_begin_it;
             subseg_it != subseg_end_it;
             (dest_it += subseg_it->size()), ++subseg_it)
        {
          m_seg1_hdr_backup.emplace_copy(dest_it, subseg_it->const_buffer());
        }
        assert(dest_it == m_seg1_hdr_backup.end());

        swap(m_seg1_hdr_backup, subseg1);
        if (!first_seg)
        {
          m_seg1_hdr_backup.make_zero();
        }

        m_serialization_segments.erase(++subseg_begin_it, subseg_end_it);
      } // else // if (unused_cap < cont_total_sz) // subseg1 cannot fit all of subseg1 + continuation sub-segs.
    } // for (split_seg in split_segs, backwards)
  } // if (split_segs_or_null) // Usually false.

  // m_serialization_segments won't change past this point for the lifetime of *this.

  // Set up capnp ArrayPtr<ArrayPtr>.  ArrayPtr is a ptr (to word) + size (in `word`s).  The backing vector:
  auto& capnp_segs = m_capnp_segments;
  assert(capnp_segs.empty() && "m_capnp_segments should have been empty so far, pre-deserialization().");
  capnp_segs.reserve(m_serialization_segments.size());

  size_t idx = 0;
  for (const auto& serialization_segment : m_serialization_segments)
  {
    const auto data_ptr = serialization_segment.const_data();
    const auto seg_sz = serialization_segment.size();

    if (((uintptr_t(data_ptr) % sizeof(word)) != 0) || ((seg_sz % sizeof(word)) != 0))
    {
      FLOW_LOG_WARNING("Heap_reader [" << *this << "]: "
                       "Serialization segment [" << idx << "] "
                       "(0-based, of [" << m_serialization_segments.size() << "], 1-based): "
                       "Heap buffer @[" << static_cast<const void*>(data_ptr) << "] sized [" << seg_sz << "]: "
                       "Starting pointer is not capnp-word-aligned, and/or size is not a capnp-word-multiple.  "
                       "Bug?  Misuse of Heap_reader?  "
                       "Either is against the API use requirements; capnp would complain and fail.");
      *err_code = error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED;
      return {};
    }
    // else

    FLOW_LOG_TRACE("Heap_reader [" << *this << "]: "
                   "Serialization segment [" << idx << "] "
                   "(0-based, of [" << m_serialization_segments.size() << "], 1-based): "
                   "Heap buffer @[" << static_cast<const void*>(data_ptr) << "] sized [" << seg_sz << "]: "
                   "Feeding into capnp deserialization engine.");
    FLOW_LOG_DATA("Segment contents: "
                  "[\n" << buffers_dump_string(serialization_segment.const_buffer(), "  ") << "].");

    capnp_segs.emplace_back(reinterpret_cast<const word*>(data_ptr), // uint8_t* -> word* = OK given C++ aliasing rules.
                            seg_sz / sizeof(word)); // (An exact multiple: the size check above ensured it.)

    /* Stats: To avoid another loop, we can accumulate these opportunistically.  Could check `bool(m_stats)`, but
     * that's slower overall, so just do it. */
    m_alloc_sz += serialization_segment.capacity();
    m_used_sz += seg_sz;

    ++idx;
  } // for (const auto& serialization_segment : m_serialization_segments)

  /* Initialize the deserialization engine by giving it the pointers to/size of the backing segment blobs.
   * It doesn't copy this array of pointers/sizes, so that array must stay alive, hence why capnp_segs is
   * really m_capnp_segments.  To be clear: not only must the blobs stay alive but so must the array referring
   * to them. */
  const Capnp_word_array_array_ptr capnp_segs_ptr{&(capnp_segs.front()), capnp_segs.size()};
  m_engine = make_unique<Capnp_heap_engine>(capnp_segs_ptr, RDR_OPTIONS);

  err_code->clear();

  /* Stats.  Past all early-exit error returns; m_engine is set (which indicates, in the stats context, that
   * deserialization() happened and succeeded).
   * (Context: If we are used by a Msg_in, that Msg_in can be emitted to the user by Channel.)
   *
   * Sample/increment everything here; the dtor decrements the gauges, partially based on
   * m_alloc_sz and m_used_sz. */
  if (m_stats)
  {
    update_hi_wmark(&m_stats->m_msgs_outstanding_hi_wmark,
                    fetch_add(&m_stats->m_msgs_outstanding, 1) + 1);
    fetch_add(&m_stats->m_alloc_lifetime_sz, m_alloc_sz);
    update_hi_wmark(&m_stats->m_alloc_outstanding_sz_hi_wmark,
                    fetch_add(&m_stats->m_alloc_outstanding_sz, m_alloc_sz) + m_alloc_sz);
    update_hi_wmark(&m_stats->m_used_outstanding_sz_hi_wmark,
                    fetch_add(&m_stats->m_used_outstanding_sz, m_used_sz) + m_used_sz);
    m_stats->m_histo_msg_alloc_sz.record_value(m_alloc_sz);
    m_stats->m_histo_msg_used_sz.record_value(m_used_sz);
  }

  // And lastly set up the structured-accessor API object that'll traverse those blobs via that engine.
  return m_engine->getRoot<Struct>();
} // Heap_reader::deserialization()

} // namespace ipc::transport::struc
