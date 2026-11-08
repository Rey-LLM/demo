#pragma once
// Exact 128-bit signed accumulator for int64 minor-unit amounts.
//
// Sums of many int64 amounts can exceed int64. Saturating addition would make
// the result depend on summation order (and therefore on thread scheduling);
// 128-bit two's-complement addition is associative, so partial sums merged in
// any order give the same exact total, which is clamped once at the end.

#include <cstdint>
#include <limits>

namespace ftv {

struct Int128Acc {
  std::uint64_t lo = 0;
  std::int64_t hi = 0;

  constexpr void add(std::int64_t v) noexcept {
    const std::uint64_t old = lo;
    lo += static_cast<std::uint64_t>(v);
    hi += (v < 0 ? -1 : 0) + (lo < old ? 1 : 0);
  }
  constexpr void add(const Int128Acc& o) noexcept {
    const std::uint64_t old = lo;
    lo += o.lo;
    hi += o.hi + (lo < old ? 1 : 0);
  }
  [[nodiscard]] constexpr std::int64_t clamp_to_int64() const noexcept {
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    if (hi == 0 && lo <= static_cast<std::uint64_t>(kMax)) return static_cast<std::int64_t>(lo);
    if (hi == -1 && lo >= static_cast<std::uint64_t>(kMin)) return static_cast<std::int64_t>(lo);
    return hi < 0 ? kMin : kMax;
  }
  [[nodiscard]] constexpr bool operator==(const Int128Acc&) const noexcept = default;
};

}  // namespace ftv
