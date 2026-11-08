#include "ingest/key_hash.hpp"

#include <xxhash.h>

#include <cstdio>
#include <cstdlib>

#include "core/flat_hash_map.hpp"
#include "ingest/csv_parser.hpp"
#include "ingest/types.hpp"

namespace ftv {

namespace {

constexpr bool is_hex(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr bool is_base58(char c) noexcept {
  // Bitcoin alphabet: digits 1-9, letters except 0 O I l.
  if (c >= '1' && c <= '9') return true;
  if (c >= 'A' && c <= 'Z') return c != 'I' && c != 'O';
  if (c >= 'a' && c <= 'z') return c != 'l';
  return false;
}

constexpr bool is_bech32_data(char c) noexcept {
  const char l = ascii_lower(c);
  // qpzry9x8gf2tvdw0s3jn54khce6mua7l
  return (l >= '0' && l <= '9' && l != '1') || (l >= 'a' && l <= 'z' && l != 'b' && l != 'i' && l != 'o');
}

bool starts_with_ci(std::string_view s, std::string_view prefix) noexcept {
  if (s.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (ascii_lower(s[i]) != prefix[i]) return false;
  }
  return true;
}

// Copies trimmed (and optionally unescaped) raw text into out.buf.
bool load_trimmed(std::string_view raw, bool escapes, NormalizedKey& out) noexcept {
  raw = trim(raw);
  if (raw.empty()) return false;
  if (escapes) {
    const std::size_t n = csv_unescape(raw, out.buf, kMaxKeyLength);
    if (n == static_cast<std::size_t>(-1)) return false;
    out.len = n;
    // Unescaping may expose new edge whitespace inside quotes: trim again.
    std::string_view v = trim(std::string_view(out.buf, out.len));
    if (v.empty()) return false;
    if (v.data() != out.buf) {
      for (std::size_t i = 0; i < v.size(); ++i) out.buf[i] = v[i];
    }
    out.len = v.size();
    return true;
  }
  if (raw.size() > kMaxKeyLength) return false;
  for (std::size_t i = 0; i < raw.size(); ++i) out.buf[i] = raw[i];
  out.len = raw.size();
  return true;
}

void to_upper(NormalizedKey& k) noexcept {
  for (std::size_t i = 0; i < k.len; ++i) k.buf[i] = ascii_upper(k.buf[i]);
}
void to_lower(NormalizedKey& k) noexcept {
  for (std::size_t i = 0; i < k.len; ++i) k.buf[i] = ascii_lower(k.buf[i]);
}

}  // namespace

std::uint64_t chain_namespace_tag(std::uint64_t chainCode8) noexcept {
  // mix64 is a bijection with mix64(0) == 0; chain codes are non-zero and the
  // domain bit makes the input differ from any jurisdiction code, so a chain
  // tag can never equal the fiat tag (0).
  return mix64(chainCode8 | kChainDomainBit);
}

bool normalize_fiat_key(std::string_view raw, bool escapes, NormalizedKey& out) noexcept {
  if (!load_trimmed(raw, escapes, out)) return false;
  to_upper(out);
  out.kind = KeyKind::Fiat;
  return true;
}

bool normalize_onchain_key(std::string_view raw, bool escapes, NormalizedKey& out) noexcept {
  if (!load_trimmed(raw, escapes, out)) return false;
  const std::string_view v(out.buf, out.len);

  if (v.size() == 42 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
    bool hex = true;
    for (std::size_t i = 2; i < v.size(); ++i) hex &= is_hex(v[i]);
    if (hex) {
      to_lower(out);  // EIP-55 checksum casing and lower-case collapse together
      out.kind = KeyKind::Evm;
      return true;
    }
  }
  if ((starts_with_ci(v, "bc1") || starts_with_ci(v, "tb1") || starts_with_ci(v, "ltc1")) && v.size() >= 14 &&
      v.size() <= 90) {
    const std::size_t sep = v.find('1');
    bool ok = true;
    for (std::size_t i = sep + 1; i < v.size(); ++i) ok &= is_bech32_data(v[i]);
    if (ok) {
      to_lower(out);
      out.kind = KeyKind::Bech32;
      return true;
    }
  }
  if (v.size() >= 25 && v.size() <= 44) {
    bool b58 = true;
    for (char c : v) b58 &= is_base58(c);
    if (b58) {
      out.kind = KeyKind::Base58;  // case-preserving
      return true;
    }
  }
  // Anything else on an ONCHAIN row is a custodial / exchange ledger id. It is
  // normalised like a fiat id and hashed in the fiat namespace so the same
  // exchange entity seen on fiat rails and on-chain becomes one BRIDGE node.
  to_upper(out);
  out.kind = KeyKind::Custodial;
  return true;
}

std::uint64_t hash_key_bytes(std::string_view normalized, std::uint64_t namespaceTag) noexcept {
  return XXH3_64bits_withSeed(normalized.data(), normalized.size(), kKeyHashSeed ^ namespaceTag);
}

std::uint64_t hash_normalized(const NormalizedKey& key, std::uint64_t chainCode8) noexcept {
  const std::uint64_t tag = key.is_chain_address() ? chain_namespace_tag(chainCode8) : kFiatNamespaceTag;
  return hash_key_bytes(key.view(), tag);
}

void CollisionVerifier::check(std::uint64_t key64, std::uint64_t namespaceTag, std::string_view normalized) {
#ifndef NDEBUG
  char tagBuf[24];
  std::snprintf(tagBuf, sizeof(tagBuf), "%016llx|", static_cast<unsigned long long>(namespaceTag));
  std::string full(tagBuf);
  full.append(normalized);
  std::lock_guard<std::mutex> lock(mutex_);
  auto [it, inserted] = seen_.try_emplace(key64, full);
  if (!inserted && it->second != full) {
    std::fprintf(stderr, "FATAL: 64-bit key collision: key64=%016llx '%s' vs '%s'\n",
                 static_cast<unsigned long long>(key64), it->second.c_str(), full.c_str());
    std::abort();
  }
#else
  (void)key64;
  (void)namespaceTag;
  (void)normalized;
#endif
}

std::size_t CollisionVerifier::size() const {
#ifndef NDEBUG
  std::lock_guard<std::mutex> lock(mutex_);
  return seen_.size();
#else
  return 0;
#endif
}

}  // namespace ftv
