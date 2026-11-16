#include "ingest/live/endpoint_pool.hpp"

#include <algorithm>

namespace ftv {

EndpointPool::EndpointPool(std::vector<EndpointConfig> endpoints, std::int64_t errorCooldownMs,
                           std::int64_t rateLimitCooldownMs, std::int64_t maxCooldownMs)
    : eps_(std::move(endpoints)),
      error_cooldown_ms_(errorCooldownMs),
      rl_cooldown_ms_(rateLimitCooldownMs),
      max_cooldown_ms_(maxCooldownMs) {
  if (eps_.size() > kMaxEndpoints) eps_.resize(kMaxEndpoints);
  for (std::size_t i = 0; i < eps_.size(); ++i) buckets_[i] = TokenBucket(eps_[i].max_rps, eps_[i].burst);
  record(0, 0, 0, RotationReason::Start);
}

bool EndpointPool::try_acquire(std::int64_t nowMs) noexcept {
  if (eps_.empty() || !healthy(active_, nowMs)) return false;
  return buckets_[active_].try_take(nowMs);
}

void EndpointPool::on_success(std::int64_t nowMs, double latencyMs) noexcept {
  (void)nowMs;
  if (eps_.empty()) return;
  EndpointHealth& h = health_[active_];
  ++h.requests;
  h.strikes = 0;
  h.error_rate *= 0.8;
  if (latencyMs >= 0.0) h.latency_ewma_ms = h.latency_ewma_ms < 0.0 ? latencyMs : h.latency_ewma_ms + 0.2 * (latencyMs - h.latency_ewma_ms);
}

double EndpointPool::score(std::size_t i) const noexcept {
  const EndpointHealth& h = health_[i];
  const double lat = h.latency_ewma_ms < 0.0 ? 250.0 : h.latency_ewma_ms;
  return lat * (1.0 + 4.0 * h.error_rate) + 1000.0 * static_cast<double>(std::min<std::uint32_t>(h.rate_limited, 8));
}

std::size_t EndpointPool::on_error(std::int64_t nowMs) noexcept {
  if (eps_.empty()) return 0;
  EndpointHealth& h = health_[active_];
  ++h.requests;
  ++h.errors;
  h.error_rate = h.error_rate * 0.8 + 0.2;
  const std::int64_t cd = std::min(max_cooldown_ms_, error_cooldown_ms_ << std::min<std::uint32_t>(h.strikes, 5));
  ++h.strikes;
  h.cooldown_until_ms = nowMs + cd;
  return rotate(nowMs, RotationReason::Error);
}

std::size_t EndpointPool::on_rate_limited(std::int64_t nowMs) noexcept {
  if (eps_.empty()) return 0;
  EndpointHealth& h = health_[active_];
  ++h.requests;
  ++h.rate_limited;
  h.error_rate = h.error_rate * 0.8 + 0.2;
  const std::int64_t cd = std::min(max_cooldown_ms_, rl_cooldown_ms_ << std::min<std::uint32_t>(h.strikes, 3));
  ++h.strikes;
  h.cooldown_until_ms = nowMs + cd;
  return rotate(nowMs, RotationReason::RateLimited);
}

std::size_t EndpointPool::rotate(std::int64_t nowMs, RotationReason why) noexcept {
  const std::size_t from = active_, n = eps_.size();
  // Next healthy endpoint in pool order after the active one.
  std::size_t pick = n;
  for (std::size_t k = 1; k <= n; ++k) {
    const std::size_t i = (from + k) % n;
    if (healthy(i, nowMs)) {
      pick = i;
      break;
    }
  }
  if (pick == n) {
    // All cooling down: the one available soonest (ties: pool order).
    pick = 0;
    for (std::size_t i = 1; i < n; ++i) {
      if (health_[i].cooldown_until_ms < health_[pick].cooldown_until_ms) pick = i;
    }
  }
  active_ = pick;
  record(nowMs, from, pick, why);
  return pick;
}

std::int64_t EndpointPool::wait_ms(std::int64_t nowMs) const noexcept {
  if (eps_.empty()) return 0;
  const std::int64_t until = health_[active_].cooldown_until_ms;
  return until > nowMs ? until - nowMs : 0;
}

void EndpointPool::record(std::int64_t nowMs, std::size_t from, std::size_t to, RotationReason why) noexcept {
  RotationEvent& e = hist_[hist_n_ % kHistory];
  e.at_ms = nowMs;
  e.from = static_cast<std::uint8_t>(from);
  e.to = static_cast<std::uint8_t>(to);
  e.reason = why;
  ++hist_n_;
}

}  // namespace ftv
