#pragma once
// Concurrent entity registry: key64 -> dense uint32 node index.
//
// 64 shards selected by the top 6 bits of the key (key64 >> 58). Each shard is
// an open-addressing table guarded by its own SpinLock (see spinlock.hpp for
// the justification). Workers deduplicate into thread-local tables while
// parsing and merge here in bulk: one lock acquisition per (worker, shard).
//
// Determinism: after all merges, assign_dense_indices() sorts the keys of each
// shard. Because a shard is exactly a contiguous range of the key space
// (selected by the key's top bits), concatenating the sorted shards in shard
// order yields the globally sorted key list, and dense index == rank of the
// key. The result is identical for any thread count or merge order.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/aligned_allocator.hpp"
#include "core/flat_hash_map.hpp"
#include "core/spinlock.hpp"
#include "core/thread_pool.hpp"

namespace ftv {

struct RegistryItem {
  std::uint64_t key;
  std::uint64_t label_offset;
  std::uint32_t label_len;
};

class EntityRegistry {
 public:
  static constexpr unsigned kShardBits = 6;
  static constexpr unsigned kShardCount = 1u << kShardBits;
  static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

  [[nodiscard]] static constexpr unsigned shard_of(std::uint64_t key) noexcept {
    return static_cast<unsigned>(key >> (64 - kShardBits));
  }

  void reserve(std::size_t expectedTotal);

  // Inserts a batch of items that all belong to `shard` under one lock.
  void merge_batch(unsigned shard, std::span<const RegistryItem> items);

  // Assigns dense indices (sorted key order). Fills keys / label arrays,
  // indexed by dense index. Returns the number of unique entities.
  std::size_t assign_dense_indices(ThreadPool& pool, AlignedVector<std::uint64_t>& keys,
                                   AlignedVector<std::uint64_t>& labelOffset, AlignedVector<std::uint16_t>& labelLen);

  // Lock-free lookup; valid only after assign_dense_indices() returned (the
  // thread pool join provides the happens-before edge for readers).
  [[nodiscard]] std::uint32_t dense_of(std::uint64_t key) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept;

 private:
  struct Entry {
    std::uint64_t label_offset;
    std::uint32_t label_len;
    std::uint32_t dense;
  };
  struct KeyIdentityHash {
    // Keys are already uniformly distributed XXH3 outputs; the shard uses the
    // top bits and the table index uses the low bits, so they are independent.
    std::size_t operator()(std::uint64_t k) const noexcept { return static_cast<std::size_t>(k); }
  };
  struct alignas(kCacheLine) Shard {
    SpinLock lock;
    FlatHashMap<std::uint64_t, Entry, KeyIdentityHash> map{16};
  };
  std::array<Shard, kShardCount> shards_;
};

}  // namespace ftv
