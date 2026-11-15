#pragma once
// Per-feed connection state machine, reconnect backoff and the REPLAY
// fallback decision. Pure logic on an injected clock: the feed threads drive
// it with steady-clock milliseconds, the tests with a fake clock.
//
//   CONNECTING --first valid message--> LIVE
//   LIVE --parse errors / rejected method / stale watchdog--> DEGRADED
//   any --HTTP 429 or a rate-limit error--> RATE_LIMITED
//   any --connect or socket failure--> OFFLINE (reconnect after backoff)
//   OFFLINE or RATE_LIMITED for > fallback_after_ms (15 s) --> replay active
//   replay active --a valid live message again--> LIVE, replay stops
//
// "Replay active" is orthogonal to the connection state: the adapter keeps
// reconnecting while the bundled capture stands in for the feed, and the HUD
// and exports show the feed as REPLAY for exactly as long as it does.

#include <cstdint>

#include "ingest/live/live_event.hpp"

namespace ftv {

enum class FeedState : std::uint8_t { Connecting = 0, Live, Degraded, RateLimited, Offline, Count };
inline constexpr const char* kFeedStateNames[] = {"CONNECTING", "LIVE", "DEGRADED", "RATE_LIMITED", "OFFLINE"};

// Exponential backoff with full jitter: attempt n waits a uniform random time
// in [0, min(cap, base * 2^n)]. base 0.5 s, cap 30 s by default.
class Backoff {
 public:
  explicit Backoff(std::uint64_t seed = 0x9E3779B97F4A7C15ull, double baseS = 0.5, double capS = 30.0) noexcept
      : state_(seed ? seed : 1), base_s_(baseS), cap_s_(capS) {}
  // Upper bound of the next delay (s), before jitter.
  [[nodiscard]] double ceiling_s() const noexcept;
  // Returns the next delay in seconds and advances the attempt counter.
  double next_s() noexcept;
  void reset() noexcept { attempt_ = 0; }
  [[nodiscard]] unsigned attempt() const noexcept { return attempt_; }

 private:
  std::uint64_t state_;
  double base_s_, cap_s_;
  unsigned attempt_ = 0;
};

struct FeedTransition {
  std::int64_t at_ms = 0;  // clock of the state machine (UTC ms when driven by the app)
  FeedState state = FeedState::Connecting;
  bool replay = false;
};

class FeedStateMachine {
 public:
  explicit FeedStateMachine(std::int64_t fallbackAfterMs = 15000, std::int64_t staleAfterMs = 30000) noexcept
      : fallback_after_ms_(fallbackAfterMs), stale_after_ms_(staleAfterMs) {}

  // Inputs. Each returns true when the visible state (state or replay flag) changed.
  bool on_connecting(std::int64_t now) noexcept;
  bool on_message(std::int64_t now) noexcept;          // a valid live message
  bool on_degraded(std::int64_t now) noexcept;         // parse errors, rejected method
  bool on_rate_limited(std::int64_t now) noexcept;     // 429 or a rate-limit error
  bool on_offline(std::int64_t now) noexcept;          // connect or socket failure
  // Watchdog and fallback timers; call periodically (e.g. 4 Hz).
  bool tick(std::int64_t now) noexcept;

  [[nodiscard]] FeedState state() const noexcept { return state_; }
  [[nodiscard]] bool replay_active() const noexcept { return replay_; }
  // The label the feed's events carry right now.
  [[nodiscard]] SourceLabel label(SourceLabel liveLabel = SourceLabel::Live) const noexcept {
    return replay_ ? SourceLabel::Replay : liveLabel;
  }
  [[nodiscard]] std::int64_t since_ms() const noexcept { return since_; }
  [[nodiscard]] std::int64_t last_message_ms() const noexcept { return last_msg_; }

 private:
  bool set(FeedState s, std::int64_t now) noexcept;

  std::int64_t fallback_after_ms_;
  std::int64_t stale_after_ms_;
  FeedState state_ = FeedState::Connecting;
  bool replay_ = false;
  std::int64_t since_ = 0;       // when state_ was entered
  std::int64_t last_msg_ = -1;
  std::int64_t down_since_ = 0;  // when the feed last stopped being LIVE/DEGRADED (or first tried)
  bool failed_ = false;          // an OFFLINE or RATE_LIMITED input since down_since_
  bool started_ = false;
};

}  // namespace ftv
