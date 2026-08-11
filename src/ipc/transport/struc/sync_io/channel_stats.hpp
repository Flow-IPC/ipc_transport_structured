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
#include <cstdint>
#include <string>
#include <vector>

namespace ipc::transport::struc::sync_io::stat
{

// Types.

/**
 * Cumulative stats from a struc::sync_io::Channel, covering both send and receive directions as well as
 * message-type breakdowns, segment/split details, expect infrastructure, and request-response correlation.
 *
 * A single bidirectional `struct` (not split at the top level) because struc::Channel is one bidirectional object.
 * Sub-structs Snd and Rcv group the directional state; a shared inner struct Msg captures
 * message-level counts identically applicable to either direction.
 *
 * @note The word *segment* in nearby documentation refers to capnp (Cap'n Proto) segments.  Please see
 *       capnp docs (https://capnproto.org/encoding.html) for a refresher on how these work; and perhaps
 *       specifically `capnp::MallocMessageBuilder` for a refresher about exponential segment size growth
 *       and such.  (Flow-IPC may use a different impl, but the idea is similar.)
 *
 * @note On the receive side, Msg::m_requests, Msg::m_requests_one_off, and
 *       Msg::m_request_responses are always zero.  The wire protocol does not distinguish
 *       notifications from requests (that is sender-local knowledge via the presence of `on_rsp_func`).
 *       All non-response user messages are counted as Msg::m_notifications on receive.
 *       This asymmetry is intentional, reserved for future protocol enrichment.
 *
 * @note When applicable and unless stated otherwise, #m_rcv counts et al *do* count messages only cached for
 *       the time being (due to being unsolicited and without an expectation of that Channel::Msg_which_in) or
 *       thrown-out due to being unexpected responses.  That is to say, an #m_rcv in-message need not be delivered
 *       to the user for it to count.
 *
 * @note Unless stated otherwise, stats apply only to the aspects of messages that are transmitted directly via
 *       an IPC channel.  That is if SHM (or some other custom substrate) is involved, the parts stored therein
 *       are not copied; hence (e.g.) Msg::m_total_segments will be incremented by only 1 -- a (small) segment is
 *       always enough to transmit a SHM-handle -- even if the serialization in SHM comprises many megabytes
 *       and several exponentially capnp-segments arranged from small to large in exponential fashion.
 *       For SHM-backed messaging, there are additional stats available which are so marked.
 */
struct Channel_stats
{
  // Types.

  /**
   * Per-message stats shared between send and receive directions.  An instance appears as
   * `Snd::m_msg` and `Rcv::m_msg`.  Sender counts at send time; receiver counts at receipt time.
   * Segment/blob stats apply to all messages (internal + user) since both paths flow through
   * `send_core()` / the receive pipeline.
   */
  struct Msg
  {
    // Constants.

    /// Number of histogram buckets for #m_histo_split_blobs_per_seg.  Covers 2, 3, 4, 5+ blobs per split segment.
    static constexpr size_t S_HISTO_SPLIT_BLOBS_N_BUCKETS = 4;
    /// Number of histogram buckets for #m_histo_msg_sz.
    static constexpr size_t S_HISTO_MSG_SZ_N_BUCKETS = 33;
    /// Bucket-0 width for #m_histo_msg_sz: matches the internal mdt header frame-prefix size.
    static constexpr size_t S_HISTO_MSG_SZ_BUCKET0_SZ = BUILDER_CONFIG_FRAME_PREFIX_SZ_VIA_STRUC_CHANNEL;
    /// Regular bucket width for #m_histo_msg_sz (bytes).
    static constexpr size_t S_HISTO_MSG_SZ_BUCKET_SZ = 2048;

    // Data.

    /**
     * Internal (out-of-band) messages.  As of this writing there is 1 kind: unexpected-response notification.
     *
     * @note While a well designed user protocol, as of this writing, will result in few or no internal messages,
     *       please be aware that those stats that are *not* documented to be sub-counts of #m_user_msgs --
     *       or as specifically pertaining to *user* messages -- *will* be based on both user messages and internal
     *       messages.  This includes at least: #m_single_segment_msgs, #m_total_segments, #m_total_low_lvl_blobs,
     *       #m_histo_msg_sz.  Relevantly consider that an internal message always consists of exactly 1
     *       minimally-sized (mdt header only) blob/unstructured message bearing no native handle.
     *       (Informally speaking: it counts but not for much.)
     */
    uint64_t m_internal_msgs = 0;

    /**
     * All user structured messages.
     *   - Send-side: 1 per the act of Msg_out going through a send API.
     *   - Receive-side: 1 per Msg_in being prepared inside `Channel` so as to immediately or eventually pass it
     *     to user; or thrown out due to being a response to a message that is not registered to await a
     *     response (active request).
     */
    uint64_t m_user_msgs = 0;

    /**
     * Sub-count of #m_user_msgs: user notifications (Channel::send() yes, Channel::async_request() no).
     * On receive: same as #m_user_msgs, because currently we do not communicate this info from sender to
     * receiver.  Accordingly #m_requests is always zero on receive.  (It is reserved for the future when
     * we might transmit this information and thus count requests versus notifications.  See class doc header.)
     */
    uint64_t m_notifications = 0;

    /**
     * Sub-count of #m_notifications: notification that is itself a response
     * (when sending: `originating_msg_or_null != nullptr`).
     */
    uint64_t m_notification_responses = 0;

    /**
     * Sub-count of #m_user_msgs: user requests (Channel::async_request(), Channel::send() no).
     * `m_notifications + m_requests == m_user_msgs`.
     * On receive: always zero (reserved; see #m_notifications and/or class doc header).
     */
    uint64_t m_requests = 0;

    /**
     * Sub-count of #m_requests: user requests each made with one-off response expectation (auto-cancel on first match).
     * Sticky requests (where responses are expected ad infinitum until specifically unregistered via API)
     * equal: `m_requests - m_requests_one_off`.
     * On receive: always zero (because so is #m_requests).
     */
    uint64_t m_requests_one_off = 0;

    /**
     * Sub-count of #m_requests: requests each of which is also a response (ping-pong pattern).
     * Requests that (which is arguably more typical) not themselves responding: `m_requests - m_request_responses`.
     * On receive: always zero (because so is #m_requests).
     */
    uint64_t m_request_responses = 0;

    /// Messages whose serializations each comprised exactly 1 segment (typical case; if SHM-backed then universal).
    uint64_t m_single_segment_msgs = 0;

    /**
     * Messages whose serializations each comprised 2+ segments.  Generally
     * `m_internal_msgs + m_user_msgs == m_single_segment_msgs + m_multi_segment_msgs`.
     *
     * 2+ segments are needed if the total space required to serialize the needed data exceeds the first segment's
     * size (which is pre-configured/allocated before any mutation send-side).
     */
    uint64_t m_multi_segment_msgs = 0;

    /// Sum of pre-split/post-reassembly segment counts across all messages.
    uint64_t m_total_segments = 0;

    /**
     * Messages for which at least 1 segment had to be split across 2+ low-level blobs.  Atypical; if SHM-backed
     * then impossible.
     *
     * A pre-send/post-receive segment split is required if:
     * a single *leaf* (capnp-`List`, `Data`, `Text` -- these are not splittable in capnp) was so large that
     * a segment had to be sized larger than regular growth suggested, just to be able to fit that leaf alone,
     * *and* that leaf/segment exceeded the IPC transport's max unstructured message size.
     */
    uint64_t m_msgs_with_split_segments = 0;

    /// Total low-level blobs (each segment of #m_total_segments maps to 1 blob unless split; in which case to 2+).
    uint64_t m_total_low_lvl_blobs = 0;

    /// Sub-count of #m_user_msgs: user messages such that each included a native handle.
    uint64_t m_handle_bearing_msgs = 0;

    /**
     * Per-segment histogram: when a segment needed pre-send splitting/post-receive reassembly, how many blobs it
     * became between split and reassembly.  Only split cases are recorded.  Buckets: [2], [3], [4], [5+].
     */
    flow::util::stat::Histogram_counter m_histo_split_blobs_per_seg{S_HISTO_SPLIT_BLOBS_N_BUCKETS, 1, 1, 2};

    /**
     * Total serialization bytes per message (all segments summed, metadata-bearing header included).
     * Bucket 0 width = header size (internal msgs, each of which is header-only, land here);
     * subsequent buckets ~2Ki; overflow into last bucket.
     *
     * Reminder: A SHM-backed message counts, but only the non-SHM-stored aspects count here.  In fact
     * every SHM-backed message, at least for a given SHM-provider and process, will be of the same small size
     * (not much larger than #S_HISTO_MSG_SZ_BUCKET0_SZ): header plus SHM-handle serialization.
     */
    flow::util::stat::Histogram_counter m_histo_msg_sz{S_HISTO_MSG_SZ_N_BUCKETS,
                                                       S_HISTO_MSG_SZ_BUCKET0_SZ, S_HISTO_MSG_SZ_BUCKET_SZ, 0};
  }; // struct Msg

  /// Send-direction stats.  Currently contains only #m_msg.
  struct Snd
  {
    // Data.

    /// Message-level stats for the send direction.
    Msg m_msg;
  };

  /// Receive-direction stats.
  struct Rcv
  {
    // Constants.

    /**
     * Bucket min-values for #m_histo_one_off_request_rtt_usec (default config): a log-scale-ish 1-2-5 ladder
     * covering 10usec through 10sec.  See #m_histo_one_off_request_rtt_usec doc header for rationale.
     */
    static inline const std::vector<flow::util::stat::Histogram_counter::value_t>
      S_HISTO_RTT_BUCKET_VAL0S_USEC
        {0, 10, 20, 50, 100, 200, 500, 1'000, 2'000, 5'000, 10'000, 20'000, 50'000,
         100'000, 200'000, 500'000, 1'000'000, 2'000'000, 5'000'000, 10'000'000};

    /**
     * Last-bucket width for #m_histo_one_off_request_rtt_usec (default config); values past it land in that
     * bucket regardless.
     */
    static constexpr flow::util::stat::Histogram_counter::value_t S_HISTO_RTT_BUCKET_N_SZ_USEC = 10'000'000;

    /// Number of histogram buckets for #m_histo_one_off_request_rtt_usec (default config).
    static inline const size_t S_HISTO_RTT_N_BUCKETS = S_HISTO_RTT_BUCKET_VAL0S_USEC.size();

    // Data.

    /**
     * Various stats for the receive direction, generally mirroring the opposing peer's Snd::m_msg.
     * Be aware some values may not be quite equal between the two due to at least (1) time (naturally)
     * and (2) in some cases slightly different semantics (always documented around here and kept to a minimum).
     */
    Msg m_msg;

    /**
     * Sub-count of `m_msg.m_notification_responses`: response arrived but no matching expectation
     * registered (user protocol error).  Matched responses = `m_msg.m_notification_responses - m_unexpected_responses`.
     */
    uint64_t m_unexpected_responses = 0;

    /**
     * Number of times and out-of-order arrival enqueued into the reassembly queue
     * (possible only if Channel::S_HAS_2_PIPES for Channel::Owned_channel).  Such an event is atypical.
     * Also it never occurs with internal messages: only user messages.
     *
     * ### Background: reassembly queue in struc::Channel et al ###
     * A 2-pipe config has one pipe for native-handle-bearing user messages and the other for all others.
     * However, that means that if I send msgs A-B (and one has a handle, the other does not), they might arrive
     * as B-A.  Until A arrives, I cannot fully contemplate B, necessarily, because via internal sequence numbers
     * I can see there is a gap.  The user protocol might rely on the original order.  So then I stick B into
     * an internal reassembly queue, until any seq-num gap before it filled with more in-messages.
     */
    uint64_t m_reassembly_q_insertions = 0;

    /// Reassembly queue depth; typically zero.  See #m_reassembly_q_insertions for some background.
    size_t m_reassembly_q_depth = 0;

    /// Max of #m_reassembly_q_depth observed so far.
    size_t m_reassembly_q_hi_wmark = 0;

    /**
     * Non-response (unsolicited) user in-messages delivered to a handler immediately (at receipt
     * handler was already registered).
     */
    uint64_t m_unsolicited_msgs_routed = 0;

    /**
     * Non-response (unsolicited) user messages cached (no handler registered yet at receipt).
     * High rate may signal late Channel::expect_msgs() et al.
     */
    uint64_t m_unsolicited_msgs_cached = 0;

    /**
     * Current count (gauge) of non-response (unsolicited) user messages received but not yet
     * `expect_*()`-registered at time of receipt (gauge).
     */
    size_t m_pending_msgs_depth = 0;

    /// Max of #m_pending_msgs_depth observed so far.
    size_t m_pending_msgs_hi_wmark = 0;

    /**
     * Total valid one-shot (Channel::expect_msg() or equivalent) registrations so far.
     * "Valid" means this discounts those calls that return `false` (called on on already-hosed channel/similar).
     */
    uint64_t m_expect_msg_count = 0;

    /**
     * Current count (gauge) of active one-shot (Channel::expect_msg() or equivalent) expectations.
     *
     * A one-shot expectation is auto-canceled on being satisfied (got message of the specified Channel::Msg_which_in)
     * once.
     */
    size_t m_expect_msg_active = 0;

    /// Cf #m_expect_msg_count: tracks sticky (Channel::expect_msgs() or equivalent: not one-off) registrations.
    uint64_t m_expect_msgs_count = 0;

    /**
     * Cf #m_expect_msg_active: tracks sticky (Channel::expect_msgs() or equivalent: not one-off) registrations.
     *
     * A sticky expectation remains active until actively canceling via Channel::undo_expect_msgs() or equivalent.
     */
    size_t m_expect_msgs_active = 0;

    /**
     * Times already-queued messages were popped on `expect_*()` registration (once per pop event, not per popped
     * message).  To be clear:
     *   - Channel::expect_msg() will (if 1+ messages are already available) pop 1 message.
     *     That counts for 1.
     *   - Channel::expect_msgs() will (if 1+ messages are already available) pop all 1+ messages.
     *     That counts for 1 [sic].
     */
    uint64_t m_expect_q_immediate_pops = 0;

    /**
     * Current count (gauge) of active one-off response expectations (one-off pending requests).  A request
     * is specified to be one-off versus not-one-off via arg to Channel::async_request() or equivalent.
     *
     * See also in Snd [sic!] Msg::m_requests and/or Msg::m_requests_one_off regarding counts of such things.
     */
    size_t m_expect_response_one_off_active = 0;

    /**
     * Current count (gauge) of active sticky response expectations (indefinitely pending requests).  A request
     * is specified to be one-off versus not-one-off via arg to Channel::async_request() or equivalent.
     * The expectation in this case remains until Channel::undo_expect_responses().
     *
     * See also in Snd [sic!] Msg::m_requests and/or Msg::m_requests_one_off regarding counts of such things.
     */
    size_t m_expect_response_sticky_active = 0;

    /**
     * Total valid Channel::remote_peer_process_liveness_check() invocations.  "Valid" means this discounts
     * those calls that return `false` (called on on already-hosed channel/similar).
     *
     * A direct sync_io::Channel user may use Channel::remote_peer_process_liveness_check(), though informally
     * we do not expect this to be particularly common.
     *
     * A struc::Channel user may cause such liveness checks indirectly by issuing a long-lasting
     * struc::Channel::sync_request(); at times an ungraceful exit/abort of the opposing process *might*
     * only be detected by such a check.
     */
    uint64_t m_liveness_checks = 0;

    /**
     * One-off `async_request()` round-trip time histogram (real time passed from said call to response receipt),
     * in microseconds.
     *
     * Only one-off requests; sticky excluded (latency grows monotonically, meaningless).
     * Default buckets: a 1-2-5 log-scale ladder from 10us through 10s (values beyond overflow into the last
     * bucket).  This captures -- with resolution proportional to magnitude -- both promptly-answered requests
     * (expect ~100us) and ones whose responses take the opposing side seconds to issue, including a mix of
     * the two in one channel.  (Non-uniform buckets mean each recording costs a short binary search instead
     * of O(1) arithmetic; immaterial at per-request frequency.)
     *
     * @note User can override bucket config via
     *       Channel::stats_configure_rcv_one_off_request_rtt_histogram() before Channel::start_and_poll()
     *       (same and transport::Channel::start(), respectively, for async-I/O-pattern transport::Channel).
     *
     * @note If you are using a sync_io::Channel directly please recall that we can process any events, including
     *       in-traffic, only when you report events to the `Channel`.  So, e.g., if you wait a second between
     *       detecting a requested event being active and informing the `Channel`, then this will be at least
     *       one second, regardless of how instantly the actual data arrived.  (If you're using
     *       struc::Channel, then its background thread should get to it ASAP however.  Then again struc::Channel
     *       ultimately simply mounts an event loop in a thread, much as you might.)
     */
    flow::util::stat::Histogram_counter m_histo_one_off_request_rtt_usec{S_HISTO_RTT_BUCKET_VAL0S_USEC,
                                                                         S_HISTO_RTT_BUCKET_N_SZ_USEC};
  }; // struct Rcv

  // Data.

  /// Send-direction stats.
  Snd m_snd;

  /// Receive-direction stats.
  Rcv m_rcv;
}; // struct Channel_stats

// Template implementations.

template<typename Visitor>
void declare_stats(std::string name_prefix, const Channel_stats* src_stats, Channel_stats* target_stats,
                   Visitor&& visitor)
{
  // m_snd.m_msg:
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_internal_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_user_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_notifications, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_notification_responses, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_requests, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_requests_one_off, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_request_responses, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_single_segment_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_multi_segment_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_total_segments, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_msgs_with_split_segments, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_total_low_lvl_blobs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_handle_bearing_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_histo_split_blobs_per_seg, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_snd.m_msg.m_histo_msg_sz, ACCUMULATOR);

  // m_rcv.m_msg:
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_internal_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_user_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_notifications, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_notification_responses, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_requests, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_requests_one_off, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_request_responses, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_single_segment_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_multi_segment_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_total_segments, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_msgs_with_split_segments, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_total_low_lvl_blobs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_handle_bearing_msgs, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_histo_split_blobs_per_seg, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_msg.m_histo_msg_sz, ACCUMULATOR);

  // m_rcv receive-specific:
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_unexpected_responses, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_reassembly_q_insertions, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_reassembly_q_depth, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_rcv.m_reassembly_q_hi_wmark, m_rcv.m_reassembly_q_depth);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_unsolicited_msgs_routed, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_unsolicited_msgs_cached, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_pending_msgs_depth, GAUGE);
  FLOW_UTIL_STAT_DECLARE_HI_WMARK(m_rcv.m_pending_msgs_hi_wmark, m_rcv.m_pending_msgs_depth);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_expect_msg_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_expect_msg_active, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_expect_msgs_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_expect_msgs_active, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_expect_q_immediate_pops, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_expect_response_one_off_active, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_expect_response_sticky_active, GAUGE);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_liveness_checks, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_rcv.m_histo_one_off_request_rtt_usec, ACCUMULATOR);
} // declare_stats(Channel_stats)

} // namespace ipc::transport::struc::sync_io::stat
