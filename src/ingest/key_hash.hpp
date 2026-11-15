#pragma once
// Entity key normalisation and 64-bit hashing (XXH3_64bits_withSeed).
//
//   key64 = XXH3_64bits_withSeed(normalized, len, kKeyHashSeed ^ namespace_tag)
//
// Namespaces: fiat identifiers (bank accounts, IBANs, BIC-qualified accounts,
// and custodial ledger ids that appear on ONCHAIN rows) share the fiat tag 0.
// On-chain addresses use a tag derived from the chain id, so the same address
// string on ETH and on BSC yields two different entities.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ftv {

inline constexpr std::uint64_t kKeyHashSeed = 0x6A09E667F3BCC909ull;  // frac(sqrt(2)), compile-time constant
inline constexpr std::uint64_t kFiatNamespaceTag = 0;
inline constexpr std::size_t kMaxKeyLength = 256;

enum class KeyKind : std::uint8_t {
  Fiat,       // fiat account on a fiat rail: trimmed, ASCII upper-case
  Custodial,  // non-address id on an ONCHAIN row (exchange ledger): fiat namespace
  Evm,        // 0x + 40 hex: lower-case hex
  Bech32,     // bc1/tb1/ltc1...: lower-case (bech32 is case-insensitive)
  Base58,     // BTC legacy, SOL, TRON...: case preserved
};

struct NormalizedKey {
  char buf[kMaxKeyLength];
  std::size_t len = 0;
  KeyKind kind = KeyKind::Fiat;
  [[nodiscard]] std::string_view view() const noexcept { return {buf, len}; }
  [[nodiscard]] bool is_chain_address() const noexcept {
    return kind == KeyKind::Evm || kind == KeyKind::Bech32 || kind == KeyKind::Base58;
  }
};

// Namespace tag for an on-chain address on chain `chainCode8` (packed ASCII).
std::uint64_t chain_namespace_tag(std::uint64_t chainCode8) noexcept;

// Normalises an identifier found on a fiat rail. Returns false if empty or
// longer than kMaxKeyLength. `escapes` collapses CSV "" pairs first.
bool normalize_fiat_key(std::string_view raw, bool escapes, NormalizedKey& out) noexcept;

// Normalises an identifier found on an ONCHAIN row, classifying it as an
// address (chain namespace) or a custodial id (fiat namespace).
bool normalize_onchain_key(std::string_view raw, bool escapes, NormalizedKey& out) noexcept;

// Hash of a normalised key in its namespace.
std::uint64_t hash_normalized(const NormalizedKey& key, std::uint64_t chainCode8) noexcept;

// Raw primitive: XXH3_64bits_withSeed(data, len, kKeyHashSeed ^ tag).
std::uint64_t hash_key_bytes(std::string_view normalized, std::uint64_t namespaceTag) noexcept;

// Debug-build collision verifier (key64 -> first namespaced string). In
// release builds check() is a no-op; see DESIGN_NOTES.md for the analysis.
class CollisionVerifier {
 public:
  void check(std::uint64_t key64, std::uint64_t namespaceTag, std::string_view normalized);
  [[nodiscard]] std::size_t size() const;

 private:
#ifndef NDEBUG
  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, std::string> seen_;
#endif
};

}  // namespace ftv
