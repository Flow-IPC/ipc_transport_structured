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

#include "ipc/transport/struc/sync_io/channel_stats.hpp"
#include <flow/util/stat/histo.hpp>
#include <flow/util/stat/stat_set.hpp>
#include <cstdint>
#include <string>

namespace ipc::transport::struc::stat
{

// Types.

/**
 * Stats specific to Channel::sync_request() -- the blocking synchronous request-response API
 * available only via struc::Channel (not struc::sync_io::Channel).  Composed into Channel_stats
 * (as Channel_stats::m_sync_req) but is also a standalone Stat_set in its own right.
 * 
 * @internal
 * It is updated, but more saliently reset via `flow::util::stat::stats_reset()`, on its own by
 * struc::Channel internals.  Then when the user accesses struc::Channel::stats(), the two parts are
 * put together into struc::stat::Channel_stats.
 */
struct Channel_sync_req_stats
{
  // Data.

  /**
   * Total valid Channel::sync_request() invocations.  "Valid" means this discounts those calls that return
   * null without emitting an error (called on on already-hosed channel/similar).
   *
   * @note It does include the situation where `sync_request()` immediately fails, thus hosing the `Channel`
   *       and emitting error.  This is a small caveat unlikely to be of much import, as such a thing can
   *       happen at most once.
   */
  uint64_t m_count = 0;

  /**
   * Channel::sync_request() real-time latency histogram: time period from initiating non-blocking send
   * to return (whether success, timeout, or error), in microseconds.  If it counts in #m_count, it is recorded
   * here, and vice versa.  A late-arriving response (#m_late_responses) is recorded as ending when the timeout
   * occurs (not when it arrives past the timeout).
   *
   * Default buckets: same config as sync_io::stat::Channel_stats::Rcv::m_histo_one_off_request_rtt_usec
   * (see its doc header for rationale) -- after all a `sync_request()` latency *is* a one-off-request RTT:
   * sync_request() internally layers a blocking wait atop a one-off `async_request()`.
   *
   * @note User can override bucket config via
   *       Channel::stats_configure_sync_req_latency_histogram() before Channel::start().
   */
  flow::util::stat::Histogram_counter m_histo_latency_usec
    {sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_BUCKET_VAL0S_USEC,
     sync_io::stat::Channel_stats::Rcv::S_HISTO_RTT_BUCKET_N_SZ_USEC};

  /**
   * Channel::sync_request() invocations (counted in #m_count) that returned
   * transport::error::Code::S_TIMEOUT.
   */
  uint64_t m_timeouts = 0;

  /**
   * Count of the event wherein response arrived after Channel::sync_request() had already timed out; the
   * response was dropped.
   *
   * High #m_late_responses relative to #m_timeouts indicates that the opposing process *is*
   * responding but too slowly -- a different diagnosis than "never responded."
   *
   * @note A thing that increments #m_late_responses also increments #m_timeouts (but not necessarily vice versa).
   */
  uint64_t m_late_responses = 0;
}; // struct Channel_sync_req_stats

/**
 * Cumulative stats from a struc::Channel, composing the underlying sync_io::stat::Channel_stats
 * with `struc::Channel`-specific stats including vis-a-vis Channel::sync_request().  #m_core contains
 * the former; #m_sync_req contains the latter (a Channel_sync_req_stats peer).
 *
 * A struc::Channel user obtains this via Channel::stats(); a sync_io::Channel user obtains
 * sync_io::stat::Channel_stats via that class's own sync_io::Channel::stats() instead.
 *
 * @see doc header for struc::Channel::stats().
 * @see doc header for #m_core; there are certain subtleties that may be of interest.
 */
struct Channel_stats
{
  // Data.

  /**
   * Stats from the underlying sync_io::Channel.
   *
   * @note While the vast majority of Channel methods are mirrored in sync_io::Channel, and vice versa,
   *       one exception is that Channel::async_request() will invoke sync_io::Channel::async_request() --
   *       but so will Channel::sync_request().  Therefore there may be counts/gauges in sync_io::stat::Channel_stats
   *       (with respect to requests generally) that one might not naively expect to be as high as observed,
   *       here in #m_core.
   */
  sync_io::stat::Channel_stats m_core;

  /// Stats specific to Channel::sync_request().
  Channel_sync_req_stats m_sync_req;
}; // struct Channel_stats

// Template implementations.

template<typename Visitor>
void declare_stats(std::string name_prefix,
                   const Channel_sync_req_stats* src_stats, Channel_sync_req_stats* target_stats,
                   Visitor&& visitor)
{
  FLOW_UTIL_STAT_DECLARE(m_count, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_histo_latency_usec, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_timeouts, ACCUMULATOR);
  FLOW_UTIL_STAT_DECLARE(m_late_responses, ACCUMULATOR);
} // declare_stats(Channel_sync_req_stats)

template<typename Visitor>
void declare_stats(std::string name_prefix, const Channel_stats* src_stats, Channel_stats* target_stats,
                   Visitor&& visitor)
{
  /* Core stats -- delegate to sync_io::stat's declare_stats() (found via ADL on sync_io::stat::Channel_stats).
   * The sync_io stats keep `name_prefix` unchanged (no "core." prefix added): they are meaningful in their own
   * right, and the context/separation should be quite clear visually. */
  sync_io::stat::declare_stats(name_prefix,
                               src_stats ? &src_stats->m_core : nullptr,
                               target_stats ? &target_stats->m_core : nullptr,
                               visitor);

  /* m_sync_req: also delegate (it is reset standalone via Channel::stats_reset()).
   * Add "sync_req." sub-prefix; as of this writing without the "sync_req." context these stats look rather
   * ambiguous at a glance; for example `count=[4]`: that is count of sync_request()s.  Hopefully you feel
   * the distinction between that and hypothetically adding `core.` (or whatever) above. */
  declare_stats(name_prefix + "sync_req.",
                src_stats ? &src_stats->m_sync_req : nullptr,
                target_stats ? &target_stats->m_sync_req : nullptr,
                visitor);
} // declare_stats(Channel_stats)

} // namespace ipc::transport::struc::stat
