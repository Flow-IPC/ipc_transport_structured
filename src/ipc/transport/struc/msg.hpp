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

#include "ipc/transport/transport_fwd.hpp"
#include "ipc/transport/struc/schema/detail/structured_msg.capnp.h"
#include "ipc/transport/struc/error.hpp"
#include "ipc/transport/struc/heap_serializer.hpp"
#include "ipc/transport/struc/struc_fwd.hpp"
#include "ipc/transport/struc/detail/struc_fwd.hpp"
#include "ipc/transport/struc/sync_io/channel_stats.hpp"
#include "ipc/transport/asio_local_stream_socket_fwd.hpp"
#include "ipc/util/native_handle.hpp"
#include "ipc/util/util_fwd.hpp"
#include <flow/error/error.hpp>
#include <boost/endian.hpp>
#include <boost/move/make_unique.hpp>
#include <vector>

namespace ipc::transport::struc
{

// Types.

/**
 * Simple utility: a capnp `MessageBuilder` operating over a single, contiguous, user-supplied area in virtual memory.
 *
 * If subsequent mutators calls are such that this area is insufficient in size, a `flow::error::Runtime_error`,
 * with error::Code::S_INVALID_ARGUMENT, is thrown.  To avoid this use a `*this` only when it is absolutely guaranteed
 * that the provided area is big enough.  (This can be quite easy with simple schemas and/or trusted data sources.)
 *
 * This is ~equal to capnp's own `MallocMessageBuilder`, when one chooses to supply a pre-existing
 * buffer for the first segment and then never needs to add more segments beyond that one.  A `*this` is just
 * leaner and easier to use for this specific purpose.
 *
 * ### Rationale ###
 * We use it internally in Flow-IPC (at least in Msg_out impl); but it is simple and useful and thus is supplied as
 * a public API to the user also.  (There is no expectation, or lack thereof, that the typical Flow-IPC user will
 * need it.)
 */
class Capped_sz_capnp_message_builder :
  public Capnp_msg_builder_interface
{
public:
  // Constructors/destructor.

  /**
   * Constructor that remembers the given memory area.  It can optionally zero that area.
   *
   * The area must remain valid until the last time `*this` is accessed (this excludes `*this` destruction which
   * does not access the area; but it includes touching the serialization through any capnp mutators).
   *
   * @note If you are able to guarantee that the `seg`-described area is already filled with zeroes, it may bring
   *       significant perf gains to specify `zero_it_please = false`.
   * @note If you are *not* able to guarantee it, you *must* specify the converse.  Otherwise intermitted undefined
   *       behaviors *will* happen... and they'll be hard to trace back to this bug.
   *
   * @param seg
   *        The area where to build message subsequently.
   * @param zero_it_please
   *        If and only if `true` we shall presently fill `seg`-described area with zeroes.
   */
  explicit Capped_sz_capnp_message_builder(const util::Blob_mutable& seg, bool zero_it_please);

  // Methods.

  /**
   * Implements `MessageBuilder` API.  Invoked by capnp, as the user mutates via `Builder`s.  Do not invoke directly.
   *
   * If called a 2nd (or 3rd, 4th, ...) time, throws `flow::error::Runtime_error` with error::Code::S_INVALID_ARGUMENT.
   * This occurs if and only if the `seg` given to ctor describes an area insufficiently large to support the
   * mutator calls invoked on `*this`.  A mutator internally determines another segment is necessary, calls
   * `allocateSegment()`, and we throw.
   *
   * @note The strange capitalization (that goes against standard Flow-IPC style) is because we are implementing
   *       a capnp API.
   *
   * @param min_sz_words
   *        See `MessageBuilder` API.
   * @return See `MessageBuilder` API.
   *         The ptr and size of the area for capnp to serialize-to... namely same area as `seg` given to ctor.
   */
  kj::ArrayPtr<::capnp::word> allocateSegment(unsigned int min_sz_words) override;

private:
  // Data.

  /// Copy of `seg` (the buffer description... not the actual buffer!) given to ctor.
  util::Blob_mutable m_seg;

  /// `true` if and only if allocateSegment() has been called.
  bool m_returned_seg;
}; // class Capped_sz_capnp_message_builder

/**
 * Simple utility: a capnp `MessageReader` operating over a single, contiguous, user-supplied area in virtual memory.
 * It is a (public sub-class of) `capnp::SegmentArrayMessageReader`.
 *
 * This is ~equal to capnp's own `SegmentArrayMessageReader`, when one chooses to supply a single segment to it.
 * A `*this` is just easier to use for this specific purpose, providing a simpler (and arguably more Flow-y)
 * ctor.
 *
 * ### Rationale ###
 * Same as for Capped_sz_capnp_message_builder.
 *
 * @internal
 * ### Impl ###
 * It's obviously quite short.  Still there is a technicality that might be non-obvious and is really a result
 * of C++ quirks:  Our super-class/work-horse `SegmentArrayMessageReader` takes a pointer and size of an array
 * of pointer/size pairs, and that array must outlive said `MessageReader`.  (In our case this array is not an array
 * but simply 1 pointer/size pair... or an array of 1 such element, if you will.)  But super-objects are initialized
 * before any would-be data members of ours, creating an ordering problem.  To get around it we use a `private` base
 * instead of a data-member and list it before the `public SegmentArrayMessageReader` base.  Hence the somewhat
 * odd-looking (but perfectly reasonable) `private` base thing.
 */
class Capped_sz_capnp_message_reader :
  private kj::ArrayPtr<const ::capnp::word>,
  public ::capnp::SegmentArrayMessageReader // Hence we also implement `::capnp::MessageReader` interface.
{
public:
  // Constructors/destructor.

  /**
   * Constructs `MessageReader` based on an existing contiguous memory area; is equivalent to the simple
   * `SegmentArrayMessageReader` ctor taking a 1-array.
   *
   * @param seg
   *        The memory area with the serialization.
   */
  explicit Capped_sz_capnp_message_reader(const util::Blob_const& seg);

private:
  // Types.

  /// Convenience alias for capnp equivalent of util::Blob_const but in `word`s as opposed to bytes.
  using Word_array_ptr_base = kj::ArrayPtr<const ::capnp::word>;

  /// Convenient alias for capnp `SegmentArrayMessageReader`.
  using Seg_array_msg_reader_base = ::capnp::SegmentArrayMessageReader;
}; // class Capped_sz_capnp_message_reader

/**
 * A structured out-message suitable to be sent via struc::Channel::send() (et al).  Publicly this can be constructed
 * directly; or via struc::Channel::create_msg().  The latter has the advantage of reusing the serialization
 * engine knobs registered with the channel and hence is simply easier to write.  If one uses the direct-construction
 * route, the builder (serialization) engine type -- Struct_builder #Builder -- must match that of the
 * struc::Channel used to `send()` a `*this`; otherwise it simply will not compile.
 *
 * @see "Lifecycle of an out-message" section of struc::Channel class doc header for useful context.
 *
 * As explained in that doc section, a Msg_out (struc::Channel::Msg_out) is akin to a container,
 * like `vector<uint8_t>`; and the #Builder template param + ctor arg are akin to the allocator
 * used by such a container.  A given `*this` represents the message payload; *and* separately/independently
 * it also represents a particular `send()`t (past/present/future) *instance* of that payload.
 *   - The message payload (as accessed via body_root() and subsequent capnp mutators/builders) is relevant
 *     throughout a `*this`'s life.  This is not unlike the data in a `vector`.
 *     - Modifying it is akin to modifying the `vector<uint8_t>`, albeit in structured-schema fashion -- available API
 *       controlled by `Body_t` template param -- and with an optionally attached #Native_handle.
 *   - The (internally-only accessed/accessible) metadata payload, required for performant operation of
 *     struc::Channel and struc::sync_io::Channel, is relevant only *during* the execution of
 *     sync_io::Channel::send() and sync_io::Channel::async_request() (and therefore wrappers
 *     struc::Channel::send(), struc::Channel::async_request(), and struc::Channel::sync_request()).
 *     - This internal metadata-payload is small (as of this writing it is 128 bytes).
 *   - This bifurcation is necessary for performance, internally, and typically it should not matter to the user of
 *     a `*this`.  The way in which it *does* matter is with regards to `const`ness of its APIs and therefore
 *     thread safety.  To wit observe that the aforementioned `Channel` send-y methods take a pointer to *mutable*
 *     Msg_out: not a ref-to-`const`; that is because such send-y methods must modify the metadata-payload of `*this`.
 *
 * As explained in that doc section, like any container, once another process has access to it -- which is
 * only possible in read-only fashion as of this writing -- modifying it must be done with concurrency/synchronization
 * considered.  If the compile-time-chosen #Builder is SHM-based, then this is a concern.
 * If it is heap-based (notably Heap_fixed_builder), then it is not: the serialization is *copied* into
 * and out of the transport.
 *
 * @note The (internally-only-used) metadata-payload section of a `*this` is as of this writing always copied
 *       when transmitted (and is small), so no concurrency/synchronization worries apply.
 *
 * After construction, one mutates the message via body_root() and/or store_native_handle_or_null().
 * You may also use orphanage() to build the message in bottom-up fashion, eventually grafting pieces in via
 * `body_root()->....adopt...()`.  See orphanage() doc header for more information on this technique.
 * Lastly consider the following section for another, more holistic bottom-up appraoch (or perhaps
 * taking the orphanage technique further).
 *
 * ### Advanced technique: Construction from earlier-prepared raw #Capnp_msg_builder_interface ###
 * To enable the straightforward operation implied above, one uses the 1st/simple ctor form which
 * by definition necessitates specifying the root schema, #Body (as to contruct one must know the type).
 * This is typically perfectly natural: a module knows it's building a message of schema #Body, so they
 * construct a `Msg_out<Body>` (which already auto-creates a blank #Body via
 * `initRoot<Body>()`), then fills it out via body_root() (which points to what `initRoot()` returned).
 * The orphanage technique above is a slight complication, in that things can be built bottom-up, but in any case
 * by then the root schema already had to be specified, even if it's actually mutated only somewhat later, once
 * the component orphan(s) can be plugged in.
 *
 * When working with capnp outside of ::ipc, users typically do basically the same thing:
 * start with `M.initRoot<Body>()`, then set stuff in its tree; or in more complex cases maybe use
 * `M.getOrphanage()` from the same `MessageBuilder M` to help build bottom-up.
 *
 * However consider a complex schema `Huge_body` built by collaboration between modules A and B.
 * Now module A may be aware the ultimate product is rooted at
 * `Huge_body`, while module B has no idea, and yet they work together to build parts of the final
 * thing.  In that case B does not necessarily think of any particular overall `Huge_body` tree but only the
 * sub-schema types it cares about; so to it the `MessageBuilder M` is just a memory pool or heap of sorts;
 * it makes whatever structures it wants (via `M.getOrphanage()`) from it but does
 * not call or know about `initRoot<Huge_body>()`.  Conceivably, even, module B is first to run its
 * procedures (before module A) and to create `M` in the first place; and only later module A perhaps
 * finally sets the root (`initRoot<HugeBody>()`) and attaches whatever orphan values module B produces,
 * hooking them into the `HugeBody`.
 *
 * The straightforward use of Msg_out, as described before, does in fact force one (from the start) to
 * specify `HugeBody` by definition (it's -- again -- a template argument to the class template), and that may not
 * be desirable in this module A-B scenario.  So to support it: use the 2nd/advanced ctor form which
 * takes over (via `std::move()`) an already-created #Builder -- not templated on any schema `Body`.
 * The #Builder exposes the direct `MessageBuilder`, so the tree can be built using whatever features in whatever
 * order one desires... and only *then* create the Msg_out.  The only constraint is that
 * by ctor time, the `MessageBuilder` (a/k/a #Capnp_msg_builder_interface) *is* initialized to be rooted at `Body`;
 * but this could be done anytime up to that point; so module B need not do it, while module A might ultimately
 * do it and then construct the `Msg_out<Body>`.  Alternatively, the user can omit performing
 * `initRoot<Body>()`, and it will be done as needed by the ctor, resulting in a blank message at that point
 * which then can be filled-out via body_root() mutations.  (Note: This is just an example.  The point is
 * with this technique one can use a `MessageBuilder` in any fashion desired, and the only constraint is
 * it is in fact rooted at some structure ``S`, at which point `Msg_out<S>` can be cted.  In that
 * sense it removes any -- albeit typically simplifying -- ipc::transport constraints on how one builds a message,
 * allowing one to use the full builder API provided by capnp without any stylistic concessions.)
 *
 * ### Resource use: RAM ###
 * Creating a `*this`, and mutating it via body_root(), uses proportionally large RAM resources; e.g.,
 * if you successfully store a multi-megabyte blob, `*this` will allocate RAM accordingly.  Conversely,
 * destroying `*this` returns such resources.  However, if #Builder is SHM-based, then a process-counting
 * ref-count (conceptually) exists.  It is 1 after construction.  It is incremented each time
 * it is `struc::Channel::send()`t.  For each such increment, there is a decrement when the receiving
 * process's counterpart Msg_in, which is always passed-around by `shared_ptr`, is no longer
 * owned by that process (when that `shared_ptr` group's ref-count drops to zero).  Once `*this` is destroyed,
 * *and* each `send()` target process has both yielded the struc::Channel::Msg_in_ptr and let that
 * `shared_ptr` drop to ref-count-zero, the backing RAM is released.
 *
 * ### Resource use: the #Native_handle (if any) ###
 * It is returned to the OS by our destructor.  This does not, necessarily, destroy whatever resource it references:
 * presumably the idea is to send it, via IPC, to the opposing process.  The handle (FD) that process receives
 * is really a new one in that process's table; there are at that point two *descriptors* referencing one
 * resource *description*, and the latter disappears in the kernel only once both descriptors go away.
 * If you do not wish to let the local handle (FD) be destroyed by `*this` destructor, consider (in Unix-type OS)
 * passing-in `dup(your_hndl)` instead of `your_hndl` itself.
 *
 * The #Native_handle, if any, stored via store_native_handle_or_null() is not a part of the
 * serialization/deserialization/sharing schema described above.  It just sits in `*this` until `*this` dtor runs
 * (or it is replaced/unloaded via store_native_handle_or_null()).  If at send() time there's one in `*this`,
 * then a copy is transmitted to the recipient process.  store_native_handle_or_null() on `*this` will never
 * have any direct effect on any received copy.
 *
 * @internal
 * Impl notes
 * ----------
 * ### Metadata sharing message payload's segment 1 ###
 * First the user constructs a `*this` and modifies the message payload; this is all essentially vanilla.
 * Then the user transmits a `*this` via e.g. sync_io::Channel::send() which takes a pointer to mutable
 * `*this`.  (Why mutable, if it's only sending?  The answer is above when explaining internally-used metadata.)
 * The user can then continue serially doing either thing an indefinite number of times.  However:
 *
 * Consider the aforementioned `Channel send()` (et al).  Conceptually what it does is still straightforward:
 * fills out certain metadata, for example a message ID, inside `*this`; then IPC-transmits the metadata
 * (which is small) and then some representation of the message payload.  Why not just deal with the metadata
 * separately?  It could after all "just" keep the metadata in a totally separate structure and even take
 * a `const Msg_out` after all.  (Indeed an earlier version of Flow-IPC did just that.)  Answer: it is because of
 * performance.  This is easiest explained with the simplest possible situation: a small message, backed by
 * a Heap_fixed_builder, requiring only 1 segment -- let's say sized 4Ki bytes.  Plus we internally must send
 * metadata (as of this writing fitting into 128 bytes) also.  The easy/elegant thing to do is make an OS-send
 * syscall for the mdt and then another for the payload.  However this is deceptively expensive at high loads:
 * each syscall entry/exit is relatively expensive; plus (details omitted) the same is ~mirrored on the receive
 * end; plus (and this can dward the syscall aspect) it is 2x the amount of allocating.
 * We've empirically shown that it is much better to "embed" the mdt as (e.g.) a header into the single memory
 * area containing the message-payload segment 1 (which is often the only segment, period).  So that is what we do.
 *
 * Any #Builder, to be compatible with this system, for this reason *must* supply a sufficiently large
 * frame-prefix area in its segment 1; this will hold the mdt header Flow-IPC internals load there.
 *
 * In short: a `Channel` send-y function (or any equivalent) must
 * call emit_serialization() to fill-out the mdt (based partially on items like the msg ID passed to that method)
 * and obtain the final list (often 1-long) of segments (buffers in memory)
 * to IPC-transmit.  Segment 1 of these will transparently contain the mdt header in a way that
 * Msg_in internals will properly decode.
 *
 * ### Facade design ###
 * You may have noted that emit_serialization() is a back-end operation: sync_io::Channel (or equivalent) needs
 * access to it, but the user need not even know of its existence.
 *
 * Indeed this API is `private`.  The detail/ `friend`-facade type Msg_out_impl exposes it publicly (but by
 * convention user has no access to detail/ types).
 *
 * Incidentally emit_serialization() is what sets the metadata aspects of the serialization; so it is not `const`.
 * It is what sync_io::Channel::send() calls that requires that guy (and similar) to take `Msg_out*` and not
 * `const Msg_out&`.
 * @endinternal
 *
 * @tparam Body_t
 *         See struc::Channel.
 * @tparam Builder_t
 *         See struc::Channel.
 */
template<typename Body_t, typename Builder_t>
class Msg_out // Reminder: It is movable but not copyable.
{
public:
  // Types.

  /// See struc::Channel::Msg_body.
  using Body = Body_t;

  /// Short-hand for capnp-generated mutating `Builder` nested class of #Body.  See body_root().
  using Body_builder = typename Body::Builder;

  /**
   * Short-hand for user-specified Struct_builder type.  An internally stored instance of this contains the user
   * payload.
   */
  using Builder = Builder_t;

  /**
   * Short-hand for user-specified Struct_builder::Config.  One can construct a #Builder via
   * `Builder(const Builder_config&)`.
   */
  using Builder_config = typename Builder::Config;

  /// Short-hand for capnp orphan factory.  See orphanage().
  using Orphanage = ::capnp::Orphanage;

  // Constructors/destructor.

  /**
   * Creates blank #Body-bearing out-message, with no native handle,
   * all of which can be modified via body_root() and the ancillary mutator store_native_handle_or_null().
   *
   * See also the alternate (advanced-technique) ctor form which may be more suitable in more complex scenarios.
   * This is discussed in our class doc header.
   *
   * ### Obtaining the #Builder_config object ###
   * In the first place it is often easier/better to not invoke this ctor directly but rather
   * invoke struc::Channel::create_msg() which will do so for you and supply the proper #Builder_config.
   * However it is definitely possible for a Msg_out to be constructed orthogonally to a
   * particular struc::Channel -- or even to a particular ipc::Session (if even applicable).  How to get
   * the `struct_builder_config` arg for this ctor then?  Informally: if you're constructing the #Builder_config
   * directly, you're probably not doing the right thing.  The following places are available to obtain one
   * for safety and efficiency (and code maintainability):
   *   - From another, compatible, struc::Channel via struc::Channel::struct_builder_config().
   *   - Heap-backed: Via `static` struc::Channel::heap_fixed_builder_config().
   *   - SHM-backed (e.g., SHM-classic):
   *     - session::shm::classic::Session_mv::session_shm_builder_config(),
   *       session::shm::classic::Session_mv::app_shm_builder_config().  Requires a `Session` object.
   *     - session::shm::classic::Session_server::app_shm_builder_config() (requires session::Client_app).
   *       Server-only; if a `Session` is not available or applicable.
   *
   * @param struct_builder_config
   *        See above.
   */
  explicit Msg_out(const Builder_config& struct_builder_config);

  /**
   * Advanced technique: Creates message by subsuming the provided already-prepared
   * `Capnp_msg_builder_interface`-bearing, root-initialized-to-#Body #Builder object (which is moved-to `*this`),
   * with no native handle, all of which can be (further) modified via body_root() and the ancillary
   * mutator store_native_handle_or_null().
   *
   * The advanced technique revolving around the use of this ctor form is discussed in our class doc header.
   * That said:
   *   - `*struct_builder.payload_msg_builder()` -- a #Capnp_msg_builder_interface -- *must* already have
   *     been root-initialized, and the root type *must* be #Body; or *must* not have been root-initialized
   *     at all (in which case the ctor will perform `initRoot<Body>()`).
   *     - If it was already root-initialized, but not with `<Body>`, then behavior is undefined, and the trouble
   *       may only be detected on the opposing side upon receipt of `*this` message, in trying to
   *       deserialize it in Msg_in.
   *     - If you chose to not root-initialize before this ctor, and therefore it performs `initRoot<Body>()`,
   *       then naturally the message root tree shall be blank, similarly to the other/vanilla ctor.
   *     - This is typically accomplished via `struct_builder.payload_msg_builder()->initRoot<Body>()`, although
   *       there are other techniques available.  See capnp docs for `capnp::MessageBuilder`
   *       (#Capnp_msg_builder_interface).
   *   - That having been guaranteed, the message may be further mutated via body_root().
   *     - Naturally it may also have already been mutated (beyond `initRoot()`/etc.), possibly to its final intended
   *       form.
   *     - Conversely, in practice, if you chose to not root-initialize `struct_builder`, you'll want to in fact
   *       use body_root() mutations to actually load the message with something useful.
   *
   * For exposition purposes: note that the other ctor form, which takes a `Builder::Config`, behaves as-if
   * delegating to the present ctor: `Msg_out{Builder{struct_builder_config}}`.
   * When creating a straightforward message in ~one place in the user code, using that other ctor form is
   * usually therefore more convenient, avoiding some boiler-plate.
   *
   * @param struct_builder
   *        See above.  Note the requirements listed.
   */
  explicit Msg_out(Builder&& struct_builder);

  /// Creates non-message; any operation except move-to or destruction results in undefined behavior subsequently.
  Msg_out();

  /**
   * Move ctor: Make `*this` equal to `src`, while `src` becomes as-if default-cted (in that any operation
   * except move-to or destruction results in undefined behavior subsequently).
   *
   * @param src
   *        Source object.
   */
  Msg_out(Msg_out&& src);

  /// Disallow copying.
  Msg_out(const Msg_out&) = delete;

  /**
   * Returns resources, including potentially significant RAM resources and native_handle_or_null(), taken throughout;
   * in the case of SHM-based RAM resources their return may be delayed until counterpart `Msg_in`s
   * are also destroyed.  See class doc header for discussion.
   */
  ~Msg_out();

  // Methods.

  /**
   * Move assignment: Destroy `*this` contents; make `*this` equal to `src`, while `src` becomes as-if
   * default-cted (in that any operation except move-to or destruction results in undefined behavior subsequently).
   * no-op if `&src == this`.
   *
   * @param src
   *        Source object.
   * @return `*this`.
   */
  Msg_out& operator=(Msg_out&& src);

  /// Disallow copying.
  Msg_out& operator=(const Msg_out&) = delete;

  /**
   * The #Body root capnp-generated mutator object.  E.g.: `this->body_root()->initSomeStruct().setSomeField(123)`
   * will create the capnp-`struct`-typed root field `someStruct` and set the value `someField` -- presumably an
   * integer -- within it.  The pointer returned shall always be the same value until `*this` is destroyed.
   *
   * #Body_builder is light-weight, so you may make a copy and then mutate via that, if desired.  However
   * whether `*(body_root())` or such a copy, such a `Builder` object may not be accessed after `*this` is destroyed.
   *
   * Informally we recommend against copying `*(body_root())`: Light-weight or not, it's a copy, and copying a pointer
   * is cheaper; and it may not live past `*this` anyway; and body_root() always returns the same pointer.  So just
   * store the pointer (or reference from it).
   *
   * ### Quick capnp tips ###
   * You should really read all of capnp docs at its web site.  They are very useful and well written and not
   * overly formal despite being quite comprehensive.  That said a couple of gotchas/tips:
   *   - On a `Builder`, `.getX()` and `.setX()` are lightning-fast, like accessing `struct` members directly --
   *     but only when `X` is of a scalar type.  Compound types, where `.getX()` returns not a native type
   *     but another `Builder`, need to perform some pointer checking and are slower.  Therefore,
   *     if you plan to `.getX()` and then `.setA()` and `.setB()` (and similar) on that `X`, you should
   *     save the result (`auto x = ....getX();`); then mutate via the saved result (`x.setA(...); x.setB(...)`).
   *   - Let `X` be a compound field, particularly `List`, `Text` (string/list-of-characters),
   *     `Data` (list-of-bytes).  It is usually, to begin, null; you must perform `.initX(size_t n)` or equivalent
   *     to initialize it (fill it with `n` zeroes).  However, suppose you are mutating a message, such as
   *     a previously-sent message, and wish to *modify* the `X`.  If the new value might have the same length (which is
   *     common), the correct thing to do is: check `.hasX()`; if `true`, modify `.getX()`;
   *     but if `false` then `.initX(n)` (as `X` was null after all).  Performing `.initX()` on an already-`.initX()`ed
   *     value works but has a nasty invisible effect: the existing datum continues taking space in the
   *     serialization; the new datum takes a new chunk of the serialization segments (and might even cause
   *     the allocation of a new segment if needed).  As of this writing capnp does not reuse such orphaned
   *     space.  If the new `n` equals the old `n`, this is a straight waste of RAM; and can lead to pathologically
   *     growing memory leaks if done many times.
   *     - However, if the new `n` is different from the preceding, then there is no choice but to
   *       re-`.initX()`.  A list/blob/string's size cannot be modified in capnp.
   *     - It is best to avoid any situation where the `n` would change; try to design your protocol differently.
   *
   * @return See above.
   */
  Body_builder* body_root();

  /**
   * Equivalent to the other body_root() overload but immutable version.  May be useful for, say, pretty-printing it
   * to log (e.g.: `capnp::prettyPrint(M.body_root()->asReader()).flatten().cStr()`).
   *
   * @return See above.
   */
  const Body_builder* body_root() const;

  /**
   * Convenience method that returns a `capnp::Orphan` factory in the same `capnp::MessageBuilder` as body_root().
   * Therefore `Orphan`s generated via the returned #Orphanage can be `adopt...()`ed by the `Builder` `*(body_root())`
   * and its various sub-fields' `Builder`s (at any depth).
   *
   * ### Suggested technique ###
   * In general, the orphan concept is discussed briefly but usefully in the "C++ serialization" section of
   * capnp docs (the web site).  We do not attempt to get into all these topics.  However consider the following
   * pattern.
   *
   * In general the Flow-IPC system places almost no constraints on #Body (the schema), but there is one:
   * Generally there must be an anonymous capnp-`union` at the top of #Body, and in practice any message will
   * eventually load the root (body_root()) with one of the `union` choices.  (This is to operate within the
   * struc::Channel::expect_msg() and struc::Channel::expect_msgs() APIs.)  The vanilla approach to building
   * the message is top-down: body_root() is pre-initialized to be a #Body; then, say, if your plan is to
   * load it with the `union` choice `someChoice`, then you'd do `body_root()->initSomeChoice()`; then
   * mutate stuff inside the `Builder` returned by that.  There could be an arbitrarily complex sub-tree within that,
   * built top-down in the same fashion.
   *
   * Instead you may choose to build it bottom-up.  For this discussion let's assume `struct SomeChoice`
   * has a number of scalars.  You would first obtain and save an `Orphanage`:
   * `auto orphan_factory = M.orphanage()`.  Next you would create an orphan, not yet connected to the root
   * body_root(), like so:
   * `auto some_choice_orphan = orphan_factory.newOrphan<SomeChoice>(); some_choice_builder = some_choice_orphan.get()`.
   * Next you'd set the scalars within it: `some_choice_builder.set...(...)`.  Lastly, when it's ready, graft it into
   * the actual root: `body_root()->adoptSomeChoice(some_choice_orphan)`.
   *
   * Due to the simplicity of this example one might wonder why not simply build it top-down in the first place.
   * Indeed.  However, if `struct SomeChoice` is a complex tree of `struct`s within `List`s within (...),
   * it would be reasonable to build the lowest levels of that tree by generating intermediate orphans
   * (via `orphan_factory.newOrphan<LowerLevelStructOrListOrWhatever>()`), adopting them into higher-level
   * orphans, and so on, finishing it via the top-level `body_root()->adoptSomeChoice()` call.
   *
   * ### Similar technique ###
   * See also the 2nd (advanced) ctor form.  It allows one to do the above but takes it further by not even
   * needing a Msg_out until the time comes to actually struc::Channel::send() the message.
   * I.e., one would work with essentially a #Capnp_msg_builder_interface directly, including
   * invoking `Capnp_msg_builder_interface.getOrphanage()`.
   *
   * @return See above.
   */
  Orphanage orphanage();

  /**
   * Store the `Native_handle` (potentially `.null()`, meaning none) in this out-message; and, unless the same
   * `.m_native_handle` is already stored, returns the previously stored native handle to the OS (similarly to dtor).
   *
   * Any non-null stored handle shall be returned to the OS by `*this` destructor; if you wish to avoid this then
   * consider (in Unix-type OS) passing-in `dup(your_hndl)` instead of `your_hndl` itself.
   *
   * @param native_handle_or_null
   *        The `Native_handle` (`.null() == true` if none) to move into `*this`.  Made `.null() == true` upon return.
   *        The latter occurs even if `this->native_handle_or_null() == native_handle_or_null` was pre-condition
   *        (meaning `this->native_handle_or_null()` was not returned to OS).
   */
  void store_native_handle_or_null(Native_handle&& native_handle_or_null);

  /**
   * Returns whatever was the last word according to store_native_handle_or_null().  Note it is a copy;
   * `*this` retains ownership of the OS resource.
   *
   * @return See ctor (which stores no handle), store_native_handle_or_null().
   */
  Native_handle native_handle_or_null() const;

  /**
   * Prints string representation to the given `ostream`.  This representation lacks newlines/indentation;
   * includes a (potentially truncated) pretty-printed representation of `body_root()` contents; and includes
   * native_handle_or_null().
   *
   * Caution: This could be an operation expensive in processor cycles and, temporarily, RAM; and thus should
   * be used judiciously.  To help drive your decision-making: This method, internally,
   *   -# uses capnp `kj::str(*(this->body_root))` to generate a *full* pretty-print of body_root() contents;
   *   -# truncates the result of the latter, as needed for a reasonably short output, and prints the result;
   *   -# adds native_handle_or_null() and possibly a few other small information items.
   *
   * Because there is no reasonably-available way to stop generating the pretty-print in step 1 upon reaching
   * a certain number of characters, the operation may take a while, if many non-default bytes have been
   * mutated-in; and before truncation the resulting string may take significant RAM accordingly.
   *
   * @param os
   *        Stream to which to write.
   */
  void to_ostream(std::ostream* os) const;

private:
  // Friends.

  /// Facade type that exposes specific `private` APIs of `*this` to other internal Flow-IPC code.
  template<typename T>
  friend struct Msg_out_impl;

  // Methods.

  /**
   * (Access via Msg_out_impl) Finalizes (for at least IPC-transmission) the serialization of metadata and
   * user-payload in memory and emits the locations/sizes of the (1+) segments in that serialization.
   * It is meant to be invoked at struc::sync_io::Channel::send() time.
   *
   * It can be invoked repeatedly: `session` can specify a different destination each time; or the same
   * destination each time; or an arbitrary mix thereof.  In particular struc::sync_io::Channel::send() (and its
   * derivatives including `async_request()`) shall each time specify the
   * channel's opposing process as the destination -- regardless of which struc::Channel's
   * struc::Channel::create_msg() originally generated `*this` (assuming that method was even used for
   * construction of `*this` -- in no way required, just often convenient).
   *
   * @see our class doc header and/or "Lifecycle of an out-message" section of struc::Channel class doc
   *      header.  Recall that where it mentions `Channel send()`, you can understand
   *      there to be a 1-1 relationship with a synchronous call to `this->emit_serialization()`.
   *
   * @note Reminder: Msg_out is for user out-messages; an internal out-message has only metadata and thus can/should
   *       be created and serialized using a single blob + simple Capped_sz_capnp_message_builder and direct
   *       load_mdt() call.
   *
   * ### Errors ###
   * See Struct_builder::emit_serialization().  This essentially forwards to that and emits its errors if any.
   * Additionally the error error::Code::S_INVALID_ARGUMENT may be emitted, if segment 1 (computed by the
   * serialization-engine given at construction) lacks a sufficiently-large frame-prefix to hold the mdt header.
   * Out-arg is untouched on error.
   *
   * @param segs_out_ptr
   *        On success (no error emitted) this is cleared and replaced with the sequence of segments as
   *        pointer/size pairs.
   * @param split_segs_out_or_null
   *        If not null, `*split_segs_out_or_null` is set (on success) to the split-segment metadata
   *        describing which of the output segments were split across multiple blobs.  Empty if no
   *        splitting occurred.  Useful for stats instrumentation without re-parsing the mdt header.
   *        Perf cost is low, as we only `move()` a thing required internally anyway.
   * @param session
   *        Specifies the opposing recipient for which the serialization is intended.
   *        If `Builder::Session` is Null_session, then `{}` is the only value to supply here.  Otherwise more
   *        information is necessary.
   * @param err_code
   *        Caution: Unlike user-facing methods, this does not throw (except capnp-generated exceptions);
   *        and `err_code` may not be null.  See above regarding emitted codes.
   * @param load_mdt_args
   *        Arguments to forward to struc::load_mdt() during the metadata-filling steps;
   *        specifically the args `session_token`, `originating_msg_id_or_none`, `id_or_none`.
   */
  template<typename... Load_mdt_args>
  void emit_serialization(Segment_bufs* segs_out_ptr, Split_segments* split_segs_out_or_null,
                          const typename Builder::Session& session,
                          Error_code* err_code, Load_mdt_args&&... load_mdt_args);

  // Data.

  /**
   * The guy serializing a #Body, as well as storing -- but not itself loading -- a frame-prefix big-enough for
   * our mdt header in its mandatory segment 1.  Underlying serialization potentially futher mutated by user via
   * body_root(); while emit_serialization() finalizes the mdt header's contents.
   */
  Builder m_builder;

  /**
   * See body_root().
   *
   * ### Rationale ###
   * We don't strictly need to save this; we could have body_root() return the #Body_builder, not a pointer to
   * it.  This would not be out of line; it is a general capnp-documented tip to act on a given `Builder` repeatedly
   * as opposed to accessing it via an accessor each time; so we could encourage the user to do that -- not
   * our problem.  (Though I (ygoldfel) am not positive that perf tip applies to the root capnp-`struct`; maybe
   * look into it sometime for education.)  It would also reduce `*this` footprint a bit and simplify/speed-up
   * the move-ctor and move-assignment somewhat (a capnp `Builder` is light-weight but not totally tiny, consisting
   * of a few pointers and integers).  So why do this?
   *
   * Honestly keeping it this way is firstly a historic artifact from an earlier situation where keeping a
   * pointer made sense in a no-longer-relevant sense (explaining how here would be uninteresting).
   * That said it's a fairly cheap little optimization that allows the repeated use of body_root() with no perf
   * penalty, and it is nice and expressive to have a non-`const` accessor returning a pointer to *the*
   * thing that would mutate `*this`.  Similarly it's nice to have a `const` accessor returning pointer-to-`const`
   * suitable for pretty-printing.  Throw in now having to change existing call sites, and voila, this decision.
   */
  Body_builder m_body_root;

  /**
   * Mdt header builder lazily de-nullified by the first successful emit_serialization() and then (by any
   * successful emit_serialization()) filled-out with appropriate mdt (e.g. the message ID).
   *
   * ### Design/rationale ###
   * Note that:
   *   - this does not own the backing (small) memory but merely provides us capnp-mutator access to it; while
   *   - #m_builder in fact owns that memory in its segment 1; so
   *   - why do we even store it as a data member rather than entirely creating/destroying this guy inside
   *     emit_serialization(), the only place where it is needed?
   *
   * Answer: This is done purely for perf in the event that emit_serialization() is successfully used 2+ times.
   * The first time we create it but choose not to destroy it.  The second time, then, struc::load_mdt() (as
   * invoked by emit_serialization()) can mutate capnp-accessed fields instead of the relatively expensive op where it
   * would:
   *   -# fill backing header area with zeroes;
   *   -# inside it have capnp create various parts (however small) from scratch and then mutate them.
   *
   * At the cost of storing, basically, a little capnp `Builder` we can avoid a bunch compute that would result
   * in the same thing anyway.
   */
  boost::movelib::unique_ptr<Capped_sz_capnp_message_builder> m_mdt_builder;

  /// The `Native_handle`, if any, embedded inside this message.
  Native_handle m_hndl_or_null;
}; // class Msg_out

/**
 * A structured in-message *instance* suitable as received and emittable (to user) by struc::Channel.  Publicly
 * these are never constructed but emitted into `Channel`-passed handlers, wrapped in `shared_ptr`
 * Channel::Msg_in_ptr.  The APIS involved include Channel::expect_msg(),
 * Channel::expect_msgs(), Channel::async_request(), and Channel::sync_request(); plus
 * similar sync_io::Channel APIs.
 *
 * Once available to the user, one accesses the in-place (zero-copy) capnp serialization via body_root(),
 * via which one can access the message's read-only contents.
 *
 * @see Msg_out
 * @see "Lifecycle of an out-message" section of struc::Channel class doc header for useful context.
 *      Note that for each Msg_out, there will exist 0+ `Msg_in`s, one per
 *      successful `send()` (et al) paired with emission to user via aformentioned handlers.  The latter represent
 *      each instance of the original message being received; the former represents the original message.
 *      Hence Msg_in access is read-only (`Reader` access only); Msg_out access
 *      is read/write (`Builder` access).
 *
 * ### Resource use: RAM ###
 * If #Reader_config is non-SHM-based, then a `*this` is analogous to a Msg_out; the
 * RAM used is released once `*this` is destroyed.  If it *is* SHM-based, then the cross-process ref-count
 * scheme explained in Msg_out doc header is in force.
 *
 * ### Resource use: the #Native_handle (if any) ###
 * Firstly see similarly named section in Msg_out doc header.  In particular note that
 * the `*this`-stored descriptor is not the same descriptor as the one in the out-message object;
 * rather it's a descriptor in a different process referring to the the same description, with an OS/kernel-held
 * ref-count.
 *
 * We wish to help you avoid leaking any stored non-null #Native_handle until process exit, so *if*
 * `*this` contains one, then it owns it, and like Msg_out will return it to the OS (Native_handle::release())
 * in dtor.  However if you wish to yourself use the #Native_handle -- which is typical in most non-error
 * scenarios -- then call emit_native_handle_or_null().  Note that it is *not* `const`; it *will* cause
 * `*this` to forget the `Native_handle`; and decision as to when/how to return it to the OS becomes yours.
 *
 * @internal
 * ### Impl notes ###
 * This follows the same facade-API pattern as Msg_out; see similar place in its doc header.
 * Same applies here.  It also similarly represents *both*
 *   - metadata-message about the associated user-message, if any, or the internal message; message ID,
 *     originating message ID, etc.; and
 *   - the user message (if any).
 *
 * Subtlety: That said, the decision to include the mdt in Msg_in is different from the one leading doing the same
 * for Msg_out.  For Msg_in it is natural regardless: We can only receive an in-message together with its metadata;
 * it was never (as an in-message) conceptually separated into those 2 parts; it did not exist, until we received it.
 * Msg_out, however, does it just for performance, because mechanically the mdt header belongs with the first segment
 * of the (outer) serialization of the user out-message.  In fact no metadata even exists until
 * send-time (Msg_out::emit_serialization()); and *after* send-time the metadata becomes irrelevant (and is
 * overwritten at next send, if any, time).
 *
 * Corollary: In the case of an internal message, a `*this` is never emitted to the user.  In that case
 * internal_msg_body_root() provides internal access to the message.  This is, as of this writing, asymmetrical
 * to Msg_out which never represents an internal message -- there is no need to, in that case, as one can just
 * use a `Blob` + Capped_sz_capnp_message_builder + load_mdt(); Msg_out is irrelevant and too-complex by comparison.
 * Msg_in, though, means an in-message... so it applies to *any* received message.
 *
 * The expected sequence of calls (via Msg_in_impl facade) required to load a `*this` with the required
 * serialization payload, so that either internal_msg_body_root() or body_root() can finally be used to access
 * the deserialized structured data, is explained in some detail in the add_serialization_segment() doc header.
 * To be clear the end user only gets access to a `*this` once body_root() is available (all that stuff has been done
 * already).
 *
 * @endinternal
 *
 * @tparam Body_t
 *         See Msg_out: this is the counterpart.
 * @tparam Reader_config_t
 *         See Msg_out: this is the counterpart.
 */
template<typename Body_t, typename Reader_config_t>
class Msg_in :
  private boost::noncopyable
{
public:
  // Types.

  /// See struc::Channel::Msg_body.
  using Body = Body_t;

  /// Short-hand for capnp-generated read-only-accessing `Reader` nested class of #Body.  See body_root().
  using Body_reader = typename Body::Reader;

  /// See struc::Channel::Reader_config.
  using Reader_config = Reader_config_t;

  // Constructors/destructor.

  /**
   * Returns resources, potentially including potentially significant RAM resources, taken before emitting to the user.
   * The value that *would* currently be returned by emit_native_handle_or_null(), if not `.null()`, is returned to
   * the OS.  (Use emit_native_handle_or_null() if you wish to make use of it first instead.)
   *
   * See class doc header for discussion.
   */
  ~Msg_in();

  // Methods.

  /**
   * The #Body root capnp-generated accessor object.  E.g.: `this->body_root().getSomeStruct().setSomeField()`
   * will access the capnp-`struct`-typed root field `someStruct` and pluck out the value `someField` -- presumably an
   * integer -- within it.  The ref returned shall always be to the same address until `*this` is destroyed.
   *
   * #Body_reader is light-weight, so you may make a copy and then access via that, if desired.  However
   * whether `body_root()` itself or such a copy, such a `Reader` object may not be accessed after `*this` is destroyed.
   *
   * Informally we recommend against value-copying body_root(): Light-weight or not, it's a copy, and copying a
   * pointer/cref is cheaper; and it may not live past `*this` anyway; and body_root() always returns the same address.
   * So just store the cref (or pointer-to-`const` with the same addr).
   *
   * ### Quick capnp tips ###
   * You should really read all of capnp docs at its web site.  They are very useful and well written and not
   * overly formal despite being quite comprehensive.  That said a couple of gotchas/tips:
   *   - On a `Reader`, `.getX()` is lightning-fast, like accessing `struct` members directly --
   *     but only when `X` is of a scalar type.  Compount types, where `.getX()` returns not a native type
   *     but another `Reader`, need to perform some pointer checking and are slower.  Therefore,
   *     if you plan to `.getX()` and then `.getA()` and `.getB()` (and similar) on that `X`, you should
   *     save the result (`auto x = ....getX();`); then access via the saved result
   *     (`a = x.getA(...); b = x.getB(...)`).
   *
   * To pretty-print (with indent/newlines) you can use: `capnp::prettyPrint(M.body_root()).flatten().cStr()`.
   *
   * @internal
   * @see add_serialization_segment() doc header for the overall expected call sequence including the present
   *      method.
   * @endinternal
   *
   * @return See above.
   */
  const Body_reader& body_root() const;

  /**
   * Returns the #Native_handle -- potentially null meaning none -- embedded in this message; and forgets
   * that #Native_handle.  The next call to `this->emit_native_handle_or_null()` shall return a
   * `.null() == true` obejct.
   *
   * @warning Returning the handle (if not null) to the OS (via Native_handle::release() or another technique),
   *          if it is undesirable to leak it until process exit, is the caller's responsibility.
   *          However if you do not call emit_native_handle_or_null(), then our dtor will handle it,
   *          thus not leaking it past `*this` existence.
   *
   * @return See above.
   */
  Native_handle emit_native_handle_or_null();

  /**
   * Size of the serialization in memory in bytes; only the serialization as transmitted via IPC counts.
   * That is, if #Reader_config means we are SHM-backed (or anything else involving an outer and inner
   * serialization), then the in-SHM (or equivalent inner) serialization does not count.
   *
   * This is meant purely informationally as for logging/reporting/alerting: not for algorithmic purposes.
   *
   * @internal
   * The precipitating use case was for stats tracking.  However it is healthy to expose this publicly too.
   * @endinternal
   *
   * @return See above.
   */
  size_t serialization_size() const;

  /**
   * Prints string representation to the given `ostream`.  This representation lacks newlines/indentation;
   * includes a (potentially truncated) pretty-printed representation of `body_root()` contents; and includes
   * value that would be returned by emit_native_handle_or_null().
   *
   * Caution: This could be an operation expensive in processor cycles and, temporarily, RAM; and thus should
   * be used judiciously.  To help drive your decision-making: This method, internally,
   *   -# uses capnp `kj::str(this->body_root)` to generate a *full* pretty-print of body_root() contents;
   *   -# truncates the result of the latter, as needed for a reasonably short output, and prints the result;
   *   -# adds handle that would be returned by emit_native_handle_or_null() and possibly a few other small
   *      information items.
   *
   * Because there is no reasonably-available way to stop generating the pretty-print in step 1 upon reaching
   * a certain number of characters, the operation may take a while, if many non-default bytes have been
   * mutated-in; and before truncation the resulting string may take significant RAM accordingly.
   *
   * @internal
   * If used by internal code before successful deserialize_mdt() and/or before successful deserialize_body()
   * (if one would even be possible in the first place -- not so with internal messages), this method will print
   * some useful things.  However the above public-facing description is no longer complete in that case.
   * In any case the method remains suitable for TRACE-logging but not INFO-logging by internal code, at least
   * in the per-message common code path.
   * @endinternal
   *
   * @param os
   *        Stream to which to write.
   */
  void to_ostream(std::ostream* os) const;

private:
  // Friends.

  /// Facade type that exposes specific `private` APIs of `*this` to other internal Flow-IPC code.
  template<typename T>
  friend struct Msg_in_impl;

  // Types.

  /**
   * capnp `struct StructuredMessage` which is the structured view of the metadata (mdt) header.
   * See detail/structured_msg.capnp.
   */
  using Mdt = schema::detail::StructuredMessage;

  /**
   * Short-hand for capnp-generated reader API for the internal-message sub-area of an #Mdt.
   * See detail/structured_msg.capnp.
   */
  using Internal_msg_body_reader = typename schema::detail::StructuredMessage::InternalMessageBody::Reader;

  /// capnp-generated reader API for #Mdt structures.
  using Mdt_reader = typename Mdt::Reader;

  /**
   * Short-hand for user-indirectly-specified Struct_reader concept impl.  This can be used to interpret
   * the user payload body_root() if any.
   */
  using Reader = typename Reader_config::Reader;

  /// Short-hand for sync_io::stat::Channel_stats.
  using Channel_stats = sync_io::stat::Channel_stats;

  // Constructors/destructor.

  /**
   * (Access via Msg_in_impl) Constructs a not-ready-for-public-consumption in-message which awaits
   * serialization-storing segments to be added via add_serialization_segment() and then finalized with
   * deserialize_mdt() and possibly deserialize_body().  After the latter 1-2: body_root() or internal_msg_body_root()
   * can be used to access the deserialized data; as can id_or_none() and similar accessors.
   *
   * @see add_serialization_segment() doc header for a more detailed description of what's next.
   *
   * @param struct_reader_config
   *        See struc::Channel ctors.  This is copied, memorized.
   */
  explicit Msg_in(const Reader_config& struct_reader_config);

  // Methods.

  /**
   * (Access via Msg_in_impl) Store the #Native_handle (potentially `.null()`, meaning none) in this in-message.
   * Call this at most once; or behavior undefined (assertion may trip).
   *
   * @param native_handle_or_null
   *        The #Native_handle (`.null() == true` if none) to move into `*this`.  Made `.null() == true` upon return.
   */
  void store_native_handle_or_null(Native_handle&& native_handle_or_null);

  /**
   * (Access via Msg_in_impl) Records the first or next, in order, unstructured-data chunk that is part of the eventual
   * serialization of both the mdt header and the user in-message payload (if any).  Once the proper number
   * (typically/ideally) of these calls were made, at various points deserialize_mdt() and (if relevant)
   * deserialize_body() are possible.
   *
   * Matching how Msg_out::emit_serialization() outputs the original serialization, this overall works as follows:
   *   -# add_serialization_segment() 1 (mandatory): After this it is possible to deserialize_mdt() which, importantly,
   *      returns the total # of calls that are needed (at least 1, including this one; but possibly 2+ if more are
   *      needed).  This first blob shall contain the mdt header and, if it's a user message (not internal message),
   *      the first chunk of the user message's serialization.
   *      - This solidifies N (total # of blobs expected) and is-user-msg (whether it's a user message or not).
   *      - If is-user-msg is false, then done (`*this` is immutable from now on).
   *        internal_msg_body_root() is available.
   *      - Other mdt accessors -- id_or_none(), etc. -- are available.
   *   -# add_serialization_segment() 2, 3, ..., N (possible): N is from the previous bullet.  This loads the remaining
   *      needed blobs comprised by the serialization of the user message.  To be clear, though, it is possible and
   *      probable that N=1, even though is-user-msg is true.  So, often there are no further
   *      add_serialization_segment() calls past the 1st one above.
   *   -# deserialize_body() (mandatory if is-user-msg): All data are available, so it sets up capnp-stuff.  Hence:
   *      - body_root() is available.
   *
   * @note add_serialization_segment() doesn't care about the following, but since we're explicating the whole
   *       process here, here it is FYI: Usually each add_serialization_segment() indeed delivers exactly
   *       one capnp-segment in the user message's serialization (plus the mdt header with the first one).  However
   *       at times a given capnp-segment is too large to fit in an IPC message and has to be split pre-send
   *       by Msg_out::emit_serialization().  Therefore deserialize_body() has to do the reverse (join some of
   *       the blobs into original segment(s)).  (This involves copying and is relatively slow, so various parts
   *       of the system take some pains to avoid it.  When the `Struct_builder` is SHM-enabled, in particular,
   *       splitting never occurs; but even otherwise we try to avoid it.  Outside our scope here to discuss though.)
   *       Hence the blobs delivered via add_serialization_segment() are not, technically, necessarily segments per se
   *       but segment chunks.  There is some mis-naming there, partially for historical reasons, but it's not too bad.
   *       In any case the caller of these APIs can assume it'll just work, as Msg_out::emit_serialization()
   *       and Msg_in::deserialize_body() take care of it.  Last thing: This joining can only work, if we know which
   *       segments were split into how many chunks; this information is in the mdt header delivered in
   *       add_serialization_segment() 1.
   *
   * @param blob
   *        The first or next chunk.  The payload must be exactly in [`blob.begin()`, `blob.end()`).
   *        In the case of the first chunk, said payload includes both the mdt header and (sub-)segment 1 following it.
   *        We take over this blob; on return it shall be ~as-if default-cted due to a move-from.
   */
  void add_serialization_segment(Segment_blob_in&& blob);

  /**
   * (Access via Msg_in_impl) To be invoked after exactly one add_serialization_segment() call:
   * finalizes the deserialization of everything except the potential user-message body.
   * This must be called strictly before any calls to internal_msg_body_root(), id_or_none(),
   * originating_msg_id_or_none(), or session_token(); and any subsequent add_serialization_segment()
   * calls (if any).
   *
   * @see add_serialization_segment() doc header for the overall expected call sequence including the present
   *      method.
   *
   * The value this returns (sans error) dictates the exact # of further add_serialization_segment() calls to make.
   *   - If 1: You may use the accessor API immediately.  internal_msg_body_root() is OK; `id_or_none() == 0`;
   *     body_root() may not be used.
   *   - Else: You may use the accessor API immediately.  `id_or_none() != 0`; internal_msg_body_root() may not be
   *     used (ever); body_root() may not be used *yet* until successful deserialize_body().
   *
   * If called before add_serialization_segment() 1, behavior is undefined (assertion may trip).
   * If called after add_serialization_segment() 2, behavior is undefined.
   * If called more than once, behavior is undefined (assertion may trip).
   *
   * If it fails (emits truthy #Error_code), it is pointless to use `*this`.  Recommend destruction.
   *
   * @param logger_ptr
   *        Logger to use for logging within this method.
   * @param err_code
   *        Caution: Unlike user-facing methods, this does not throw (except capnp-generated exceptions);
   *        and `err_code` may not be null.  Other than that:
   *        #Error_code generated: falsy on success, else:
   *        error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA (opposing Msg_out code
   *        filled out the fields in an unexpected way),
   *        anything emitted by Struct_reader::deserialization().
   * @param stats_rcv_msg_internal_only
   *        Method updates the following receive-side stats as appropriate *if and only if*
   *        `*this` represents an internal message (otherwise deserialize_body() is to be used for this):
   *        Channel_stats::Msg::m_internal_msgs, Channel_stats::Msg::m_total_low_lvl_blobs,
   *        Channel_stats::Msg::m_total_segments, Channel_stats::Msg::m_single_segment_msgs,
   *        Channel_stats::Msg::m_histo_msg_sz.  On error some or all stats may still be updated; informally
   *        we consider this harmless, as an error here is catastrophic (as opposed to a normal channel-hosing).
   * @return Total # of add_serialization_segment() calls required -- *including* the 1 that has already occurred.
   *         See above.
   */
  size_t deserialize_mdt(flow::log::Logger* logger_ptr, Error_code* err_code,
                         Channel_stats::Msg* stats_rcv_msg_internal_only);

  /**
   * (Access via Msg_in_impl) To be invoked after `deserialize_mdt() == N`, and add_serialization_segment() has been
   * called N times: finalizes the deserialization of the user-message body.  This must be called before any calls
   * to body_root().
   *
   * @see add_serialization_segment() doc header for the overall expected call sequence including the present
   *      method.
   *
   * If called before a post-`deserialize_mdt()` add_serialization_segment(), behavior is undefined (assertion may
   * trip).  If called more than once, behavior is undefined (assertion may trip).
   *
   * If it fails (emits truthy #Error_code), it is pointless to use `*this`.  Recommend destruction.
   *
   * @param err_code
   *        See deserialize_mdt().
   * @param stats_rcv_msg
   *        Method updates the following receive-side stats as appropriate:
   *        Channel_stats::Msg::m_user_msgs, Channel_stats::Msg::m_notifications,
   *        Channel_stats::Msg::m_notification_responses, Channel_stats::Msg::m_handle_bearing_msgs,
   *        Channel_stats::Msg::m_total_low_lvl_blobs, Channel_stats::Msg::m_msgs_with_split_segments,
   *        Channel_stats::Msg::m_histo_split_blobs_per_seg, Channel_stats::Msg::m_total_segments,
   *        Channel_stats::Msg::m_single_segment_msgs, Channel_stats::Msg::m_multi_segment_msgs,
   *        Channel_stats::Msg::m_histo_msg_sz.  On error some or all stats may still be updated; informally
   *        we consider this harmless, as an error here is catastrophic (as opposed to a normal channel-hosing).
   */
  void deserialize_body(Error_code* err_code, Channel_stats::Msg* stats_rcv_msg);

  /**
   * (Access via Msg_in_impl) To be called only after deserialize_mdt(), returns the message ID of this in-message;
   * 0 means it's an internal message (internal_msg_body_root() applies), else it's a user message (body_root()
   * applies).
   *
   * @return See above.
   */
  msg_id_t id_or_none() const;

  /**
   * (Access via Msg_in_impl) To be called only after deserialize_mdt(), returns the message ID of the out-message
   * to which this in-message claims to be responding; or 0 if it is not a response.
   *
   * @return See above.
   */
  msg_id_t originating_msg_id_or_none() const;

  /**
   * (Access via Msg_in_impl) To be called only after deserialize_mdt(), similar to body_root() but for the
   * internal-message root.  See #Internal_msg_body_reader doc header.
   *
   * @return See above.
   */
  Internal_msg_body_reader internal_msg_body_root() const;

  /**
   * (Access via Msg_in_impl) To be called only after deserialize_mdt(), returns session token tagging this in-message.
   *
   * @return See above.
   */
  const Session_token& session_token() const;

  /**
   * (Access via Msg_in_impl) The #Mdt root capnp-generated accessor object.  May be useful for, say, pretty-printing it
   * to log (e.g.: `capnp::prettyPrint(M.mdt_root()).flatten().cStr()`).  We do not recommend its use for
   * other purposes; stylistically it is better to access items via individual accessors like session_token()
   * or internal_msg_body_root().  See #Mdt doc header.
   *
   * @return See above.
   */
  const Mdt_reader& mdt_root() const;

  // Data.

  /// Sum of blob sizes in add_serialization_segment() calls so far.
  size_t m_total_sz;

  /**
   * A very simple capnp::MessageReader that provides structured access to the mdt header.
   * Null until successful deserialize_mdt(), non-null thereafter.  At that point #m_mdt_root is available
   * for capnp-accessor access to the stuff, but that guy is lightweight and needs the daddy `MessageReader`
   * (this thing here) to live.  (Also the underlying memory, of course, has to remain valid/unchanged for either
   * of them to make any sense.)
   */
  std::optional<Capped_sz_capnp_message_reader> m_mdt_reader;

  /**
   * The Struct_reader concept impl that does much of the heavy lifting (e.g., it might be SHM-enabled; or just
   * be a basic Heap_reader) when it comes to the user message's deserialization (if any).
   * Things like add_serialization_segment() and deserialize_body() mostly forward to it.  However: all work
   * w/r/t the mdt header is up to `*this` proper; by the time deserialize_body() invokes
   * `m_body_reader.deserialization()` we would have already grokked the mdt stuff fully and fixed-up blob 1
   * inside #m_body_reader to begin *after* the mdt header.  Hence Struct_reader::deserialization() can actually
   * interpret the user message without being aware of the mdt header that shares a buffer with the user-message
   * serialization.  (Reminder: That setup is for perf.)
   *
   * @note `m_body_reader` owns the memory containing all of the serialization of everything.
   *       add_serialization_segment() transfers the caller-supplied `Blob` to it via move-semantics.
   */
  Reader m_body_reader;

  /**
   * Meaningful from add_serialization_segment() on, this simply describes the memory
   * area that begins with the mdt header and ends anywhere past its last byte.  In actual fact, for simplicity,
   * it is simply `B.const_buffer()`, where `B` was passed to the first add_serialization_segment() call.
   * (Even though this includes possible seg 1 user-message payload that follows the mdt header, when deserializing
   * the header anything such trailing stuff is never looked-at.  There's no point in finding the actual end of
   * the header's serialization.)
   *
   * Before that: it is default-cted.  After deserialize_mdt() the values themselves are no longer needed, but
   * it is left alone, because `bool(m_mdt_header_area.data())` is useful for determining whether
   * add_serialization_segment() has been called already.
   *
   * By then we have the #m_mdt_root, which is all we wanted in the first place, and that continues to work fine,
   * as long as this memory area still holds the same *contents*.  This *description* of those contents is not required
   * anymore (but, again, is as of this writing used as a flag).
   */
  util::Blob_const m_mdt_header_area;

  /// Starts `false`; becomes `true` immutably once deserialize_mdt() succeeds.
  bool m_mdt_deserialized_ok;

  /// Starts `false`; becomes `true` immutably once deserialize_body() succeeds (never if it's an internal message).
  bool m_body_deserialized_ok;

  /// See internal_msg_body_root(); meaningless until #m_mdt_deserialized_ok.
  Mdt_reader m_mdt_root;

  /// See body_root(); meaningless until #m_body_deserialized_ok.
  Body_reader m_body_root;

  /**
   * Starts uninitialized, this is assigned exactly once by deserialize_mdt(), storing the session-token that
   * was embedded in the in-message.  This is cached from the encoding in the tree accessed via #m_mdt_root.
   * It is cached for session_token() perf only.
   */
  Session_token m_session_token;

  /// See store_native_handle_or_null().
  Native_handle m_hndl_or_null;
}; // class Msg_in

// Free functions: in *_fwd.hpp.

// Template implementations.

/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_STRUCT_MSG_OUT \
  template<typename Body_t, typename Builder_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_STRUCT_MSG_OUT \
  Msg_out<Body_t, Builder_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_STRUCT_MSG_IN \
  template<typename Body_t, typename Reader_config_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_STRUCT_MSG_IN \
  Msg_in<Body_t, Reader_config_t>

// Msg_out template implementations.

TEMPLATE_STRUCT_MSG_OUT
CLASS_STRUCT_MSG_OUT::Msg_out() :
  m_body_root(nullptr) // capnp deletes default ctor to "discourage incorrect usage" but makes this available.
{
  // OK then.  Useless *this until moved-to.
}

TEMPLATE_STRUCT_MSG_OUT
CLASS_STRUCT_MSG_OUT::Msg_out
  (const Builder_config& struct_builder_config) :

  /* As promised, in this simple (typical) form we create the new capnp::MessageBuilder and initRoot<Body>() it
   * for them.  Simply delegating as follows causes getRoot<Body>() to act as-if initRoot<Body>() due to lacking
   * an initialized root at that point. */
  Msg_out(Builder{struct_builder_config})
{
  // OK then.  Mutate away, user.
}

TEMPLATE_STRUCT_MSG_OUT
CLASS_STRUCT_MSG_OUT::Msg_out(Builder&& struct_builder) :

  /* In this more advanced (from user's PoV) ctor form they already are giving us m_builder and guaranteeing
   * its getRoot<Body>() is either already a thing... or it is not root-initialized at all.
   * getRoot<Body>() by us will thus not modify the tree, if it is initialized; or will create a blank tree
   * (same as the other ctor), if it is not.  The only problematic situation -- as advertised -- is
   * if they root-initialized it to something other than Body (e.g., initRoot<SomeOtherThing>()); then getRoot<Body>()
   * will try to treat non-Body as Body, and something will probably blow up at some point
   * (possibly during deserialization). */
  m_builder(std::move(struct_builder)), // Eat theirs!
  m_body_root(m_builder.payload_msg_builder()->template getRoot<Body>()) // Pluck out theirs, or initialize if needed!
{
  // OK then.  Mutate away, user (though they may well have finalized m_body_root tree already).
}

TEMPLATE_STRUCT_MSG_OUT
CLASS_STRUCT_MSG_OUT::Msg_out(Msg_out&& src) :
  Msg_out()
{
  operator=(std::move(src));

  /* (This comment is by ygoldfel, original author of this code -- here and all of this class template and file.)
   * This implementation is clearly correct (functionally); one can see a similar pattern used in many places in,
   * e.g., STL/Boost.  However the original, and all else being equal (which it's not; read on), my preferred
   * implementation of this move ctor is simply: `= default;`.  In that original state I tested this guy and
   * ran into the following.
   *
   * As of this writing, to test this (and all of Msg_out and much else besides), I've been using the
   * transport_test.exec exercise-mode section; and the unit_test.exec test suite (both in the `ipc` meta-project,
   * though most of the relevant .../test/... code for unit_test.exec is in `ipc_shm_arena_lend` project therein).
   * (Normally we don't speak of test code inside source code proper like this, but read on to see why the
   * exception.)  transport_test has passed in all configurations (build/run envs) without issue.
   * unit_test.exec has passed in at least the following (all Linux) except the *asterisked* one:
   *   - gcc-9, -O0 or: (-O3 + LTO disabled or LTO (with fat-object-generation on) enabled (for all of libflow
   *     and libipc_... and the test code proper)).
   *   - clang-17 + libc++ (LLVM-10) (note: not GNU stdc++), -O0 or: (-O3 + LTO disabled or LTO (-flto=thin) enabled*
   *     (for all of libflow and libipc_... and the test code proper)).
   *
   * The *asterisk* there denotes the one config where a problem was observed.  Namely,
   * Shm_session_test.In_process_array unit_test failed, seg-faulting before the test could complete (no actual
   * test-failed assertions had triggered up to the seg-fault point).  The seg-fault was completely consistent
   * (at least given a particular machine): in session::Client_session_impl::mdt_builder(), just near the end
   * of the method, a Msg_out was being move-cted (presumably using this ctor here) from another Msg_out
   * that had just been constructed a few lines above.  Granted, that source Msg_out was constructed and
   * populated (rather straightforwardly) in another thread, but a synchronization mechanism was used to ensure
   * this happened synchronously before the code that seg-faulted (this move-ctor) would proceed to be called.
   * (I verified quite studiously that nothing untoward was happening there; clearly the source object was
   * created cleanly before signaling the thread waiting to proceed with this move-ct to quit waiting and proceed.)
   * (In addition note that this Msg_out ctor was, as seen in debugger, clearly getting auto-inlined; since the prob
   * occurred with LTO but not without -- though gcc's LTO had no issue, but I digress -- this may be significant.)
   *
   * First I labored to isolate where the problem occurred; there was no corrupted memory or anything strange like
   * that; and it fairly clearly really was in the move ctor (which, again, was just `= default;` at the time).
   * Since the move ctor was auto-generated, it was somewhat challenging to see where the problem happened, though
   * admittedly due to time pressure I did not delve into the move ctors *that* would've invoked: for
   * m_builder, m_body_root, m_hndl_or_null (doing so might be a good idea; but keep reading).
   *
   * At that point, just to see what's up, I "jiggled" the implementation into its current form.  Empirically
   * speaking the problem went away, and everything passed swimmingly with no instability.  Hence I left the
   * work-around in place.  So where does it leave us?
   *
   * 1, in terms of correctness and generated-code quality: As noted, the new impl is clearly correct.  In terms of
   * generated-code quality, it is potentially a few instructions slower than the auto-generated ctor:
   * the three m_* are first initialized to their null states and then move-assigned; plus
   * store_native_handle_or_null() checks m_hndl_or_null for null (which it is, so that `if` no-ops).
   * It's simple/quick stuff, and it might even get optimized away with -O3, but nevertheless skipping to
   * move-ction of the members would be more certain to not do that unneeded zeroing stuff.  Subjectively: it's
   * probably fine (undoubtedly there are far heavier perf concerns than a few extra zeroings).
   *
   * 2, there's the mystery.  I.e., sure, the replacement code is fine and is unlikely to be a perf problem;
   * but why isn't the preferred `= default;` the one in actual use?  Why did it cause the failure, though only
   * in a very particular build/run environment (clang-17, LLVM-10 libc++, with thin-LTO), when similar ones
   * (such as same but without LTO) were fine?  At this stage I honestly do not know and will surely file a
   * ticket to investigate/solve.  Briefly the culprit candidates are:
   *   - Msg_out code itself.  Is there some unseen uninitialized datum?  Some basic assumption being ignored or
   *     C++ rule being broken? ###
   *   - Something in capnp-generated move-ctor (as of this writing capnp version = 1.0.1, from late 2023) or
   *     m_builder move-ctor.
   *   - Something in clang-17 + thin-LTO optimizer improperly reordering instructions.
   *
   * I cannot speculate much about which it is; can only say after a few hours of contemplating possibilities:
   * no candidates for ### (see above) being at fault has jumped out at me.  That said, no run-time sanitizer
   * has run through this code as of this writing (though the code analyzer, Coverity, has not complained);
   * that could help.  Sanitizer or not, generally:
   * given 0-2 days of investigation by an experienced person surely we can narrow this down to a
   * minimal repro case, etc. etc.  So it is solvable: just needs to be done.
   *
   * Until then: (1) this remains a mystery; and (2) there is an acceptable work-around.  (Though the mystery
   * is personally disconcerting to me; e.g., as of this writing, I cannot name another such mystery in this
   * entire code base.) */
} // CLASS_STRUCT_MSG_OUT::Msg_out(Msg_out&&)

TEMPLATE_STRUCT_MSG_OUT
CLASS_STRUCT_MSG_OUT::~Msg_out()
{
  // As promised return it (if any) to OS.
  store_native_handle_or_null({});

  // The rest of cleanup is automatic.
}

TEMPLATE_STRUCT_MSG_OUT
CLASS_STRUCT_MSG_OUT& CLASS_STRUCT_MSG_OUT::operator=(Msg_out&& src)
{
  if (&src == this)
  {
    return *this;
  }
  // else

  m_builder = std::move(src.m_builder);

  /* I was a bit worried about this perf-wise (and note this of course similarly auto-occurs in the =default-ed
   * move ctor), but I (ygoldfel) did some due diligence on it:
   *   - It is after all move()able (or this wouldn't compile), with implicitly-default move ops.
   *     This doesn't "nullify" src.m_body_root, but that is perfectly fine; m_builder sure is made as-if
   *     default-cted, and anyway using (other than move-to) of a moved-from `*this` is advertised as undefined
   *     behavior.
   *   - `Builder`s are generally treated as light-weight in capnp docs.
   *  - Looking inside the current (version 0.10) code shows that a capnp-`struct` Builder is, internally,
   *    entirely consisting of something called StructBuilder, and the latter consists of 4 pointers and 2
   *    integers.
   * So it is fine (and so is the move ctor). */
  m_body_root = std::move(src.m_body_root);
  m_mdt_builder = std::move(src.m_mdt_builder);

  // The following is why we didn't simply do =default.
  if (m_hndl_or_null != src.m_hndl_or_null)
  {
    // As promised return it (if any) to OS, as that is what would happen if *this were destroyed.
    store_native_handle_or_null({});
  }
  // else { this->m_hndl_or_null shall be unchanged; so junking it within the OS would make that handle garbage. }

  m_hndl_or_null = std::move(src.m_hndl_or_null);
  /* Commenting the following out, as it is of low import other than in terms of cleanliness; but keeping it
   * as opportunistic exposition of how Native_handle move-assignment works:
   *   assert(src.m_hndl_or_null.null() && "Native_handle move-from should nullify src."); */

  return *this;
} // Msg_out::operator=(move)

TEMPLATE_STRUCT_MSG_OUT
typename CLASS_STRUCT_MSG_OUT::Body_builder* CLASS_STRUCT_MSG_OUT::body_root()
{
  return &m_body_root;
}

TEMPLATE_STRUCT_MSG_OUT
const typename CLASS_STRUCT_MSG_OUT::Body_builder* CLASS_STRUCT_MSG_OUT::body_root() const
{
  return const_cast<Msg_out*>(this)->body_root();
}

TEMPLATE_STRUCT_MSG_OUT
typename CLASS_STRUCT_MSG_OUT::Orphanage CLASS_STRUCT_MSG_OUT::orphanage()
{
  return m_builder.payload_msg_builder()->getOrphanage();
  /* Aside: Per capnp docs ("C++ serialization") one can also do:
   *   return ::capnp::Orphanage::getForMessageContaining(*(body_root())); */
}

TEMPLATE_STRUCT_MSG_OUT
void CLASS_STRUCT_MSG_OUT::store_native_handle_or_null(Native_handle&& native_handle_or_null)
{
  if (native_handle_or_null != m_hndl_or_null)
  {
    m_hndl_or_null.release(); // Junk that handle in the OS.  No-ops if null.
  }

  m_hndl_or_null = std::move(native_handle_or_null);

  /* To be clear... as the above despite its syntactic simplicity can be semantically confusing:
   *   If src == dst: post-condition:
   *     dst is unchanged; src is null.
   *   If src != dst: post-condition:
   *     <prev dst value> is returned to OS; dst == <prev src value>; src is null. */
}

TEMPLATE_STRUCT_MSG_OUT
Native_handle CLASS_STRUCT_MSG_OUT::native_handle_or_null() const
{
  return m_hndl_or_null;
}

TEMPLATE_STRUCT_MSG_OUT
template<typename... Load_mdt_args>
void CLASS_STRUCT_MSG_OUT::emit_serialization(Segment_bufs* segs_out_ptr,
                                              Split_segments* split_segs_out_or_null,
                                              const typename Builder::Session& session,
                                              Error_code* err_code, Load_mdt_args&&... load_mdt_args)
{
  using util::Blob_mutable;
  using util::Blob_const;
  using boost::movelib::make_unique;
  constexpr size_t HDR_SZ = BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL;

  assert(err_code);
  auto& segs_out = *segs_out_ptr;

  /* In the (hopefully rare) case where a seg ended up too big to fit into the per-segment cap in m_builder
   * such a seg shall be split into (replaced by) small-enough sub-segment views into it.  We will essentially just
   * emit those via segs_out to caller; and it will just send them all out probably, one per (now small-enough)
   * message; but the receiver will need to reassemble (un-split, join) such segs before accessing the serialization.
   * Without knowing which of the segs_out are post-split sub-segs, as opposed to just being the mainstream case of
   * the original segs, this reassembly could not happen.  Hence split_segs shall hold that information in compact
   * form -- and we shall encode it the metadata header (along with a few other important things). */
  Split_segments split_segs; // Again: This is info about *which* segs were split; not the segs themselves.
  Blob_mutable hdr_blob; // Header location ahead of seg 1.

  m_builder.emit_serialization(segs_out_ptr, &hdr_blob, &split_segs, session, err_code);
  if (*err_code)
  {
    return;
  }
  // else

  const auto n_segs = segs_out.size();
  assert(n_segs != 0);

  if (hdr_blob.size() < HDR_SZ)
  {
    *err_code = error::Code::S_INVALID_ARGUMENT; // As advertised.
    return;
  }
  // else: From here on, no errors are possible.

  if (!m_mdt_builder)
  {
    // First emit_serialization() call (first send over IPC probably).
    m_mdt_builder = make_unique<Capped_sz_capnp_message_builder>
                      (hdr_blob,
                       false); // It is pre-zeroed by `Builder m_builder` when seg 1 is allocated.
  }
  // else { 2nd/3rd/... emit_serialization().  struc::load_mdt() will modify existing capnp-tree (unless it can't). }
  if (!struc::load_mdt(m_mdt_builder.get(), nullptr, std::forward<Load_mdt_args>(load_mdt_args)...,
                       n_segs,
                       split_segs.empty() ? nullptr : &split_segs))
  {
    /* We were trying to reuse m_mdt_builder for perf, but its existing split-segs thing is incompatible; so we cannot
     * reuse it and must make it afresh.  (Because this is rare, perf-wise it should be OK.  Also load_mdt() takes care
     * to check the bad condition first-thing hopefully getting us here snappily.) */
    m_mdt_builder = make_unique<Capped_sz_capnp_message_builder>
                      (hdr_blob,
                       true); // In this case there is stuff there from pre-existing m_mdt_builder; must re-zero it.
#ifndef NDEBUG
    const bool ok =
#endif
    struc::load_mdt(m_mdt_builder.get(), nullptr, std::forward<Load_mdt_args>(load_mdt_args)...,
                    n_segs,
                    split_segs.empty() ? nullptr : &split_segs);
    assert(ok && "We've made a fresh *m_mdt_builder, so there should be no reason it would fail.");
  }
  // else { Fast-path; nothing exotic.  m_mdt_builder was fresh or was reused successfully. }

  /* As of this writing we do not consult m_mdt_builder (in its form now, until the next round of emit_serialization()
   * if any) from this point on; while the data it wrote remains for further use, since m_builder is
   * still around.
   *
   * That said by not consulting it we *will* ignore the following bit of information: How much space (of HDR_SZ)
   * was *actually* necessary for what load_mdt() loaded; m_mdt_builder->getSegmentsForOutput()[0].asBytes().size()
   * would be that; but it is stored in m_mdt_builder which is gonna be ignored.  In fact we could destroy it now
   * and are only keeping it around for a more efficient struc::load_mdt() if they decide to emit_serialization() again.
   *
   * That is fine however: The idea is *all* of seg1 (and any -- possibly zero -- more needed segments
   * output by emit_serialization() here) shall be transmitted.  The receiver shall first process the entire
   * seg1 blob -- which contains HDR_SZ bytes of mdt; then seg1 of the user message after that; but capnp will
   * just ignore anything beyond the blob-leading mdt serialization at this stage.  Then, the receiver will
   * process the rest of seg1 plus any further segments (whose # is contained in mdt by the way: arg to load_mdt()),
   * yielding the user message.
   *
   * Yes, it *is* OK to give a capnp MessageReader a seg size greater (or equal to, obv) than the actual size used;
   * capnp will not go into areas that are of no interest to it anyway, and it doesn't have an unnecessary barf-response
   * to their existence. */

  // Output split-segment metadata if requested (for stats instrumentation by caller).
  if (split_segs_out_or_null)
  {
    *split_segs_out_or_null = std::move(split_segs);
  }

  /* Lastly output the list of `Blob_const`s (to IPC-transmit presumably) describing the n_segs segments... great --
   * segs_out is already that.  Caveat: at the moment the load_mdt()-loaded stuff lives in the frame-prefix
   * area ([0, .start()) range) of seg1's Blob container.  So "correct" seg1 to include that area; thus the
   * contiguous buffer that is the header plus seg1 shall be the first IPC-message. */
  auto& seg1 = segs_out.front();
  seg1 = Blob_const{hdr_blob.data(), hdr_blob.size() + seg1.size()};
} // Msg_out::emit_serialization()

TEMPLATE_STRUCT_MSG_OUT
void CLASS_STRUCT_MSG_OUT::to_ostream(std::ostream* os_ptr) const
{
  using util::String_view;

  constexpr size_t MAX_SZ = 256;
  constexpr String_view TRUNC_SUFFIX = "... )"; // Fake the end to look like the end of the real pretty-print.

  auto& os = *os_ptr;

  // This is not a public API but OK to output publicly methinks.
  os << "[n_serialization_segs[" << m_builder.n_serialization_segments() << "] ";

  const auto hndl_or_null = native_handle_or_null();
  if (!hndl_or_null.null())
  {
    // As of this writing it's, like, "native_hndl[<the FD>]" -- that looks pretty good and pithy.
    os << hndl_or_null << ' ';
  }
  // else { No need to output anything; pithier. }

  /* prettyPrint() gives an indented multi-line version; this gives a single-line one.  Seems there's no way to
   * truncate it "on the fly"; a full-printout string must be made first (which is too bad; @todo revisit). */
  const kj::String capnp_str = kj::str(*(body_root()));
  if (capnp_str.size() > MAX_SZ)
  {
    os << String_view{capnp_str.begin(), MAX_SZ - TRUNC_SUFFIX.size()} << TRUNC_SUFFIX;
  }
  else
  {
    os << capnp_str.cStr();
  }

  os << "]@" << this;
} // Msg_out::to_ostream()

template<typename Body_t, typename Struct_builder_t>
std::ostream& operator<<(std::ostream& os, const Msg_out<Body_t, Struct_builder_t>& val)
// Doxygen 1.9.4 gets confused here with *_STRUCT_MSG_OUT; avoid 'em to work-around it.  @todo Revisit with later ver.
{
  val.to_ostream(&os);
  return os;
}

// Msg_in template implementations.

TEMPLATE_STRUCT_MSG_IN
CLASS_STRUCT_MSG_IN::Msg_in(const Reader_config& struct_reader_config) :
  m_total_sz(0),
  m_body_reader(struct_reader_config),
  m_mdt_deserialized_ok(false),
  m_body_deserialized_ok(false)
  // m_session_token is uninitialized garbage.
{
  // That's it: need to feed segment(s) into m_body_reader before can deserialize anything.
}

TEMPLATE_STRUCT_MSG_IN
void CLASS_STRUCT_MSG_IN::store_native_handle_or_null(Native_handle&& native_handle_or_null)
{
  assert(m_hndl_or_null.null() && "Call this at most once (probably upon finalizing 1st segment as well).");

  m_hndl_or_null = std::move(native_handle_or_null);
}

TEMPLATE_STRUCT_MSG_IN
CLASS_STRUCT_MSG_IN::~Msg_in() = default;

TEMPLATE_STRUCT_MSG_IN
void CLASS_STRUCT_MSG_IN::add_serialization_segment(Segment_blob_in&& blob)
{
  assert((!m_body_deserialized_ok)
         && "Do not call add_serialization_segment() after both deserialize_*().");

  m_total_sz += blob.size();

  if (!m_mdt_header_area.data())
  {
    /* This is (sub-)segment 1.  Grab the location and size (actually more than its real size; that's fine)
     * of the mdt header which precedes the actual (sub-)segment 1 if any. */
    m_mdt_header_area = blob.const_buffer();

    /* We got that, and only we care about it; m_body_reader does not and merely promises not to blow away
     * the underlying memory until its own destruction.  So now we massage `blob`, so that it starts past the
     * header.  To the extent m_body_reader must perform further deserialization, it'll now ignore the header
     * (other than letting it exist). */
    constexpr auto HDR_SZ = BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL;
    assert((blob.start() == 0)
           && "We haven't chged start() yet; why is it framing something already?  Bug?");
    /* Subtlety: If blob is somehow too small for the header even, it's an error, but for now we'll just let it go
     * and leave it to deserialize_mdt() which will be actually analyzing contents of area m_mdt_header_area.  We can't
     * emit any error in any case.  So for now just protect against a weird .start_past_prefix() call via min(). */
    blob.start_past_prefix(std::min(size_t(HDR_SZ), blob.size()));
  }
  // else { It is (sub-)seg 2+; they don't have headers. }

  m_body_reader.add_serialization_segment(std::move(blob));
} // Msg_in::add_serialization_segment()

TEMPLATE_STRUCT_MSG_IN
size_t CLASS_STRUCT_MSG_IN::serialization_size() const
{
  return m_total_sz;
}

TEMPLATE_STRUCT_MSG_IN
size_t CLASS_STRUCT_MSG_IN::deserialize_mdt(flow::log::Logger* logger_ptr, Error_code* err_code,
                                            Channel_stats::Msg* stats_rcv_msg_internal_only)
{
  using util::String_view;
  using flow::util::ostream_op_string;
  using boost::endian::little_to_native;
  using std::exception;

  assert(err_code);
  assert(stats_rcv_msg_internal_only && "For code simplicity we assume this is always of interest for now.");
  assert((!m_mdt_reader) && "Do not call deserialize_mdt() after it returns.");
  assert(m_mdt_header_area.data()
         && "Do not call deserialize_mdt() unless you've called add_serialization_segment() once.");
  /* Also it must not be called after a_s_s() 2; always between 1 and 2 (by contract).
   *
   * @todo Check it somehow.  Though officially it's undefined behavior, so we aren't obligated to check it.
   * ...Really it would be tighter if deserialize_{body|mdt}() weren't APIs but simply happened automatically
   * after each appropriate a_s_s() call.  Only problem is Error_code emission; but a_s_s() could have that...
   * though only the 1st and last one would ever emit; which is a bit "loose" after all.  Revisit.  It's internal
   * code, though; we don't have to precious about such things (but it's nice to be tight). */

  auto& msg_if_internal = *stats_rcv_msg_internal_only;

  /* Let's recap the situation.  add_serialization_segment() 1 was called before us.  m_mdt_header_area describes
   * the header area (plus more, which we ignore) ahead of any actual (sub-)seg 1; which has itself
   * been saved in its permanent owner, m_builder.  That guy's [begin(), end()) range is just past the mdt-header,
   * size HDR_SZ (see below) at most.  So everything is cool already; except we must now
   * interpret the mdt-header for various key info, including whether there *is* a user-message body
   * (otherwise it's an internal message). */

  // Error helper.
  const auto error_out = [&](String_view msg) -> size_t
  {
    {
      FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT); // Log on errors (at least).
      FLOW_LOG_WARNING(msg);
    }

    *err_code = error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA;
    m_mdt_deserialized_ok = false; // Mark us as failed again.
    return 0;
  }; // const auto error_out =

  /* See add_serialization_segment(): if something went wrong, the header might not be sufficiently big.
   * Won't m_mdt_reader-> accessors below catch that?  Answer: well, maybe.  What if the header itself is
   * deserializable despite the unexpected size, so the mdt deserialization works fine?  Then the next step
   * would be, at least possibly, deserializing the body whose first (sub-)seg is supposed to lie just past
   * the header-reserved area.  This is now nonsense, as there's nothing there.
   *
   * So in short... just make sure there's at least the full reserved area for the header. */
  constexpr auto HDR_SZ = BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL;
  if (m_mdt_header_area.size() < HDR_SZ)
  {
    return error_out(ostream_op_string("In-message contains (possibly valid) mdt-header "
                                       "but there are not enough bytes to hold the entire reserved header area: "
                                       "The entire blob is not even max-mdt-header-size [", HDR_SZ, "] long but "
                                       "rather sized only [", m_mdt_header_area.size(), "].  Other side misbehaved?"));
  }
  // else

  /* Set up the super-simple/fast capnp::MessageReader for the mdt-header.  Don't bother decreasing its .size()
   * to just past the mdt-header; to m_mdt_reader anything past the mdt-header serialization is ignored garbage.
   * It doesn't affect correctness or perf. */
  m_mdt_reader.emplace(m_mdt_header_area);

  // Grab the StructuredMessage::Reader of the mdt-header.
  try
  {
    m_mdt_root = m_mdt_reader->getRoot<Mdt>();
  }
  catch (const exception& exc)
  {
    return error_out(ostream_op_string("In-mdt-header could not be capnp-deserialized; capnp threw exception [",
                                       exc.what(), "].  Other side misbehaved?"));
  }
  // Got here: Now the mdt-related accessors will work (including for us below).

  m_mdt_deserialized_ok = true;

  /* Refer to structured_msg.capnp StructuredMessage.  m_mdt_root is ready for us.  Let's go over it:
   *   - authHeader.sessionToken: We decode it here and save it (for accessor perf).
   *   - id: It's accessible through accessor id_or_none() (perf is good enough to not need caching).
   *     However we check it for correctness and emit error if bad.
   *   - originatingMessageIdOrNone: Ditto; though there are no invalid values => no error applicable (0 means this
   *     not a response).
   *   - internalMessageBody: Present if and only if there will be no m_body_root (no user message, just mdt). */

  // Deal with sessionToken.  Decode as mandated in .capnp Uuid doc header.  @todo Factor this out into a util method.

  if (!m_mdt_root.hasAuthHeader())
  {
    return error_out("In-mdt-header has null .authHeader.  Other side misbehaved?");
  }
  // else
  const auto auth_header = m_mdt_root.getAuthHeader();

  if (!auth_header.hasSessionToken())
  {
    return error_out("In-mdt-header has null .authHeader.  Other side misbehaved?");
  }
  // else

  const auto capnp_uuid = auth_header.getSessionToken();
  static_assert(decltype(m_session_token)::static_size() == 2 * sizeof(uint64_t),
                "World is broken: UUIDs expected to be 16 bytes!");
  auto& first8 = *(reinterpret_cast<uint64_t*>(m_session_token.data())); // m_session_token is aligned, so this is too.
  auto& last8 = *(reinterpret_cast<uint64_t*>(m_session_token.data() + sizeof(uint64_t))); // As is this.
  first8 = little_to_native(capnp_uuid.getFirst8()); // Reminder: Likely no-op + copy of uint64_t.
  last8 = little_to_native(capnp_uuid.getLast8()); // Ditto.

  const auto id_or_0 = id_or_none();
  if (m_mdt_root.isInternalMessageBody())
  {
    if (id_or_0 != 0)
    {
      return error_out("In-mdt-header top union specifies .internalMessageBody; but .id=/=0, the sentinel value.  "
                       "Other side misbehaved?");
    }
    // else

    /* And that's that.  Stuff like IDs may still be wrong compared to preceding messages, which we can't check yet.
     * struc::Channel will before any emission of anything to user. */
    assert(!*err_code);

    /* Stats: internal msg is always single-seg, single-blob.  We will not do deserialize_body() (there is no body),
     * so we update these now in this case as advertised. */
    {
      ++msg_if_internal.m_internal_msgs;
      ++msg_if_internal.m_single_segment_msgs;
      ++msg_if_internal.m_total_segments;
      ++msg_if_internal.m_total_low_lvl_blobs;
      msg_if_internal.m_histo_msg_sz.record_value(m_total_sz);
    }
    return 0; // Means we're an internal message.
  }
  // else if (!.isInternalMessageBody()):

  // Very exciting... we have the medatata, and it indicates this is part of user message (not internal message).

  if (id_or_0 == 0)
  {
    return error_out("In-mdt-header top union specifies no .internalMessageBody; but .id=0, the sentinel value.  "
                     "Other side misbehaved?");
  }
  // else

  const size_t n_body_segs = m_mdt_root.getBodySerializationInfo().getNumBodySerializationSegments();
  if (n_body_segs == 0)
  {
    return error_out("In-mdt-header top union specifies no .internalMessageBody; and .id=0, the sentinel value; "
                     "but body-segment-count is 0 which is illegal.  Other side misbehaved?");
  }
  // else

  return n_body_segs; // And that's that.  (Same comment as above in internal-message case.)
} // Msg_in::deserialize_mdt()

TEMPLATE_STRUCT_MSG_IN
void CLASS_STRUCT_MSG_IN::deserialize_body(Error_code* err_code, Channel_stats::Msg* stats_rcv_msg)
{
  assert(err_code);
  assert(m_mdt_deserialized_ok && "Do not call deserialize_body() until deserialize_mdt() succeeds.");
  assert((id_or_none() != 0) && "Do not call deserialize_body() on internal messages.");
  assert((!m_body_deserialized_ok) && "Do not call deserialize_body() after it returns.");
  assert(stats_rcv_msg && "For code simplicity we assume this is always of interest for now.");

  auto& msg = *stats_rcv_msg;
  const auto body_serialization_info = m_mdt_root.getBodySerializationInfo();
  // Careful; we will modify this; at first it's post-split blobs; then it becomes pre-split segments.
  size_t n_body_segs = body_serialization_info.getNumBodySerializationSegments();

  // Stats.  There's more below, but use n_body_segs before it is "corrected" into an actual segment count.
  msg.m_total_low_lvl_blobs += n_body_segs;

  /* We have the segments, but to deserialize we may need to reassemble split-segments (context: rarely but
   * possibly sometimes, though for e.g. Reader being shm::Reader it would be never).  So as
   * emit_serialization()/load_mdt() makes a capnp-List from a Split_segments, we do the opposite.
   * (We could have done so in deserialize_mdt() and saved it in an m_, but we're only going to be called once,
   * and nothing else needs that info, so why keep more state?) */

  if (body_serialization_info.hasSplitSegmentsOrNull())
  {
    const auto split_segs_list = body_serialization_info.getSplitSegmentsOrNull();
    const auto n_split_segs = split_segs_list.size(); // (0 is illegal, but just let m_body_reader deal with it.)

    Split_segments split_segs;
    split_segs.reserve(n_split_segs);
    for (size_t idx = 0; idx != n_split_segs; ++idx)
    {
      auto split_segs_list_element = split_segs_list[idx];
      const auto n_cont = split_segs_list_element.getNContSubsegs();
      split_segs.emplace_back(Split_segment{ size_t(split_segs_list_element.getStartIdx()),
                                             size_t(n_cont) });

      // Stats.
      msg.m_histo_split_blobs_per_seg.record_value(n_cont + 1); // E.g.: 2 continuation blobs <=> 3 blobs.
      /* Progressive "correct" n_body_segs.
       * E.g.: 3 blobs post-split = one segment <=> n_cont = 2.  So: 3-2=1 actual segment.  Hence -= 2.*/
      n_body_segs -= n_cont;
    }

    m_body_root = m_body_reader.template deserialization<Body>(&split_segs, // Won't change split_segs; don't worry.
                                                               err_code);
    if (!*err_code) // We don't have to gate this, but we can; it's somewhat convenient for the assert().
    {
      assert((n_split_segs != 0) && "If other side messed up and set that to zero, err_code would reflect that now.");
      ++msg.m_msgs_with_split_segments; // For sure splitting occurred.
    }
  } // if (body_serialization_info.hasSplitSegmentsOrNull())
  else
  {
    // Fast-path (should generally be the norm): no split_segs -- no reassembly required.
    m_body_root = m_body_reader.template deserialization<Body>(nullptr, err_code);
  }

  if (*err_code)
  {
    return;
  }
  // else: Now body_root() works.
  m_body_deserialized_ok = true;

  /* Stats (all but split-related and m_total_low_lvl_blobs, which is also split-related really).
   *
   * Reminder: In our contract, and in stat-keeping generally, we have allowed ourselves in the event
   * of catastrophic error (*err_code becoming truthy above is indeed that) for some stats to be recorded
   * but not necessarily all. */
  {
    ++msg.m_user_msgs;
    /* On receive side m_requests/m_requests_one_off/m_request_responses are always 0 (can't distinguish
     * requests from notifications).  All user messages count as notifications.  m_notification_responses
     * counts those with originating_msg_id (receiver *can* see this). */
    ++msg.m_notifications;
    (originating_msg_id_or_none() == 0) || (++msg.m_notification_responses);
    m_hndl_or_null.null() || (++msg.m_handle_bearing_msgs);
    msg.m_total_segments += n_body_segs;
    ++((n_body_segs == 1) ? msg.m_single_segment_msgs : msg.m_multi_segment_msgs);
    msg.m_histo_msg_sz.record_value(m_total_sz);
  }
} // Msg_in::deserialize_body()

TEMPLATE_STRUCT_MSG_IN
msg_id_t CLASS_STRUCT_MSG_IN::id_or_none() const
{
  assert(m_mdt_deserialized_ok && "Call deserialize_mdt() successfully before calling accessors.");
  return m_mdt_root.getId();
}

TEMPLATE_STRUCT_MSG_IN
msg_id_t CLASS_STRUCT_MSG_IN::originating_msg_id_or_none() const
{
  assert(m_mdt_deserialized_ok && "Call deserialize_mdt() successfully before calling accessors.");
  return m_mdt_root.getOriginatingMessageIdOrNone(); // 0 means none.
}

TEMPLATE_STRUCT_MSG_IN
const Session_token& CLASS_STRUCT_MSG_IN::session_token() const
{
  assert(m_mdt_deserialized_ok && "Call deserialize_mdt() successfully before calling accessors.");
  return m_session_token;
}

TEMPLATE_STRUCT_MSG_IN
typename CLASS_STRUCT_MSG_IN::Internal_msg_body_reader CLASS_STRUCT_MSG_IN::internal_msg_body_root() const
{
  assert(m_mdt_deserialized_ok && "Call deserialize_mdt() successfully before calling accessors.");
  assert((id_or_none() == 0) && "Access internal_msg_body_root() only if `id_or_none() == 0`.");
  return m_mdt_root.getInternalMessageBody();
}

TEMPLATE_STRUCT_MSG_IN
const typename CLASS_STRUCT_MSG_IN::Body_reader& CLASS_STRUCT_MSG_IN::body_root() const
{
  assert(m_body_deserialized_ok && "Call deserialize_body() successfully before calling body_root() accessor.");
  return m_body_root;
}

TEMPLATE_STRUCT_MSG_IN
const typename CLASS_STRUCT_MSG_IN::Mdt_reader& CLASS_STRUCT_MSG_IN::mdt_root() const
{
  assert(m_mdt_deserialized_ok && "Call deserialize_mdt() successfully before calling mdt_root() accessor.");
  return m_mdt_root;
}

TEMPLATE_STRUCT_MSG_IN
Native_handle CLASS_STRUCT_MSG_IN::emit_native_handle_or_null()
{
  return Native_handle{std::move(m_hndl_or_null)};
}

TEMPLATE_STRUCT_MSG_IN
void CLASS_STRUCT_MSG_IN::to_ostream(std::ostream* os_ptr) const
{
  using util::String_view;

  constexpr size_t MAX_SZ = 256;
  constexpr String_view TRUNC_SUFFIX = "... )"; // Fake the end to look like the end of the real pretty-print.

  auto& os = *os_ptr;

  os << '[';

  if (!m_hndl_or_null.null())
  {
    // As of this writing it's, like, "native_hndl[<the FD>]" -- that looks pretty good and pithy.
    os << m_hndl_or_null << ' ';
  }
  // else { No need to output anything; pithier. }

  if (m_mdt_deserialized_ok)
  {
    const auto id_or_0 = id_or_none();
    if (id_or_0 == 0)
    {
      /* Internal message.  Might as well simply print the entire metadata-header; it's all interesting, and
       * an internal message never reaches the user; so internal code may well want to print all this.
       * Plus there's simply nothing else to print, at all, so it's complete.
       * As in Msg_out::to_ostream() use the non-indent/newline pretty-print but no truncation needed. */
      os << ::kj::str(internal_msg_body_root()).cStr();
    }
    else // if (id_or_0 != 0)
    {
      /* User message.  We should be somewhat judicious as to what to print; it does not strictly have to be
       * stuff available via public APIs -- but don't go overboard either.  E.g., session token is of low interest
       * and takes space more than anything and is an internal detail; but message ID and originating message ID
       * (if any) both help correlate messages versus other messages. */
      os << "id[" << id_or_0 << "] ";
      const auto originating_msg_id_or_0 = originating_msg_id_or_none();
      if (originating_msg_id_or_0 != 0)
      {
        os << "rsp_to_id[" << originating_msg_id_or_0 << "] ";
      }

      if (m_body_deserialized_ok)
      {
        // Similarly to Msg_out::to_ostream() print the body but truncated if needed.  @todo Code reuse?
        const kj::String capnp_str = kj::str(body_root());
        if (capnp_str.size() > MAX_SZ)
        {
          os << String_view{capnp_str.begin(), MAX_SZ - TRUNC_SUFFIX.size()} << TRUNC_SUFFIX;
        }
        else
        {
          os << capnp_str.cStr();
        }
      }
      else // if (!m_body_deserialized_ok)
      {
        os << " ( incomplete )"; // Perhaps we're being printed internally, before deserialize_body().
      }
    } // else if (id_or_0 != 0)
  }
  else // if (!m_mdt_deserialized_ok)
  {
    os << "( incomplete )"; // Perhaps we're being printed internally, before deserialize_mdt().
  }

  os << " sz[" << serialization_size() << "]]@" << this;
} // Msg_in::to_ostream()

TEMPLATE_STRUCT_MSG_IN
std::ostream& operator<<(std::ostream& os, const CLASS_STRUCT_MSG_IN& val)
{
  val.to_ostream(&os);
  return os;
}

#undef TEMPLATE_STRUCT_MSG_OUT
#undef CLASS_STRUCT_MSG_OUT
#undef TEMPLATE_STRUCT_MSG_IN
#undef CLASS_STRUCT_MSG_IN

} // namespace ipc::transport::struc
