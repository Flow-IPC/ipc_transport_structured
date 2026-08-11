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

#include "ipc/transport/struc/struc_fwd.hpp"
#include <flow/util/stat/histo.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ipc::transport::struc::stat
{

// Types.

/**
 * Simple bucket configuration for one flow::util::stat::Histogram_counter inside Serializer_stats::Snd et al;
 * aggregate-initializable.  build() returns a fresh `Histogram_counter`.  Two scales are expressible:
 *   - *Linear* (#m_ratio zero): `{n_buckets, bucket0_sz, bucket_sz}`, straight into the like-named
 *     `Histogram_counter` ctor args (with `bucket0_val0` always 0): bucket 0 is `[0, m_bkt0)`; each
 *     subsequent bucket is #m_bkt wide.
 *   - *Geometric* (#m_ratio at least 2): `{n_buckets, floor, 0, ratio}`: bucket 0 is `[0, floor)` -- the
 *     catch-all below the ladder -- and each subsequent bucket spans `ratio` times its predecessor:
 *     bucket 1 = `[floor, floor * ratio)`, etc.  (As always values beyond the last bucket land in it
 *     regardless.)  This scale suits open-ended value regimes spanning several orders of magnitude, where
 *     any practical linear scale would lump everything past its reach into the last bucket.
 */
struct Histo_cfg
{
  // Data.

  /// See Histogram_counter ctor (either scale; total bucket count including the bucket-0 catch-all).
  size_t m_n_bkts;
  /// Linear scale: see Histogram_counter ctor; geometric scale: the ladder floor (bucket 0 = `[0, m_bkt0)`).
  size_t m_bkt0;
  /// Linear scale: see Histogram_counter ctor; geometric scale: meaningless; must be zero.
  size_t m_bkt;
  /// Zero => linear scale; else (then it must be >= 2) => geometric scale with this bucket-to-bucket size ratio.
  size_t m_ratio = 0;

  // Methods.

  /**
   * Builds a fresh Histogram_counter using these knobs; see `struct` doc header for their semantics per scale.
   * @return See above.
   */
  flow::util::stat::Histogram_counter build() const;
}; // struct Histo_cfg

/**
 * Cumulative stats describing capnp-driven memory-allocation activities on behalf of a struc::Struct_builder +
 * struc::Struct_reader impl pair (usually <=> Msg_out and Msg_in respectively), such that they both belong
 * to a conceptually similar use-case for needing memory-for-capnp.  That is admittedly very abstract-sounding
 * and arguably is best explained by giving some background and a practical example.
 *
 * @see Msg_out and Msg_in doc headers.
 * @see Struct_builder concept doc header (also Struct_reader as desired).
 * @see Doc headers for impls thereof: Heap_fixed_builder, Heap_reader; shm::Builder, shm::Reader.
 *
 * It is possible to write custom impls of Struct_builder and Struct_reader, and it is possible (albeit not
 * the envisioned primary use case) to use any of them independently of Msg_out and Msg_in (and struc::Channel)
 * respectively.  Even then, the existing impls, used with our Msg_out and Msg_in, are the canonical/reference
 * situation.  So we assume it for this doc header.
 *
 * In short: there is, first, the sender side.  One creates Msg_out, which creates Struct_builder (a specific
 * impl) on its own behalf.  One capnp-mutates it via capnp-generated setters and other mutators.  When
 * desired (which can be never, or once, or many times) one sends it via, e.g., Channel::send().
 *
 * In short: there is, also, the receiver side.  On receipt of some representation of a `Msg_out`-at-send-time,
 * struc::Channel (et al) creates Msg_in which creates Struct_reader (a specific impl) on its own behalf.
 * It reads aspects of the serialization received via IPC and makes-available a (read-only) capnp-generated
 * accessor API for reading the contents of the Msg_in by the user.
 *
 * Now consider the two canonical available `Struct_builder`s and counterpart `Struct_reader`s.  Note that
 * Msg_out and Msg_in, respectively, are template-parameterized on the builder and reader impl.
 *
 * ### Pure-heap builder/reader pair: Heap_fixed_builder, Heap_reader ###
 * There is just the outer serialization; it is directly mutated by the user (send-side) and read by the user
 * (receive-side), with a full copy occurring when the bytes travel into an IPC-transport (at send time) and
 * then out of it on the opposing side (different process probably).
 *
 * Msg_out / Heap_fixed_builder are used for capnp-mutation (by user) causing ongoing allocation (by builder
 * internals).  This is tracked by Snd: #m_snd.
 *
 * Msg_in / Heap_reader are used for capnp-access (by user), following one-time allocation and fill-out of
 * required segments as received via IPC.  This is tracked by Rcv: #m_rcv.
 *
 * So a `*this`, in this context, tracks counts of events and gauges of currently-outstanding resources
 * (e.g., messages or allocated or serialization-used bytes therein), split up into those on behalf of the
 * *sender-side* (so Heap_fixed_builder) aspects, and the *receiver-side* (so Heap_reader) ones.
 *
 * ### SHM-backed builder/reader pair: shm::Builder, shm::Reader ###
 * Please see doc header for Rcv: section Background.  In short:
 *   - shm::Builder internally keeps a Heap_fixed_builder, and shm::Reader a Heap_reader, because the
 *     outer-serialization, containing exclusively a 1-segment capnp-message holding a SHM-handle to the
 *     core/inner-serialization, is still otherwise-identically used: allocate, mutate, send, receive,
 *     allocate, access.
 *     - That is one `*this` with #m_snd and #m_rcv.
 *   - The true, user-mutated (sender-side) and user-accessed (receiver-side) capnp data (the
 *     core/inner-serialization) is in SHM.
 *     - That is a 2nd, separate `*this` with #m_snd -- and an unused (zeroed-out) #m_rcv.  The latter
 *       is because no in-SHM allocation is required on receipt; that's the point of SHM.
 *
 * The key thing to grasp is that while shm::Builder *wraps* both a Heap_fixed_builder again *and* code
 * that allocates in SHM, the stats corresponding to each of the two types of in-memory stuff -- for
 * pragmatic reasons -- are tracked separately, in separate `Serializer_stats`es.  Quantities in one are
 * not necessarily a subset of the respective ones in the other.
 *
 * ### Key aliases ###
 * Three Serializer_stats variants exist in Flow-IPC as of this writing, each with a ready-to-use
 * default-constructible alias:
 *
 *   - struc::stat::Heap_serializer_stats: pure-heap user messages (alias defined in struc_fwd.hpp).
 *   - struc::shm::stat::Outer_serializer_stats: heap-backed outer-serialization for a SHM-backed user message
 *     (low byte volume; each message has 1 segment of a constant, tiny size).
 *   - struc::shm::stat::Core_serializer_stats: SHM-backed core/inner-serialization (actual user payload).
 *     - To repeat: #m_rcv is unused/all-zeroes.
 *
 * That said, these stat-set `struct`s can be used in other, custom contexts as desired.
 *
 * Concurrency
 * -----------
 * In the mainstream use-cases above, stat-updating can occur from multiple threads and concurrently to
 * potential consumption by the user (you).  General heap-or-SHM allocation can *potentially* be too
 * frequent to keep un-sharded stats; but in the Msg_out / Msg_in / Struct_builder / Struct_reader context
 * updates shall not be so frequent as to cause significant contention.  Therefore all non-histogram stat
 * members are `atomic`.  (`Histogram_counter` is internally `atomic`, in that each bucket's count-increment
 * and loads are atomic.)  Mainstream stat-updating is expected to be done on globally-maintained
 * Serializer_stats objects.
 *
 * @see `flow::util::stat` namespace doc header for an in-depth discussion of multi-thread stat-keeping
 *      and perf.
 *
 * Any mechanism for maintaining such globals is fine; but we recommend (and Flow-IPC actively uses)
 * `flow::util::stat::Stat_set_list` and its singleton-providing wrapper `flow::util::stat::Global_stats`.
 *
 * Why concrete-type Serializer_stats + template Serializer_stats_p?
 * -----------------------------------------------------------------
 * Almost all relevant code -- whether stat-producer (the builders and readers) or stat-consumer (you) --
 * only accesses each Serializer_stats via its #m_snd and #m_rcv parts after creation.  E.g.,
 * Heap_fixed_builder is given a pointer to an #m_snd and increments Snd::m_msgs at the proper points.
 *
 * Each Serializer_stats has to be stored somewhere, though, and first initialized.  Most members are
 * trivially init'd at declaration.  However, certain `Histogram_counter`s have a number/width of buckets
 * that depends on the context (e.g., shm::stat::Core_serializer_stats messages can be huge, while
 * shm::stat::Outer_serializer_stats ones are tiny).  So Serializer_stats's ctor takes Histo_cfg knobs (as do
 * #m_snd's and #m_rcv's ctors).
 *
 * However, in the mainstream, the stats here are expected to be kept as globals via `Global_stats`, which
 * requires its `Stat_set` template-parameter type to be default-constructible.  As noted, our
 * Serializer_stats is not (otherwise some histograms would be useless in some cases).  So
 * Serializer_stats_p<Cfg> is a thin templated subclass with no extra data that default-constructs by
 * passing `Cfg::S_HISTO_*` Histo_cfg constants into the base ctor.  Pointers/refs cast safely between the
 * two.
 *
 * The key tactical point is:
 *   - ~All stat-consuming/-producing code deals with Serializer_stats (no template) ptrs/refs.
 *   - When specifying the `Global_stats::Stat_set` template-parameter, use Serializer_stats_p (template),
 *     with the desired knobs for the particular type-of-builder-and-reader.  Then, on calling
 *     `flow::util::stat::Stat_set_list::stats_[mutable_][_at|_default]()`, safely up-cast the returned ref
 *     to `[const] Serializer_stats&`.
 */
struct Serializer_stats
{
  // Types.

  /**
   * Send-side (during-serialization) stats, captured at hooks within the Struct_builder
   * impl: typically each `capnp::MessageBuilder::allocateSegment()` call and the builder's destructor.
   *
   * Context: Typically `Struct_builder`s are used as core components of `Msg_out`s, which are in turn
   * sent via struc::Channel and struc::sync_io::Channel; though Struct_builder impls are available
   * for direct use also.  See Serializer_stats doc header for more such context.
   */
  struct Snd
  {
    // Data.

    /**
     * Total `Struct_builder` (usually <=> Msg_out) instances ever that triggered 1+ allocation through the
     * builder.  Distinct from `Channel`-layer message counts: that one counts *send invocations*,
     * this one counts *builder instances*.  A single Msg_out can be sent more than once.
     */
    std::atomic<uint64_t> m_msgs = 0;

    /**
     * Gauge: How many of items counted by #m_msgs are currently-alive: increment at first-alloc, decrement
     * at `Struct_builder` (usually <=> Msg_out) dtor or move-from.
     */
    std::atomic<size_t> m_msgs_outstanding = 0;

    /// High water-mark of #m_msgs_outstanding observed so far.
    std::atomic<size_t> m_msgs_outstanding_hi_wmark = 0;

    /**
     * Lifetime sum of allocated bytes across all `capnp::MessageBuilder::allocateSegment()` calls (over all items
     * counted in #m_msgs).  Excludes frame-prefix bytes (covered separately by #m_frame_lifetime_sz).  Includes
     * past-end slack within each segment (i.e., counts the segment's allocated capacity, not just the bytes
     * capnp ends up using).
     *
     * @note The aforementioned "past-end slack" might be quite high for a given segment at first; then as one
     *       mutates the capnp structure in standard fashion, the slack decreases, until either the whole thing
     *       is destroyed, or there's no space for some new datum, and another segment is created to hold that
     *       one.  We modify #m_alloc_lifetime_sz in `allocateSegment()`; no notion of "past-end slack" is
     *       usefully measurable at that point.  Grabbing it per-segment in builder dtor/move-from is possible but is
     *       not what we do here.
     *
     * Total physical heap allocation = #m_alloc_lifetime_sz plus #m_frame_lifetime_sz.
     */
    std::atomic<uint64_t> m_alloc_lifetime_sz = 0;

    /// Gauge: Of bytes tracked by #m_alloc_lifetime_sz, how many are currently allocated as opposed to now-freed?
    std::atomic<size_t> m_alloc_outstanding_sz = 0;

    /// High water-mark of #m_alloc_outstanding_sz observed so far.
    std::atomic<size_t> m_alloc_outstanding_sz_hi_wmark = 0;

    /**
     * Per-`Struct_builder` (usually <=> Msg_out) histogram of total allocated bytes through the builder
     * (see #m_alloc_lifetime_sz regarding inclusion semantics) -- sampled at end-of-life of the builder
     * (`Struct_builder` dtor or move-from).
     *
     * Builders that have not yet been destroyed are absent from this histogram (use #m_msgs_outstanding /
     * #m_msgs_outstanding_hi_wmark to detect those).
     */
    flow::util::stat::Histogram_counter m_histo_msg_alloc_sz;

    /**
     * Per-`Struct_builder` (usually <=> Msg_out) histogram of bytes actually used by capnp (sum of
     * `getSegmentsForOutput()` segment `.size()`s) -- sampled at end-of-life of the builder
     * (`Struct_builder` dtor or move-from).  Slack between #m_histo_msg_alloc_sz and this histogram = capnp's
     * internal past-end waste (space where it could not fit mutated-in data and chose to go to a new segment instead).
     */
    flow::util::stat::Histogram_counter m_histo_msg_used_sz;

    /**
     * Per-`Struct_builder` (usually <=> Msg_out) histogram of segment count -- sampled at end-of-life of the builder
     * (`Struct_builder` dtor or move-from).  Buckets: [1], [2], [3+].
     */
    flow::util::stat::Histogram_counter m_histo_segs_per_msg{3, 1, 1, 0};

    /**
     * Lifetime count of `capnp::MessageBuilder::allocateSegment()` calls in which the leaf-driven minimum size
     * requested by capnp exceeded the builder's preferred next-segment size at that point.  An operator signal:
     * "users have leaves bigger than your starting or exponential-growth-driven segment size -- consider tuning
     * the initial-segment size up and/or looking into the design of your schema."  It is not necessarily a
     * terrible thing; depends; but knowing about big leaves is good.  In capnp a big leaf as of this writing
     * can only be a `List`, `Data`, or `Text` field.
     */
    std::atomic<uint64_t> m_big_leaf_alloc_count = 0;

    /**
     * Per-event histogram of leaf-driven minimum size as passed to `capnp::MessageBuilder::allocateSegment()`
     * (but in bytes!) for each event counted in #m_big_leaf_alloc_count.  Answers "how big are those big leaves?"
     */
    flow::util::stat::Histogram_counter m_histo_big_leaf_sz;

    /**
     * `Struct_builder` (usually <=> Msg_out) count, where each builder experienced at least 1 big-leaf event
     * (counted in #m_big_leaf_alloc_count).  Distinguishes "one outlier message with N big leaves" from
     * "X% of messages have at least 1 big leaf."
     */
    std::atomic<uint64_t> m_msgs_with_big_leaf = 0;

    /**
     * `Struct_builder` (usually <=> Msg_out) count of builders observing the *capacity-lock* event:
     * in `capnp::MessageBuilder::allocateSegment()`, when computing the ~exponentially-growing next-segment
     * size (used only if there *is* another `allocateSegment()`), the value was clamped to the configured max.
     * The event is counted regardless of whether that next `allocateSegment()` does occur or not.
     *
     * This event
     *   - can happen once, or not at all, per builder;
     *   - as of this writing is not observed in the shm::Builder core (in-SHM) allocating algorithm, as the
     *     cap pragmatically exists to avoid exceeding an IPC-transport's single-message size -- a concern that
     *     is irrelevant to in-SHM (or equivalent zero-copy) allocations.
     */
    std::atomic<uint64_t> m_seg_grow_cap_lock_count = 0;

    /**
     * Lifetime sum of frame-prefix bytes across all `capnp::MessageBuilder::allocateSegment()` calls.
     *
     * The frame prefix is a (usually small, in fact for most segments zero in practice) configurably-sized
     * area reserved in front of the actual capnp-segment space, when the buffer for the segment is allocated.
     * So the frame prefix (if any) can hold some data maintained by someone else; capnp segment begins after
     * that.
     */
    std::atomic<uint64_t> m_frame_lifetime_sz = 0;

    // Constructors.

    /**
     * Constructor; supplies bucket-structure knobs for the `Histogram_counter`s in `*this` whose useful shape
     * varies by use-case.  E.g., in-SHM messages can plausibly be much larger than IPC-transport-constrained
     * heap-based ones; so a Heap_fixed_builder versus shm::Builder might use different knobs here.
     *
     * @param msg_alloc_sz_cfg
     *        For #m_histo_msg_alloc_sz.
     * @param msg_used_sz_cfg
     *        For #m_histo_msg_used_sz.
     * @param big_leaf_sz_cfg
     *        For #m_histo_big_leaf_sz.
     */
    Snd(Histo_cfg msg_alloc_sz_cfg, Histo_cfg msg_used_sz_cfg, Histo_cfg big_leaf_sz_cfg);
  }; // struct Snd

  /**
   * Receive-side (during-deserialization) stats, captured at hooks within the Struct_reader impl.
   *
   * Context: Typically `Struct_reader`s are used as core components of `Msg_in`s, which are in turn
   * received (and then given to the user via on-receive handlers) via struc::Channel and
   * struc::sync_io::Channel; though Struct_reader impls are available for direct use also.  See
   * Serializer_stats doc header for more such context.
   *
   * ### Background / This is all zeroes for in-SHM-related stats ###
   * This is a bit subtle, if one is not fully experienced in how builders/readers work (Struct_builder concept
   * doc header is good background reading), so read carefully:
   *
   * Consider Heap_fixed_builder, as used in the most straightforward, purely-heap-backed setup.  Let's assume
   * we're in the most-mainstream context (with Msg_out).  When the Msg_out is populated/modified/destroyed,
   * that's Snd.  If this message is sent -- which can be done 0, 1, 2... times over any
   * suitable `Channel` -- then on the receiving side, the resulting capnp-serialization is copied out of the
   * IPC transport.  For this, buffer(s) is/are allocated (again, in possibly different process(es)).  That's
   * Rcv.
   *
   * Now consider shm::Builder, the recommended zero-copy-friendly Struct_builder.  There are now two
   * serializations: the core/inner one; and the outer one, where the SHM-handles-to-the-inner-serialization-
   * segments-in-SHM are encoded.  So shm::Builder uses *two* types of allocation on the *builder* (sending)
   * side:
   *   - the Heap_fixed_builder for the outer serialization (in practice small and simple: per message, always
   *     1 segment of a constant small size enough for a SHM-handle only);
   *   - the shm::Builder allocating *actual* user data in actual SHared Memory (SHM).
   *
   * That's tracked, again, by Snd, but two of them: one for the latter (more interesting) and
   * one for the former (probably less interesting).
   *
   * Now the punchline: when a message is received, all of the above regarding Rcv and
   * shm::Reader still holds... except that the whole point of the *core/inner* (in-SHM) storage is that it
   * need be done only once; there's no copying, so no in-SHM allocating occurs on receipt.  Therefore:
   *   - the Heap_reader still keeps stats in a Rcv; *but*:
   *   - there is nothing that would update the Rcv for the directly-in-SHM items.  So they
   *     stay zero in the containing Serializer_stats.
   *
   * We could have structured things a bit differently (less symmetrically) to avoid this; but the resulting
   * reduction in boilerplate is arguably worth the trade-off.
   */
  struct Rcv
  {
    // Data.

    /**
     * Gauge: Currently-alive `Struct_reader` (usually <=> Msg_in) instances.
     *
     * @internal
     * Incremented at deserialization-start time, decremented at reader dtor.
     * @endinternal
     */
    std::atomic<size_t> m_msgs_outstanding = 0;

    /// High water-mark of #m_msgs_outstanding observed so far.
    std::atomic<size_t> m_msgs_outstanding_hi_wmark = 0;

    /**
     * Lifetime sum of segment-blob capacity across all `Struct_reader` (usually <=> Msg_in) instances ever.
     * Sampled at a given reader's deserialization-start time, same point as when incrementing
     * #m_msgs_outstanding.
     *
     * Certain complicating niggles aside, it works basically like this inside at least sync_io::Channel:
     *   -# Need to receive a segment; prepare definitely-big-enough buffer: N bytes.
     *   -# Receive it over a transport::Blob_receiver (e.g., transport::Native_socket_stream): message was
     *      M<=N bytes.
     *   -# OK, this is (a) segment in the capnp-serialization we are receiving: M useful bytes; (N minus M)
     *      unused bytes.
     *
     * Here we are tracking the N, not the M.
     */
    std::atomic<uint64_t> m_alloc_lifetime_sz = 0;

    /// Gauge: Of bytes tracked by #m_alloc_lifetime_sz, how many are currently allocated as opposed to now-freed?
    std::atomic<size_t> m_alloc_outstanding_sz = 0;

    /// High water-mark of #m_alloc_outstanding_sz observed so far.
    std::atomic<size_t> m_alloc_outstanding_sz_hi_wmark = 0;

    /**
     * Gauge: Subset of #m_alloc_outstanding_sz that contains actual capnp-serialization payload.
     *
     * @see Doc header for #m_alloc_lifetime_sz: Here we track M.  See #m_alloc_outstanding_sz which
     *      tracks N.  The difference between the two is the total unused (harshly: wasted) space at this time.
     */
    std::atomic<size_t> m_used_outstanding_sz = 0;

    /// High water-mark of #m_used_outstanding_sz observed so far.
    std::atomic<size_t> m_used_outstanding_sz_hi_wmark = 0;

    /// Histogram tracking per-`Struct_reader` (usually <=> Msg_in) contribution to #m_alloc_lifetime_sz.
    flow::util::stat::Histogram_counter m_histo_msg_alloc_sz{9, 64 * 1024, 64 * 1024, 0};

    /// Subset of bytes tracked in #m_histo_msg_alloc_sz that contains actual capnp-serialization payload.
    flow::util::stat::Histogram_counter m_histo_msg_used_sz;

    // Constructors.

    /**
     * Constructor; supplies bucket-structure knobs for the variable-shape `Histogram_counter`(s) in `*this`.
     * See Snd::Snd() ctor doc header; same explanation applies here.
     *
     * @param msg_used_sz_cfg
     *        For #m_histo_msg_used_sz.
     */
    explicit Rcv(Histo_cfg msg_used_sz_cfg);
  }; // struct Rcv

  // Data.

  /// Send-side (during-serialization) stats.
  Snd m_snd;

  /// Receive-side (during-deserialization) stats.
  Rcv m_rcv;

  // Constructors.

  /**
   * Constructor.
   *
   * @param snd_msg_alloc_sz_cfg
   *        See Snd::Snd() ctor.
   * @param snd_msg_used_sz_cfg
   *        See Snd::Snd() ctor.
   * @param snd_big_leaf_sz_cfg
   *        See Snd::Snd() ctor.
   * @param rcv_msg_used_sz_cfg
   *        See Rcv::Rcv() ctor.
   */
  Serializer_stats(Histo_cfg snd_msg_alloc_sz_cfg, Histo_cfg snd_msg_used_sz_cfg,
                   Histo_cfg snd_big_leaf_sz_cfg, Histo_cfg rcv_msg_used_sz_cfg);
}; // struct Serializer_stats

/**
 * Templated no-data, non-polymorphic subclass of Serializer_stats whose precipitating use-case is to satisfy
 * `flow::util::stat::Global_stats<Stat_set>`'s default-constructibility requirement for `Stat_set`.
 *
 * @see Serializer_stats doc header, section "Why concrete-type...".  It explains that we exist to take #Cfg
 *      at compile-time, so the concrete type's ctor can require no args.  It further explains that
 *      all stat-keeping and stat-updating code would be working on a `Serializer_stats*` (or `&`), obtained
 *      via safe up-cast from a `Serializer_stats_p<...knobs...>`.
 *
 * @tparam Cfg_t
 *         A type with `static constexpr Histo_cfg` members `S_HISTO_SND_MSG_ALLOC_SZ`,
 *         `S_HISTO_SND_MSG_USED_SZ`, `S_HISTO_SND_BIG_LEAF_SZ`, `S_HISTO_RCV_MSG_USED_SZ`,
 *         matching the order of the ctor args to Serializer_stats::Serializer_stats().
 */
template<typename Cfg_t>
struct Serializer_stats_p :
  public Serializer_stats
{
  // Types.

  /// Alias to template parameter `Cfg_t`.
  using Cfg = Cfg_t;

  // Constructors.

  /// Default-constructs by passing Cfg's `Histo_cfg` members into the base ctor.
  Serializer_stats_p();
}; // struct Serializer_stats_p

/**
 * Cfg type for #Heap_serializer_stats: pure-heap user messages.  Histograms are sized for the pure-heap
 * regime with the `Native_socket_stream` transport (should work okay for others): first-segment default 8Ki floor,
 * transport hard-cap 64Ki ceiling per segment (expensive split/reassembly does occur if a segment has to exceed
 * this anyway).
 */
struct Heap_serializer_stats_cfg
{
  // Constants.

  /**
   * See Serializer_stats::Snd::m_histo_msg_alloc_sz.
   * Assumed config (see our doc header) is such that: Seg 0 should be 8Ki always, so that's bucket 0; then 4Ki
   * steps up to and through approx 64Ki.
   */
  static constexpr Histo_cfg S_HISTO_SND_MSG_ALLOC_SZ{16, 8 * 1024, 4 * 1024};

  /**
   * See Serializer_stats::Snd::m_histo_msg_used_sz.
   * Bucket 0 will catch tiny messages; then 4Ki steps up to and through approx 64Ki; after that perhaps
   * user should consider SHM-backing instead?
   */
  static constexpr Histo_cfg S_HISTO_SND_MSG_USED_SZ{18, 1 * 1024, 4 * 1024};

  /**
   * See Serializer_stats::Snd::m_histo_big_leaf_sz.
   * Assumed config (see our doc header) is such that: Seg sizes start at 8Ki through 64Ki cap, so
   * categorize big-leaf sizes that would break through those along that range.
   */
  static constexpr Histo_cfg S_HISTO_SND_BIG_LEAF_SZ{16, 8 * 1024, 4 * 1024};

  /// See Serializer_stats::Rcv::m_histo_msg_used_sz.  This one mirrors #S_HISTO_SND_MSG_USED_SZ.
  static constexpr Histo_cfg S_HISTO_RCV_MSG_USED_SZ{18, 1 * 1024, 4 * 1024};
}; // struct Heap_serializer_stats_cfg

/**
 * Empty type: distinguishing tag for the pure-heap (non-SHM) `flow::util::stat::Global_stats` singleton
 * holding the cumulative #Heap_serializer_stats; reach it via #Heap_serializer_global_stats.
 *
 * @see shm::Shm_msg_outer_tag for the SHM-side counterparts.
 */
struct Pure_heap_tag {};

// Template implementations.

template<typename Cfg_t>
Serializer_stats_p<Cfg_t>::Serializer_stats_p() :
  Serializer_stats(Cfg::S_HISTO_SND_MSG_ALLOC_SZ, Cfg::S_HISTO_SND_MSG_USED_SZ, Cfg::S_HISTO_SND_BIG_LEAF_SZ,
                   Cfg::S_HISTO_RCV_MSG_USED_SZ)
{
  // That's all.
}

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Serializer_stats* src_stats, Serializer_stats* target_stats,
                   Visitor&& visitor)
{
  // m_snd:
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msgs_outstanding, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_snd.m_msgs_outstanding_hi_wmark, m_snd.m_msgs_outstanding);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_alloc_lifetime_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_alloc_outstanding_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_snd.m_alloc_outstanding_sz_hi_wmark, m_snd.m_alloc_outstanding_sz);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_histo_msg_alloc_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_histo_msg_used_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_histo_segs_per_msg, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_big_leaf_alloc_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_histo_big_leaf_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msgs_with_big_leaf, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_seg_grow_cap_lock_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_frame_lifetime_sz, ACCUMULATOR);

  // m_rcv:
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msgs_outstanding, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_rcv.m_msgs_outstanding_hi_wmark, m_rcv.m_msgs_outstanding);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_alloc_lifetime_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_alloc_outstanding_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_rcv.m_alloc_outstanding_sz_hi_wmark, m_rcv.m_alloc_outstanding_sz);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_used_outstanding_sz, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_rcv.m_used_outstanding_sz_hi_wmark, m_rcv.m_used_outstanding_sz);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_histo_msg_alloc_sz, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_histo_msg_used_sz, ACCUMULATOR);
} // declare_stats(Serializer_stats)

} // namespace ipc::transport::struc::stat
