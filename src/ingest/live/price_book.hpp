#pragma once
// Latest USD prices of the chain assets, written by the exchange adapters
// (Coinbase and Kraken tickers and trades) and read by the chain adapters to
// value transactions. Lock-free: one relaxed atomic double per asset plus
// the time it was set; a reader may see a price from a moment earlier, which
// is fine for valuation.

#include <array>
#include <atomic>
#include <cstdint>

#include "ingest/live/live_event.hpp"

namespace ftv {

class PriceBook {
 public:
  void set(Asset a, double usd, std::int64_t ts_ms) noexcept {
    const auto i = static_cast<std::size_t>(a);
    if (i >= kAssetCount || !(usd > 0.0)) return;
    px_[i].store(usd, std::memory_order_relaxed);
    ts_[i].store(ts_ms, std::memory_order_relaxed);
  }
  // 0 when unknown. USD is always 1.
  [[nodiscard]] double usd(Asset a) const noexcept {
    if (a == Asset::USD) return 1.0;
    const auto i = static_cast<std::size_t>(a);
    return i < kAssetCount ? px_[i].load(std::memory_order_relaxed) : 0.0;
  }
  [[nodiscard]] std::int64_t updated_ms(Asset a) const noexcept {
    const auto i = static_cast<std::size_t>(a);
    return i < kAssetCount ? ts_[i].load(std::memory_order_relaxed) : 0;
  }

 private:
  std::array<std::atomic<double>, kAssetCount> px_{};
  std::array<std::atomic<std::int64_t>, kAssetCount> ts_{};
};

}  // namespace ftv
