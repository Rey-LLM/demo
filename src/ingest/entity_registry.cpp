#include "ingest/entity_registry.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace ftv {

void EntityRegistry::reserve(std::size_t expectedTotal) {
  const std::size_t perShard = expectedTotal / kShardCount + 64;
  for (auto& s : shards_) s.map.reserve(perShard);
}

void EntityRegistry::merge_batch(unsigned shard, std::span<const RegistryItem> items) {
  Shard& s = shards_[shard];
  std::lock_guard<SpinLock> guard(s.lock);
  for (const RegistryItem& it : items) {
    bool inserted = false;
    Entry& e = s.map.find_or_insert(it.key, inserted);
    if (inserted || it.label_offset < e.label_offset) {
      e.label_offset = it.label_offset;
      e.label_len = it.label_len;
    }
    if (inserted) e.dense = kInvalidIndex;
  }
}

std::size_t EntityRegistry::assign_dense_indices(ThreadPool& pool, AlignedVector<std::uint64_t>& keys,
                                                 AlignedVector<std::uint64_t>& labelOffset,
                                                 AlignedVector<std::uint16_t>& labelLen) {
  std::array<std::vector<std::uint64_t>, kShardCount> sorted;
  pool.parallel_for_range(kShardCount, 1, [&](std::size_t b, std::size_t e) {
    for (std::size_t s = b; s < e; ++s) {
      auto& v = sorted[s];
      v.reserve(shards_[s].map.size());
      shards_[s].map.for_each([&v](std::uint64_t k, const Entry&) { v.push_back(k); });
      std::sort(v.begin(), v.end());
    }
  });
  std::array<std::size_t, kShardCount + 1> offset{};
  for (unsigned s = 0; s < kShardCount; ++s) offset[s + 1] = offset[s] + sorted[s].size();
  const std::size_t total = offset[kShardCount];
  keys.resize(total);
  labelOffset.resize(total);
  labelLen.resize(total);
  pool.parallel_for_range(kShardCount, 1, [&](std::size_t b, std::size_t e) {
    for (std::size_t s = b; s < e; ++s) {
      const auto& v = sorted[s];
      for (std::size_t i = 0; i < v.size(); ++i) {
        const std::size_t dense = offset[s] + i;
        Entry* entry = shards_[s].map.find(v[i]);
        entry->dense = static_cast<std::uint32_t>(dense);
        keys[dense] = v[i];
        labelOffset[dense] = entry->label_offset;
        labelLen[dense] = static_cast<std::uint16_t>(std::min<std::uint32_t>(entry->label_len, 0xFFFF));
      }
    }
  });
  return total;
}

std::uint32_t EntityRegistry::dense_of(std::uint64_t key) const noexcept {
  const Entry* e = shards_[shard_of(key)].map.find(key);
  return e ? e->dense : kInvalidIndex;
}

std::size_t EntityRegistry::size() const noexcept {
  std::size_t n = 0;
  for (const auto& s : shards_) n += s.map.size();
  return n;
}

}  // namespace ftv
