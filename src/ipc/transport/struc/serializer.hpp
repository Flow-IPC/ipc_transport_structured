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
 *   - Call payload_msg_builder() which returns a `MessageBuilder*`; mutate the pointee as usual
 *     (likely `initRoot<SomeUserSchema>()` and so on).
 *   - Call emit_serialization().  This emits a #Segment_ptrs container which is basically a
 *     list of *pointers* to `flow::util::Blob` (which is much like `vector<uint8_t>`), where each `Blob` therein
 *     is stored within `*this` until destruction.
 *   - Transmit each `Blob`, in order, by copying into a transport (e.g., by using a Blob_sender).
 *     Note that the transport must be capable of transmitting these blobs while maintaining ordering and
 *     boundaries and without barfing due to anything, including a single blob, being too large.
 *     (It is up to the Struct_builder impl to document any such limitations it might have.)
 *   - (`*this` can now be destroyed.)
 *   - Use the counterpart Struct_reader on the other side of this transport to obtain access
 *     to this serialization.  See Struct_reader doc header.
 *
 * The following is an optional continuation of the lifecycle of a Struct_builder.  There are two
 * varieties: read-only and read/write.  The read-only lifecycle continuation is as follows:
 *   - Call emit_serialization() again.
 *   - Transmit the emitted `Blob`s again into some transport again (the same one, a different one, whatever).
 *   - Again use the counterpart Struct_reader on the other side of this transport to obtain access
 *     to this serialization.
 *   - Repeat as many times as desired.
 *
 * I.e., a message -- once mutated and serialized/sent -- can be re-sent as many times as desired.
 * The concept implementer shall strive to make emit_serialization() be reasonably performant on the 2nd, 3rd, ...
 * call.  While all capnp serializations are zero-copy (before copying into transport), no particular care
 * should be required to accomplish this; at worst it's a matter of grabbing the segment pointers and emitting
 * them into a user-provided target container.  There typically will be only a few; with SHM-based builders
 * just one.
 *
 * Lastly, then, the read/write lifecycle continuation is as follows:
 *   - (Only if it is a SHM-based serialization) Receive some guarantee that the recipient is done reading
 *     the deserialization (e.g., an ack message).
 *     - If it is not SHM-based, then this is not necessary: The deserialization is fully a copy of `*this`
 *       serialization, so mutating the original has no effect on the recipient.
 *     - (Wait, what?  SHM-based?  Yes: See "Copying versus zero-copy" section below.)
 *   - Further mutate the payload `MessageBuilder` (as accessed via payload_msg_builder()).
 *   - Call emit_serialization() again.
 *   - Transmit the emitted `Blob`s into some transport (the same one, a different one, whatever) -- again.
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
 * the same as `MallocMessageBuilder`.)  The benefit is clear: there's no cross-process cleanup,
 * and the internal code is just much simpler.  Conversely the negative
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
   * Internal Struct_builder impl code will be querying the knobs; the user may be explicitly constructing
   * Config and then constructing Struct_builder repeatedly from that same Config.  Most likely, however,
   * various utilities (like Session_mv::heap_fixed_builder_config()) will return ready-made `Config`s without
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
    flow::log::Logger m_logger_ptr;

    /// An arbitrary, but cheaply-copyable in aggregate, set of knobs like (e.g.) this one follow.
    Example_type m_example_struct;

    /// Or like this one.
    example_t m_example_val;
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
   * Returns the serialization in the form of a sequence of 1+ pointers to `Blob`s which are guaranteed to remain
   * valid and unchanged until `*this` is destroyed, or the contents are further mutated via payload_msg_builder().
   * These are *appended* to `*target_blobs`.
   *
   * You may call this method more than once per `*this`.  However, if it fails, `*this` shall be considered useless.
   * Please read the concept doc header carefully regarding the context and proper use of calling
   * emit_serialization() more than once.
   *
   * The result (`*target_blobs` contents) is (generally) meaningless once `*this` is mutated further.
   *
   * ### Errors ###
   * emit_serialization() may emit an error via standard (to Flow-IPC) semantics if the serialization procedure in
   * some way fails.  Mutations on payload_msg_builder() shall be assumed to work and not throw or otherwise emit
   * errors, outside of misuse of capnp or bugs in capnp.  However emit_serialization() may report an error
   * synchronously, even if internally it is known there is an error earlier than the emit_serialization() call itself.
   *
   * @param target_blobs
   *        On success (no error emitted) this is appended-to with the sequence of pointers to segments
   *        as `Blob`s.
   * @param session
   *        Specifies the opposing recipient for which the serialization is intended.
   *        If #Session is Null_session, then `Session()` is the only value to supply here.  Otherwise more
   *        information is necessary.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.
   */
  void emit_serialization(Segment_ptrs* target_blobs, const Session& session, Error_code* err_code = 0) const;

  /**
   * Returns what `target_blobs.size()` would return after calling `emit_serialization(&target_blobs)` (with
   * an empty `target_blobs` going-in), right now.
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
 *   - Call `add_serialization_segment(M)`, where M is the maximum number of bytes the next blob-read out of the
 *     transport might return when reading the next serialization segment produced by Struct_builder.
 *     (M, in practice, is limited by at least the transport and how it is used; and the segment sizes
 *     the Struct_builder impl in practice produces.)
 *     - `*this` shall obtain an area in memory at least M bytes long into which it is guaranteed safe for
 *       for the user to direct-write, until the next add_serialization_segment(), deserialization(), or dtor
 *       call (whichever happens earliest).
 *     - add_serialization_segment() shall return a *pointer* to a `flow::util::Blob` whose
 *       [`begin()`, `end()`) range represents that memory area (with `.size()` equal to M).
 *       - If the impl determines that its (not specified by this concept) memory acquisition algorithm
 *         is incapable of obtaining M bytes, it shall return null.  The user must be ready for this
 *         eventuality.
 *   - Copy the next segment's contents out of the transport into the aforementioned `Blob`.
 *   - Use `flow::util::Blob::resize()` (or similar methods that invoke it, notably `start_past_prefix_inc()`)
 *     to modify the [`begin()`, `end()`) range such that it stores the exact contents of the segment.
 *     - In practice: It at least means `resize(K)`, where `K <= M`, to mark the actual size of the received
 *       segment, since M is the *max* possible size.  So the garbage area into which no bytes were written must
 *       be excluded.
 *     - It may also mean sliding around `begin()` and/or `end()` to deliberately exclude any framing information
 *       used by the user's protocol when transmitting segments.  This is important to maintain zero-copy perf.
 *     - Behavior is undefined if the user calls `.make_zero()` on that blob.  Only `.resize()` (and its
 *       convenience wrappers like `start_past_prefix_inc()`) may be invoked.
 *   - If and only if the serialization produced by the Struct_builder consists of 2+ segments:
 *     Repeat the previous 3 bullet points, in order, for each of the additional 1+ segments.
 *   - Lastly execute Struct_reader::deserialization<Struct>() (at most once) to obtain obtain a capnp-generated
 *     `Struct::Reader`.
 *     - Access the structured contents that the other side had mutated-in via the latter-returned `Struct::Reader`.
 *   - Finally, the Struct_reader `*this` can be destroyed.
 *
 * ### Copying versus zero-copy ###
 * See the same-named section of Struct_builder doc header.  Similar concerns apply here.  Generally speaking,
 * how a Struct_reader works is dictated by the decisions made for its Struct_builder counterpart.
 *
 * ### Allocation and perf ###
 * See the same-named section of Struct_builder doc header first.  Similar concerns apply here, but
 * actually the decision about how to obtain memory areas as returned by add_serialization_segment() in
 * Struct_reader can be 100% decoupled from how Struct_builder counterpart acquires memory.
 * E.g., the builder might allocate from heap, while the reader might allocate from a single
 * buffer allocated by `Config`, just because the latter has a single-threaded receiver setup, while the former
 * involves multi-threaded concurrently-sending chaos.  One can mix and match: the outer serialization is
 * always copied-into the transport (then destroyed) on the builder side and copied-from the transport
 * on the reader side.  The manner in which memory is acquired before each write (builder case)
 * or before each read (reader case) can be determined by the algorithm used by the application on each
 * side.
 *
 * ### Why is add_serialization_segment() designed this way? ###
 * It might seem unnecessarily complex: user must call add_serialization_segment(), then write to that `Blob`
 * and adjust its range.  They could conceivably just allocate their own `Blob` and then `move()` it into
 * `*this` via a single, simpler `add_serialization_segment(Blob&&)` call.  So why is it done the former way?
 *
 * Answer: It's to support the possibilities discussed in the "Allocation and perf" section above.
 * add_serialization_segment() places the responsibility of obtaining target memory for the blob-read
 * on the Struct_reader concept impl.  Hence different concept impls can be swapped in for different
 * perf/use-case trade-offs.  One would use a pool -- another would use the heap... etc.
 */
class Struct_reader // Note: movable, not copyable.
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
    flow::log::Logger m_logger_ptr;

    /// An arbitrary, but cheaply-copyable in aggregate, set of knobs like (e.g.) this one follow.
    Example_type m_example_struct;

    /// Or yet another!  Note in no way are these necessarily symmetric to Struct_builder::Config counterpart.
    example_t m_example_val;
  }; // struct Config

  // Constructors/destructor.

  /// Default ctor, leaving `*this` in a state only suitable for destruction or being moved-to.
  Struct_reader();

  /**
   * Main ctor, creating a new functioning builder according to the knobs in `Config config` arg.
   *
   * @param config
   *        Struct_reader::Config storing the knobs controlling how `*this` will work.
   */
  explicit Struct_reader(const Config& config)

  /// Disallow copy construction.
  Struct_reader(const Struct_reader&) = delete;

  /**
   * Move-constructs `*this` to be equal to `src`, while `src` becomes as-if defaulted-cted.
   *
   * @param src
   *        Moved-from object that becomes as-if default-cted.
   */
  Struct_reader(Struct_reader&& src);

  /// Destructor.  Do not use the `Reader` `deserialization()` or any copies thereof past this.
  ~Struct_reader();

  // Methods.

  /// Disallow copy assignment.
  Struct_reader& operator=(const Struct_reader&) = delete;

  /**
   * Move-assigns `*this` to be equal to `src`, while `src` becomes as-if defaulted-cted; or no-op
   * if `&src == this`.
   *
   * @param src
   *        Moved-from object that becomes as-if default-cted, unless it is `*this`.
   * @return `*this`.
   */
  Struct_reader& operator=(Struct_reader&& src);

  // Methods.

  /**
   * Prior to deserialization() obtains a memory area `max_sz` bytes long into which the user may write-to
   * until the next add_serialization_segment(), deserialization(), or dtor call (whichever happens first);
   * returns a pointer to that area as described by the pointed-to `Blob`'s [`begin()`, `end()`) range.
   * If the concept impl decides `max_sz` bytes are not available, returns null.  `*this` shall not be used
   * subsequent to such an eventuality.
   *
   * To use deserialization() subsequently, you must:
   *   - call this at least once (successfully);
   *   - call this (N - 1) more times (successfully), where N is the number of segments produced by the counterpart
   *     Struct_builder.
   *     - So N >= 1 by definition.
   *
   * The required sequence for each in-segment is:
   *   - Call add_serialization_segment(), which yields pointer to a `Blob`.
   *   - Write segment serialization into (some subrange of) that `Blob`'s [`begin()`, `end()`) range
   *     (presumably directly out of the transport).
   *   - Adjust said `Blob`'s [`begin()`, `end()`) range to contain exactly the serialization segment.
   *     - You may use `.resize()` or its convenience wrappers `.start_past_prefix()` and `.start_past_prefix_inc()`.
   *       Use these to (1) mark the actual segment's size (which may well be < `max_sz`) and (2)
   *       discard any framing data that came in together with the segment serialization.
   *     - You may not `.make_zero()` (or behavior is undefined).
   *     - You may not use `.resize()` and friends to cause `begin()` to go left or `end()` to go right.
   *       They can only remain unchanged or travel right and left respectively (the range's edges cannot "extend"
   *       past the original range provided).
   *
   * After these steps, either add_serialization_segment() again (if more segments are forthcoming), or
   * call deserialization() (to use the structured data), or invoke dtor (if `*this` is no longer of interest).
   *
   * ### Alignment ###
   * The returned `Blob`'s begin-to-end range shall be word-aligned, meaning `.const_data()` shall be on a word
   * boundary, meaning the numeric value thereof shall be a multiple of `sizeof(void*)`.  This is the guarantee
   * of add_serialization_segment().
   *
   * Accordingly, any `.resize()` (etc.) done on the returned `Blob` by the user *must* result in a
   * `.const_data()` (a/k/a `.begin()`) that is *still* word-aligned by the same definition.  Informally there
   * are ~2 ways to guarantee this on the user's part:
   *   - Just don't change `begin()` (do not use prefix frame data; you may use postfix frame data instead).  Or:
   *   - If you require frame data to live in front of the segment, in your protocol ensure that
   *     the size of such frame data is a multiple of `sizeof(void*)`.
   *
   * @param max_sz
   *        `Blob` to which the returned pointer points shall have `.size() == max_sz`.
   *        (It may have equal or larger `.capacity()`.)  See above.
   * @return Pointer to the `Blob` inside which to place, subsequently, the next segment's contents.  See above.
   */
  flow::util::Blob* add_serialization_segment(size_t max_sz);

  /**
   * After all serialization segments have been acquired in RAM via add_serialization_segment() and finalized
   * via begin-end-range adjustment, this returns a reference to the reader object whose capnp-generated
   * accessors can read the structured contents of `*this`.  Behavior is undefined if this is called before all
   * add_serialization_segment() calls have been made (and they must have been successful).  Behavior is undefined
   * if add_serialization_segment() is called after this is called.
   *
   * deserialization() shall be called no more than once.
   *
   * The returned `Reader` shall remain valid until `*this` is destroyed.  (Therefore any copies thereof
   * shall also be valid in the same time frame.  However there is no perf benefit to making such copies, and
   * there is likely a small penalty to doing so.)
   *
   * @tparam Struct
   *         Same meaning as in the vanilla capnp use of `MessageReader::getRoot<Struct>()`.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  #Error_code generated:
   *        error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS (add_serialization_segment() was called
   *        an insufficient number of times; in particular 0 times always triggers this);
   *        error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED (add_serialization_segment() was called with
   *        a segment that starts at a misaligned address);
   *        implementation may also emit other errors.
   * @return See above.  If and only if `err_code` is non-null, and truthy `*err_code` is emitted, a useless
   *         (default-cted) `Reader` is returned.
   */
  template<typename Struct>
  typename Struct::Reader deserialization(Error_code* err_code = 0);
}; // class Struct_reader

} // namespace ipc::transport::struc

#endif // IPC_DOXYGEN_ONLY
