// Hash determinism, normalisation and namespace separation.

#include <xxhash.h>

#include "ingest/key_hash.hpp"
#include "ingest/types.hpp"
#include "test_framework.hpp"

using namespace ftv;

namespace {
std::uint64_t fiat(std::string_view s) {
  NormalizedKey k;
  if (!normalize_fiat_key(s, false, k)) return 0;
  return hash_normalized(k, 0);
}
std::uint64_t onchain(std::string_view s, std::string_view chain) {
  NormalizedKey k;
  if (!normalize_onchain_key(s, false, k)) return 0;
  return hash_normalized(k, pack_code8(chain));
}
}  // namespace

TEST_CASE("hash: deterministic and equal to the documented formula") {
  const std::uint64_t a = fiat("GB29NWBK60161331926819");
  const std::uint64_t b = fiat("GB29NWBK60161331926819");
  CHECK_EQ(a, b);
  CHECK(a != 0);
  // key64 = XXH3_64bits_withSeed(normalized, len, SEED ^ tag)
  const std::string_view s = "GB29NWBK60161331926819";
  CHECK_EQ(a, XXH3_64bits_withSeed(s.data(), s.size(), kKeyHashSeed ^ kFiatNamespaceTag));
  // Different identifiers hash differently.
  CHECK(fiat("GB29NWBK60161331926819") != fiat("GB29NWBK60161331926818"));
}

TEST_CASE("hash: fiat normalisation trims and upper-cases") {
  CHECK_EQ(fiat("  gb29nwbk60161331926819\t"), fiat("GB29NWBK60161331926819"));
  CHECK_EQ(fiat("us-021000021-0012345678"), fiat("US-021000021-0012345678"));
  NormalizedKey k;
  CHECK(!normalize_fiat_key("   ", false, k));
  CHECK(!normalize_fiat_key("", false, k));
  std::string longKey(kMaxKeyLength + 1, 'A');
  CHECK(!normalize_fiat_key(longKey, false, k));
}

TEST_CASE("hash: EVM addresses lower-cased, base58 case preserved") {
  const char* checksum = "0x52908400098527886E0F7030069857D2E4169EE7";
  const char* lower = "0x52908400098527886e0f7030069857d2e4169ee7";
  CHECK_EQ(onchain(checksum, "ETH"), onchain(lower, "ETH"));
  NormalizedKey k;
  CHECK(normalize_onchain_key(checksum, false, k));
  CHECK(k.kind == KeyKind::Evm);
  CHECK(k.view() == lower);

  // Base58 (Solana) is case-sensitive: different case => different entity.
  const char* sol = "7EcDhSYGxXyscszYEp35KHN8vvw3svAuLKTzXwCFLtV";
  const char* solFlipped = "7ecDhSYGxXyscszYEp35KHN8vvw3svAuLKTzXwCFLtV";
  CHECK(normalize_onchain_key(sol, false, k));
  CHECK(k.kind == KeyKind::Base58);
  CHECK(k.view() == sol);
  CHECK(onchain(sol, "SOL") != onchain(solFlipped, "SOL"));

  // Bech32 is case-insensitive.
  CHECK_EQ(onchain("BC1QAR0SRRR7XFKVY5L643LYDNW9RE59GTZZWF5MDQ", "BTC"),
           onchain("bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq", "BTC"));
}

TEST_CASE("hash: namespace separation across chains and fiat") {
  const char* addr = "0x52908400098527886e0f7030069857d2e4169ee7";
  const std::uint64_t eth = onchain(addr, "ETH");
  const std::uint64_t bsc = onchain(addr, "BSC");
  const std::uint64_t fiatSame = fiat(addr);
  CHECK(eth != bsc);
  CHECK(eth != fiatSame);
  CHECK(bsc != fiatSame);
  CHECK(chain_namespace_tag(pack_code8("ETH")) != kFiatNamespaceTag);
  CHECK(chain_namespace_tag(pack_code8("ETH")) != chain_namespace_tag(pack_code8("BTC")));
}

TEST_CASE("hash: custodial ids on ONCHAIN rows share the fiat namespace (bridges)") {
  NormalizedKey k;
  CHECK(normalize_onchain_key("exch-aurora-0007", false, k));
  CHECK(k.kind == KeyKind::Custodial);
  CHECK_EQ(onchain("exch-aurora-0007", "ETH"), fiat("EXCH-AURORA-0007"));
  CHECK_EQ(onchain("EXCH-AURORA-0007", "BTC"), fiat("EXCH-AURORA-0007"));
}

TEST_CASE("hash: CSV escape collapsing before hashing") {
  NormalizedKey k;
  CHECK(normalize_fiat_key("ACME \"\"EU\"\" 1", true, k));
  CHECK(k.view() == "ACME \"EU\" 1");
}

TEST_CASE("hash: collision verifier accepts repeats") {
  CollisionVerifier v;
  v.check(1, 0, "A");
  v.check(1, 0, "A");
  v.check(2, 0, "B");
#ifndef NDEBUG
  CHECK_EQ(v.size(), 2u);
#endif
}
