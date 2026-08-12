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

#include "ipc/transport/struc/test/channel_stats_test.hpp"

namespace ipc::transport::struc::test
{

// A slice of the test battery (MqType-spanning + ShmType-NONE + knob-less tests); see similarly named .hpp.

STATS_TEST(send_receive_stats)

/* Field-coverage manifest.  stats_field_names() reflects the type's full declared field list, and every
 * field must appear in exactly one of the two lists below -- covered (asserted somewhere in this suite) or
 * skipped (deliberately unasserted, with its why).  A newly-added struct field fails this test until
 * someone classifies it here: that is the point.  (The covered-claims themselves are maintained by review;
 * this enforces completeness of the classification, not the existence of the asserts.)
 *
 * Note: this manifests the async-I/O-pattern Channel_stats, which composes sync_io::stat::Channel_stats
 * (same names, no extra prefix) plus the sync_req group; so this one manifest classifies both types'
 * fields.  (The Blob_snd/rcv_stats also consumed by this suite are manifested in their own suite:
 * blob_transport_stats_test.) */
TEST(Struc_channel_stats_test, field_coverage_manifests)
{
  using flow::util::stat::stats_field_names;
  using std::sort;
  using std::string;
  using std::vector;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  const auto check_manifest = [](util::String_view stat_set_name, vector<string> declared,
                                 vector<string> covered, const vector<string>& skipped)
  {
    // The table itself (in declaration order), for the eyeball:
    const auto in = [](const vector<string>& vec, const string& name)
                      { return std::find(vec.begin(), vec.end(), name) != vec.end(); };
    std::cout << "Field-coverage manifest [" << stat_set_name << "] "
                 "(" << covered.size() << " covered, " << skipped.size() << " skipped):\n";
    for (const auto& name : declared)
    {
      std::cout << "  " << (in(skipped, name) ? "SKIPPED: "
                                              : in(covered, name) ? "covered: "
                                                                  : "*** UNCLASSIFIED: ")
                << name << '\n';
    }

    auto& classified = covered;
    classified.insert(classified.end(), skipped.begin(), skipped.end());
    sort(classified.begin(), classified.end());
    sort(declared.begin(), declared.end());
    EXPECT_EQ(declared, classified) << "Unclassified/stale stat-field classification for "
                                       "[" << stat_set_name << "].";
  };

  check_manifest
    ("struc::Channel_stats", stats_field_names<stat::Channel_stats>(),
     { // Covered: asserted in this suite.
       "snd.msg.user_msgs", "snd.msg.notifications", "snd.msg.notification_responses",
       "snd.msg.requests", "snd.msg.requests_one_off", "snd.msg.request_responses",
       "snd.msg.single_segment_msgs", "snd.msg.multi_segment_msgs", "snd.msg.total_segments",
       "snd.msg.msgs_with_split_segments", "snd.msg.total_low_lvl_blobs", "snd.msg.handle_bearing_msgs",
       "snd.msg.histo_msg_sz",
       "rcv.msg.user_msgs", "rcv.msg.notifications", "rcv.msg.notification_responses",
       "rcv.msg.requests", "rcv.msg.requests_one_off", "rcv.msg.request_responses",
       "rcv.msg.single_segment_msgs", "rcv.msg.multi_segment_msgs", "rcv.msg.total_segments",
       "rcv.msg.msgs_with_split_segments", "rcv.msg.total_low_lvl_blobs",
       "rcv.msg.handle_bearing_msgs", "rcv.msg.histo_msg_sz",
       "rcv.unexpected_responses",
       "rcv.reassembly_q_insertions", "rcv.reassembly_q_depth", "rcv.reassembly_q_hi_wmark",
       "rcv.unsolicited_msgs_routed", "rcv.unsolicited_msgs_cached",
       "rcv.pending_msgs_depth", "rcv.pending_msgs_hi_wmark",
       "rcv.expect_msg_count", "rcv.expect_msg_active", "rcv.expect_msgs_count", "rcv.expect_msgs_active",
       "rcv.expect_q_immediate_pops",
       "rcv.expect_response_one_off_active", "rcv.expect_response_sticky_active",
       "rcv.liveness_checks", "rcv.histo_one_off_request_rtt_usec",
       "sync_req.count", "sync_req.histo_latency_usec", "sync_req.timeouts", "sync_req.late_responses" },
     { /* Skipped deliberately:
        * It's doable to trigger some internal (non-user) messages; at least one can trigger an unsolicited
        * request-response.  A slight pain.  @todo.  Alternatively could have transport_test opportunistically
        * check some stats, when it triggers such things intentionally anyway. */
       "snd.msg.internal_msgs", "rcv.msg.internal_msgs",
       // @todo We do check some split-message things.  Could probably cover this too.
       "snd.msg.histo_split_blobs_per_seg", "rcv.msg.histo_split_blobs_per_seg" });
} // TEST(Struc_channel_stats_test, field_coverage_manifests)

TEST(Struc_channel_stats_test, stats_reset_semantics) { test_stats_reset_semantics(); }

TEST(Struc_channel_stats_test, info_collector_pre_peer_states_ShmNone)
{
  test_info_collector_pre_peer_states<session::schema::ShmType::NONE>();
}

TEST(Struc_channel_stats_test, smc_info_collector_smoke_ShmNone)
{
  test_smc_info_collector_smoke<session::schema::ShmType::NONE>();
}

TEST(Struc_channel_stats_test, info_collector_print_and_afterlife)
{
  test_info_collector_print_and_afterlife();
}

#undef STATS_TEST

} // namespace ipc::transport::struc::test
