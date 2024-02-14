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
#include "ipc/transport/struc/error.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/error/error.hpp>
#include <boost/move/make_unique.hpp>

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
 * Implements Struct_builder concept by straightforwardly allocating fixed-size segments on-demand in
 * the regular heap and serializing directly inside those segments.  That is, each mutation via payload_msg_builder()
 * may, as needed, trigger a `malloc()` (or similar) of size N passed to the ctor of Heap_fixed_builder; and
 * each buffer returned by emit_serialization() will be of size N or less.
 *
 * ### Failure mode ###
 * As long as each leaf capnp object set via mutation of payload_msg_builder() can be serialized in N bytes or fewer,
 * emit_serialization() shall succeed.  For fixed-size leaves this is generally not a problem; for reasonable N
 * in the KiB, things such as integers and enumerations won't be a problem.  However for variable-size
 * objects, particularly strings and blobs and lists and the like, it can be easy to exceed N.  In this case,
 * even though internally the problem could be detected at mutation time, it will be reported
 * in the next emit_serialization() call (as mandated by the concept API).  Specifically
 * error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG shall be emitted.  See "Reasonable uses" below for
 * discussion on avoiding this fate.
 *
 * ### Reasonable uses ###
 * This is suitable for transmission via a Blob_sender (or Native_handle_sender) wherein each blob (or meta-blob,
 * respectively) sent has either a hard size limit (meaning a Blob_sender::send_blob() or
 * Native_handle_sender::send_native_handle() call will fail if this limit is exceeded) or an effective limit
 * (meaning transmission performance is poor in the same event).  Indeed the use cases that informed this strategy
 * were:
 *   - Blob_stream_mq_sender::send_blob() shall fail if `blob.size() >= N`, where `N` is the message size limit
 *     passed to its ctor.
 *   - `Native_socket_stream::send_*()` shall fail if
 *     `blob.size() >= sync_io::Native_socket_stream::S_MAX_META_BLOB_LENGTH`.
 *     - Moreover Native_socket_stream informally is documented to feature good performance compared to
 *       the MQ-based streams when not exceeding the "10s-of-KiB range" (quoting its doc header as of this writing).
 *
 * In "Failure mode" above we pointed out an immediate danger if one uses Heap_fixed_builder to naively serialize
 * arbitrary user-supplied schemas.  Simply put, if one mutates payload_msg_builder() in such a
 * way as to introduce a big leaf, such as a large image blob,
 * then emit_serialization()) will emit error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG; and that's the end
 * of that.
 *
 * Even if one avoids this somehow -- let's say by always fragmenting large things into smaller-size leaves
 * (itself undesirable: the point of serialization of structured data is to not have to worry about data representation
 * at the application level) -- this can lead to, well, a fragmented representation, inefficiently using each N-sized
 * segment with too much unused space.  So what to do?
 *
 * One could write a builder similar to Heap_fixed_builder minus the `_fixed` part; certainly capnp
 * supplies the tools needed to simply allocate large-enough segments (internally, a `capnp::MallocMessageBuilder`-like
 * `MessageBuilder` is used already; we'd just need to use different
 * knob values on `MallocMessageBuilder` or similar and be less draconian in what segment sizes
 * emit_serialization()) will allow).  Of course then the transport mechanism used for the resulting segments would need
 * to be able to handle larger blobs.  Blob_stream_mq_sender already supports larger max message sizes, and
 * Native_socket_stream with a larger or variable `S_MAX_META_BLOB_LENGTH` could be written.  Or even without
 * that one could split any too-large segments across multiple blobs.  However that's
 * hardly consolation in practice: too much RAM use could result; and/or the copying involved would eventually
 * prove too expensive.  So... probably the non-`_fixed`/multi-blob-segment approaches are not good enough,
 * at least not in general.  So what to do?
 *
 * At least one approach is to use a 2-layer approach.  The true serialization of the user's schema could
 * be placed in SHared Memory (SHM).  Then only a handle (or handles) to this SHM-stored serialization could
 * be transmitted via Heap_fixed_builder; even a *single* segment of small size would be enough to transmit
 * as much, as each handle is (more or less) a pointer.  Although the SHM-based builder (documented elsewhere)
 * would be the one used at the higher level, internally it would leverage Heap_fixed_builder to transmit the
 * handle(s) to SHM.
 *
 * Hence Heap_fixed_builder remains of use; but keep in mind the above limitations and (at least its original)
 * intended use.
 *
 * The above discussion is a more specific treatment of the outer-serialization/inner-serialization discussion
 * in the Struct_builder concept doc header.
 *
 * @internal
 * ### Implementation ###
 * If one is familiar with capnp, then one can probably quickly guess how this class template accomplishes
 * what it promises in just the brief summary at the top of this doc header:
 * Heap_fixed_builder_capnp_message_builder is what we use internally; it's like `MallocMessageBuilder`
 * but (1) mandates the equivalent of `AllocationStrategy::FIXED_SIZE` always and (2) has support
 * for a framing prefix and postfix coexisting within each allocated buffer per segment
 * (per our ctor args).  As it must, that builder (like `MallocMessageBuilder`) is documented
 * to allocate a larger-than-N segment if necessary to encode a particular too-large leaf.
 * We do not stop this in and of itself; but emit_serialization() simply refuses to return this resulting serialization,
 * if it detects than indeed such a segment had to be allocated during preceding payload_msg_builder() mutations.
 * So `*this` will allocate and eat the needed RAM; but it simply will refuse to return it `public`ly.
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
   * Implements Struct_builder::Config sub-concept.  In this impl: The data members control Heap_fixed_builder's
   * behavior as follows:
   *
   * Heap_fixed_builder ctor configured by Config creates builder that shall heap-allocate segments of the given size on
   * demand subsequently.  A successful Heap_fixed_builder::emit_serialization() call shall return individual
   * segment sizes never exceeding #m_segment_sz (for the [`begin()`, `end()`) area), with
   * `start() == #m_frame_prefix_sz`, and with `capacity() - start() - size() >= #m_frame_postfix_sz`.
   *
   * However, internally, larger segments may still be allocated, namely if a Heap_fixed_builder::payload_msg_builder()
   * mutation supplies a too-large leaf -- but, if this occurs, Heap_fixed_builder::emit_serialization() shall
   * emit an error and no successful serialization.  That said be aware you'll still be charged for the resulting
   * RAM use in the meantime.  Informal recommendations:
   *   - Do not use Heap_fixed_builder if this is a danger given the schema in question.
   *     See Heap_fixed_builder doc header for discussion of alternative approaches.
   *   - If the problem does occur (Heap_fixed_builder::emit_serialization() does emit the error) nevertheless,
   *     it is pointless to keep using `*this`; destroy it and take the appropriate exceptional-error-contingency steps.
   */
  struct Config
  {
    // Types.

    /// Implements concept API.
    using Builder = Heap_fixed_builder;

    // Data.

    /// Logger to use for logging subsequently.
    flow::log::Logger* m_logger_ptr;
    /// See `struct` doc header.
    size_t m_segment_sz;
    /// See `struct` doc header.
    size_t m_frame_prefix_sz;
    /// See `struct` doc header.
    size_t m_frame_postfix_sz;
  }; // class Config

  /**
   * Implements concept API.  This being a non-zero-copy Struct_builder, no information is needed for
   * emit_serialization() beyond the payload itself.
   */
  using Session = Null_session;

  // Constructors/destructor.

  /**
   * Implements concept API.
   * @see Struct_builder::Struct_builder(): implemented concept.
   */
  Heap_fixed_builder();

  /**
   * Implements concept API.  See Config doc header for information on how `*this` behavior is controlled by `config`.
   *
   * @param config
   *        See above.
   *
   * @see Struct_builder::Struct_builder(): implemented concept.
   */
  explicit Heap_fixed_builder(const Config& config);

  /// Disallow copy construction.
  Heap_fixed_builder(const Heap_fixed_builder&) = delete;

  /**
   * Implements concept API.
   *
   * @param src
   *        See above.
   *
   * @see Struct_builder::Struct_builder(): implemented concept.
   */
  Heap_fixed_builder(Heap_fixed_builder&& src);

  /**
   * Implements concept API.  In this impl: frees all segments allocated on-demand so far.
   * @see Struct_builder::~Struct_builder(): implemented concept.
   */
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
   *
   * @see Struct_builder::Struct_builder(): implemented concept.
   */
  Heap_fixed_builder& operator=(Heap_fixed_builder&& src);

  /**
   * Implements concept API.
   *
   * @return See above.
   *
   * @see Struct_builder::payload_msg_builder(): implemented concept.
   */
  Capnp_msg_builder_interface* payload_msg_builder();

  /**
   * Implements concept API.
   *
   * ### Errors ###
   * As discussed in class doc header, you should be on guard for the particular error
   * error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG.  See class doc header for discussion on how to avoid
   * it.  As of this writing no other failure modes exist.
   *
   * @param target_blobs
   *        See above.  Also recall (see ctor) that for each returned `blob`:
   *        individual segment sizes shall never exceed
   *        Config::m_segment_sz` (for the [`begin()`, `end()`) area), with `start() == m_frame_prefix_sz`, and
   *        with `capacity() - start() - size() >= m_frame_postfix_sz`.
   * @param session
   *        See above.  In this case the proper value is `NULL_SESSION`.
   * @param err_code
   *        See above.  #Error_code generated: error::Code::S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG (a previous
   *        mutation introduced a leaf whose serialization would necessitate allocating a segment exceeding
   *        Config::m_segment_sz -- see ctor -- in size).
   *
   * @see Struct_builder::emit_serialization(): implemented concept.
   */
  void emit_serialization(Segment_ptrs* target_blobs, const Session& session, Error_code* err_code = 0) const;

  /**
   * Implements concept API.
   *
   * @return See above.
   *
   * @see Struct_builder::n_serialization_segments(): implemented concept.
   */
  size_t n_serialization_segments() const;

private:
  // Types.

  /// Alias for the capnp `MallocMessageBuilder`-like serialization engine of ours.
  using Capnp_heap_engine = Heap_fixed_builder_capnp_message_builder;

  // Data.

  /// See ctor.  We only store it to be able to emit `S_INTERNAL_ERROR_SERIALIZE_LEAF_TOO_BIG` in emit_serialization().
  size_t m_segment_sz;

  /**
   * The capnp builder engine which really does the work including owning the needed allocated segments so far.
   * payload_msg_builder() simply returns (up-cast of) `&m_engine`.
   *
   * ### Why the `unique_ptr` wrapper? ###
   * This is subtler than it appears: We need to be move-ctible and move-assignable; and #m_engine therefore
   * does too -- and in fact #Capnp_heap_engine *is*.  So the `unique_ptr` wrapping appears superfluous and a
   * waste of compute and space at least.  However, see the note on move-ctibility/assignability in
   * Heap_fixed_builder_capnp_message_builder doc header: It *is* those things, because super-class
   * (interface) #Capnp_msg_builder_interface is, but any #Capnp_msg_builder_interface sub-class's move (if available)
   * has to copy ~200 bytes at least.  Whether the extra alloc/dealloc is better than a copy of that length is a
   * bit questionable, but we suspect it is at least somewhat better.
   *
   * As a side benefit: it's an easy way to detect a moved-from `*this` and `assert()` in some situations.
   */
  boost::movelib::unique_ptr<Capnp_heap_engine> m_engine;
}; // class Heap_fixed_builder

/**
 * Implements Struct_reader concept by straightforwardly interpreting a serialization by Heap_fixed_builder
 * or any other builder that produces segments directly readable via `SegmentArrayMessageReader`.
 *
 * While written originally specificaly as the counterpart to Heap_fixed_builder, it would work with any
 * builder that produces an equivalent serialization modulo using potentially different segment sizes.
 * That is why it is called `Heap_reader` -- not `Heap_reader`.
 *
 * The most important thing to recognize about Heap_reader: add_serialization_segment() -- which is required
 * (by the concept) to acquire and return memory segments of the size requested by the user -- shall allocate
 * a new `Blob` of the specified size in regular heap (via `new[]`, that is).
 *
 * ### Possible futher impls that would iterate upon Heap_reader ###
 * See Struct_reader "Allocation and perf" doc header section.  It discusses, indirectly, ideas for
 * other reader impls that would be compatible with Heap_fixed_builder (and equivalents) but that
 * would obtain from sources other than `new`ing in regular heap.  E.g.: fixed reused `Blob`; pool allocation.
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
  }; // class Config

  // Constructors/destructor.

  /**
   * Implements concept API.  See Config doc header for information on how `*this` behavior is controlled by `config`.
   *
   * @param config
   *        See above.
   *
   * @see Struct_reader::Struct_reader(): implemented concept.
   */
  explicit Heap_reader(const Config& config);

  /**
   * Implements concept API.  In this impl: frees all segments allocated via add_serialization_segment() so far.
   * @see Struct_reader::~Struct_reader(): implemented concept.
   */
  ~Heap_reader();

  // Methods.

  /**
   * Implements concept API.  Reminder: you must `.resize()` the returned `Blob` in-place to indicate the
   * size of the actual segment (and possibly omitting prefix or postfix frame data), before attempting
   * deserialization().
   *
   * @param max_sz
   *        See above.
   * @return See above.  Note: This impl never returns null.  However any code relying on the concept
   *         Struct_reader, and not this specific impl (Heap_reader), should naturally refrain
   *         from relying on this fact.  Struct_reader::add_serialization_segment() may return null.
   *
   * @see Struct_reader::add_serialization_segment(): implemented concept.
   */
  flow::util::Blob* add_serialization_segment(size_t max_sz);

  /**
   * Implements concept API.
   *
   * @tparam Struct
   *         See above.
   * @param err_code
   *        See above.  #Error_code generated:
   *        error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS (add_serialization_segment() was never called),
   *        error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED (add_serialization_segment()-returned segment
   *        was modified subsequently to start at a misaligned address).
   * @return See above.
   *
   * @see Struct_reader::deserialization(): implemented concept.
   */
  template<typename Struct>
  typename Struct::Reader deserialization(Error_code* err_code = 0);

private:
  // Types.

  /// Alias for the capnp `SegmentArrayMessageReader` deserialization engine.
  using Capnp_heap_engine = ::capnp::SegmentArrayMessageReader;

  /// Alias to pointer to #Capnp_heap_engine.
  using Capnp_heap_engine_ptr = boost::movelib::unique_ptr<Capnp_heap_engine>;

  /**
   * Alias to capnp type that's similar to `boost::asio::const_buffer` but stores `word*` and `word` count
   * instead of `uint8_t*` and byte count.
   */
  using Capnp_word_array_ptr = kj::ArrayPtr<const ::capnp::word>;

  // Data.

  /**
   * The actual serialization supplied so far, as divided up into segments (`Blob`s -- byte arrays).
   * Before the first add_serialization_segment(): empty.
   * After that but before deserialization(): Each `Blob`, except possibly the last, is finalized.
   * At entry to deserialization() or later: Each `Blob` is finalized.
   */
  std::vector<boost::movelib::unique_ptr<flow::util::Blob>> m_serialization_segments;

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
template<typename Channel_obj, typename Message_body>
using Channel_via_heap
  = Channel<Channel_obj, Message_body,
            Heap_fixed_builder::Config, Heap_reader::Config>;

// Free functions: in *_fwd.hpp.

// Template implementations.

template<typename Struct>
typename Struct::Reader Heap_reader::deserialization(Error_code* err_code)
{
  using flow::error::Runtime_error;
  using flow::util::buffers_dump_string;
  using boost::movelib::make_unique;
  using std::vector;
  using ::capnp::word;
  using Capnp_word_array_array_ptr = kj::ArrayPtr<const Capnp_word_array_ptr>;
  using Capnp_struct_reader = typename Struct::Reader;

  // Helper to emit error via usual semantics.  We return a ref so can't use FLOW_ERROR_EXEC_AND_THROW_ON_ERROR().
  const auto emit_error = [&](const Error_code& our_err_code) -> Capnp_struct_reader
  {
    if (err_code)
    {
      *err_code = our_err_code;
      return Capnp_struct_reader();
    }
    // else
    throw Runtime_error(our_err_code, "Heap_reader::deserialization()");
    return Capnp_struct_reader(); // Doesn't get here.
  };

  if (m_serialization_segments.empty())
  {
    return emit_error(error::Code::S_DESERIALIZE_FAILED_INSUFFICIENT_SEGMENTS);
  }
  // else: OK so far.

  assert((!m_engine) && "Do not call deserialization() more than once.");

  // Set up capnp ArrayPtr<ArrayPtr>.  ArrayPtr is a ptr (to word) + size (in `word`s).  The backing vector:
  auto& capnp_segs = m_capnp_segments;
  assert(m_capnp_segments.empty() && "m_capnp_segments should have been empty so far, pre-deserialization().");
  capnp_segs.reserve(m_serialization_segments.size());

  size_t idx = 0;
  for (const auto& serialization_segment : m_serialization_segments)
  {
    const auto data_ptr = serialization_segment->const_data();
    const auto seg_size = serialization_segment->size();

    if ((uintptr_t(data_ptr) % sizeof(void*)) != 0)
    {
      FLOW_LOG_WARNING("Heap_reader [" << *this << "]: "
                       "Serialization segment [" << idx << "] "
                       "(0 based, of [" << m_serialization_segments.size() << "], 1-based): "
                       "Heap buffer @[" << static_cast<const void*>(data_ptr) << "] sized [" << seg_size << "]: "
                       "Starting pointer is not this-architecture-word-aligned.  Bug?  Misuse of Heap_reader?  "
                       "Misalignment is against the API use requirements; capnp would complain and fail.");
      return emit_error(error::Code::S_DESERIALIZE_FAILED_SEGMENT_MISALIGNED);
    }
    // else

    FLOW_LOG_TRACE("Heap_reader [" << *this << "]: "
                   "Serialization segment [" << idx << "] "
                   "(0 based, of [" << m_serialization_segments.size() << "], 1-based): "
                   "Heap buffer @[" << static_cast<const void*>(data_ptr) << "] sized [" << seg_size << "]: "
                   "Feeding into capnp deserialization engine.");
    FLOW_LOG_DATA("Segment contents: "
                  "[\n" << buffers_dump_string(serialization_segment->const_buffer(), "  ") << "].");

    capnp_segs.emplace_back(reinterpret_cast<const word*>(data_ptr), // uint8_t* -> word* = OK given C++ aliasing rules.
                            seg_size / sizeof(word)); // @todo Maybe also check that seg_size is a multiple?  assert()?

    ++idx;
  } // for (const auto& serialization_segment : m_serialization_segments)

  /* Initialize the deserialization engine by giving it the pointers to/size of the backing segment blobs.
   * It doesn't copy this array of pointers/sizes, so that array must stay alive, hence why capnp_segs is
   * really m_capnp_segments.  To be clear: not only must the blobs stay alive but so must the array referring
   * to them. */
  const Capnp_word_array_array_ptr capnp_segs_ptr(&(capnp_segs.front()), capnp_segs.size());
  m_engine = make_unique<Capnp_heap_engine>(capnp_segs_ptr);

  if (err_code)
  {
    err_code->clear();
  }

  // And lastly set up the structured-accessor API object that'll traverse those blobs via that engine.
  return m_engine->getRoot<Struct>();
} // Heap_reader::deserialization()

} // namespace ipc::transport::struc
