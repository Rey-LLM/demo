#pragma once
// Open-addressing hash map with linear probing and SoA storage (control bytes,
// keys and values in separate cache-aligned arrays). Used for the per-worker
// thread-local aggregation tables and the registry shards.
//
// Capacity is reserved up-front from input-size estimates so that the per-row
// path normally performs no allocation; growth (rehash at 70% load) is the
// exceptional slow path.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "core/aligned_allocator.hpp"

namespace ftv {

template <class K, class V, class Hash, class Eq = std::equal_to<K>>
class FlatHashMap {
 public:
  explicit FlatHashMap(std::size_t expected = 16) { reserve(expected); }

  void reserve(std::size_t expected) {
    std::size_t cap = 16;
    while (cap * 7 < expected * 10) cap <<= 1;
    if (cap > ctrl_.size()) rehash(cap);
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return ctrl_.size(); }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  // Returns the value slot for k, inserting a value-initialised V if absent.
  V& find_or_insert(const K& k, bool& inserted) {
    if ((size_ + 1) * 10 > ctrl_.size() * 7) rehash(ctrl_.size() * 2);
    std::size_t i = Hash{}(k) & mask_;
    for (;;) {
      if (ctrl_[i] == 0) {
        ctrl_[i] = 1;
        keys_[i] = k;
        vals_[i] = V{};
        ++size_;
        inserted = true;
        return vals_[i];
      }
      if (Eq{}(keys_[i], k)) {
        inserted = false;
        return vals_[i];
      }
      i = (i + 1) & mask_;
    }
  }

  [[nodiscard]] V* find(const K& k) noexcept {
    std::size_t i = Hash{}(k) & mask_;
    for (;;) {
      if (ctrl_[i] == 0) return nullptr;
      if (Eq{}(keys_[i], k)) return &vals_[i];
      i = (i + 1) & mask_;
    }
  }
  [[nodiscard]] const V* find(const K& k) const noexcept { return const_cast<FlatHashMap*>(this)->find(k); }

  template <class F>
  void for_each(F&& f) const {
    for (std::size_t i = 0; i < ctrl_.size(); ++i) {
      if (ctrl_[i]) f(keys_[i], vals_[i]);
    }
  }
  template <class F>
  void for_each_mut(F&& f) {
    for (std::size_t i = 0; i < ctrl_.size(); ++i) {
      if (ctrl_[i]) f(keys_[i], vals_[i]);
    }
  }

  void clear() noexcept {
    for (auto& c : ctrl_) c = 0;
    size_ = 0;
  }

  void release() noexcept {
    AlignedVector<std::uint8_t>().swap(ctrl_);
    AlignedVector<K>().swap(keys_);
    AlignedVector<V>().swap(vals_);
    size_ = 0;
    mask_ = 0;
  }

 private:
  void rehash(std::size_t newCap) {
    AlignedVector<std::uint8_t> oldCtrl(newCap, 0);
    AlignedVector<K> oldKeys(newCap);
    AlignedVector<V> oldVals(newCap);
    oldCtrl.swap(ctrl_);
    oldKeys.swap(keys_);
    oldVals.swap(vals_);
    mask_ = newCap - 1;
    size_ = 0;
    for (std::size_t i = 0; i < oldCtrl.size(); ++i) {
      if (!oldCtrl[i]) continue;
      std::size_t j = Hash{}(oldKeys[i]) & mask_;
      while (ctrl_[j]) j = (j + 1) & mask_;
      ctrl_[j] = 1;
      keys_[j] = oldKeys[i];
      vals_[j] = std::move(oldVals[i]);
      ++size_;
    }
  }

  AlignedVector<std::uint8_t> ctrl_;
  AlignedVector<K> keys_;
  AlignedVector<V> vals_;
  std::size_t size_ = 0;
  std::size_t mask_ = 0;
};

// Finaliser from SplitMix64 / Murmur3; good avalanche for integer keys.
constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return x;
}

}  // namespace ftv
