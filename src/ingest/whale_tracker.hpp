#pragma once
// Per-currency top-K of the largest single transactions ("whales").
//
// Amounts are compared only within one currency: ftviz has no FX table, so a
// USD amount and a BTC amount are never ranked against each other. Each parse
// worker keeps its own tracker (fixed-size arrays, no allocation per row); the
// trackers are merged after the parse. The ranking is a strict total order
// (|amount| desc, then timestamp, keys, channel), so the merged top-K is the
// same for any thread count or chunk size.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ftv {

struct WhaleCandidate {
  std::int64_t amount_minor = 0;
  std::int64_t ts_ms = 0;
  std::uint64_t src = 0;  // key64
  std::uint64_t dst = 0;  // key64
  std::uint32_t currency = 0;
  std::uint32_t channel = 0;
};

inline std::uint64_t whale_magnitude(std::int64_t v) noexcept {
  const auto u = static_cast<std::uint64_t>(v);
  return v < 0 ? 0ull - u : u;  // |v| without overflow for INT64_MIN
}

// True if `a` ranks before `b` (larger first).
inline bool whale_before(const WhaleCandidate& a, const WhaleCandidate& b) noexcept {
  const std::uint64_t ma = whale_magnitude(a.amount_minor);
  const std::uint64_t mb = whale_magnitude(b.amount_minor);
  if (ma != mb) return ma > mb;
  if (a.ts_ms != b.ts_ms) return a.ts_ms < b.ts_ms;
  if (a.src != b.src) return a.src < b.src;
  if (a.dst != b.dst) return a.dst < b.dst;
  return a.channel < b.channel;
}

class WhaleTracker {
 public:
  static constexpr std::size_t kMaxCurrencies = 32;
  static constexpr std::size_t kPerCurrency = 16;

  void offer(const WhaleCandidate& c) noexcept {
    Bucket* b = nullptr;
    for (std::uint32_t i = 0; i < used_; ++i) {
      if (buckets_[i].currency == c.currency) {
        b = &buckets_[i];
        break;
      }
    }
    if (!b) {
      if (used_ == kMaxCurrencies) return;  // currencies beyond the cap carry no whales
      b = &buckets_[used_++];
      b->currency = c.currency;
    }
    if (b->count < kPerCurrency) {
      b->items[b->count++] = c;
      if (b->count == kPerCurrency) b->recompute_min();
      return;
    }
    if (!whale_before(c, b->items[b->min_index])) return;
    b->items[b->min_index] = c;
    b->recompute_min();
  }

  template <class F>
  void for_each(F&& f) const {
    for (std::uint32_t i = 0; i < used_; ++i) {
      for (std::uint32_t k = 0; k < buckets_[i].count; ++k) f(buckets_[i].items[k]);
    }
  }

 private:
  struct Bucket {
    std::uint32_t currency = 0;
    std::uint32_t count = 0;
    std::uint32_t min_index = 0;
    std::array<WhaleCandidate, kPerCurrency> items{};

    void recompute_min() noexcept {
      std::uint32_t m = 0;
      for (std::uint32_t k = 1; k < count; ++k) {
        if (whale_before(items[m], items[k])) m = k;
      }
      min_index = m;
    }
  };
  std::array<Bucket, kMaxCurrencies> buckets_{};
  std::uint32_t used_ = 0;
};

// Merges worker candidates: per currency the top kPerCurrency by whale_before,
// returned grouped by currency (ascending code), each group in rank order.
inline std::vector<WhaleCandidate> merge_whales(std::vector<WhaleCandidate> all) {
  std::sort(all.begin(), all.end(), [](const WhaleCandidate& a, const WhaleCandidate& b) {
    if (a.currency != b.currency) return a.currency < b.currency;
    return whale_before(a, b);
  });
  std::vector<WhaleCandidate> out;
  std::size_t i = 0;
  while (i < all.size()) {
    std::size_t j = i;
    while (j < all.size() && all[j].currency == all[i].currency) ++j;
    for (std::size_t k = i; k < std::min(j, i + WhaleTracker::kPerCurrency); ++k) out.push_back(all[k]);
    i = j;
  }
  return out;
}

}  // namespace ftv
