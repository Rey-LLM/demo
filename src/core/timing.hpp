#pragma once
// Monotonic timing helpers.

#include <chrono>
#include <cstdint>

namespace ftv {

class Stopwatch {
 public:
  using Clock = std::chrono::steady_clock;
  Stopwatch() noexcept : start_(Clock::now()) {}
  void reset() noexcept { start_ = Clock::now(); }
  [[nodiscard]] double seconds() const noexcept {
    return std::chrono::duration<double>(Clock::now() - start_).count();
  }
  [[nodiscard]] double millis() const noexcept { return seconds() * 1000.0; }
  // Returns elapsed milliseconds and restarts the watch.
  double lap_ms() noexcept {
    const auto now = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - start_).count();
    start_ = now;
    return ms;
  }

 private:
  Clock::time_point start_;
};

}  // namespace ftv
