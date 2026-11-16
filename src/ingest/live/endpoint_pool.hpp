#pragma once
// Ordered pool of public RPC endpoints for one chain (ETH, SOL), each with
// its own token-bucket limiter sized to that endpoint's published limit, a
// health score and a cooldown. Pure logic on an injected clock (tests drive
// it with a fake clock and fake 429s).
//
// Rules (DESIGN_NOTES "RPC endpoint pools"):
//   * One endpoint is active at a time. Every request first takes a token
//     from the ACTIVE endpoint's bucket; when the bucket is empty the request
//     is skipped (the caller drops that follow-up and waits). An empty
//     bucket never moves traffic to another endpoint: the pool does not
//     spread load to get around a limit it is already at.
//   * Rotation happens only on a provider-side failure: a connect/TLS/HTTP
//     error, a timeout, or an explicit 429 / rate-limit error. The failed
//     endpoint enters a cooldown (longer after a 429, doubling on repeats up
//     to a cap) and the next healthy endpoint in pool order becomes active.
//   * Health: latency EWMA (alpha 0.2), error-rate EWMA over requests and a
//     429 count. With several endpoints healthy, rotation prefers pool order
//     (the config lists the preferred endpoint first).
//   * When every endpoint is cooling down, the one whose cooldown ends first
//     is chosen and `wait_ms()` says how long to wait before using it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ftv {

// Token bucket: `rate` tokens/s, capacity `burst`.
class TokenBucket {
 public:
  TokenBucket() = default;
  TokenBucket(double rate, double burst) noexcept : rate_(rate > 0.0 ? rate : 0.1), burst_(burst >= 1.0 ? burst : 1.0),
                                                    tokens_(burst_) {}
  bool try_take(std::int64_t nowMs) noexcept {
    refill(nowMs);
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
  }
  [[nodiscard]] double tokens(std::int64_t nowMs) noexcept {
    refill(nowMs);
    return tokens_;
  }
  [[nodiscard]] double rate() const noexcept { return rate_; }

 private:
  void refill(std::int64_t nowMs) noexcept {
    if (last_ < 0) last_ = nowMs;
    if (nowMs > last_) {
      tokens_ += static_cast<double>(nowMs - last_) * 1e-3 * rate_;
      if (tokens_ > burst_) tokens_ = burst_;
    }
    last_ = nowMs > last_ ? nowMs : last_;
  }
  double rate_ = 1.0;
  double burst_ = 1.0;
  double tokens_ = 1.0;
  std::int64_t last_ = -1;
};

struct EndpointConfig {
  std::string url;          // WebSocket URL (subscriptions)
  std::string rpc_url;      // HTTP JSON-RPC URL (follow-up requests); empty: same socket
  double max_rps = 1.0;     // our request budget = the published per-IP limit (or a conservative default)
  double burst = 1.0;
  std::string limit_note;   // the published limit as stated by the provider
  std::string limit_source; // URL of the provider's documentation
  std::string as_of;        // when the limit was retrieved
  bool pending_tx = false;  // supports eth_subscribe newPendingTransactions
};

enum class RotationReason : std::uint8_t { Start = 0, Error, RateLimited, Recovered };
inline constexpr const char* kRotationReasonNames[] = {"start", "error", "rate_limited", "recovered"};

struct RotationEvent {
  std::int64_t at_ms = 0;
  std::uint8_t from = 0, to = 0;
  RotationReason reason = RotationReason::Start;
};

struct EndpointHealth {
  double latency_ewma_ms = -1.0;
  double error_rate = 0.0;      // EWMA of 0/1 per request outcome
  std::uint32_t requests = 0;
  std::uint32_t errors = 0;
  std::uint32_t rate_limited = 0;  // 429s
  std::int64_t cooldown_until_ms = 0;
  std::uint32_t strikes = 0;    // consecutive failures (cooldown doubling)
};

class EndpointPool {
 public:
  static constexpr std::size_t kMaxEndpoints = 8;
  static constexpr std::size_t kHistory = 64;

  EndpointPool() = default;
  EndpointPool(std::vector<EndpointConfig> endpoints, std::int64_t errorCooldownMs = 30000,
               std::int64_t rateLimitCooldownMs = 120000, std::int64_t maxCooldownMs = 900000);

  [[nodiscard]] std::size_t size() const noexcept { return eps_.size(); }
  [[nodiscard]] std::size_t active() const noexcept { return active_; }
  [[nodiscard]] const EndpointConfig& endpoint(std::size_t i) const noexcept { return eps_[i]; }
  [[nodiscard]] const EndpointConfig& current() const noexcept { return eps_[active_]; }
  [[nodiscard]] const EndpointHealth& health(std::size_t i) const noexcept { return health_[i]; }

  // A request may go to the active endpoint now (takes one token). Never
  // switches endpoints.
  bool try_acquire(std::int64_t nowMs) noexcept;
  // Outcome of a request (or a successful connect).
  void on_success(std::int64_t nowMs, double latencyMs) noexcept;
  // Provider-side failure / 429: cools the active endpoint down and rotates.
  // Returns the new active index.
  std::size_t on_error(std::int64_t nowMs) noexcept;
  std::size_t on_rate_limited(std::int64_t nowMs) noexcept;
  // Milliseconds until the active endpoint's cooldown ends (0: usable now).
  [[nodiscard]] std::int64_t wait_ms(std::int64_t nowMs) const noexcept;
  [[nodiscard]] bool healthy(std::size_t i, std::int64_t nowMs) const noexcept {
    return nowMs >= health_[i].cooldown_until_ms;
  }
  // Health score (lower is better): latency x (1 + 4 x error rate) + 1000 per 429 (decayed by strikes).
  [[nodiscard]] double score(std::size_t i) const noexcept;

  // Rotation history (ring of the last kHistory events, oldest first).
  [[nodiscard]] std::size_t history_count() const noexcept { return hist_n_ < kHistory ? hist_n_ : kHistory; }
  [[nodiscard]] const RotationEvent& history(std::size_t k) const noexcept {
    const std::size_t first = hist_n_ < kHistory ? 0 : hist_n_ % kHistory;
    return hist_[(first + k) % kHistory];
  }
  [[nodiscard]] std::uint64_t rotations() const noexcept { return hist_n_ > 0 ? hist_n_ - 1 : 0; }

 private:
  std::size_t rotate(std::int64_t nowMs, RotationReason why) noexcept;
  void record(std::int64_t nowMs, std::size_t from, std::size_t to, RotationReason why) noexcept;

  std::vector<EndpointConfig> eps_;
  std::array<TokenBucket, kMaxEndpoints> buckets_{};
  std::array<EndpointHealth, kMaxEndpoints> health_{};
  std::size_t active_ = 0;
  std::int64_t error_cooldown_ms_ = 30000, rl_cooldown_ms_ = 120000, max_cooldown_ms_ = 900000;
  std::array<RotationEvent, kHistory> hist_{};
  std::uint64_t hist_n_ = 0;
};

}  // namespace ftv
