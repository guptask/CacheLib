/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace facebook {
namespace cachelib {

// Hotness of root prefixes. Every access adds to a score that halves every
// `halfLifeSecs`; the time of the last access breaks ties between equal scores.
class RootHotness {
 public:
  explicit RootHotness(uint32_t halfLifeSecs = 600, size_t numShards = 64)
      : halfLifeSecs_(std::max(1u, halfLifeSecs)),
        shards_(std::max<size_t>(1, numShards)) {}

  void recordAccess(uint64_t root, uint32_t nowSecs, double weight = 1.0) {
    auto& shard = shardFor(root);
    std::lock_guard<std::mutex> l(shard.mutex);
    auto& e = shard.entries[root];
    e.score = decayed(e, nowSecs) + weight;
    e.lastAccessSecs = nowSecs;
  }

  // 0 for a root that was never seen.
  double score(uint64_t root, uint32_t nowSecs) const {
    const auto& shard = shardFor(root);
    std::lock_guard<std::mutex> l(shard.mutex);
    auto it = shard.entries.find(root);
    return it == shard.entries.end() ? 0.0 : decayed(it->second, nowSecs);
  }

  uint32_t lastAccess(uint64_t root) const {
    const auto& shard = shardFor(root);
    std::lock_guard<std::mutex> l(shard.mutex);
    auto it = shard.entries.find(root);
    return it == shard.entries.end() ? 0 : it->second.lastAccessSecs;
  }

  // true if `a` is colder than `b`.
  bool colder(uint64_t a, uint64_t b, uint32_t nowSecs) const {
    const double sa = score(a, nowSecs);
    const double sb = score(b, nowSecs);
    if (sa != sb) {
      return sa < sb;
    }
    return lastAccess(a) < lastAccess(b);
  }

  // The coldest of `roots` other than `exclude`. Returns false if there is none.
  bool coldest(const std::vector<uint64_t>& roots,
               uint64_t exclude,
               uint32_t nowSecs,
               uint64_t& out) const {
    return pick(roots, exclude, nowSecs, out, /* hottest */ false);
  }

  bool hottest(const std::vector<uint64_t>& roots,
               uint64_t exclude,
               uint32_t nowSecs,
               uint64_t& out) const {
    return pick(roots, exclude, nowSecs, out, /* hottest */ true);
  }

  void forget(uint64_t root) {
    auto& shard = shardFor(root);
    std::lock_guard<std::mutex> l(shard.mutex);
    shard.entries.erase(root);
  }

  // Drops roots whose score decayed below `minScore`. Returns how many.
  size_t prune(uint32_t nowSecs, double minScore) {
    size_t n = 0;
    for (auto& shard : shards_) {
      std::lock_guard<std::mutex> l(shard.mutex);
      for (auto it = shard.entries.begin(); it != shard.entries.end();) {
        if (decayed(it->second, nowSecs) < minScore) {
          it = shard.entries.erase(it);
          ++n;
        } else {
          ++it;
        }
      }
    }
    return n;
  }

  size_t size() const {
    size_t n = 0;
    for (const auto& shard : shards_) {
      std::lock_guard<std::mutex> l(shard.mutex);
      n += shard.entries.size();
    }
    return n;
  }

 private:
  struct Entry {
    double score{0};
    uint32_t lastAccessSecs{0};
  };

  struct Shard {
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, Entry> entries;
  };

  double decayed(const Entry& e, uint32_t nowSecs) const {
    if (nowSecs <= e.lastAccessSecs) {
      return e.score;
    }
    const double age = static_cast<double>(nowSecs - e.lastAccessSecs);
    return e.score * std::exp2(-age / static_cast<double>(halfLifeSecs_));
  }

  bool pick(const std::vector<uint64_t>& roots,
            uint64_t exclude,
            uint32_t nowSecs,
            uint64_t& out,
            bool hottest) const {
    bool found = false;
    for (auto root : roots) {
      if (root == exclude) {
        continue;
      }
      if (!found || (hottest ? colder(out, root, nowSecs)
                             : colder(root, out, nowSecs))) {
        out = root;
        found = true;
      }
    }
    return found;
  }

  Shard& shardFor(uint64_t root) { return shards_[mix(root) % shards_.size()]; }
  const Shard& shardFor(uint64_t root) const {
    return shards_[mix(root) % shards_.size()];
  }

  static uint64_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
  }

  const uint32_t halfLifeSecs_;
  std::vector<Shard> shards_;
};

} // namespace cachelib
} // namespace facebook
