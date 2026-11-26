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

#include "cachelib/cachebench/cache/Cache.h"
#include "cachelib/cachebench/util/CacheConfig.h"

namespace facebook::cachelib::cachebench {
namespace {

TEST(CacheConfigRootTest, RootOptionsAreOffByDefault) {
  const CacheConfig config{folly::dynamic::object()};

  EXPECT_EQ(0u, config.rootPrefixLen);
  EXPECT_EQ(0u, config.rootCount);
  EXPECT_FALSE(config.enableRootSlabTier);
}

TEST(CacheConfigRootTest, ParsesRootOptions) {
  const CacheConfig config{folly::dynamic::object("rootPrefixLen", 2)(
      "rootCount", 64)("enableRootSlabTier", true)("rootHotnessHalfLifeSecs",
                                                   30)(
      "rootSlabWorkerIntervalMs", 200)("rootMinFreeSlabsPerPool", 3)(
      "rootMaxSlabsPerIteration", 4)};

  EXPECT_EQ(2u, config.rootPrefixLen);
  EXPECT_EQ(64u, config.rootCount);
  EXPECT_TRUE(config.enableRootSlabTier);
  EXPECT_EQ(30u, config.rootHotnessHalfLifeSecs);
  EXPECT_EQ(200u, config.rootSlabWorkerIntervalMs);
  EXPECT_EQ(3u, config.rootMinFreeSlabsPerPool);
  EXPECT_EQ(4u, config.rootMaxSlabsPerIteration);
}

TEST(CacheConfigRootTest, TierNeedsPrefixAndNvm) {
  CacheConfig config{folly::dynamic::object("cacheSizeMB", 64)(
      "enableRootSlabTier", true)};
  EXPECT_THROW(Cache<LruAllocator>{config}, std::invalid_argument);

  config = CacheConfig{folly::dynamic::object("cacheSizeMB", 64)(
      "enableRootSlabTier", true)("rootPrefixLen", 1)};
  EXPECT_THROW(Cache<LruAllocator>{config}, std::invalid_argument);
}

TEST(CacheRootTest, RootOfIsStableAndBounded) {
  Cache<LruAllocator> cache{CacheConfig{folly::dynamic::object(
      "cacheSizeMB", 64)("rootPrefixLen", 2)("rootCount", 8)}};

  // keys that share the first two bytes share a root.
  EXPECT_EQ(cache.rootOf("abXXXX"), cache.rootOf("abYYYYYY"));
  EXPECT_NE(kNoRootHash, cache.rootOf("abXXXX"));
  for (int i = 0; i < 1000; i++) {
    const auto root = cache.rootOf(std::to_string(i));
    EXPECT_GE(root, 1u);
    EXPECT_LE(root, 8u);
  }
}

TEST(CacheRootTest, RootPlacementHoldsAfterRootedSets) {
  Cache<LruAllocator> cache{CacheConfig{folly::dynamic::object(
      "cacheSizeMB", 64)("rootPrefixLen", 1)("rootCount", 4)}};

  for (int i = 0; i < 2000; i++) {
    auto handle = cache.allocate(0, std::to_string(i), 1024);
    ASSERT_NE(nullptr, handle);
    cache.insertOrReplace(handle);
  }
  EXPECT_NO_THROW(cache.checkRootPlacement());
}

} // namespace
} // namespace facebook::cachelib::cachebench
