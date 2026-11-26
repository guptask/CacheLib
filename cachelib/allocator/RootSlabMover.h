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

#include <functional>

#include "cachelib/common/PeriodicWorker.h"

namespace facebook {
namespace cachelib {

// Runs the cache's root slab tier maintenance every interval.
class RootSlabMover : public PeriodicWorker {
 public:
  explicit RootSlabMover(std::function<void()> fn) : fn_(std::move(fn)) {}

  ~RootSlabMover() override { stop(std::chrono::seconds(0)); }

 private:
  void work() final { fn_(); }

  std::function<void()> fn_;
};

} // namespace cachelib
} // namespace facebook
