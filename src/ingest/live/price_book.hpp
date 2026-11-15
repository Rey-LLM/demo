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

// ECB euro foreign exchange reference rates as served by the FX feed
// (Frankfurter, re-based to USD). Written by the FX thread, read by the HUD
// and the exporter. Published once per working day around 16:00 CET, so the
// FX feed is labelled DELAYED, never LIVE.
class FxBoard {
 public:
  void set(Asset a, double usdPerUnit) noexcept {
    const auto i = static_cast<std::size_t>(a);
    if (i < kAssetCount && usdPerUnit > 0.0) usd_[i].store(usdPerUnit, std::memory_order_relaxed);
  }
  void set_date(std::uint32_t yyyymmdd, std::int64_t fetchedMs) noexcept {
    date_.store(yyyymmdd, std::memory_order_relaxed);
    fetched_ms_.store(fetchedMs, std::memory_order_relaxed);
  }
  // USD per unit of `a` (0 when unknown).
  [[nodiscard]] double usd(Asset a) const noexcept {
    const auto i = static_cast<std::size_t>(a);
    return i < kAssetCount ? usd_[i].load(std::memory_order_relaxed) : 0.0;
  }
  [[nodiscard]] std::uint32_t date() const noexcept { return date_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::int64_t fetched_ms() const noexcept { return fetched_ms_.load(std::memory_order_relaxed); }

 private:
  std::array<std::atomic<double>, kAssetCount> usd_{};
  std::atomic<std::uint32_t> date_{0};
  std::atomic<std::int64_t> fetched_ms_{0};
};

class PriceBook {
 public:
  void set(Asset a, double usd, std::int64_t ts_ms) noexcept {
    const auto i = static_cast<std::size_t>(a);
    if (i >= kAssetCount || !(usd > 0.0)) return;
    px_[i].store(usd, std::memory_order_relaxed);
    ts_[i].store(ts_ms, std::memory_order_relaxed);
  }
  // 0 when unknown. USD is always 1; USDT is at par until a USDT/USD tick arrives.
  [[nodiscard]] double usd(Asset a) const noexcept {
    if (a == Asset::USD) return 1.0;
    if (a == Asset::USDT) {
      const double p = px_[static_cast<std::size_t>(a)].load(std::memory_order_relaxed);
      return p > 0.0 ? p : 1.0;
    }
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
