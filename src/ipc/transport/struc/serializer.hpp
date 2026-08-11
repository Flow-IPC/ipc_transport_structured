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

// Not compiled: for documentation only.  Contains concept docs as of this writing.
#ifndef IPC_DOXYGEN_ONLY
static_assert(false, "As of this writing this is a documentation-only \"header\" "
                       "(the \"source\" is for humans and Doxygen only).");
#else // ifdef IPC_DOXYGEN_ONLY

namespace ipc::transport::struc
{

// Types.

/**
 * A documentation-only *concept* defining the behavior of an object capable of zero-copy-serializing,
 * similar to `capnp::MessageBuilder` but geared to transmission over pipe-like IPC transports.
 * At its simplest it is conceptually identical to `MessageBuilder` but, crucially, also allows for the (optional)
 * 2-tier architecture wherein only a small handle is transmitted over an IPC pipe, while the user-controlled
 * tree is stored in a single central location (most likely in shared memory) and is itself never copied.
 * The following discussion assumes sophisticated familiarity with capnp (Cap'n Proto).
 *
 * @see Struct_reader, the counterpart concept.
 *
 * The above description may sound quite a lot like the capnp-supplied `virtual` interface,
 * `capnp::MessageBuilder`.  And some of this concept's most prominent impls have conceptual counterparts
 * in the implementing classes of `MessageBuilder`; for example Heap_fixed_builder
 * evokes `capnp::MallocMessageBuilder` (and in fact internally uses a custom `MessageBuilder` quite
 * similar to `MallocMessageBuilder`).
 *
 * Indeed, at its simplest, Struct_builder is conceptually similar to `capnp::MessageBuilder`:
 *   - It must store a concretely-typed `MessageBuilder` that the user would mutate via
 *     payload_msg_builder(), which returns a pointer to said `MessageBuilder`.  One would typically
 *     mutate it via `->initRoot<SomeUserSchema>()` and so on.
 *     Through this one can set the structured data within the data structure.
 *   - As one does so, the payload bits are set (by capnp-generated code) in zero-copy fashion inside 1+
 *     segments somewhere in RAM (or other vaddr-mapped storage), without any user intervention outside of the
 *     actual capnp mutator calls.
 *   - One can then obtain the resulting serialization -- consisting of 1+ segments in RAM.
 *     `MessageBuilder::getSegmentsForOutput()` is how it's done in `MessageBuilder`;
 *     while we have emit_serialization().
 *
 * Ultimately, in fact, any Struct_builder is always implemented internally as a wrapper around
 * one `MessageBuilder` or another (quite possibly a custom one also implemented by us).
 *
 * So why does Struct_builder exist, outside of cosmetic niceties?  Answer: It is designed around (the possiblity of)
 * two-tier serialization (discussed in "Copying versus zero-copy" below), namely for SHM-based IPC.
 * `MessageBuilder`, alone, does not supply the necessary facilities to make the SHM/two-tier use-case easy;
 * we do.  A good way of looking at it is: Struct_builder allows one to combine two `capnp::MessageBuilder`s,
 * using one for user-facing mutation pre-transmission-over-IPC; and the other `MessageBuilder` for the tiny
 * SHM-handle that's copied over at transmission-over-IPC time.  payload_msg_builder() provides access to the
 * user-mutating; emit_serialization() provides access to the IPC-transmissible bits.  When the serialization
 * strategy is 1-tier, Struct_builder is essentially isomorphic to `MessageBuilder`.  When it is 2-tier,
 * it adds significant conceptual value.
 *
 * In any case a Struct_builder formally works as follows.
 *   - Construct it via `Struct_builder(const Struct_builder::Config&)` ctor form, where Struct_builder::Config
 *     is a small (cheaply-copyable) type storing a few knobs controlling Struct_builder behavior.
 *     (It exists to be able to easily store the same set of knob values to keep reusing it, as well to ease generic
 *     programming.  The knob set for one Struct_builder impl may hugely differ from another's.
 *     Informally, Config is likely to be an *aggregate* `struct` hence aggregate-initializable
 *     (e.g., `Config c{ knob1_val, knob2_val };`).
 *     - This implies that each Struct_builder impl shall feature this ctor form, such that it is possible/advised
 *       to pass the same (equal by value) Config as args each time with no ill effects within reason.
 *     - Struct_builder must also be efficiently move-constructible and move-assignable but not copyable.
 *     - Lastly Struct_builder::Config *must* contain a `size_t` knob, by convention named `m_frame_prefix_sz` or
 *       some stylistically-appropriate variation thereof.  It shall be used in a specific way as noted below
 *       in the emit_serialization() step.
 *   - Call payload_msg_builder() which returns a `MessageBuilder*`; mutate the pointee as usual
 *     (likely `initRoot<SomeUserSchema>()` and so on).
 *   - Call emit_serialization().  This emits a #Segment_bufs container which is a
 *     list of pointer/size pairs, each describing a blob to transmit via IPC (in order) in the next step.
 *     The memory thus described is stored within `*this` until destruction.
 *     - In addition, the first blob in #Segment_bufs *must* be preceded by `m_frame_prefix_sz` zeroed bytes;
 *       and the actual first capnp-segment must begin at exactly just past this area.  (This satisfies the
 *       framing requirement for properly performant operation of certain other aspects of Flow-IPC.)
 *     - In addition, each blob in #Segment_bufs must be of reasonable size; where *reasonable size* is
 *       defined as follows.  Consider size S for a given emitted blob.  If S is always small enough to
 *       fit in *any* sane Blob_sender (e.g., shm::Builder only outputs SHM-handles, each being <= 512 bytes, then
 *       any sane Blob_sender will handle it fine), then we're done with this requirement.  Otherwise:
 *     - The max value for S must be configurable via Struct_builder::Config knob, by convention named
 *       `m_seg_and_frame_sz_cap` or some stylistically-appropriate variation thereof.  If emit_serialization()
 *       has a given internally-stored capnp-segment X with serialized-size (plus `m_frame_prefix_sz` for seg 1)
 *       exceeding `m_seg_and_frame_sz_cap`, it must express it as a series of 2+ sub-segments in #Segment_bufs.
 *       In this case it must also emit a description of this *split* via the #Split_segments out-arg.
 *       This information must be transmitted to the Struct_reader along with the blobs, so it can rejoin
 *       sub-segments into the full segments before deserialization.  (See next bullet.)
 *   - Transmit each of the #Segment_bufs, in order, by copying into a transport (e.g., by using a Blob_sender).
 *     Note we have guaranteed that the transport shall not barf due a too-large #Segment_bufs element.
 *     - If your builder is known to at times have to emit a non-null #Split_segments, meaning it sometimes has
 *       to emit sub-segments (see preceding bullet), then the logical contents of the `Split_segments` must
 *       also be transmitted.  Informally we recommend using the `m_frame_prefix_sz`-sized header to transmit
 *       this (and also other) information, a/k/a *metadata (mdt)*.  Formally however the impl of this aspect is
 *       outside the scope of this concept (outside of formally providing the seg 1-adjoining frame prefix space).
 *   - (`*this` can now be destroyed.)
 *   - Use the counterpart Struct_reader on the other side of this transport to obtain access
 *     to this serialization.  See Struct_reader doc header.
 *
 * The following is an optional continuation of the lifecycle of a Struct_builder.  There are two
 * varieties: read-only and read/write.  The read-only lifecycle continuation is as follows:
 *   - Call emit_serialization() again, producing #Segment_bufs and, possibly, #Split_segments again.
 *   - Transmit the emitted blobs and split-segments info again into some transport again (the same one, a
 *     different one, whatever).
 *   - Again use the counterpart Struct_reader on the other side of this transport to obtain access
 *     to this serialization.
 *   - Repeat as many times as desired.
 *
 * I.e., a message -- once mutated and serialized/sent -- can be re-sent as many times as desired.
 * The concept implementer shall strive to make emit_serialization() be reasonably performant on the 2nd, 3rd, ...
 * call.  While all capnp serializations are zero-copy (before copying into transport), no particular care
 * should be required to accomplish this; at worst it's a matter of grabbing the segment buffer-descriptions +
 * split-segments description (optionally) and emitting them into user-provided target container(s).
 * There typically will be only a few; with SHM-based builders just one.
 *
 * Lastly, then, the read/write lifecycle continuation is as follows:
 *   - (Only if it is a SHM-based serialization) Receive some guarantee that the recipient is done reading
 *     the deserialization (e.g., an ack message).
 *     - If it is not SHM-based, then this is not necessary: The deserialization is fully a copy of `*this`
 *       serialization, so mutating the original has no effect on the recipient.
 *     - (Wait, what?  SHM-based?  Yes: See "Copying versus zero-copy" section below.)
 *   - Further mutate the payload `MessageBuilder` (as accessed via payload_msg_builder()).
 *   - Call emit_serialization() again.
 *   - Transmit the emitted blobs and, possibly, split-segments info into some transport (the same one,
 *     a different one, whatever) -- again.
 *   - Again use the counterpart Struct_reader on the other side of this transport to obtain access
 *     to this serialization.
 *   - Repeat as many times as desired.
 *
 * I.e., it is much like the read-only variant above, except in a SHM-based builder variety one must
 * take concurrent read/write access into consideration.
 *
 * ### Copying versus zero-copy ###
 * You may note that the above unapologetically declares that the serialization's blobs must be *copied* into
 * the transport.  Isn't this a bad requirement?  Isn't it bad to copy the entirety of the serialization?
 * Answer: yes, it's bad, but consider what it means to copy the *entirety* of a serialization.  Imagine a wacky
 * scenario where an impl of this concept leverages the file system to store the serialization on disk, since
 * disk has lots of storage capacity.  It would be very bad to copy the contents of the disk file into the transport
 * (and hence out of it, at the opposing end): it would destroy the whole point of storing on disk to begin with... but:
 * One could, instead of copying the *contents* of the disk file, copy the file's name into the transport
 * (and on the other side, read the file based on the received name).
 *
 * That is to say: The impl may (and indeed, for at least some important applications, should/must) store
 * the actual "inner" serialization (the one being mutated via mutators called on `payload_msg_builder()`) in bulk
 * where it wants; for example in shared memory.  emit_serialization() can/should then emit segments (ideally,
 * just one -- and a small one for that matter) comprised by the "outer" serialization, which conceptually might
 * be a handle referencing the (more-persistent) location of the "inner" (actual) serialization.  This has the
 * following important constraints:
 *   - This outer (emitted by emit_serialization()) serialization by definition must still be a capnp-`struct`.
 *     - The inner one, too, must still be a capnp-`struct`.
 *   - The inner serialization's lifetime must exceed that of `*this` to be of any use!  Otherwise the "handle"
 *     in the outer serialization will be potentially useless by the time Struct_reader counterpart tries
 *     to access it.
 *     - This requirement is no joke: obviously it's necessary; but it means one must worry about cross-process cleanup
 *       of the resources (such as SHM-allocated buffer(s)) used up by the inner serialization.
 *
 * To be clear, this inner-vs-outer serialization paradigm is *not* required.  It is entirely reasonable, for
 * some applications, to encode everything into the outer (required) serialization and not use any indirection or
 * inner serialization at all.  (Heap_fixed_builder -- the simplest of all -- does just that and is basically
 * the same as `MallocMessageBuilder`; modulo adding the split-segment algorithm.)  The benefit is clear:
 * there's no cross-process cleanup, and the internal code is just much simpler.  Conversely the negative
 * is clear: copying large things may be expensive or even impossible (depending on the transport).
 *
 * Both have their place.  In practice, some special-use channels may use a deliberately simple schema
 * with small, scalar-composed payloads.  Then an outer-serialization-only impl (Heap_fixed_builder, say)
 * is perfect.  When this is not so, use a more complex one (typically, we think, one based on SHM; see shm::Builder).
 *
 * ### Allocation and perf ###
 * For this section, please assume we are dealing with a simple (outer-serialization-only/fully-copied) impl
 * of Struct_builder.  The most obvious impl that comes to mind is one that, each time capnp requests a
 * new segment of a certain minimum size, simply allocates on the regular heap.  That's what `MallocMessageBuilder`
 * does, and similarly Heap_fixed_builder does so as well.
 *
 * That's fine.  However, in advanced cases, it is conceivable to want more performance.  Regular-heap allocation,
 * depending on the size and underlying allocator impl (tcmalloc/jemalloc/ptmalloc...), can be somewhat expensive.
 * In avoiding heap-allocation one could use other systems: memory pools; or even just a single pre-allocated
 * area to be reused by *every* new `*this`.  Let's briefly think about each of those.
 *
 * Pools: This is not too different from a regular-heap allocator like Heap_fixed_builder.  It would just
 * allocate from (insert pool library here).  Heck, it could even use the shared-buffer functionality of a
 * single `flow::util::Sharing_blob` to dole out memory chunks from a single heap-allocated buffer.
 *
 * Single pre-allocated area to be reused by *every* new `*this`: This would be, of course, blindingly fast:
 * probably `Config` would allocate a single `Sharing_blob` and each `*this` it created would be told to use that
 * blob, no questions asked.  The problem, obviously, is that 2+ `*this`es might step on each other concurrently.
 * I.e., any use of this would basically require a pattern wherein a `*this` is created, mutated, emitted,
 * destroyed; *then* same for the next out-message; and so on.  Then there is no problem.  Simply put, then:
 * it is reasonable to develop such an impl of Struct_builder -- if one requires the user to carefully
 * ensure each out-message is entirely handled before work on the next one begins.  For some applications that
 * may be reasonable.
 *
 * Similar reasoning could be extended to 2-tier (zero-copy/SHM-based) impls of Struct_builder.  Naturally
 * however one would need to ensure the given payload is fully consumed by the opposing side (Struct_reader)
 * before creating the next `*this`; it is not enough for merely the local `*this` to be destroyed, as
 * the underlying payload may be referred-to by a Struct_reader still.
 */
class Struct_builder // Note: movable, not copyable.
{
public:
  // Types.

  /**
   * Copy-ctible, copy-assignable, default-ctible type -- informally, cheaply copyable and likely an *aggregate*
   * `struct` -- objects of which store data that Struct_builder main ctor knows how to interpret as knobs controlling
   * its behavior.  While a plain-old-`struct` is a solid choice, a more complex API for a given Struct_builder impl
   * is conceivable.  In general, the simpler to construct and query, but especially construct (ideally by
   * aggregate initialization), the better.  Cheap copyability is essential.
   *
   * A handful of knobs are potentially required (details are inside `struct Config` body).  For each required knob:
   * Struct_builder::emit_serialization() must emit blob(s) that follow this knob in a specific way
   * (see emit_serialization() doc header).  The knob name can differ from the suggested name but *should*
   * not differ except due to stylistic constraints.  The suggest name assumes the aggregate `struct` setup, but
   * as noted above your setup may differ.
   *
   * Internal Struct_builder impl code will be querying the knobs; the user may be explicitly constructing
   * Config and then constructing Struct_builder repeatedly from that same Config.  Most likely, however,
   * various utilities (like struc::Channel::heap_fixed_builder_config()) will return ready-made `Config`s without
   * user's direct input.
   *
   * As you can see, there are not many requirements: this sub-concept is essentially meant to be (usually)
   * an aggregation of what would normally be individual ctor args to Struct_builder, a few little scalars
   * at that.  It *can* however conceivably feature ctors and more advanced behavior (while retaining cheap
   * copyability).
   *
   * While Config must default-ctible, Struct_builder may stipulate that such an as-if-default-cted
   * Config being passed to Struct_builder ctor = undefined behavior or otherwise invalid.
   */
  struct Config
  {
  public:
    // Types.

    /// Alias to containing Struct_builder.  Useful for generic programming.
    using Builder = Struct_builder;

    // Data.

    /// First knob is typically the `Logger*` (null allowed) the Struct_builder will use.
    flow::log::Logger* m_logger_ptr;

    /**
     * (Required knob) Controls the size of the zero-initialized area at the start of segment 1-containing blob 1
     * emitted by emit_serialization().  See `emit_serialization()` doc header.
     */
    size_t m_frame_prefix_sz;

    /**
     * (Required knob, if and only if the Struct_builder impl can produce capnp-segments too large for at least
     * some sane transports) Controls the maximum size of each blob emitted by emit_serialization().
     * See `emit_serialization()` doc header.
     */
    size_t m_seg_and_frame_sz_cap;

    /// An arbitrary, but cheaply-copyable in aggregate, set of knobs like (e.g.) this one follow.
    Example_type m_example_struct;

    /// Or like this one.
    example_t m_example_val;

    /**
     * Penultimate knob, present/relevant when there are two layers of serialization as in SHM/zero-copy
     * `Struct_builder`s, is typically the stats structure pointee (null allowed) for the *inner*
     * serialization.  Not mandated by concept, even when relevant, but recommended.
     */
    stat::Serializer_stats::Snd* m_core_stats;

    /**
     * Last knob is typically the stats structure pointee (null allowed) for either the *only* or *outer*
     * (if there are two layers as in SHM/zero-copy `Struct_builder`s) serialization.
     * Not mandated by concept but recommended.
     */
    stat::Serializer_stats::Snd* m_stats;
  }; // class Config

  /**
   * Type objects of which specify to emit_serialization() the opposing recipient for which the serialization
   * is intended; Null_session if no information is necessary to specify this.  Informally: typically, in
   * a non-zero-copy builder the serialization itself would contain all necessary information, hence this
   * would be Null_session; zero-copy (SHM-based) builder needs to specify a non-empty type here.
   */
  using Session = unspecified;

  // Constructors/destructor.

  /// Default ctor, leaving `*this` in a state only suitable for destruction or being moved-to.
  Struct_builder();

  /**
   * Main ctor, creating a new builder according to the knobs in `Config config` arg.
   *
   * @param config
   *        Struct_builder::Config storing the knobs controlling how `*this` will work.
   */
  explicit Struct_builder(const Config& config)

  /// Disallow copy construction.
  Struct_builder(const Struct_builder&) = delete;

  /**
   * Move-constructs `*this` to be equal to `src`, while `src` becomes as-if defaulted-cted.
   *
   * @param src
   *        Moved-from object that becomes as-if default-cted.
   */
  Struct_builder(Struct_builder&& src);

  /// Destructor.  Do not use `*payload_msg_builder()` or any copies thereof past this.
  ~Struct_builder();

  // Methods.

  /// Disallow copy assignment.
  Struct_builder& operator=(const Struct_builder&) = delete;

  /**
   * Move-assigns `*this` to be equal to `src`, while `src` becomes as-if defaulted-cted; or no-op
   * if `&src == this`.
   *
   * @param src
   *        Moved-from object that becomes as-if default-cted, unless it is `*this`.
   * @return `*this`.
   */
  Struct_builder& operator=(Struct_builder&& src);

  /**
   * Pointer to the payload #Capnp_msg_builder_interface, suitable for mutation by the user.  Informally: we expect the
   * outside use of some subset of the pointee methods `initRoot()`, `getRoot()`, `setRoot()`, `adoptRoot()`,
   * `getOrphanage()` but *not* `getSegmentsForOutput()`.  To access the serialization properly you must use
   * emit_serialization() instead.  Otherwise you'd be foregoing the main point of Struct_builder's existence which
   * is to allow for a 2-tier serialization (as described in the concept doc header) and therefore true zero-copy
   * end-to-end.
   *
   * ### Errors ###
   * See emit_serialization().
   *
   * @return See above.  Always returns the same pointer.
   */
  Capnp_msg_builder_interface* payload_msg_builder();

  /**
   * Returns a description of the IPC-friendly serialization (which is guaranteed to remain
   * valid and unchanged until `*this` is destroyed, or the contents are further mutated via payload_msg_builder()),
   * including a split-view of each too-long capnp-segment (if any) and the header area (if any) immediately
   * preceding the first aforementioned serialization segment.
   *
   * You may call this method more than once per `*this`.  However, if it fails, `*this` shall be considered useless.
   * Please read the concept doc header carefully regarding the context and proper use of calling
   * emit_serialization() more than once.  The result is (generally) meaningless once `*this` is mutated further.
   *
   * A formal description is (we feel) best expressed as an algorithm.  Consider the IPC-transmissible serialization
   * (if an inner and outer serialization are relevant -- see class doc header -- then this is the outer
   * serialization).  By definition it consists of 1+ segments (each described fully by a location and size in memory);
   * with H bytes immediately preceding segment 1 reserved for a frame prefix/header.  H is Config::m_frame_prefix_sz.
   * Also by established conventions any given segment that, when combined with its header
   * (0 bytes for all segments except segment 1), exceeds size limit Config::m_seg_and_frame_sz_cap shall need to
   * be split into 2+ views, each below that limit (so instead of 1 IPC-message for that segment, one would use 2+
   * IPC-messages).  (If there is no Config::m_seg_and_frame_sz_cap, meaning a `*this` by its nature *always*
   * produces small outer-serialization segment(s), then pretend `split_segs == nullptr`.  The described algorithm
   * will then reduce to essentially skipping the split-segments steps; and since the premise is the resulting
   * seg-sizes are always small-enough, it'll never emit error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG.)
   *
   * Then, given these assumptions, on success:
   *   - `*target_blobs` shall be cleared and appended-to, so as to contain in order each serialization segment
   *     view (ptr, size), not including any header.
   *     - Each such segment whose `.size()` (plus `hdr_blob->size()` for seg 1) would have exceeded
   *       Config::m_seg_and_frame_sz_cap shall instead be represented by the 2+ post-split sub-segment elements
   *       composing that segment (contiguously).  Therefore each actual `X.size()` shall *not* exceed that limit, where
   *       `X` is any element in out-arg `*target_blobs`.
   *     - `*split_segs` shall be cleared and appended-to, indicating which of `*target_blobs` are in fact
   *       splits as opposed to whole segments.  By IPC-transmitting some encoding of the `*target_blobs`-described
   *       buffers' contents *and* the contents of `*split_segs`, the receiver can assemble a copy of the pre-split
   *       segments.  In fact typically the `hdr_blob->size()`-sized header is where one would shove an encoding
   *       of `*split_segs`; so the receiver would (1) interpret the header of agreed-upon size just ahead of
   *       mandatory seg 1; (2) reassemble any split-segments based on that; and (3) be able to finally use
   *       the (outer) serialization.
   *   - `*hdr_blob` out-arg shall describe area of size H immediately preceding seg 1.  Caller could then fill-out
   *     appropriate header values within this area.
   *
   * `split_segs = nullptr` is legal.  In this case:
   *   - Each pre-split segment must be sized at most Config::m_seg_and_frame_sz_cap; or if there is no such knob
   *     then it must be *small*, meaning small enough to fit into any reasonable IPC-transport's single message.
   *   - If that is not the case: error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG shall be emitted.
   *   - Therefore, in that mode, no splitting is indeed possible.  This is useful, at least, in the inner+outer
   *     serialization setup (as with zero-copy; see class doc header), wherein the bulky data is not what is
   *     IPC-transmitted; and what *is* IPC-transmitted (the outer serialization) typically contains only a small
   *     handle to the aforementioned (probably SHM-stored) bulky data.  In such cases usually we can guarantee
   *     a small, constant-or-smaller-sized, single-segment serialization; and a split being necessary indicates
   *     a design or code bug.
   *     - For example: as of this writing shm::Builder internally stores a Heap_fixed_builder and in its
   *       shm::Builder::emit_serialization() passes `split_segs = nullptr` to Heap_fixed_builder::emit_serialization().
   *
   * ### The zero-initialization of `*hdr_blob`-described area ###
   * This is a finicky requirement, so be careful to read/grok this.
   *
   * `*hdr_blob` and `target_blobs->front()` are, by definition, adjacent (in that order) parts at the start of
   * a heap-allocated buffer.  Its structure is, in order:
   *   - `*hdr_blob`-described frame-prefix area for the caller's use.
   *   - N bytes allocated for segment 1.
   *     - M (which is at most N) of these bytes are at emit_serialization() time actually in use by the serialization.
   *     - The rest ((M - N) bytes; possibly 0) are a garbage area that ended up not being used by the serialization.
   *
   * The heap-allocated buffer containing these items is allocated well in advance of emit_serialization() 1.
   * The requirement:
   *   - When this occurs, all of the `*hdr_blob->size()` area **must** be zeroed and then not touched by `*this`
   *     subsequently, forever.
   *
   * Corollary: The *first* time emit_serialization() is called, that area *will* be zeroed, a fact the caller can
   * and should use to their advantage.  After that, the caller is free to use it however they want: shove capnp
   * stuff in there, zero it again... whatever it wants.  However it is important to rememeber that
   * emit_serialization() **shall not** re-zero the area each time; it is guaranteed zeroed only after the *1st*
   * emit_serialization().
   *
   * ### Errors ###
   * emit_serialization() may emit an error via standard (to Flow-IPC) semantics if the serialization procedure in
   * some way fails.  Mutations on payload_msg_builder() shall be assumed to work and not throw or otherwise emit
   * errors, outside of misuse of capnp or bugs in capnp.  However emit_serialization() may report an error
   * synchronously, even if internally it is known there is an error earlier than the emit_serialization() call itself.
   *
   * See above regarding the specific error `INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG`.
   *
   * @param target_blobs
   *        On success (no error emitted) this is cleared and appended-to with the sequence of segments and/or
   *        post-split sub-segments, representing the pre-split segments comprised by the outer capnp-serialization
   *        of `*this`.  In the case of segment 1, byte 1 is that of the first serialization segment (or sub-segment 1
   *        thereof) -- not the header (see `hdr_blob`) immediately preceding it.
   * @param hdr_blob
   *        On success (no error emitted) `*hdr_blob` shall be set to describe the area just ahead of
   *        `target_blobs->front()` of size Config::m_frame_prefix_sz as supplied to ctor.  This area shall always
   *        be valid; and `*hdr_blob` shall always be set to the same thing for `*this` (modulo a move-from or
   *        move-to).  Lastly see above section "The zero-initialization..." regarding subtle zeroing guarantees.
   * @param split_segs
   *        If null, `*target_blobs` shall contain only whole segments; and if splitting were necessary to fit into
   *        the limit, then `INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG` shall be emitted.
   *        If not null, `*split_segs` shall indicate precisely which of `*target_blobs` represent sub-segments
   *        requiring separate transmission and reassembly.
   * @param session
   *        Specifies the opposing recipient for which the serialization is intended.
   *        If #Session is Null_session, then `Session{}` is the only value to supply here.  Otherwise more
   *        information is necessary.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  Generated codes:
   *        error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG (see above; only if `split_segs` is null),
   *        possibly more (implementation-dependent).
   */
  void emit_serialization(Segment_bufs* target_blobs, util::Blob_mutable* hdr_blob, Split_segments* split_segs,
                          const Session& session, Error_code* err_code = nullptr) const;

  /**
   * How many capnp-segments `*this` currently owns (has allocated) in total.
   *
   * @note If emit_serialization() is called with `split_segs == nullptr` and succeeds: `target_blobs->size()` will
   *       equal this.  However if `split_segs != nullptr` the size may exceed `n_serialization_segments()`, as
   *       splitting may be required.
   *
   * @warning As of this writing this accessor is intended for logging/reporting only.  The more practically salient
   *          similar piece of into is `target_blobs->size()` after emit_serialization(); and that number may or may
   *          not equal n_serialization_segments().
   *
   * @return See above.
   */
  size_t n_serialization_segments() const;
}; // class Struct_builder

/**
 * A documentation-only *concept* that is, conceptually, roughly what `capnp::MessageReader` is to
 * `capnp::MessageBuilder`, to be used on an in-message serialized by a counterpart
 * Struct_builder, having been transmitted over an IPC transmitter of blobs.  The following discussion assumes
 * understanding of Struct_builder (see its doc header).
 *
 * @see Struct_builder, the counterpart concept.
 *
 * Here is how a Struct_reader is used (in order):
 *   - Suppose the counterpart Struct_builder was used as documented in its doc header, for a given out-message.
 *   - Construct a matching Struct_reader on the other side.
 *     - See also Struct_reader::Config.
 *   - Allocate a #Segment_blob_in (`flow::util::Basic_blob` or derivative).  Call it `S`.
 *     Copy the next segment's contents out of the transport into the aforementioned `Segment_blob_in`.
 *     (Ensure its `.begin()` and `.end()` exactly define (sub-)seg 1 by the time you later call deserialization().)
 *     Call `add_serialiazation_segment(S)`.
 *   - If and only if the serialization produced by the Struct_builder consists of 2+ (sub-)segments:
 *     Repeat the previous bullet point for each of the additional 1+ (sub-)segments.
 *   - Lastly execute Struct_reader::deserialization<Struct>() (at most once) to obtain obtain a capnp-generated
 *     `Struct::Reader`.  The in-arg to this call shall be a #Split_segments structure describing how (some of) the
 *     sub-segments passed-to add_serialization_segment() above must be re-joined to obtained the original
 *     actual segments; or null if no splitting was done (hence no joining is needed).  (Hence you must have somehow
 *     passed-through this information over IPC, along with the (sub-)segments themselves.)
 *     - Access the structured contents that the other side had mutated-in via the latter-returned `Struct::Reader`.
 *   - Finally, the Struct_reader `*this` can be destroyed.
 *
 * ### Copying versus zero-copy ###
 * See the same-named section of Struct_builder doc header.  Similar concerns apply here.  Generally speaking,
 * how a Struct_reader works is dictated by the decisions made for its Struct_builder counterpart.
 */
class Struct_reader // Note: not copyable; could be movable but see copy ctor doc below.
{
public:
  // Types.

  /**
   * Analogous to Struct_builder::Config but for deserialization.
   * @see Struct_builder::Config
   */
  struct Config
  {
  public:
    // Types.

    /// Alias to containing Struct_reader.  Useful for generic programming.
    using Reader = Struct_reader;

    // Data.

    /// First knob is typically the `Logger*` (null allowed) the Struct_reader will use.
    flow::log::Logger* m_logger_ptr;

    /// An arbitrary, but cheaply-copyable in aggregate, set of knobs like (e.g.) this one follow.
    Example_type m_example_struct;

    /// Or yet another!  Note in no way are these necessarily symmetric to Struct_builder::Config counterpart.
    example_t m_example_val;

    /**
     * Last knob is typically the stats structure pointee (null allowed) for the rcv-side cumulative
     * stats counterpart to Struct_builder::Config::m_stats.  Not mandated by concept but recommended.
     *
     * @note No counterpart to Struct_builder::Config::m_core_stats: in any 2-layer/zero-copy
     *       impl the inner (SHM-resident) deserialization is structurally a no-op (no allocation
     *       at rcv-time), so there is nothing to track on the inner rcv side.
     */
    stat::Serializer_stats::Rcv* m_stats;
  }; // struct Config

  // Constructors/destructor.

  /**
   * Main ctor, creating a new functioning reader according to the knobs in `Config config` arg.
   *
   * @param config
   *        Struct_reader::Config storing the knobs controlling how `*this` will work.
   */
  explicit Struct_reader(const Config& config);

  /**
   * Disallow copy construction.
   *
   * @note Concept does not require movability either, though it is allowed.  Generically, as of this writing,
   *       nothing relies on it.
   */
  Struct_reader(const Struct_reader&) = delete;

  /// Destructor.  Do not use the `Reader` `deserialization()` or any copies thereof past this.
  ~Struct_reader();

  // Methods.

  /// Disallow copy assignment.
  Struct_reader& operator=(const Struct_reader&) = delete;

  /**
   * Prior to deserialization() records and owns via move-semantics the next in-order IPC-received (sub-)segment,
   * from the list of 1+ such (sub-)segments which together contain the (outer) serialization to be decoded by
   * deserialization().  The supplied #Segment_blob_in shall be moved into `*this` and stored there until
   * destruction or move-from.
   *
   * To use deserialization() subsequently, you must:
   *   - call this at least once;
   *   - call this (N - 1) more times, where N is the number of (sub-)segments produced by the counterpart
   *     Struct_builder::emit_serialization();
   *   - ensure that, including for the first such call, `blob.begin()` points at byte 1 of the (sub-)segment and not
   *     any (irrelevant to `*this` framing header that might precede it).
   *
   * The memory area *described by* `blob` shall remain valid until deserialization().
   * For the *first* add_serialization_segment() call, the memory area *described by* `blob` shall remain valid
   * until destruction or move-from.  (Rationale: It preserves any potential header Struct_builder may have also
   * preserved as a special case, in seg 1 only.)
   *
   * @param blob
   *        Blob to move into `*this`.  See above.
   */
  void add_serialization_segment(Segment_blob_in&& blob);

  /**
   * After all serialization segments have been acquired in RAM and recorded via add_serialization_segment(),
   * this returns a reader object whose capnp-generated accessors can read the structured contents of `*this`.
   * The memory area given to the *first* add_serialization_segment() preceding this call shall not be invalidated
   * by this call.  (Any subsequently-supplied ones may be, presumably as/if manifested by the joining
   * procedure in deserialization().)
   *
   * Behavior is undefined if this is called before all add_serialization_segment() calls have been made.
   * Behavior is undefined if add_serialization_segment() is called after this is called.
   *
   * deserialization() shall be called no more than once.
   *
   * The returned `Reader` shall remain valid until `*this` is destroyed or moved-from.
   *
   * @tparam Struct
   *         Same meaning as in the vanilla capnp use of `MessageReader::getRoot<Struct>()`.
   * @param split_segs_or_null
   *        Value equal to that generated by the serializer/sender's Struct_builder::emit_serialization() call;
   *        null if that generated an empty sequence (no splitting occurred, no reassembly required).
   *        Context: this value, or lack thereof, would likely have been
   *        IPC-transmitted along with the segments (registered via add_serialization_segment())
   *        themselves.  In particular the seg 1 header area maintained by the Struct_builder is a possible place to
   *        encode this info (as well as the other likely metadata value: the # of segments to expect).
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS (add_serialization_segment() was called
   *        an insufficient number of times; in particular 0 times always triggers this),
   *        error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED (add_serialization_segment() was called with
   *        a segment whose finalized `.begin()` is a misaligned address),
   *        error::Code::S_DESERIALIZE_FAILED_REASSEMBLY_FAILED (something wrong with the contents of
   *        `*split_segs_or_null` including but not limited to it being `.empty()`),
   *        implementation may also emit other errors.
   * @return See above.  If and only if `err_code` is non-null, and truthy `*err_code` is emitted, a useless
   *         (default-cted) `Reader` is returned.
   */
  template<typename Struct>
  typename Struct::Reader deserialization(const Split_segments* split_segs_or_null, Error_code* err_code = nullptr);
}; // class Struct_reader

} // namespace ipc::transport::struc

#endif // IPC_DOXYGEN_ONLY
