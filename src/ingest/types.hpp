#pragma once
// Shared ingest value types: channels, packed ASCII codes, currency scales.

#include <array>
#include <cstdint>
#include <string_view>

namespace ftv {

enum class Channel : std::uint8_t { SWIFT = 0, SEPA, ACH, FEDWIRE, CHAPS, ZENGIN, ONCHAIN, INTERNAL, Count };
inline constexpr std::size_t kChannelCount = static_cast<std::size_t>(Channel::Count);

constexpr std::array<std::string_view, kChannelCount> kChannelNames = {
    "SWIFT", "SEPA", "ACH", "FEDWIRE", "CHAPS", "ZENGIN", "ONCHAIN", "INTERNAL"};

constexpr char ascii_upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }
constexpr char ascii_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

constexpr std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

// Packs up to 4 ASCII characters (upper-cased) little-endian into a uint32.
// Returns 0 for empty or over-long input; callers treat 0 as "invalid".
constexpr std::uint32_t pack_code4(std::string_view s) noexcept {
  if (s.empty() || s.size() > 4) return 0;
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(ascii_upper(s[i]));
    if (c < 0x21 || c > 0x7E) return 0;
    v |= static_cast<std::uint32_t>(c) << (8 * i);
  }
  return v;
}

// Packs up to 8 ASCII characters (upper-cased). Bit 63 is never set by valid
// ASCII input, which lets callers use it as a domain flag (see kChainDomainBit).
constexpr std::uint64_t pack_code8(std::string_view s) noexcept {
  if (s.empty() || s.size() > 8) return 0;
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(ascii_upper(s[i]));
    if (c < 0x21 || c > 0x7E) return 0;
    v |= static_cast<std::uint64_t>(c) << (8 * i);
  }
  return v;
}

// Location codes recorded per entity endpoint: a packed jurisdiction (fiat
// domain) or a packed chain id with the chain-domain bit set.
inline constexpr std::uint64_t kChainDomainBit = 1ull << 63;

inline void unpack_code(std::uint64_t code, char (&out)[9]) noexcept {
  code &= ~kChainDomainBit;
  for (int i = 0; i < 8; ++i) out[i] = static_cast<char>((code >> (8 * i)) & 0xFF);
  out[8] = '\0';
}

// Number of decimal places stored in amount_minor for a currency. Amounts are
// fixed-point int64 minor units; crypto assets use a scale that keeps realistic
// values inside int64 (ETH in gwei, not wei). See DESIGN_NOTES.md.
constexpr int currency_scale(std::uint32_t code) noexcept {
  switch (code) {
    case pack_code4("JPY"): case pack_code4("KRW"): case pack_code4("VND"):
    case pack_code4("CLP"): case pack_code4("ISK"): case pack_code4("PYG"):
      return 0;
    case pack_code4("BHD"): case pack_code4("KWD"): case pack_code4("OMR"):
    case pack_code4("JOD"): case pack_code4("TND"): case pack_code4("IQD"):
      return 3;
    case pack_code4("BTC"):
      return 8;
    case pack_code4("ETH"): case pack_code4("SOL"):
      return 9;
    case pack_code4("TRX"): case pack_code4("USDT"): case pack_code4("USDC"):
      return 6;
    default:
      return 2;
  }
}

}  // namespace ftv
