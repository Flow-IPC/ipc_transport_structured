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
#include "ipc/transport/asio_local_stream_socket_fwd.hpp"
#include "ipc/util/native_handle.hpp"
#include <boost/endian.hpp>

namespace ipc::transport::struc
{

// Types.

/**
 * A structured out-message suitable to be sent via struc::Channel::send().  Publicly this can be constructed
 * directly; or via struc::Channel::create_msg().  The latter has the advantage of reusing the serialization
 * engine knobs registered with the channel and hence is simply easier to write.  If one uses the direction-construction
 * route, the builder (serialization) engine type -- Struct_builder #Builder -- must match that of the
 * struc::Channel used to `send()` a `*this`; otherwise it simply will not compile.
 *
 * @see "Lifecycle of an out-message" section of struc::Channel class doc header for useful context.
 *
 * As explained in that doc section, a Msg_out (struc::Channel::Msg_out) is akin to a container,
 * like `vector<uint8_t>`; and the #Builder template param + ctor arg are akin to the allocator
 * used by such a container.  A given `*this` represents the message payload; it does not represent a particular
 * `send()`t (past/present/future) *instance* of that payload.  Modifying it is akin to modifying the
 * `vector<uint8_t>`, albeit in structured-schema fashion -- available API controlled by `Message_body` template
 * param -- and with an optionally attached #Native_handle.
 *
 * As explained in that doc section, like any container, once another process has access to it -- which is
 * only possible in read-only fashion as of this writing -- modifying it must be done with concurrency/synchronization
 * considered.  If the compile-time-chosen #Builder is SHM-based, then this is a concern.
 * If it is heap-based (notably Heap_fixed_builder), then it is not: the serialization is *copied* into
 * and out of the transport.
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
 * ### Resource use (RAM) ###
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
 * ### Impl notes ###
 * Some important aspects of this class template's API, which must be accessed by the ipc::transport (structured
 * layer) internals only, are `protected`.  struc::Channel then sub-classes this guy and exposes them publicly
 * (to itself).  This avoids `friend` which would would be not quite as clean, since `private` parts would be
 * exposed too, and so on.  In other words this is a class an API -- not a mere data-store -- but there is
 * an internally used API which is left `protected`.  As of this writing it's essentially emit_serialization()
 * which the user does not access directly (struc::Channel::send() internals do).
 *
 * The internally-used APIs are reasonably self-explanatory, so just see their doc headers.
 * @endinternal
 *
 * @tparam Message_body
 *         See struc::Channel.
 * @tparam Struct_builder_t
 *         See struc::Channel.
 */
template<typename Message_body, typename Struct_builder_t>
class Msg_out
{
public:
  // Types.

  /// See struc::Channel::Msg_body.
  using Body = Message_body;

  /// Short-hand for capnp-generated mutating `Builder` nested class of #Body.  See body_root().
  using Body_builder = typename Body::Builder;

  /**
   * Short-hand for user-specified Struct_builder type.  An internally stored instance of this contains the user
   * payload.
   */
  using Builder = Struct_builder_t;

  /**
   * Short-hand for user-specified Struct_builder::Config.  One can construct a #Builder via
   * `Builder(const Builder_config&)`.
   */
  using Builder_config = typename Builder::Config;

  /// Short-hand for capnp orphan factory.  See orphanage().
  using Orphanage = ::capnp::Orphanage;

  // Constructors/destructor.

  /**
   * Creates blank #Body-bearing out-message message, with no native handle,
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
   *   - Heap-backed:
   *     - If you have a target struc::Channel::Owned_channel: via `static`
   *       struc::Channel::heap_fixed_builder_config().
   *     - Otherwise: session::Session_mv::heap_fixed_builder_config() (`static` or non-`static`).
   *   - SHM-backed (e.g., SHM-classic):
   *     - session::shm::classic::Session_mv::session_shm_builder_config(),
   *       session::shm::classic::Session_mv::app_shm_builder_config().  Requires a `Session` object.
   *     - session::shm::classic::Session_server::app_shm_builder_config() (requires session::Client_app).
   *       Server-only; if a `Session` is not available or applicable.
   *
   * @param struct_builder_config
   *        See above.  This is neither memorized nor copied.
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
   * delegating to the present ctor: `Msg_out(Builder(struct_builder_config))`.
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
   * Store the `Native_handle` (potentially `.null()`, meaning none) in this out-message; no-ops if the same
   * `.m_native_handle` already stored; otherwise returns the previously stored native handle to the OS
   * (similarly to dtor).
   *
   * @param native_handle_or_null
   *        The `Native_handle` (`.null() == true` if none) to move into `*this`.  Made `.null() == true` upon return.
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

protected:
  // Methods.

  /**
   * Returns the serialization in the form of a sequence of 1+ `Blob`s.
   * It is meant to be invoked at struc::Channel::send() time.
   *
   * It can be invoked repeatedly: `session` can specify a different destination each time; or the same
   * destination each time; or an arbitrary mix thereof.  In particular struc::Channel::send() (and its
   * derivatives struc::Channel::sync_request(), struc::Channel::async_request()) shall each time specify the
   * channel's opposing process as the destination -- regardless of which struc::Channel's
   * struc::Channel::create_msg() originally generated `*this` (assuming that method was even used for
   * construction of `*this` -- in no way required, just often convenient).
   *
   * @see our class doc header and/or "Lifecycle of an out-message" section of struc::Channel class doc
   *      header.  Recall that where it mentions struc::Channel::send(), you can understand
   *      there a 1-1 relationship with a synchronous call to `this->emit_serialization()`.
   *
   * ### Errors ###
   * See Struct_builder::emit_serialization().  This essentially forwards to that and emits its errors if any.
   * Out-arg `native_handle_or_null` is untouched on error.
   *
   * @param target_blobs
   *        On success (no error emitted) this is cleared and replaced with the sequence of segments as `Blob`s.
   * @param session
   *        Specifies the opposing recipient for which the serialization is intended.
   *        If `Builder::Session` is Null_session, then `Session()` is the only value to supply here.  Otherwise more
   *        information is necessary.
   * @param err_code
   *        See `flow::Error_code` docs for error reporting semantics.  See above.
   */
  void emit_serialization(Segment_ptrs* target_blobs, const typename Builder::Session& session,
                          Error_code* err_code) const;

  /**
   * Returns what `target_blobs.size()` would return after calling `emit_serialization(&target_blobs)` (with
   * an empty `target_blobs` going-in), right now.
   *
   * @return See above.
   */
  size_t n_serialization_segments() const;

private:
  // Data.

  /// The guy serializing a #Body.  Underlying serialization potentially futher mutated by user via body_root().
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

  /// The `Native_handle`, if any, embedded inside this message.
  Native_handle m_hndl_or_null;
}; // class Msg_out

/**
 * A structured in-message *instance* suitable as received and emittable (to user) by struc::Channel. Publicly
 * these are never constructed but emitted into `Channel`-passed handlers, wrapped into `shared_ptr`
 * struc::Channel::Msg_in_ptr.  These handlers include struc::Channel::expect_msg(),
 * struc::Channel::expect_msgs(), and struc::Channel::async_request() and struc::Channel::sync_request().
 * Once available to the user, one accesses the in-place (zero-copy) capnp serialization via body_root(),
 * via which one can access the message's read-only contents.
 *
 * @see Msg_out
 * @see "Lifecycle of an out-message" section of struc::Channel class doc header for useful context.
 *      Note that for each Msg_out, there will exist 0+ `Msg_in`s, one per
 *      successful `send()` paired with emission to user via aformentioned handlers.  The latter represent
 *      each instance of the original message being received; the former represents the original message.
 *      Hence Msg_in access is read-only (`Reader` access only); Msg_out access
 *      is read/write (`Builder` access).

 * ### Resource use (RAM) ###
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
 * Even if `*this` stores a non-null #Native_handle, it will never return it to the OS on your behalf.
 * You may do so if and when desired.  This is different from Msg_out which "owns" its copy
 * and will return it to the OS at destruction.
 *
 * @internal
 * ### Impl notes ###
 * This follows the same facade-API pattern as Msg_out; see similar place in its doc header.
 * Same applies here.  However: Msg_in represents an *instance* of a received message.  Therefore it
 * contains *both*
 *   - metadata-message about the associated user-message, if any, or the internal message; message ID,
 *     originating message ID, etc.; and
 *   - the user message (if any).
 *
 * So, in fact, on the sender side struc::Channel maintains the user's Msg_out (if any) and
 * a user-invisible (created at send() time) Msg_mdt_out, which is just a somewhat-decorated-API
 * Msg_out over an internal-use schema.  On the receiver side Msg_in stores
 * *both* counterpart read-only deserializations.
 *   - body_root() provides `public` user access to the user body;
 *   - `protected` accessors provide access to the other stuff, like id_or_none() and originating_msg_id_or_none(),
 *     which struc::Channel uses to maintain operation.  These are `public`ly exposed via Msg_out_impl.
 *
 * In the case of an internal message, a `*this` is never emitted to the user.  In that case
 * internal_msg_body_root() provides internal access to the message.
 *
 * @endinternal
 *
 * @tparam Message_body
 *         See Msg_in: this is the counterpart.
 * @tparam Struct_reader_config
 *         See Msg_out: this is the counterpart.
 */
template<typename Message_body, typename Struct_reader_config>
class Msg_in :
  private boost::noncopyable
{
public:
  // Types.

  /// See struc::Channel::Msg_body.
  using Body = Message_body;

  /// Short-hand for capnp-generated read-only-accessing `Reader` nested class of #Body.  See body_root().
  using Body_reader = typename Body::Reader;

  /// See struc::Channel::Reader_config.
  using Reader_config = Struct_reader_config;

  // Constructors/destructor.

  /**
   * Returns resources, potentially including potentially significant RAM resources, taken before emitting to the user.
   * native_handle_or_null(), even if not `.null()`, is not returned to the OS.
   *
   * See class doc header for discussion.
   */
  ~Msg_in();

  /// Methods.

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
   * @return See above.
   */
  const Body_reader& body_root() const;

  /**
   * The #Native_handle -- potentially null meaning none -- embedded in this message.
   *
   * @return See above.
   */
  Native_handle native_handle_or_null() const;

  /**
   * Prints string representation to the given `ostream`.  This representation lacks newlines/indentation;
   * includes a (potentially truncated) pretty-printed representation of `body_root()` contents; and includes
   * native_handle_or_null().
   *
   * Caution: This could be an operation expensive in processor cycles and, temporarily, RAM; and thus should
   * be used judiciously.  To help drive your decision-making: This method, internally,
   *   -# uses capnp `kj::str(this->body_root)` to generate a *full* pretty-print of body_root() contents;
   *   -# truncates the result of the latter, as needed for a reasonably short output, and prints the result;
   *   -# adds native_handle_or_null() and possibly a few other small information items.
   *
   * Because there is no reasonably-available way to stop generating the pretty-print in step 1 upon reaching
   * a certain number of characters, the operation may take a while, if many non-default bytes have been
   * mutated-in; and before truncation the resulting string may take significant RAM accordingly.
   *
   * @internal
   * If used by internal code before successful deserialize_mdt() and/or before successful deserialize_body()
   * (if one would even be possible in the first place -- not so with internal messages), this method will print
   * useful things.  Moreover the above public-facing description is no longer complete in that case.
   * In any case the method remains suitable for TRACE-logging but not INFO-logging by internal code, at least
   * in the per-message common code path.
   * @endinternal
   *
   * @param os
   *        Stream to which to write.
   */
  void to_ostream(std::ostream* os) const;

protected:
  // Types.

  /// `Reader` counterpart to Msg_mdt_out::Internal_msg_body_builder.
  using Internal_msg_body_reader = typename schema::detail::StructuredMessage::InternalMessageBody::Reader;

  /// Same as `Msg_mdt_out::Body`.
  using Mdt = schema::detail::StructuredMessage;

  /// Same as `Msg_mdt_out::Body_builder` but the `Reader` instead.
  using Mdt_reader = typename Mdt::Reader;

  // Constructors/destructor.

  /**
   * Constructs a not-ready-for-public-consumption in-message which awaits serialization-storing segments to be
   * added via add_serialization_segment() and then finalized with deserialize_mdt() and possibly deserialize_body().
   * After the latter 1-2: body_root() or internal_msg_body_root() can be used to access the deserialized data; as can
   * id_or_none() and similar accessors.
   *
   * @param struct_reader_config
   *        See struc::Channel ctors.  This is copied, memorized.
   */
  explicit Msg_in(const Reader_config& struct_reader_config);

  // Methods.

  /**
   * Store the #Native_handle (potentially `.null()`, meaning none) in this in-message.  Call this at most once;
   * or behavior undefined (assertion may trip).
   *
   * @param native_handle_or_null
   *        The #Native_handle (`.null() == true` if none) to move into `*this`.  Made `.null() == true` upon return.
   */
  void store_native_handle_or_null(Native_handle&& native_handle_or_null);

  /**
   * Prior to `deserialization_*()` obtains a memory area `max_sz` bytes long into which the user may write-to
   * until the next add_serialization_segment(), `deserialization_*()`, or dtor call (whichever happens first);
   * returns a pointer to that area as described by the pointed-to `Blob`'s [`begin()`, `end()`) range.
   * If the reader impl decides `max_sz` bytes are not available, returns null.  `*this` shall not be used
   * subsequent to such an eventuality.
   *
   * This essentially forwards to the appropriate Struct_reader::add_serialization_segment(); hence the same
   * requirements, including w/r/t alignment and subsequent storing-into and modification of returned `Blob`,
   * apply as described in that doc header.  That said:
   *   - The first add_serialization_segment() call must be the *sole* segment of the metadata
   *     message (corresponding to Msg_mdt_out).  After the returned `Blob` is filled-out and `.resize()`d,
   *     you must invoke deserialize_mdt() which shall return the number of additional
   *     add_serialization_segment() calls to make.  If that number is 0 (<=> id_or_none() is 0 <=> there is an
   *     internal message in the Msg_mdt_out), then there shall be no further add_serialization_segment() calls;
   *     internal_msg_body_root() can be used.  Otherwise:
   *   - The following add_serialization_segment() calls (the 2nd, 3rd, ...) ones shall be, in order, comprised by
   *     the user-message serialization (corresponding to `Msg_out<Message_body>`).  After each call
   *     fill-out and `.resize()` the returned `Blob`.  After the last call call deserialize_body(); then
   *     body_root() can be used.
   *
   * @param max_sz
   *        See Struct_reader::add_serialization_segment().
   * @return See Struct_reader::add_serialization_segment().
   */
  flow::util::Blob* add_serialization_segment(size_t max_sz);

  /**
   * To be invoked after exactly one successful add_serialization_segment() call (and that `Blob` being filled-out
   * and `.resize()`d): finalizes the deserialization of everything except the potential user-message body.
   * This must be called strictly before any calls to internal_msg_body_root(), id_or_none(),
   * originating_msg_id_or_none(), or session_token(); and any subsequent add_serialization_segment()
   * calls (if any).
   *
   * The value this returns (sans error) dictates the exact # of further add_serialization_segment() calls to make.
   *   - If 0: You may use the accessor API immediately.  internal_msg_body_root() is OK; `id_or_none() == 0`;
   *     body_root() may not be used.
   *   - Else: You may use the accessor API immediately.  `id_or_none() != 0`; internal_msg_body_root() may not be
   *     used; body_root() may not be used *yet* until successful deserialize_body().
   *
   * If called before add_serialization_segment(), behavior is undefined (assertion may trip).
   *
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
   * @return See above.
   */
  size_t deserialize_mdt(flow::log::Logger* logger_ptr, Error_code* err_code);

  /**
   * To be invoked after `deserialize_mdt() == N`, and add_serialization_segment() was called N times (with all
   * N `Blob`s filled-out and `.resize()`d): finalizes the deserialization of the user-message body.
   * This must be called strictly before any calls to body_root().
   *
   * If called before a post-deserialize_mdt() add_serialization_segment(), behavior is undefined (assertion may trip).
   *
   * If called more than once, behavior is undefined (assertion may trip).
   *
   * If it fails (emits truthy #Error_code), it is pointless to use `*this`.  Recommend destruction.
   *
   * @param err_code
   *        See deserialize_mdt().
   */
  void deserialize_body(Error_code* err_code);

  /**
   * To be called only after deserialize_mdt(), returns the message ID of this in-message; 0 means it's an internal
   * message (internal_msg_body_root() applies), else it's a user message (body() applies).
   *
   * @return See above.
   */
  msg_id_t id_or_none() const;

  /**
   * To be called only after deserialize_mdt(), returns the message ID of the out-message to which this in-message
   * claims to be responding; or 0 if it is not a response.
   *
   * @return See above.
   */
  msg_id_t originating_msg_id_or_none() const;

  /**
   * To be called only after deserialize_mdt(), similar to body_root() but for the internal-message root.
   * See #Internal_msg_body_reader doc header.
   *
   * @return See above.
   */
  Internal_msg_body_reader internal_msg_body_root() const;

  /**
   * To be called only after deserialize_mdt(), returns session token tagging this in-message.
   *
   * @return See above.
   */
  const Session_token& session_token() const;

  /**
   * The #Mdt root capnp-generated accessor object.  May be useful for, say, pretty-printing it
   * to log (e.g.: `capnp::prettyPrint(M.mdt_root()).flatten().cStr()`).  We do not recommend its use for
   * other purposes; stylistically it is better to access items via individual accessors like session_token()
   * or internal_msg_body_root().
   *
   * @return See above.
   */
  const Mdt_reader& mdt_root() const;

private:
  // Types.

  /**
   * Same as Msg_out::Builder but the reader that can decode what that serializing `Builder` did.
   * This deserializes the metadata and (if any) user message.
   */
  using Reader = typename Reader_config::Reader;

  // Data.

  /// See ctor.
  Reader_config m_reader_config;

  /**
   * Deserializes the metadata sub-message, invisible to user: the thing describing the user message
   * (if any) or describing and containing the internal message (otherwise).
   * Essentially: the first add_serialization_segment() forwards to the same
   * method on this #m_mdt_reader.  (We ensure on the opposing side the metadata structure is always 1 segment, no
   * more.)  This stores a few things like message ID; plus possibly an internal-body; otherwise the #
   * of times add_serialization_segment() must be called to complete #m_body_reader serialization.  In the former
   * case #m_body_reader is left null: there is no user message.
   *
   * deserialize_mdt() forwards to `.deserialization()` on #m_mdt_reader.  Then the post-deserialization accessors
   * become available except body_root().
   */
  std::optional<Reader> m_mdt_reader;

  /**
   * Like #m_mdt_reader but for the user message if any.  Essentially: the 2nd, 3rd, ... add_serialization_segment()
   * forward to the same method on this #m_body_reader.  This stores the user-message payload if any.  If none this
   * is left null: there is no user message.
   *
   * deserialize_body() forwards to the same method on #m_body_reader.  Then body_root() becomes available.
   */
  std::optional<Reader> m_body_reader;

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
  template<typename Message_body, typename Struct_builder_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_STRUCT_MSG_OUT \
  Msg_out<Message_body, Struct_builder_t>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define TEMPLATE_STRUCT_MSG_IN \
  template<typename Message_body, typename Struct_reader_config>
/// Internally used macro; public API users should disregard (same deal as in struc/channel.hpp).
#define CLASS_STRUCT_MSG_IN \
  Msg_in<Message_body, Struct_reader_config>

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
  Msg_out(Builder(struct_builder_config))
{
  // OK then.  Mutate away, user.
}

TEMPLATE_STRUCT_MSG_OUT
CLASS_STRUCT_MSG_OUT::Msg_out(Builder&& struct_builder) :

  /* In this more advanced (from user's PoV) ctor form they already are giving us m_builder and guaranteeing
   * its getRoot<Body>() is either already a thing... or it is not root-initialized at all.
   * getRoot<Body>() by us will thus not modify the tree, if it is initialized; or create a blank tree
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
   *     and libipc_* and the test code proper)).
   *   - clang-17 + libc++ (LLVM-10) (note: not GNU stdc++), -O0 or: (-O3 + LTO disabled or LTO (-flto=thin) enabled*
   *     (for all of libflow and libipc_* and the test code proper)).
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
  store_native_handle_or_null(Native_handle());

  // The rest of cleanup is automatic.
}

TEMPLATE_STRUCT_MSG_OUT
CLASS_STRUCT_MSG_OUT& CLASS_STRUCT_MSG_OUT::operator=(Msg_out&& src)
{
  m_builder = std::move(src.m_builder);

  /* I was a bit worried about this (and note this of course similarly auto-occurs in the =default-ed move ctor) but
   * I (ygoldfel) did some due diligence on it:
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

  // The following is why we didn't simply do =default.
  if (m_hndl_or_null != src.m_hndl_or_null)
  {
    // As promised return it (if any) to OS, as that is what would happen if *this were destroyed.
    store_native_handle_or_null(Native_handle());
  }

  m_hndl_or_null = std::move(src.m_hndl_or_null);

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
void CLASS_STRUCT_MSG_OUT::store_native_handle_or_null (Native_handle&& native_handle_or_null)
{
  if (native_handle_or_null == m_hndl_or_null)
  {
    return;
  }
  // else
  if (!m_hndl_or_null.null())
  {
    asio_local_stream_socket::release_native_peer_socket(std::move(m_hndl_or_null)); // OK if it is .null().
  }
  m_hndl_or_null = std::move(native_handle_or_null);
}

TEMPLATE_STRUCT_MSG_OUT
Native_handle CLASS_STRUCT_MSG_OUT::native_handle_or_null() const
{
  return m_hndl_or_null;
}

TEMPLATE_STRUCT_MSG_OUT
void CLASS_STRUCT_MSG_OUT::emit_serialization(Segment_ptrs* target_blobs, const typename Builder::Session& session,
                                              Error_code* err_code) const
{
  m_builder.emit_serialization(target_blobs, session, err_code); // Let it emit error or not.
}

TEMPLATE_STRUCT_MSG_OUT
size_t CLASS_STRUCT_MSG_OUT::n_serialization_segments() const
{
  return m_builder.n_serialization_segments();
}

TEMPLATE_STRUCT_MSG_OUT
void CLASS_STRUCT_MSG_OUT::to_ostream(std::ostream* os_ptr) const
{
  using util::String_view;

  constexpr size_t MAX_SZ = 256;
  constexpr String_view TRUNC_SUFFIX = "... )"; // Fake the end to look like the end of the real pretty-print.

  auto& os = *os_ptr;

  // This is not a public API but OK to output publicly methinks.
  os << "[n_serialization_segs[" << n_serialization_segments() << "] ";

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
    os << String_view(capnp_str.begin(), MAX_SZ - TRUNC_SUFFIX.size()) << TRUNC_SUFFIX;
  }
  else
  {
    os << capnp_str.cStr();
  }

  os << "]@" << this;
} // Msg_out::to_ostream()

TEMPLATE_STRUCT_MSG_OUT
std::ostream& operator<<(std::ostream& os, const CLASS_STRUCT_MSG_OUT& val)
{
  val.to_ostream(&os);
  return os;
}

// Msg_in template implementations.

TEMPLATE_STRUCT_MSG_IN
CLASS_STRUCT_MSG_IN::Msg_in(const Reader_config& struct_reader_config) :
                                       m_reader_config(struct_reader_config),
                                       m_mdt_deserialized_ok(false),
m_body_deserialized_ok(false)
  // m_session_token is uninitialized garbage.
{
  // That's it: need to feed segments into m_*_reader before can deserialize anything.
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
flow::util::Blob* CLASS_STRUCT_MSG_IN::add_serialization_segment(size_t max_sz)
{
  using boost::movelib::make_unique;

  assert((!m_body_deserialized_ok)
         && "Do not call add_serialization_segment() after both deserialize_*().");

  // Fill them out in the order described in their doc headers.

  if (!m_mdt_reader)
  {
    m_mdt_reader.emplace(m_reader_config);
    return m_mdt_reader->add_serialization_segment(max_sz);
  }
  // else
  if (!m_body_reader)
  {
    m_body_reader.emplace(m_reader_config);
  }
  return m_body_reader->add_serialization_segment(max_sz);
} // Msg_in::add_serialization_segment()

TEMPLATE_STRUCT_MSG_IN
size_t CLASS_STRUCT_MSG_IN::deserialize_mdt(flow::log::Logger* logger_ptr, Error_code* err_code)
{
  using util::String_view;
  using boost::endian::little_to_native;

  assert(err_code);
  assert((!m_mdt_deserialized_ok) && "Do not call deserialize_mdt() after it returns.");
  assert(m_mdt_reader && "Must call add_serialization_segment() exactly 1x before deserialize_mdt().");

  m_mdt_root = m_mdt_reader->template deserialization<Mdt>(err_code);
  if (*err_code)
  {
    return 0;
  }
  // else: Now the accessors will work (including for us below).
  m_mdt_deserialized_ok = true;

  FLOW_LOG_SET_CONTEXT(logger_ptr, Log_component::S_TRANSPORT); // Log on errors (at least).

  /* Refer to structured_msg.capnp StructuredMessage.  m_mdt_reader has initialized it for us.  Now to fill it in:
   * Let's go over it:
   *   - authHeader.sessionToken: We decode it here and save it (for accessor perf).
   *   - id: It's accessible through accessor id_or_none() (perf is good enough to not need caching).
   *     However we check it for correctness and emit error if bad.
   *   - originatingMessageOrNull: Basically there's a msg_id_t in there too (ditto, originating_msg_id_or_none()).
   *     However we check it for correctness and emit error if bad.
   *   - internalMessageBody: Present if and only if there will be no m_body_reader. */

  // Deal with sessionToken.  Decode as mandated in .capnp Uuid doc header.  @todo Factor this out into a util method.

  // Error helper.
  const auto error_out = [&](String_view msg) -> size_t
  {
    *err_code = error::Code::S_STRUCT_CHANNEL_INTERNAL_PROTOCOL_MISUSED_SCHEMA;
    FLOW_LOG_WARNING(msg);
    m_mdt_deserialized_ok = false; // Mark us as failed again.
    return 0;
  }; // const auto error_out =

  if (!m_mdt_root.hasAuthHeader())
  {
    return error_out("In-mdt-message has null .authHeader.  Other side misbehaved?");
  }
  // else
  const auto auth_header = m_mdt_root.getAuthHeader();

  if (!auth_header.hasSessionToken())
  {
    return error_out("In-mdt-message has null .authHeader.  Other side misbehaved?");
  }
  // else

  const auto capnp_uuid = auth_header.getSessionToken();
  static_assert(decltype(m_session_token)::static_size() == 2 * sizeof(uint64_t),
                "World is broken: UUIDs expected to be 16 bytes!");
  auto& first8 = *(reinterpret_cast<uint64_t*>(m_session_token.data)); // m_session_token is aligned, so this is too.
  auto& last8 = *(reinterpret_cast<uint64_t*>(m_session_token.data + sizeof(uint64_t))); // As is this.
  first8 = little_to_native(capnp_uuid.getFirst8()); // Reminder: Likely no-op + copy of uint64_t.
  last8 = little_to_native(capnp_uuid.getLast8()); // Ditto.

  // As planned check .originatingMessageOrNull for basic correctness.
  if (m_mdt_root.hasOriginatingMessageOrNull() && (originating_msg_id_or_none() == 0))
  {
    return error_out("In-mdt-message top union specifies .originatingMessageOrNull.id but it is 0.  Responses to "
                     "internal messages (with .id=sentinel) are not allowed.  Other side misbehaved?");
  }
  // else: no problem there.

  const auto id_or_0 = id_or_none();
  if (m_mdt_root.isInternalMessageBody())
  {
    if (id_or_0 != 0)
    {
      return error_out("In-mdt-message top union specifies .internalMessageBody; but .id=/=0, the sentinel value.  "
                       "Other side misbehaved?");
    }
    // else

    /* And that's that.  Stuff like IDs may still be wrong compared to preceding messages, which we can't check yet.
     * struc::Channel will before any emission to user. */
    assert(!*err_code);
    return 0;
  }
  // else if (!.isInternalMessageBody())

  if (id_or_0 == 0)
  {
    return error_out("In-mdt-message top union specifies no .internalMessageBody; but .id=0, the sentinel value.  "
                     "Other side misbehaved?");
  }
  // else

  const size_t n_body_segs = m_mdt_root.getNumBodySerializationSegments();
  if (n_body_segs == 0)
  {
    return error_out("In-mdt-message top union specifies no .internalMessageBody; and .id=0, the sentinel value; "
                     "but body-segment-count is 0 which is illegal.  Other side misbehaved?");
  }
  // else

  // And that's that.  (Same comment as above.)
  return n_body_segs;
} // Msg_in::deserialize_mdt()

TEMPLATE_STRUCT_MSG_IN
void CLASS_STRUCT_MSG_IN::deserialize_body(Error_code* err_code)
{
  assert(err_code);
  assert(m_mdt_deserialized_ok && "Do not call deserialize_body() until deserialize_mdt() succeeds.");
  assert((id_or_none() != 0) && "Do not call deserialize_body() on internal messages.");
  assert((!m_body_deserialized_ok) && "Do not call deserialize_body() after it returns.");
  assert(m_body_reader && "Must call add_serialization_segment() at least once after deserialize_mdt() but before "
                          "deserialize_body().");

  m_body_root = m_body_reader->template deserialization<Body>(err_code);
  if (*err_code)
  {
    return;
  }
  // else: Now body_root() works.
  m_body_deserialized_ok = true;
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
  return m_mdt_root.hasOriginatingMessageOrNull()
           ? m_mdt_root.getOriginatingMessageOrNull().getId()
           : 0;
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
  assert((!m_body_reader) && "Access internal_msg_body_root() only if `id_or_none() == 0`.");
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
Native_handle CLASS_STRUCT_MSG_IN::native_handle_or_null() const
{
  return m_hndl_or_null;
}

TEMPLATE_STRUCT_MSG_IN
void CLASS_STRUCT_MSG_IN::to_ostream(std::ostream* os_ptr) const
{
  using util::String_view;

  constexpr size_t MAX_SZ = 256;
  constexpr String_view TRUNC_SUFFIX = "... )"; // Fake the end to look like the end of the real pretty-print.

  auto& os = *os_ptr;

  os << '[';

  const auto hndl_or_null = native_handle_or_null();
  if (!hndl_or_null.null())
  {
    // As of this writing it's, like, "native_hndl[<the FD>]" -- that looks pretty good and pithy.
    os << hndl_or_null << ' ';
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
          os << String_view(capnp_str.begin(), MAX_SZ - TRUNC_SUFFIX.size()) << TRUNC_SUFFIX;
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

  os << "]@" << this;
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
