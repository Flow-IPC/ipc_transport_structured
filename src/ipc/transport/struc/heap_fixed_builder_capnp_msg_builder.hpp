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

namespace ipc::transport::struc
{

// Types.

/**
 * A `capnp::MessageBuilder` used by Heap_fixed_builder: similar to a `capnp::MallocMessageBuilder`
 * with the FIXED_SIZE alloc-strategy but with framing space around the allocated segment(s).
 *
 * It can also be used as a #Capnp_msg_builder_interface (`capnp::MessageBuilder`) independently of the rest
 * of ipc::transport or even ::ipc, although that was not the impetus for its development.
 *
 * ### Move-ctible and move-assignable ###
 * Super-class #Capnp_msg_builder_interface, which is an abstract class but not a pure interface (it has data
 * and concrete methods), is move-ctible and move-assignable (but not copyable).  Accordingly, the present
 * class is as well.  (So is our peer `capnp::MallocMessageBuilder`.)  However: as of this writing
 * (newest version capnp 0.10) #Capnp_msg_builder_interface consists of a raw data area that = 1 `bool` + 22
 * words; or at least 176 bytes in a 64-bit system.  Its (defaulted) move ctor/assignment will thus need to
 * copy the 176+ bytes.  That is a little "fat."  We add another 3 `size_t`, a pointer, and a `vector` of 1+
 * pointers, or 30+ bytes. Hence the user may wish to wrap us in a `unique_ptr`, and only then use the move
 * semantics; as the trade-off against the extra alloc/dealloc is arguably worth it.  The same arguably holds
 * for any #Capnp_msg_builder_interface sub-class (interface implementation).
 *
 * Style note: Much of the rest of the code explicitly documents deleted copy-ctor/assignment and defaulted
 * move-ctor/assignment (as separate items each with a doc header); we do not.  This is of course for brevity;
 * but we justify it in that the abstract class (interface) we implement (#Capnp_msg_builder_interface) omits them
 * as well.
 *
 * ### Rationale ###
 * Why we'd use a `MallocMessageBuilder`-like approach is explained within Heap_fixed_builder doc header.
 * So why not just use that actual class?  Answer: We send the resulting serialization segments over IPC pipes
 * (Blob_sender, Native_handle_sender), and in some cases struc::Channel needs to surround the serialization
 * with a bit of metadata.  `MallocMessageBuilder` will, for each segment, allocate X bytes -- and then potentially
 * fill all of them.  The only way for struc::Channel to work with this is to allocate another buffer of
 * X + (frame size) bytes and then copy the X bytes into it -- losing the adherence to the zero-copy-when-possible
 * paradigm.  Hence this class which will use `flow::util::Blob` machinery to maintain the frame space.
 *
 * (Update: The previous paragraph remains correct in that in order to support the framing feature it was necessary
 * to write this class, as `MallocMessageBuilder` does not have it.  However struc::Channel no longer needs
 * to frame "a bit of metadata" around the serialization; hence it does not require this framing feature; hence
 * we could now get rid of the present class and just use `MallocMessageBuilder`.
 * Nevertheless I (ygoldfel) have kept this class around, as it is a nice feature in general.  Plus niceties
 * like emit_segment_blobs() and n_segments() remain nice.)
 *
 * Note that in order to make use of the added feature (frame space) one should *not* access the serialization result
 * via the usual `MessageBuilder::getSegmentsForOutput()`.  That still works, and that method is essential in
 * the impl of this class; but it returns only the actual serialization segments (each one's start and size,
 * not including for example the allocated-but-unused data probably at the tail of the allocated buffer).
 * Technically it would indeed be correct/safe to simply write frame prefix data by subtracting `frame_prefix_sz`
 * from the segment's start pointer and writing there; and similarly write postfix data immediately past the end of the
 * start ptr + size.  However that is error-prone and inconvenient.  Call emit_segment_blobs() instead: it will return
 * the `flow::util::Blob`s which will have set `start()` and `size()` appropriately (see emit_segment_blobs()
 * doc header).
 */
class Heap_fixed_builder_capnp_message_builder :
  public Capnp_msg_builder_interface
{
public:
  // Types.

  // Constructors/destructor.

  /**
   * Constructs the message-builder, with the policy to allocate the segments in a particular way, always
   * with a fixed size.  Namely, capnp will call allocateSegment() with `min_sz * sizeof(word) == M`.  This will:
   *   - Create an internal `flow::util::Blob B` with:
   *     - `capacity()` = `max(M, segment_sz) + frame_prefix_sz + frame_postfix_sz`, up to ceiling-nearest word;
   *     - `start()` = `frame_prefix_sz`.
   *     - `size()` = `capacity() - start() - frame_postfix_sz` which equals ~`max(M, segment_sz)`.
   *   - Return `ArrayPtr` pointing at `B.begin()` (which is buffer start + `start()`),
   *     sized `B.size()` (that is, `max(M, segment_sz)` adjust to ceiling-nearest word).
   *
   * Furthermore, this will cause the following emit_segment_blobs() behavior:
   *   - It will emit a sequence of `flow::util::Blob` pointers to the above.
   *   - Each `Blob` pointer will correspond to an earlier allocateSegment() call 1-1.
   *   - The pointee's `start()` will be as above.
   *   - The pointee's `size()` will equal the serialization segment's size as determined ultimately by capnp
   *     (per `getSegmentsForOutput()`).
   *     This will be at most `max(M, segment_sz)` (less if it didn't need every single byte to hold the
   *     serialization ultimately -- it is typical some bytes are wasted).
   *   - Its `capacity() - start() - size() >= frame_postfix_sz`.
   *
   * In this way the consumer of emit_segment_blobs() (presumably struc::Channel) will be able to
   * adjust `start()` and `size()` of each serialization `Blob` to add framing info if needed.
   *
   * @param logger_for_blobs
   *        Passed to each `Blob` ctor.
   * @param segment_sz
   *        See above.
   * @param frame_prefix_sz
   *        Guaranteed space for prefix framing.  See above.
   *        This must be a multiple of `sizeof(void*)`; else behavior undefined (assertion may trip).
   * @param frame_postfix_sz
   *        Guaranteed space for postfix framing.  See above.
   */
  explicit Heap_fixed_builder_capnp_message_builder
             (flow::log::Logger* logger_for_blobs, size_t segment_sz, size_t frame_prefix_sz, size_t frame_postfix_sz);

  // Methods.

  /**
   * Returns pointers to `Blob`s which are the serialization segments at this time.  They are as follows:
   *   - Each `Blob` holds a serialization segment, in order.  There shall be at least 1.
   *   - For each `Blob B` (see ctor):
   *     - `start() == frame_prefix_sz`.
   *     - `capacity() - start() - size() >= frame_postfix_sz`.
   *     - [`begin()`, `end()`) is the serialization segment.  Recall that `begin()` starts at position `start()`,
   *       while `end()` is at position `start() + size()`.
   *
   * Therefore one can use `B.resize()` and similar and then add frame data without breaking zero-copy.
   *
   * This method can be invoked more than once; however the emitted result is meaningless once any mutation occurs
   * subsequently.
   *
   * @param target_blob_ptrs
   *        It is appended-to.  Recall these are mere pointers (into `*this`-managed heap memory).
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
   * @param min_sz
   *        See `MessageBuilder` API.
   *        The allocated segment will allow for a serialization of at most
   *        `min(min_sz * sizeof(word), segment_sz)` bytes, where `segment_sz` was passed to ctor
   *        (adjusted to the ceiling-nearest word).
   * @return See `MessageBuilder` API.
   *         The ptr and size of the area for capnp to serialize-to.
   *         (As noted in ctor doc header: `frame_prefix_sz` bytes will precede this area, and
   *         `frame_postfix_sz` will succeed it.)
   */
  kj::ArrayPtr<::capnp::word> allocateSegment(unsigned int min_sz) override;

private:
  // Data.

  /// See ctor.
  flow::log::Logger* m_logger_for_blobs;
  /// See ctor.
  const size_t m_segment_sz;
  /// See ctor.
  const size_t m_frame_prefix_sz;
  /// See ctor.
  const size_t m_frame_postfix_sz;

  /**
   * Before emit_segment_blobs(), stores each segment allocated as requested by capnp via allocateSegment() `virtual`
   * API, with `.size()` equal to `.capacity()` and >= `min_sz` (arg to that method); immediately after
   * emit_segment_blobs() each `.size()` is adjusted to indicate the # of bytes actually used in that segment in
   * the final serialization.  In addition the framing requirements (#m_frame_prefix_sz and #m_frame_postfix_sz)
   * are requested.  emit_segment_blobs() yields an equally sized list of pointers to the `.size()`-adjusted per-segment
   * `Blob`s.
   */
  std::vector<boost::movelib::unique_ptr<flow::util::Blob>> m_serialization_segments_plus_frame_space;
}; // class Heap_fixed_builder_capnp_message_builder

} // namespace ipc::transport::struc
