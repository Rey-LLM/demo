#pragma once
// Allocation-free text builder over a fixed-size char array. Used for HUD
// lines that are rebuilt while the render loop runs, where heap allocation is
// forbidden. All formatting goes through std::to_chars (locale-independent,
// never allocates); output that does not fit is truncated.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>

namespace ftv {

template <std::size_t N>
class FixedText {
 public:
  void clear() noexcept { len_ = 0; }
  [[nodiscard]] std::string_view view() const noexcept { return {buf_, len_}; }
  [[nodiscard]] std::size_t size() const noexcept { return len_; }

  FixedText& str(std::string_view s) noexcept {
    const std::size_t n = std::min(s.size(), N - len_);
    if (n) std::memcpy(buf_ + len_, s.data(), n);
    len_ += n;
    return *this;
  }
  FixedText& ch(char c) noexcept {
    if (len_ < N) buf_[len_++] = c;
    return *this;
  }
  FixedText& u64(std::uint64_t v) noexcept {
    const auto r = std::to_chars(buf_ + len_, buf_ + N, v);
    if (r.ec == std::errc()) len_ = static_cast<std::size_t>(r.ptr - buf_);
    return *this;
  }
  // Integer with thousands separators (1,234,567).
  FixedText& grouped(std::uint64_t v) noexcept {
    char tmp[24];
    const auto r = std::to_chars(tmp, tmp + sizeof(tmp), v);
    const auto digits = static_cast<std::size_t>(r.ptr - tmp);
    for (std::size_t i = 0; i < digits; ++i) {
      if (i > 0 && (digits - i) % 3 == 0) ch(',');
      ch(tmp[i]);
    }
    return *this;
  }
  FixedText& fixed(double v, int precision) noexcept {
    if (!std::isfinite(v)) return str("-");
    const auto r = std::to_chars(buf_ + len_, buf_ + N, v, std::chars_format::fixed, precision);
    if (r.ec == std::errc()) len_ = static_cast<std::size_t>(r.ptr - buf_);
    return *this;
  }
  // Compact magnitude: 950, 12.4K, 1.62M, 3.10G.
  FixedText& si(double v, int precision) noexcept {
    const double a = std::fabs(v);
    if (a >= 1e9) return fixed(v / 1e9, precision).ch('G');
    if (a >= 1e6) return fixed(v / 1e6, precision).ch('M');
    if (a >= 1e4) return fixed(v / 1e3, precision).ch('K');
    return fixed(v, a >= 100.0 ? 0 : precision);
  }
  // Pads with spaces up to the given column.
  FixedText& pad_to(std::size_t column) noexcept {
    while (len_ < column && len_ < N) buf_[len_++] = ' ';
    return *this;
  }

 private:
  char buf_[N]{};
  std::size_t len_ = 0;
};

}  // namespace ftv
