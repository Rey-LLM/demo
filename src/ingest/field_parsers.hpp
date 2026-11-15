#pragma once
// Zero-allocation field parsers. All functions operate on std::string_view
// slices of the mapped input, use std::from_chars / hand-written digit loops,
// and never consult the C locale.

#include <cstdint>
#include <optional>
#include <string_view>

#include "ingest/types.hpp"

namespace ftv {

// Parses a decimal amount ("-1234.56", "+7", "0.000123") into int64 minor
// units with `scale` fractional digits. Extra fractional digits are truncated
// toward zero (reported through *truncated). Returns false on syntax error or
// int64 overflow. No floating point is used at any point.
bool parse_amount_minor(std::string_view s, int scale, std::int64_t& out, bool* truncated = nullptr) noexcept;

// Parses either an integer epoch in milliseconds ("1710505200123") or an
// ISO-8601 UTC timestamp: "YYYY-MM-DD[T| ]HH:MM:SS[.fff...][Z|+HH:MM|-HH:MM]".
bool parse_timestamp_ms(std::string_view s, std::int64_t& out) noexcept;

// Case-insensitive channel name lookup.
bool parse_channel(std::string_view s, Channel& out) noexcept;

// Days since 1970-01-01 for a proleptic Gregorian civil date (H. Hinnant).
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9u)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

}  // namespace ftv
