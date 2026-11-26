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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace facebook {
namespace cachelib {

// Evicts whole slabs of the coldest root to Navy and promotes them back, see
// CacheAllocator::allocateWithRoot. Needs a Navy cache.
struct RootSlabTierConfig {
  bool enabled{false};

  // a root's access count halves after this long without accesses.
  uint32_t hotnessHalfLifeSecs{600};

  // period of the background worker; 0 runs only the inline demotion on
  // allocation failure and promotion on read miss.
  std::chrono::milliseconds workerInterval{0};

  // the worker demotes cold slabs while a pool has fewer free slabs.
  uint32_t minFreeSlabsPerPool{1};

  // limit of slabs the worker demotes or promotes per pool and iteration.
  uint32_t maxSlabsPerIteration{2};
};

struct RootSlabTierStats {
  uint64_t slabsDemoted{0};
  uint64_t itemsDemoted{0};
  uint64_t slabsPromoted{0};
  uint64_t itemsPromoted{0};
  // images navy refused; their items were dropped.
  uint64_t imageWriteFailures{0};
  // images navy evicted before they were promoted.
  uint64_t imagesDropped{0};
  // promoted items that expired or found no memory.
  uint64_t itemsLostOnPromote{0};
  uint64_t imagesInNvm{0};
};

// Navy key prefix of the objects that hold the items of one evicted slab.
inline constexpr std::string_view kSlabImageKeyPrefix = "\x01\x02slab-image:";

inline bool isSlabImageKey(std::string_view key) {
  return key.size() >= kSlabImageKeyPrefix.size() &&
         key.substr(0, kSlabImageKeyPrefix.size()) == kSlabImageKeyPrefix;
}

// A slab image is a sequence of records:
//   u32 keySize | key | u32 payloadSize | payload
// where the payload is a serialized NvmItem.
class SlabImageWriter {
 public:
  void add(std::string_view key, std::string_view payload) {
    append(static_cast<uint32_t>(key.size()));
    data_.append(key);
    append(static_cast<uint32_t>(payload.size()));
    data_.append(payload);
    ++numRecords_;
  }

  const std::string& data() const noexcept { return data_; }
  size_t numRecords() const noexcept { return numRecords_; }

 private:
  void append(uint32_t v) {
    data_.append(reinterpret_cast<const char*>(&v), sizeof(v));
  }

  std::string data_;
  size_t numRecords_{0};
};

class SlabImageReader {
 public:
  explicit SlabImageReader(std::string_view data) : data_(data) {}

  // Reads the next record. Returns false at the end or on malformed data.
  bool next(std::string_view& key, std::string_view& payload) {
    return readSized(key) && readSized(payload);
  }

 private:
  bool readSized(std::string_view& out) {
    uint32_t size;
    if (data_.size() - pos_ < sizeof(size)) {
      return false;
    }
    std::memcpy(&size, data_.data() + pos_, sizeof(size));
    pos_ += sizeof(size);
    if (data_.size() - pos_ < size) {
      return false;
    }
    out = data_.substr(pos_, size);
    pos_ += size;
    return true;
  }

  std::string_view data_;
  size_t pos_{0};
};

// In-memory index of the slab images that sit in Navy, by root. Not persisted:
// images left in Navy after a restart are only ever reclaimed by Navy.
class RootNvmIndex {
 public:
  struct Image {
    std::string key;
    uint32_t numItems{0};
    uint64_t bytes{0};
    int8_t pid{0};
  };

  std::string makeKey(uint64_t root) {
    return std::string(kSlabImageKeyPrefix) + std::to_string(root) + "/" +
           std::to_string(seq_.fetch_add(1, std::memory_order_relaxed));
  }

  void add(uint64_t root, Image image) {
    std::lock_guard<std::mutex> l(mutex_);
    keyToRoot_[image.key] = root;
    images_[root].push_back(std::move(image));
  }

  // Removes and returns up to `maxImages` images of the root.
  std::vector<Image> take(uint64_t root, size_t maxImages = SIZE_MAX) {
    std::lock_guard<std::mutex> l(mutex_);
    auto it = images_.find(root);
    if (it == images_.end()) {
      return {};
    }
    auto& all = it->second;
    const size_t n = std::min(maxImages, all.size());
    std::vector<Image> images(std::make_move_iterator(all.end() - n),
                              std::make_move_iterator(all.end()));
    all.resize(all.size() - n);
    if (all.empty()) {
      images_.erase(it);
    }
    for (const auto& image : images) {
      keyToRoot_.erase(image.key);
    }
    return images;
  }

  // pool of the first image of the root, if it has any.
  bool poolOf(uint64_t root, int8_t& pid) const {
    std::lock_guard<std::mutex> l(mutex_);
    auto it = images_.find(root);
    if (it == images_.end() || it->second.empty()) {
      return false;
    }
    pid = it->second.front().pid;
    return true;
  }

  // Navy dropped the image. Returns false if the image is not indexed.
  bool drop(const std::string& imageKey, Image* dropped = nullptr) {
    std::lock_guard<std::mutex> l(mutex_);
    auto rit = keyToRoot_.find(imageKey);
    if (rit == keyToRoot_.end()) {
      return false;
    }
    const auto root = rit->second;
    keyToRoot_.erase(rit);
    auto& images = images_[root];
    for (auto it = images.begin(); it != images.end(); ++it) {
      if (it->key == imageKey) {
        if (dropped) {
          *dropped = std::move(*it);
        }
        images.erase(it);
        break;
      }
    }
    if (images.empty()) {
      images_.erase(root);
    }
    return true;
  }

  bool contains(uint64_t root) const {
    std::lock_guard<std::mutex> l(mutex_);
    return images_.count(root) != 0;
  }

  std::vector<uint64_t> roots() const {
    std::lock_guard<std::mutex> l(mutex_);
    std::vector<uint64_t> roots;
    roots.reserve(images_.size());
    for (const auto& kv : images_) {
      roots.push_back(kv.first);
    }
    return roots;
  }

  size_t numImages() const {
    std::lock_guard<std::mutex> l(mutex_);
    return keyToRoot_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::vector<Image>> images_;
  std::unordered_map<std::string, uint64_t> keyToRoot_;
  std::atomic<uint64_t> seq_{0};
};

} // namespace cachelib
} // namespace facebook
