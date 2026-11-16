#include "ingest/live/feed_state.hpp"

#include <algorithm>
#include <cmath>

namespace ftv {

namespace {

std::uint64_t splitmix(std::uint64_t& s) noexcept {
  std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

bool healthy(FeedState s) noexcept { return s == FeedState::Live || s == FeedState::Degraded; }

}  // namespace

double Backoff::ceiling_s() const noexcept {
  const double c = base_s_ * std::ldexp(1.0, static_cast<int>(std::min(attempt_, 30u)));
  return std::min(cap_s_, c);
}

double Backoff::next_s() noexcept {
  const double ceil = ceiling_s();
  ++attempt_;
  const double u = static_cast<double>(splitmix(state_) >> 11) * (1.0 / 9007199254740992.0);  // [0, 1)
  return u * ceil;
}

bool FeedStateMachine::set(FeedState s, std::int64_t now) noexcept {
  if (s == state_) return false;
  if (healthy(state_) && !healthy(s)) {
    down_since_ = now;
    failed_ = false;
  }
  state_ = s;
  since_ = now;
  return true;
}

bool FeedStateMachine::on_connecting(std::int64_t now) noexcept {
  if (!started_) {
    started_ = true;
    down_since_ = now;
  }
  return set(FeedState::Connecting, now);
}

bool FeedStateMachine::on_message(std::int64_t now) noexcept {
  last_msg_ = now;
  bool changed = set(FeedState::Live, now);
  if (replay_) {
    replay_ = false;  // recovery: the live feed takes over again
    changed = true;
  }
  return changed;
}

bool FeedStateMachine::on_degraded(std::int64_t now) noexcept {
  if (!healthy(state_)) return false;  // only a connected feed can be degraded
  return set(FeedState::Degraded, now);
}

bool FeedStateMachine::on_rate_limited(std::int64_t now) noexcept {
  if (!started_) {
    started_ = true;
    down_since_ = now;
  }
  const bool changed = set(FeedState::RateLimited, now);
  failed_ = true;
  return changed | tick(now);
}

bool FeedStateMachine::on_offline(std::int64_t now) noexcept {
  if (!started_) {
    started_ = true;
    down_since_ = now;
  }
  const bool changed = set(FeedState::Offline, now);
  failed_ = true;
  return changed | tick(now);
}

bool FeedStateMachine::on_geo_blocked(std::int64_t now) noexcept {
  if (!started_) {
    started_ = true;
    down_since_ = now;
  }
  bool changed = set(FeedState::GeoBlocked, now);
  failed_ = false;  // not a failure the capture may stand in for
  if (replay_) {
    replay_ = false;
    changed = true;
  }
  return changed;
}

bool FeedStateMachine::tick(std::int64_t now) noexcept {
  bool changed = false;
  // Stale-feed watchdog: connected but silent.
  if (state_ == FeedState::Live && last_msg_ >= 0 && now - last_msg_ > stale_after_ms_) {
    changed |= set(FeedState::Degraded, now);
  }
  // Fallback: down (offline or rate limited, reconnect attempts included) for
  // longer than fallback_after_ms_.
  if (!replay_ && !healthy(state_) && state_ != FeedState::GeoBlocked && failed_ &&
      now - down_since_ > fallback_after_ms_) {
    replay_ = true;
    changed = true;
  }
  return changed;
}

}  // namespace ftv
