#include "ingest/field_parsers.hpp"

#include <charconv>
#include <limits>

namespace ftv {

namespace {

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Parses exactly `n` digits starting at s[pos]; advances pos.
bool fixed_digits(std::string_view s, std::size_t& pos, int n, unsigned& out) noexcept {
  if (pos + static_cast<std::size_t>(n) > s.size()) return false;
  unsigned v = 0;
  for (int i = 0; i < n; ++i) {
    const char c = s[pos++];
    if (!is_digit(c)) return false;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  out = v;
  return true;
}

// Days in month `mon` (1..12) of `year` in the proleptic Gregorian calendar.
constexpr unsigned days_in_month(unsigned year, unsigned mon) noexcept {
  if (mon == 2) return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29u : 28u;
  return (mon == 4 || mon == 6 || mon == 9 || mon == 11) ? 30u : 31u;
}

bool mul_add_checked(std::int64_t& acc, int digit) noexcept {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  if (acc > (kMax - digit) / 10) return false;
  acc = acc * 10 + digit;
  return true;
}

}  // namespace

bool parse_amount_minor(std::string_view s, int scale, std::int64_t& out, bool* truncated) noexcept {
  s = trim(s);
  if (s.empty()) return false;
  bool negative = false;
  if (s.front() == '-' || s.front() == '+') {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }
  if (s.empty()) return false;

  std::int64_t acc = 0;
  std::size_t i = 0;
  std::size_t intDigits = 0;
  while (i < s.size() && is_digit(s[i])) {
    if (!mul_add_checked(acc, s[i] - '0')) return false;
    ++i;
    ++intDigits;
  }
  int fracDigits = 0;
  bool dropped = false;
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && is_digit(s[i])) {
      if (fracDigits < scale) {
        if (!mul_add_checked(acc, s[i] - '0')) return false;
        ++fracDigits;
      } else if (s[i] != '0') {
        dropped = true;  // truncated toward zero
      }
      ++i;
    }
    if (intDigits == 0 && fracDigits == 0 && !dropped) {
      // "." alone or ".000" with scale 0: require at least one digit somewhere.
      bool anyDigit = false;
      for (char c : s) anyDigit |= is_digit(c);
      if (!anyDigit) return false;
    }
  } else if (intDigits == 0) {
    return false;
  }
  if (i != s.size()) return false;  // trailing garbage, exponents, separators
  for (; fracDigits < scale; ++fracDigits) {
    if (!mul_add_checked(acc, 0)) return false;
  }
  out = negative ? -acc : acc;
  if (truncated) *truncated = dropped;
  return true;
}

bool parse_timestamp_ms(std::string_view s, std::int64_t& out) noexcept {
  s = trim(s);
  if (s.empty()) return false;

  // Pure integer => epoch milliseconds.
  bool allDigits = true;
  for (std::size_t k = (s.front() == '-' ? 1 : 0); k < s.size(); ++k) allDigits &= is_digit(s[k]);
  if (allDigits && s.size() > (s.front() == '-' ? 1u : 0u)) {
    std::int64_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
  }

  std::size_t p = 0;
  unsigned year = 0, mon = 0, day = 0, hh = 0, mm = 0, ss = 0;
  if (!fixed_digits(s, p, 4, year)) return false;
  if (p >= s.size() || s[p++] != '-') return false;
  if (!fixed_digits(s, p, 2, mon)) return false;
  if (p >= s.size() || s[p++] != '-') return false;
  if (!fixed_digits(s, p, 2, day)) return false;
  if (p < s.size() && (s[p] == 'T' || s[p] == 't' || s[p] == ' ')) {
    ++p;
    if (!fixed_digits(s, p, 2, hh)) return false;
    if (p >= s.size() || s[p++] != ':') return false;
    if (!fixed_digits(s, p, 2, mm)) return false;
    if (p >= s.size() || s[p++] != ':') return false;
    if (!fixed_digits(s, p, 2, ss)) return false;
  }
  // Impossible dates (2024-02-31, 2023-02-29) are malformed: days_from_civil
  // would otherwise roll them silently into the next month.
  if (mon < 1 || mon > 12 || day < 1 || day > days_in_month(year, mon)) return false;
  if (hh > 23 || mm > 59 || ss > 60) return false;

  std::int64_t millis = 0;
  if (p < s.size() && (s[p] == '.' || s[p] == ',')) {
    ++p;
    int digits = 0;
    while (p < s.size() && is_digit(s[p])) {
      if (digits < 3) millis = millis * 10 + (s[p] - '0');
      ++digits;
      ++p;
    }
    if (digits == 0) return false;
    for (int k = digits; k < 3; ++k) millis *= 10;
  }

  std::int64_t offsetMin = 0;
  if (p < s.size()) {
    const char z = s[p];
    if (z == 'Z' || z == 'z') {
      ++p;
    } else if (z == '+' || z == '-') {
      ++p;
      unsigned oh = 0, om = 0;
      if (!fixed_digits(s, p, 2, oh)) return false;
      if (p < s.size() && s[p] == ':') ++p;
      if (!fixed_digits(s, p, 2, om)) return false;
      offsetMin = static_cast<std::int64_t>(oh * 60 + om) * (z == '+' ? 1 : -1);
    }
  }
  if (p != s.size()) return false;

  const std::int64_t days = days_from_civil(static_cast<std::int64_t>(year), mon, day);
  const std::int64_t secs = days * 86400 + static_cast<std::int64_t>(hh) * 3600 + static_cast<std::int64_t>(mm) * 60 +
                            static_cast<std::int64_t>(ss) - offsetMin * 60;
  out = secs * 1000 + millis;
  return true;
}

bool parse_channel(std::string_view s, Channel& out) noexcept {
  s = trim(s);
  const std::uint64_t code = pack_code8(s);
  switch (code) {
    case pack_code8("SWIFT"): out = Channel::SWIFT; return true;
    case pack_code8("SEPA"): out = Channel::SEPA; return true;
    case pack_code8("ACH"): out = Channel::ACH; return true;
    case pack_code8("FEDWIRE"): out = Channel::FEDWIRE; return true;
    case pack_code8("CHAPS"): out = Channel::CHAPS; return true;
    case pack_code8("ZENGIN"): out = Channel::ZENGIN; return true;
    case pack_code8("ONCHAIN"): out = Channel::ONCHAIN; return true;
    case pack_code8("INTERNAL"): out = Channel::INTERNAL; return true;
    default: return false;
  }
}

}  // namespace ftv
