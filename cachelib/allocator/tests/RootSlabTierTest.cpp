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

#include <gtest/gtest.h>

#include "cachelib/allocator/RootHotness.h"
#include "cachelib/allocator/RootSlabImage.h"

namespace facebook::cachelib::tests {

TEST(RootHotness, ScoreDecaysAndOrdersRoots) {
  RootHotness h(/* halfLifeSecs */ 10);
  h.recordAccess(1, 100);
  h.recordAccess(1, 100);
  h.recordAccess(2, 100);

  EXPECT_DOUBLE_EQ(2.0, h.score(1, 100));
  EXPECT_NEAR(1.0, h.score(1, 110), 1e-9);
  EXPECT_EQ(0.0, h.score(3, 100));
  EXPECT_TRUE(h.colder(2, 1, 100));
  EXPECT_FALSE(h.colder(1, 2, 100));
}

TEST(RootHotness, PicksColdestAndHottestExcludingOne) {
  RootHotness h(600);
  h.recordAccess(1, 0, 5);
  h.recordAccess(2, 0, 1);
  h.recordAccess(3, 0, 3);

  uint64_t out = 0;
  ASSERT_TRUE(h.coldest({1, 2, 3}, 0, 0, out));
  EXPECT_EQ(2u, out);
  ASSERT_TRUE(h.coldest({1, 2, 3}, 2, 0, out));
  EXPECT_EQ(3u, out);
  ASSERT_TRUE(h.hottest({1, 2, 3}, 0, 0, out));
  EXPECT_EQ(1u, out);
  EXPECT_FALSE(h.coldest({2}, 2, 0, out));
  EXPECT_FALSE(h.coldest({}, 0, 0, out));
}

TEST(RootHotness, PruneAndForget) {
  RootHotness h(1);
  h.recordAccess(1, 0);
  h.recordAccess(2, 100);
  EXPECT_EQ(2u, h.size());
  EXPECT_EQ(1u, h.prune(100, 0.5));
  EXPECT_EQ(1u, h.size());
  h.forget(2);
  EXPECT_EQ(0u, h.size());
}

TEST(SlabImage, RoundTrip) {
  SlabImageWriter w;
  w.add("k1", "value-one");
  w.add("key-two", "");
  w.add("k3", std::string(1000, 'x'));
  EXPECT_EQ(3u, w.numRecords());

  SlabImageReader r(w.data());
  std::string_view k, v;
  ASSERT_TRUE(r.next(k, v));
  EXPECT_EQ("k1", k);
  EXPECT_EQ("value-one", v);
  ASSERT_TRUE(r.next(k, v));
  EXPECT_EQ("key-two", k);
  EXPECT_TRUE(v.empty());
  ASSERT_TRUE(r.next(k, v));
  EXPECT_EQ("k3", k);
  EXPECT_EQ(1000u, v.size());
  EXPECT_FALSE(r.next(k, v));
}

TEST(SlabImage, TruncatedDataIsRejected) {
  SlabImageWriter w;
  w.add("key", "payload");
  auto data = w.data();
  data.resize(data.size() - 1);
  SlabImageReader r(data);
  std::string_view k, v;
  EXPECT_FALSE(r.next(k, v));
}

TEST(RootNvmIndex, AddTakeAndDrop) {
  RootNvmIndex idx;
  auto k1 = idx.makeKey(7);
  auto k2 = idx.makeKey(7);
  EXPECT_NE(k1, k2);
  EXPECT_TRUE(isSlabImageKey(k1));
  EXPECT_FALSE(isSlabImageKey("plain-key"));

  idx.add(7, {k1, 10, 100});
  idx.add(7, {k2, 20, 200});
  idx.add(9, {idx.makeKey(9), 1, 1});
  EXPECT_TRUE(idx.contains(7));
  EXPECT_EQ(3u, idx.numImages());

  RootNvmIndex::Image dropped;
  EXPECT_TRUE(idx.drop(k1, &dropped));
  EXPECT_EQ(10u, dropped.numItems);
  EXPECT_FALSE(idx.drop(k1));

  auto taken = idx.take(7);
  ASSERT_EQ(1u, taken.size());
  EXPECT_EQ(k2, taken[0].key);
  EXPECT_FALSE(idx.contains(7));
  EXPECT_TRUE(idx.take(7).empty());
  EXPECT_EQ(1u, idx.numImages());
  EXPECT_EQ(std::vector<uint64_t>{9}, idx.roots());
}

TEST(RootNvmIndex, BoundedTakeKeepsTheRest) {
  RootNvmIndex idx;
  for (int i = 0; i < 3; i++) {
    idx.add(4, {idx.makeKey(4), 1, 1, 5});
  }
  int8_t pid = 0;
  ASSERT_TRUE(idx.poolOf(4, pid));
  EXPECT_EQ(5, pid);
  EXPECT_FALSE(idx.poolOf(8, pid));

  EXPECT_EQ(2u, idx.take(4, 2).size());
  EXPECT_TRUE(idx.contains(4));
  EXPECT_EQ(1u, idx.numImages());
  EXPECT_EQ(1u, idx.take(4, 2).size());
  EXPECT_FALSE(idx.contains(4));
  EXPECT_EQ(0u, idx.numImages());
}

} // namespace facebook::cachelib::tests
