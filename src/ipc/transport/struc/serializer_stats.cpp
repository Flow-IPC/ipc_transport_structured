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

#include "ipc/transport/struc/serializer_stats.hpp"
#include <flow/util/stat/stat_set_list.hpp>
#include <cassert>
#include <ostream>
#include <vector>

namespace ipc::transport::struc::stat
{

// Histo_cfg implementations.

flow::util::stat::Histogram_counter Histo_cfg::build() const
{
  using flow::util::stat::Histogram_counter;
  using value_t = Histogram_counter::value_t;
  using std::vector;

  if (m_ratio == 0)
  {
    // Linear scale.
    return Histogram_counter{m_n_bkts, value_t(m_bkt0), value_t(m_bkt), 0};
  }
  // else: Geometric scale.  Expand the ladder into the arbitrary-bucket-widths ctor form.

  assert((m_ratio >= 2) && (m_n_bkts >= 2) && (m_bkt0 >= 1) && (m_bkt == 0)
         && "Geometric-scale Histo_cfg: require ratio >= 2, n_bkts >= 2, floor >= 1, m_bkt zero (unused).");

  vector<value_t> bkt_val0s(m_n_bkts);
  bkt_val0s[0] = 0; // Below-floor catch-all: [0, m_bkt0).
  auto val0 = value_t(m_bkt0);
  for (size_t idx = 1; idx != m_n_bkts; ++idx, val0 *= value_t(m_ratio))
  {
    bkt_val0s[idx] = val0;
  }
  /* Last-bucket width such that its expositional max is the next ladder rung -- keeping the geometric
   * progression uniform through the end.  (Beyond-max values land in the last bucket anyway.) */
  return Histogram_counter{bkt_val0s, bkt_val0s.back() * value_t(m_ratio - 1)};
}

// Serializer_stats::Snd implementations.

Serializer_stats::Snd::Snd(Histo_cfg msg_alloc_sz_cfg, Histo_cfg msg_used_sz_cfg, Histo_cfg big_leaf_sz_cfg) :
  m_histo_msg_alloc_sz(msg_alloc_sz_cfg.build()),
  m_histo_msg_used_sz(msg_used_sz_cfg.build()),
  m_histo_big_leaf_sz(big_leaf_sz_cfg.build())
{
  // Nothing.
}

// Serializer_stats::Rcv implementations.

Serializer_stats::Rcv::Rcv(Histo_cfg msg_used_sz_cfg) :
  m_histo_msg_used_sz(msg_used_sz_cfg.build())
{
  // Nothing.
}

// Serializer_stats implementations.

Serializer_stats::Serializer_stats(Histo_cfg snd_msg_alloc_sz_cfg, Histo_cfg snd_msg_used_sz_cfg,
                                   Histo_cfg snd_big_leaf_sz_cfg, Histo_cfg rcv_msg_used_sz_cfg) :
  m_snd(snd_msg_alloc_sz_cfg, snd_msg_used_sz_cfg, snd_big_leaf_sz_cfg),
  m_rcv(rcv_msg_used_sz_cfg)
{
  // Nothing.
}

// Free function implementations.

void heap_serializer_info_dump(Heap_serializer_stats* target_stats)
{
  assert(target_stats);
  flow::util::stat::stats_assign(target_stats, Heap_serializer_global_stats::get().stats_default());
}

std::ostream& operator<<(std::ostream& os, const Serializer_stats& val)
{
  return os << flow::util::stat::print(val);
}

} // namespace ipc::transport::struc::stat
