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

#include "ipc/transport/struc/test/serializer_stats_test.hpp"

namespace ipc::transport::struc::test
{

// A slice of the test battery (the heap-backed + knob-less tests); see similarly named .hpp.

/* Checks Histo_cfg::build() in both its scales: linear (pass-through to the like-named Histogram_counter
 * ctor args) and geometric (ladder expansion into the arbitrary-bucket-widths ctor form). */
TEST(Struc_serializer_stats_test, histo_cfg_build)
{
  // Linear {3, 8, 4}: buckets [0, 8), [8, 12), [12, ...).
  {
    constexpr stat::Histo_cfg CFG{3, 8, 4};
    auto histo = CFG.build();
    histo.record_value(0);
    histo.record_value(7);
    histo.record_value(8);
    histo.record_value(15);
    histo.record_value(1'000'000); // Overflow: last bucket.
    EXPECT_EQ(histo.count_for_bucket(0), 2u);
    EXPECT_EQ(histo.count_for_bucket(1), 1u);
    EXPECT_EQ(histo.count_for_bucket(2), 2u);
  }

  // Geometric {4, 8, 0, x2}: buckets [0, 8) (catch-all), [8, 16), [16, 32), [32, ...) (exposition-max 63).
  {
    constexpr stat::Histo_cfg CFG{4, 8, 0, 2};
    auto histo = CFG.build();
    histo.record_value(7); // Below-floor catch-all.
    histo.record_value(8);
    histo.record_value(15);
    histo.record_value(31);
    histo.record_value(32);
    histo.record_value(63);
    histo.record_value(64); // Past exposition-max: still last bucket.
    histo.record_value(1'000'000); // Ditto.
    EXPECT_EQ(histo.count_for_bucket(0), 1u);
    EXPECT_EQ(histo.count_for_bucket(1), 2u);
    EXPECT_EQ(histo.count_for_bucket(2), 1u);
    EXPECT_EQ(histo.count_for_bucket(3), 4u);
  }

  /* The production Core (SHM-payload) cfg constants: spot-check each ladder's floor, mid-range, and reach.
   * Nothing else asserts these actual configs (the send/receive suites are deliberately width-agnostic), so a
   * fat-fingered constant would otherwise fly.  Scheme: floor - 1 -> bucket 0; floor -> bucket 1; floor * 1000
   * -> some interior bucket; 4Gi -> the last bucket, being past every ladder's expositional max.  A wrong
   * ratio or bucket count shifts the ladder's reach and flunks the interior/last checks. */
  {
    const auto spot_check_ladder = [](const stat::Histo_cfg& cfg, int64_t floor)
    {
      auto histo = cfg.build();
      histo.record_value(floor - 1);
      histo.record_value(floor);
      histo.record_value(floor * 1000);
      histo.record_value(int64_t(4) * 1024 * 1024 * 1024);
      EXPECT_EQ(histo.count_for_bucket(0), 1u) << "Floor [" << floor << "].";
      EXPECT_EQ(histo.count_for_bucket(1), 1u) << "Floor [" << floor << "].";
      EXPECT_EQ(histo.count_for_bucket_containing_outcome(floor * 1000), 1u) << "Floor [" << floor << "].";
      EXPECT_EQ(histo.count_for_bucket(cfg.m_n_bkts - 1), 1u) << "Floor [" << floor << "].";
    };
    spot_check_ladder(shm::stat::Core_serializer_stats_cfg::S_HISTO_SND_MSG_ALLOC_SZ, 8 * 1024);
    spot_check_ladder(shm::stat::Core_serializer_stats_cfg::S_HISTO_SND_MSG_USED_SZ, 1024);
    spot_check_ladder(shm::stat::Core_serializer_stats_cfg::S_HISTO_SND_BIG_LEAF_SZ, 64 * 1024);
  }
}

/* Field-coverage manifest.  stats_field_names() reflects the type's full declared field list, and every
 * field must appear in exactly one of the two lists below -- covered (asserted somewhere in this suite) or
 * skipped (deliberately unasserted, with its why).  A newly-added struct field fails this test until
 * someone classifies it here: that is the point.  (The covered-claims themselves are maintained by review;
 * this enforces completeness of the classification, not the existence of the asserts.) */
TEST(Struc_serializer_stats_test, field_coverage_manifests)
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

  stat::Histo_cfg dummy{ 2, 1, 1 };
  check_manifest
    ("Serializer_stats", stats_field_names<stat::Serializer_stats>(dummy, dummy, dummy, dummy),
     { // Covered: asserted in this suite.  (Nothing skipped: the full surface is asserted.)
       "snd.msgs", "snd.msgs_outstanding", "snd.msgs_outstanding_hi_wmark",
       "snd.alloc_lifetime_sz", "snd.alloc_outstanding_sz", "snd.alloc_outstanding_sz_hi_wmark",
       "snd.histo_msg_alloc_sz", "snd.histo_msg_used_sz", "snd.histo_segs_per_msg",
       "snd.big_leaf_alloc_count", "snd.histo_big_leaf_sz", "snd.msgs_with_big_leaf",
       "snd.seg_grow_cap_lock_count", "snd.frame_lifetime_sz",
       "rcv.msgs_outstanding", "rcv.msgs_outstanding_hi_wmark",
       "rcv.alloc_lifetime_sz", "rcv.alloc_outstanding_sz", "rcv.alloc_outstanding_sz_hi_wmark",
       "rcv.used_outstanding_sz", "rcv.used_outstanding_sz_hi_wmark",
       "rcv.histo_msg_alloc_sz", "rcv.histo_msg_used_sz" },
     {});
} // TEST(Struc_serializer_stats_test, field_coverage_manifests)

TEST(Struc_serializer_stats_test, send_receive_heap)
{
  test_send_receive_serializer_stats_heap();
}

TEST(Struc_serializer_stats_test, reset_semantics)
{
  test_serializer_stats_reset_semantics();
}

TEST(Struc_serializer_stats_test, direct_builder_smoke)
{
  test_direct_builder_smoke();
}

/* Smoke-test + console demo of the serializer global-land grab-and-print conveniences.  (The underlying stats
 * themselves are well-exercised by the preceding test cases -- which, conveniently, also leave the
 * globals populated with real numbers for us to show off in a suite run.)  Namely:
 *   - struc::stat::heap_serializer_info_dump() + direct `ostream <<` of a Serializer_stats;
 *   - struc::shm::stat::serializer_info_dump<Arena>() + `ostream <<` of the resulting Serializer_info_dump
 *     (multi-line and single-line; and the core's inherently-unused rcv-side stats omitted from the printout). */
TEST(Struc_serializer_stats_test, info_dump_and_print)
{
  using ipc::shm::classic::Pool_arena;
  using shm::stat::Serializer_info_dump;
  using flow::util::ostream_op_string;

  FLOW_LOG_SET_CONTEXT(g_logger_console, Log_component::S_TEST);

  // Heap-only type: the grabbed singleton-snapshot must equal a direct read of same.
  stat::Heap_serializer_stats heap_stats;
  stat::heap_serializer_info_dump(&heap_stats);
  {
    const auto& direct = stat::Heap_serializer_global_stats::get().stats_default();
    EXPECT_EQ(heap_stats.m_snd.m_msgs.load(), direct.m_snd.m_msgs.load());
    EXPECT_EQ(heap_stats.m_snd.m_alloc_lifetime_sz.load(), direct.m_snd.m_alloc_lifetime_sz.load());
    EXPECT_EQ(histo_total(heap_stats.m_snd.m_histo_msg_used_sz), histo_total(direct.m_snd.m_histo_msg_used_sz));
  }
  FLOW_LOG_INFO("Heap-serializer global stats (heap_serializer_info_dump() + direct ostream<<): "
                "[" << heap_stats << "].");

  // SHM type: everything in serializer global-land for a given SHM-provider (SHM-classic here).
  Serializer_info_dump dump;
  shm::stat::serializer_info_dump<Pool_arena>(&dump);
  // No serializer traffic since the heap-grab above => the m_heap part must equal it.
  EXPECT_EQ(dump.m_heap.m_snd.m_msgs.load(), heap_stats.m_snd.m_msgs.load());
  EXPECT_EQ(dump.m_heap.m_snd.m_alloc_lifetime_sz.load(), heap_stats.m_snd.m_alloc_lifetime_sz.load());

  FLOW_LOG_INFO("Serializer_info_dump for Arena=Pool_arena (multi-line):\n" << dump << '.');
  dump.m_fmt.m_multiline = false;
  FLOW_LOG_INFO("Ditto (single-line): [" << dump << "].");

  // Printout invariants: the 3 labeled sections; core's rcv-side omitted (while heap's/outer's are present).
  const auto str = ostream_op_string(dump);
  const auto core_pos = str.find("shm-backed-core:");
  EXPECT_NE(str.find("heap-backed:"), string::npos);
  EXPECT_NE(str.find("heap-backed-envelope:"), string::npos);
  EXPECT_NE(core_pos, string::npos);
  EXPECT_NE(str.find("rcv."), string::npos); // rcv-side stats are present...
  EXPECT_LT(str.find("rcv."), core_pos); // ...before the core section (i.e., the cut did not over-trim)...
  EXPECT_EQ(str.find("rcv.", core_pos), string::npos); // ...but within the core section: omitted.
}

} // namespace ipc::transport::struc::test
