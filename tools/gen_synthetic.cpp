// gen_synthetic: writes realistic multi-hub transaction datasets (CSV or .ftxb)
// for ftviz. Deterministic for a given --seed.
//
// Population model
//  * Fiat entities live in regions that map to the default hubs (NEW_YORK,
//    LONDON, FRANKFURT, ZURICH, TOKYO, SINGAPORE, HONG_KONG) and are grouped
//    into communities of ~400 (corporate / correspondent clusters).
//  * On-chain addresses live on ETH, BTC, SOL and TRON, also in communities.
//  * Exchanges use custodial ledger ids that appear both on fiat rails and on
//    ONCHAIN rows, so ftviz detects them as BRIDGE entities.
//  * A small offshore population uses jurisdictions with no hub mapping.
// Transaction mix: mostly intra-community, some cross-community, SWIFT
// cross-region flows, on-chain transfers, ramps through exchanges, offshore.
// Activity is power-law skewed and half of the transfers reuse a "favourite"
// counterparty, so aggregated edges carry realistic repeat counts.
//
// Usage:
//   gen_synthetic --rows 10000000 --out data.csv [--entities N] [--seed S]
//                 [--format csv|ftxb] [--malformed-rate 0.0001] [--crlf]

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "ingest/ftxb_format.hpp"
#include "ingest/types.hpp"

namespace {

using ftv::Channel;

// ---------------------------------------------------------------------------
// Deterministic RNG: xoshiro256** seeded through SplitMix64.
// ---------------------------------------------------------------------------
struct Rng {
  std::uint64_t s[4];
  explicit Rng(std::uint64_t seed) {
    for (auto& v : s) {
      seed += 0x9E3779B97F4A7C15ull;
      std::uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      v = z ^ (z >> 31);
    }
  }
  static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  std::uint64_t next() {
    const std::uint64_t result = rotl(s[1] * 5, 7) * 9;
    const std::uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
  }
  double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  std::uint64_t below(std::uint64_t n) { return n ? next() % n : 0; }
  double normal() {
    const double u1 = std::max(uniform(), 1e-300);
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

constexpr std::uint64_t mix(std::uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

// ---------------------------------------------------------------------------
// Population tables
// ---------------------------------------------------------------------------
struct Juris {
  const char* code;
  const char* currency;
  Channel rail;
  double weight;
};

struct Region {
  const char* name;
  double weight;
  std::vector<Juris> juris;
};

const std::vector<Region> kRegions = {
    {"NEW_YORK", 0.26, {{"US", "USD", Channel::ACH, 0.75}, {"CA", "CAD", Channel::INTERNAL, 0.10},
                        {"BR", "BRL", Channel::INTERNAL, 0.08}, {"MX", "MXN", Channel::INTERNAL, 0.07}}},
    {"LONDON", 0.20, {{"GB", "GBP", Channel::CHAPS, 0.55}, {"FR", "EUR", Channel::SEPA, 0.20},
                      {"IE", "EUR", Channel::SEPA, 0.10}, {"AE", "AED", Channel::INTERNAL, 0.15}}},
    {"FRANKFURT", 0.12, {{"DE", "EUR", Channel::SEPA, 0.70}, {"NL", "EUR", Channel::SEPA, 0.20},
                         {"AT", "EUR", Channel::SEPA, 0.10}}},
    {"ZURICH", 0.07, {{"CH", "CHF", Channel::INTERNAL, 0.90}, {"LI", "CHF", Channel::INTERNAL, 0.10}}},
    {"TOKYO", 0.15, {{"JP", "JPY", Channel::ZENGIN, 0.85}, {"KR", "KRW", Channel::INTERNAL, 0.15}}},
    {"SINGAPORE", 0.11, {{"SG", "SGD", Channel::INTERNAL, 0.60}, {"AU", "AUD", Channel::INTERNAL, 0.25},
                         {"IN", "INR", Channel::INTERNAL, 0.15}}},
    {"HONG_KONG", 0.09, {{"HK", "HKD", Channel::INTERNAL, 0.80}, {"CN", "CNY", Channel::INTERNAL, 0.20}}},
};

struct ChainDef {
  const char* code;
  double weight;
  std::vector<std::pair<const char*, double>> assets;  // currency, weight
  double typical_amount;                               // in asset units
};

const std::vector<ChainDef> kChains = {
    {"ETH", 0.45, {{"ETH", 0.45}, {"USDT", 0.35}, {"USDC", 0.20}}, 0.6},
    {"BTC", 0.25, {{"BTC", 1.0}}, 0.04},
    {"SOL", 0.17, {{"SOL", 0.6}, {"USDC", 0.4}}, 12.0},
    {"TRON", 0.13, {{"USDT", 0.8}, {"TRX", 0.2}}, 900.0},
};

const char* const kOffshore[] = {"KY", "VG", "PA", "BS", "", "XX"};
const char* const kExchangeNames[] = {"AURORA", "BOREAL", "CIRRUS", "DELTAX", "EQUINOX", "FATHOM", "GLACIER",
                                      "HALCYON", "IRIDIUM", "JUNIPER", "KESTREL", "LUMEN"};

enum class Kind : std::uint8_t { Fiat, Chain, Exchange, Offshore };

struct Entity {
  Kind kind;
  std::uint8_t group;  // region or chain index
  std::uint8_t juris;  // jurisdiction index inside the region / offshore index
  std::uint32_t local; // index inside its group
};

constexpr std::uint32_t kCommunitySize = 400;

struct Population {
  std::vector<Entity> entities;
  std::vector<std::vector<std::uint32_t>> fiat_by_region;  // entity ids
  std::vector<std::vector<std::uint32_t>> chain_members;
  std::vector<std::uint32_t> exchanges;
  std::vector<std::uint32_t> offshore;
};

template <class T, class W>
std::size_t pick_weighted(Rng& rng, const std::vector<T>& items, W weight) {
  double total = 0;
  for (const auto& it : items) total += weight(it);
  double u = rng.uniform() * total;
  for (std::size_t i = 0; i < items.size(); ++i) {
    u -= weight(items[i]);
    if (u <= 0) return i;
  }
  return items.size() - 1;
}

Population build_population(std::uint64_t count, Rng& rng) {
  Population p;
  p.fiat_by_region.resize(kRegions.size());
  p.chain_members.resize(kChains.size());
  p.entities.reserve(count);
  const std::uint64_t nExchange = std::max<std::uint64_t>(24, count / 2000);
  const std::uint64_t nOffshore = count * 3 / 100;
  const std::uint64_t nChain = count * 31 / 100;
  for (std::uint64_t i = 0; i < count; ++i) {
    Entity e{};
    if (i < nExchange) {
      e.kind = Kind::Exchange;
      e.group = static_cast<std::uint8_t>(pick_weighted(rng, kRegions, [](const Region& r) { return r.weight; }));
      e.juris = 0;
      e.local = static_cast<std::uint32_t>(p.exchanges.size());
      p.exchanges.push_back(static_cast<std::uint32_t>(i));
    } else if (i < nExchange + nOffshore) {
      e.kind = Kind::Offshore;
      e.juris = static_cast<std::uint8_t>(rng.below(std::size(kOffshore)));
      e.local = static_cast<std::uint32_t>(p.offshore.size());
      p.offshore.push_back(static_cast<std::uint32_t>(i));
    } else if (i < nExchange + nOffshore + nChain) {
      e.kind = Kind::Chain;
      e.group = static_cast<std::uint8_t>(pick_weighted(rng, kChains, [](const ChainDef& c) { return c.weight; }));
      e.local = static_cast<std::uint32_t>(p.chain_members[e.group].size());
      p.chain_members[e.group].push_back(static_cast<std::uint32_t>(i));
    } else {
      e.kind = Kind::Fiat;
      e.group = static_cast<std::uint8_t>(pick_weighted(rng, kRegions, [](const Region& r) { return r.weight; }));
      e.juris = static_cast<std::uint8_t>(
          pick_weighted(rng, kRegions[e.group].juris, [](const Juris& j) { return j.weight; }));
      e.local = static_cast<std::uint32_t>(p.fiat_by_region[e.group].size());
      p.fiat_by_region[e.group].push_back(static_cast<std::uint32_t>(i));
    }
    p.entities.push_back(e);
  }
  return p;
}

// ---------------------------------------------------------------------------
// Identifier formatting (deterministic from the entity id)
// ---------------------------------------------------------------------------
constexpr char kHex[] = "0123456789abcdef";
constexpr char kBase58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr char kBech32[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

std::size_t append_digits(char* out, std::uint64_t h, int n) {
  for (int i = 0; i < n; ++i) {
    out[i] = static_cast<char>('0' + h % 10);
    h /= 10;
    if (h == 0) h = mix(static_cast<std::uint64_t>(i) * 0x9E37u + 1);
  }
  return static_cast<std::size_t>(n);
}

// Writes the canonical identifier for entity `id`; `variant` selects a
// cosmetic spelling (case / padding) that normalises to the same key.
std::size_t format_id(const Population& pop, std::uint32_t id, unsigned variant, char* out) {
  const Entity& e = pop.entities[id];
  std::uint64_t h = mix(id * 0x9E3779B97F4A7C15ull + 0x1234);
  std::size_t n = 0;
  switch (e.kind) {
    case Kind::Fiat: {
      const Juris& j = kRegions[e.group].juris[e.juris];
      if (j.code[0] == 'U' && j.code[1] == 'S') {
        std::memcpy(out, "US-", 3);
        n = 3;
        n += append_digits(out + n, h, 9);
        out[n++] = '-';
        n += append_digits(out + n, mix(h), 10);
      } else if (j.code[0] == 'J' && j.code[1] == 'P') {
        std::memcpy(out, "JP-", 3);
        n = 3;
        n += append_digits(out + n, h, 4);
        out[n++] = '-';
        n += append_digits(out + n, mix(h + 1), 3);
        out[n++] = '-';
        n += append_digits(out + n, mix(h + 2), 7);
      } else {
        // IBAN-like: CC + 2 check digits + 4 letter bank code + 14 digits.
        out[n++] = j.code[0];
        out[n++] = j.code[1];
        n += append_digits(out + n, h, 2);
        for (int i = 0; i < 4; ++i) out[n++] = static_cast<char>('A' + (mix(h + 7 + i) % 26));
        n += append_digits(out + n, mix(h + 3), 14);
      }
      if (variant == 1) {  // lower-case spelling: normalises to the same key
        for (std::size_t i = 0; i < n; ++i) out[i] = ftv::ascii_lower(out[i]);
      }
      return n;
    }
    case Kind::Exchange: {
      n = static_cast<std::size_t>(std::snprintf(out, 64, "EXCH-%s-%04u",
                                                 kExchangeNames[e.local % std::size(kExchangeNames)], e.local));
      return n;
    }
    case Kind::Offshore: {
      n = static_cast<std::size_t>(std::snprintf(out, 64, "OFS-%s-", kOffshore[e.juris][0] ? kOffshore[e.juris] : "NA"));
      n += append_digits(out + n, h, 10);
      return n;
    }
    case Kind::Chain: {
      const char* chain = kChains[e.group].code;
      if (std::strcmp(chain, "ETH") == 0) {
        out[0] = '0';
        out[1] = 'x';
        std::uint64_t s = h;
        for (int i = 0; i < 40; ++i) {
          if (i % 16 == 0) s = mix(h + static_cast<std::uint64_t>(i));
          char c = kHex[s & 15];
          s >>= 4;
          // EIP-55-like mixed case for variant 1 (normalises to lower-case).
          if (variant == 1 && c >= 'a' && ((s >> 1) & 1)) c = static_cast<char>(c - 32);
          out[2 + i] = c;
        }
        return 42;
      }
      if (std::strcmp(chain, "BTC") == 0) {
        std::memcpy(out, "bc1q", 4);
        std::uint64_t s = h;
        for (int i = 0; i < 38; ++i) {
          if (i % 12 == 0) s = mix(h + static_cast<std::uint64_t>(i));
          out[4 + i] = kBech32[s & 31];
          s >>= 5;
        }
        return 42;
      }
      const bool tron = std::strcmp(chain, "TRON") == 0;
      const int len = tron ? 33 : 44;
      std::size_t start = 0;
      if (tron) out[start++] = 'T';
      std::uint64_t s = h;
      for (int i = 0; i < len; ++i) {
        if (i % 10 == 0) s = mix(h + static_cast<std::uint64_t>(i) * 31);
        out[start + static_cast<std::size_t>(i)] = kBase58[s % 58];
        s /= 58;
      }
      return start + static_cast<std::size_t>(len);
    }
  }
  return n;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------
class Writer {
 public:
  explicit Writer(std::FILE* f) : f_(f), buf_(8u << 20) {}
  ~Writer() { flush(); }
  void put(std::string_view s) {
    if (pos_ + s.size() > buf_.size()) flush();
    std::memcpy(buf_.data() + pos_, s.data(), s.size());
    pos_ += s.size();
  }
  void put(char c) {
    if (pos_ + 1 > buf_.size()) flush();
    buf_[pos_++] = c;
  }
  void put_bytes(const void* p, std::size_t n) { put(std::string_view(static_cast<const char*>(p), n)); }
  void flush() {
    if (pos_) std::fwrite(buf_.data(), 1, pos_, f_);
    pos_ = 0;
  }

 private:
  std::FILE* f_;
  std::vector<char> buf_;
  std::size_t pos_ = 0;
};

void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
}

std::size_t format_timestamp(std::int64_t ms, char* out) {
  const std::int64_t secs = ms / 1000;
  const std::int64_t days = secs / 86400;
  const std::int64_t rem = secs % 86400;
  int y;
  unsigned mo, d;
  civil_from_days(days, y, mo, d);
  return static_cast<std::size_t>(std::snprintf(out, 32, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", y, mo, d,
                                                static_cast<int>(rem / 3600), static_cast<int>((rem / 60) % 60),
                                                static_cast<int>(rem % 60), static_cast<int>(ms % 1000)));
}

std::size_t format_amount(std::int64_t minor, int scale, char* out) {
  std::size_t n = 0;
  if (minor < 0) {
    out[n++] = '-';
    minor = -minor;
  }
  std::int64_t pow10 = 1;
  for (int i = 0; i < scale; ++i) pow10 *= 10;
  auto r = std::to_chars(out + n, out + n + 24, minor / pow10);
  n = static_cast<std::size_t>(r.ptr - out);
  if (scale > 0) {
    out[n++] = '.';
    std::int64_t frac = minor % pow10;
    for (int i = scale - 1; i >= 0; --i) {
      out[n + static_cast<std::size_t>(i)] = static_cast<char>('0' + frac % 10);
      frac /= 10;
    }
    n += static_cast<std::size_t>(scale);
  }
  return n;
}

struct Tx {
  std::uint32_t src, dst;
  Channel channel;
  const char* currency;
  const char* src_juris;
  const char* dst_juris;
  const char* chain;
  std::int64_t amount_minor;
  std::int64_t ts_ms;
  unsigned src_variant, dst_variant;
};

const char* juris_of(const Population& pop, std::uint32_t id) {
  const Entity& e = pop.entities[id];
  switch (e.kind) {
    case Kind::Fiat: return kRegions[e.group].juris[e.juris].code;
    case Kind::Exchange: return kRegions[e.group].juris[0].code;
    case Kind::Offshore: return kOffshore[e.juris];
    case Kind::Chain: return "";
  }
  return "";
}

std::int64_t to_minor(double units, const char* currency) {
  const int scale = ftv::currency_scale(ftv::pack_code4(currency));
  double v = units;
  for (int i = 0; i < scale; ++i) v *= 10.0;
  if (v > 9.0e17) v = 9.0e17;
  return static_cast<std::int64_t>(std::llround(v));
}

double fiat_units(Rng& rng, const char* currency) {
  double base = std::exp(std::log(650.0) + 1.5 * rng.normal());
  if (std::strcmp(currency, "JPY") == 0) base *= 150.0;
  else if (std::strcmp(currency, "KRW") == 0) base *= 1300.0;
  else if (std::strcmp(currency, "INR") == 0) base *= 83.0;
  else if (std::strcmp(currency, "HKD") == 0 || std::strcmp(currency, "CNY") == 0) base *= 7.5;
  return base;
}

class Generator {
 public:
  Generator(const Population& pop, std::uint64_t seed) : pop_(pop), rng_(seed ^ 0xA5A5A5A5DEADBEEFull) {}

  std::uint32_t member(const std::vector<std::uint32_t>& group, std::uint32_t community) {
    const std::uint32_t communities = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(group.size() / kCommunitySize));
    const std::uint32_t c = community % communities;
    const std::uint32_t begin = c * kCommunitySize;
    const std::uint32_t size =
        (c + 1 == communities) ? static_cast<std::uint32_t>(group.size()) - begin : kCommunitySize;
    // Power-law activity inside the community.
    const double u = rng_.uniform();
    const auto k = static_cast<std::uint32_t>(static_cast<double>(size) * u * u * u);
    return group[begin + std::min(k, size - 1)];
  }

  std::uint32_t favourite(const std::vector<std::uint32_t>& group, std::uint32_t src) {
    const std::uint32_t local = pop_.entities[src].local;
    const std::uint32_t communities = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(group.size() / kCommunitySize));
    const std::uint32_t c = std::min(local / kCommunitySize, communities - 1);
    const std::uint32_t begin = c * kCommunitySize;
    const std::uint32_t size =
        (c + 1 == communities) ? static_cast<std::uint32_t>(group.size()) - begin : kCommunitySize;
    const std::uint64_t pick = mix(src * 31ull + rng_.below(4)) % size;
    return group[begin + static_cast<std::uint32_t>(pick)];
  }

  std::uint32_t community_of(std::uint32_t id) const { return pop_.entities[id].local / kCommunitySize; }

  Tx next(std::uint64_t row) {
    Tx t{};
    t.ts_ms = 1704067200000LL + static_cast<std::int64_t>(rng_.below(366ull * 86400000ull));
    t.src_variant = rng_.uniform() < 0.15 ? 1u : 0u;
    t.dst_variant = rng_.uniform() < 0.15 ? 1u : 0u;
    t.chain = "";
    const double r = rng_.uniform();
    (void)row;
    if (r < 0.79) {
      // Fiat flows: intra-community, cross-community, cross-region SWIFT.
      const std::size_t reg = pick_weighted(rng_, kRegions, [](const Region& x) { return x.weight; });
      const auto& group = pop_.fiat_by_region[reg];
      const auto communities = static_cast<std::uint32_t>(std::max<std::size_t>(1, group.size() / kCommunitySize));
      t.src = member(group, static_cast<std::uint32_t>(rng_.below(communities)));
      if (r < 0.60) {
        t.dst = rng_.uniform() < 0.5 ? favourite(group, t.src) : member(group, community_of(t.src));
      } else if (r < 0.70) {
        t.dst = member(group, static_cast<std::uint32_t>(rng_.below(communities)));
      } else {
        std::size_t reg2 = pick_weighted(rng_, kRegions, [](const Region& x) { return x.weight; });
        if (reg2 == reg) reg2 = (reg2 + 1 + rng_.below(kRegions.size() - 1)) % kRegions.size();
        const auto& g2 = pop_.fiat_by_region[reg2];
        // Correspondent banking: cross-region flows concentrate on the most
        // active members of the first communities.
        t.dst = member(g2, static_cast<std::uint32_t>(rng_.below(4)));
      }
      const Entity& se = pop_.entities[t.src];
      const Juris& sj = kRegions[se.group].juris[se.juris];
      if (r >= 0.70) {
        t.channel = Channel::SWIFT;
        const double c = rng_.uniform();
        t.currency = c < 0.5 ? "USD" : (c < 0.75 ? "EUR" : sj.currency);
      } else {
        t.channel = sj.rail;
        if (sj.rail == Channel::ACH && rng_.uniform() < 0.3) t.channel = Channel::FEDWIRE;
        t.currency = sj.currency;
      }
      t.amount_minor = to_minor(fiat_units(rng_, t.currency) * (r >= 0.70 ? 20.0 : 1.0), t.currency);
    } else if (r < 0.94) {
      // On-chain transfers inside one chain.
      const std::size_t ch = pick_weighted(rng_, kChains, [](const ChainDef& c) { return c.weight; });
      const auto& group = pop_.chain_members[ch];
      const auto communities = static_cast<std::uint32_t>(std::max<std::size_t>(1, group.size() / kCommunitySize));
      t.src = member(group, static_cast<std::uint32_t>(rng_.below(communities)));
      t.dst = rng_.uniform() < 0.5 ? favourite(group, t.src)
                                   : member(group, rng_.uniform() < 0.85 ? community_of(t.src)
                                                                        : static_cast<std::uint32_t>(rng_.below(communities)));
      t.channel = Channel::ONCHAIN;
      t.chain = kChains[ch].code;
      const auto& assets = kChains[ch].assets;
      std::size_t a = pick_weighted(rng_, assets, [](const auto& p) { return p.second; });
      t.currency = assets[a].first;
      const bool stable = std::strcmp(t.currency, "USDT") == 0 || std::strcmp(t.currency, "USDC") == 0;
      const double units = stable ? fiat_units(rng_, "USD")
                                  : std::exp(std::log(kChains[ch].typical_amount) + 1.4 * rng_.normal());
      t.amount_minor = to_minor(units, t.currency);
    } else if (r < 0.975) {
      // Ramps through exchanges (custodial ids on both fiat rails and chains).
      const std::uint32_t ex = pop_.exchanges[rng_.below(pop_.exchanges.size())];
      const Entity& ee = pop_.entities[ex];
      if (rng_.uniform() < 0.5) {
        const auto& group = pop_.fiat_by_region[ee.group];
        const std::uint32_t cust = member(group, static_cast<std::uint32_t>(rng_.below(
                                                     std::max<std::size_t>(1, group.size() / kCommunitySize))));
        const bool deposit = rng_.uniform() < 0.6;
        t.src = deposit ? cust : ex;
        t.dst = deposit ? ex : cust;
        const Juris& j = kRegions[ee.group].juris[0];
        t.channel = j.rail;
        t.currency = j.currency;
        t.amount_minor = to_minor(fiat_units(rng_, t.currency) * 3.0, t.currency);
      } else {
        const std::size_t ch = pick_weighted(rng_, kChains, [](const ChainDef& c) { return c.weight; });
        const auto& group = pop_.chain_members[ch];
        const std::uint32_t wallet = member(group, static_cast<std::uint32_t>(
                                                       rng_.below(std::max<std::size_t>(1, group.size() / kCommunitySize))));
        const bool withdraw = rng_.uniform() < 0.55;
        t.src = withdraw ? ex : wallet;
        t.dst = withdraw ? wallet : ex;
        t.channel = Channel::ONCHAIN;
        t.chain = kChains[ch].code;
        t.currency = kChains[ch].assets[0].first;
        t.amount_minor = to_minor(std::exp(std::log(kChains[ch].typical_amount * 4) + 1.2 * rng_.normal()), t.currency);
      }
    } else {
      // Offshore / unresolved flows.
      t.src = pop_.offshore[rng_.below(pop_.offshore.size())];
      if (rng_.uniform() < 0.4) {
        t.dst = pop_.offshore[rng_.below(pop_.offshore.size())];
      } else {
        const std::size_t reg = pick_weighted(rng_, kRegions, [](const Region& x) { return x.weight; });
        t.dst = member(pop_.fiat_by_region[reg], static_cast<std::uint32_t>(rng_.below(3)));
      }
      if (rng_.uniform() < 0.5) std::swap(t.src, t.dst);
      t.channel = Channel::SWIFT;
      t.currency = "USD";
      t.amount_minor = to_minor(fiat_units(rng_, "USD") * 40.0, "USD");
    }
    if (t.channel != Channel::ONCHAIN) {
      t.src_juris = juris_of(pop_, t.src);
      t.dst_juris = juris_of(pop_, t.dst);
    } else {
      t.src_juris = "";
      t.dst_juris = "";
    }
    return t;
  }

  Rng& rng() { return rng_; }

 private:
  const Population& pop_;
  Rng rng_;
};

void usage() {
  std::fprintf(stderr,
               "usage: gen_synthetic --rows N --out FILE [--entities M] [--seed S] [--format csv|ftxb]\n"
               "                     [--malformed-rate R] [--crlf]\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t rows = 1000000;
  std::uint64_t entities = 0;
  std::uint64_t seed = 42;
  std::string out = "synthetic.csv";
  std::string format;
  double malformedRate = 0.0;
  bool crlf = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--rows") rows = std::strtoull(next("--rows"), nullptr, 10);
    else if (a == "--entities") entities = std::strtoull(next("--entities"), nullptr, 10);
    else if (a == "--seed") seed = std::strtoull(next("--seed"), nullptr, 10);
    else if (a == "--out") out = next("--out");
    else if (a == "--format") format = next("--format");
    else if (a == "--malformed-rate") malformedRate = std::strtod(next("--malformed-rate"), nullptr);
    else if (a == "--crlf") crlf = true;
    else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", argv[i]);
      usage();
      return 2;
    }
  }
  if (entities == 0) entities = std::max<std::uint64_t>(2000, rows / 10);
  if (format.empty()) format = (out.size() >= 5 && out.compare(out.size() - 5, 5, ".ftxb") == 0) ? "ftxb" : "csv";
  if (format != "csv" && format != "ftxb") {
    usage();
    return 2;
  }

  Rng popRng(seed);
  const Population pop = build_population(entities, popRng);
  Generator gen(pop, seed);

  std::FILE* f = std::fopen(out.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s for writing\n", out.c_str());
    return 1;
  }
  char srcBuf[96], dstBuf[96], tsBuf[40], amtBuf[48], txBuf[32];
  std::uint64_t malformed = 0;

  if (format == "csv") {
    Writer w(f);
    const char* eol = crlf ? "\r\n" : "\n";
    w.put("tx_id,timestamp_utc,src_account,dst_account,amount,currency,channel,src_jurisdiction,dst_jurisdiction,chain");
    w.put(eol);
    for (std::uint64_t row = 0; row < rows; ++row) {
      const Tx t = gen.next(row);
      const std::size_t txn = static_cast<std::size_t>(
          std::snprintf(txBuf, sizeof(txBuf), "TX%012llx", static_cast<unsigned long long>(mix(row + seed) & 0xFFFFFFFFFFFFull)));
      if (malformedRate > 0 && gen.rng().uniform() < malformedRate) {
        // Deliberately broken rows exercise the malformed-row path.
        static const char* const kBad[] = {"BROKEN,ROW", "TX1,2024-13-45T00:00:00Z,A,B,12.5,USD,ACH,US,US,",
                                           "TX2,2024-01-01T00:00:00Z,A,B,12;5,USD,ACH,US,US,",
                                           "TX3,2024-01-01T00:00:00Z,\"unterminated,B,1,USD,ACH,US,US,",
                                           "TX4,2024-01-01T00:00:00Z,A,B,1,USD,TELEPORT,US,US,"};
        w.put(kBad[gen.rng().below(std::size(kBad))]);
        w.put(eol);
        ++malformed;
        continue;
      }
      const std::size_t sn = format_id(pop, t.src, t.src_variant, srcBuf);
      const std::size_t dn = format_id(pop, t.dst, t.dst_variant, dstBuf);
      const std::size_t tn = format_timestamp(t.ts_ms, tsBuf);
      const std::size_t an = format_amount(t.amount_minor, ftv::currency_scale(ftv::pack_code4(t.currency)), amtBuf);
      w.put(std::string_view(txBuf, txn));
      w.put(',');
      w.put(std::string_view(tsBuf, tn));
      w.put(',');
      const bool quote = (row % 97) == 0;  // some quoted identifiers (RFC 4180)
      if (quote) w.put('"');
      w.put(std::string_view(srcBuf, sn));
      if (quote) w.put('"');
      w.put(',');
      w.put(std::string_view(dstBuf, dn));
      w.put(',');
      w.put(std::string_view(amtBuf, an));
      w.put(',');
      w.put(t.currency);
      w.put(',');
      w.put(ftv::kChannelNames[static_cast<std::size_t>(t.channel)]);
      w.put(',');
      w.put(t.src_juris);
      w.put(',');
      w.put(t.dst_juris);
      w.put(',');
      w.put(t.chain);
      w.put(eol);
    }
    w.flush();
  } else {
    // .ftxb: entity strings are stored once in the pool, followed by tx ids.
    std::vector<char> pool;
    std::vector<std::uint32_t> poolOff(pop.entities.size());
    std::vector<std::uint16_t> poolLen(pop.entities.size());
    pool.reserve(pop.entities.size() * 32);
    for (std::uint32_t id = 0; id < pop.entities.size(); ++id) {
      const std::size_t n = format_id(pop, id, 0, srcBuf);
      poolOff[id] = static_cast<std::uint32_t>(pool.size());
      poolLen[id] = static_cast<std::uint16_t>(n);
      pool.insert(pool.end(), srcBuf, srcBuf + n);
    }
    const std::uint64_t txBase = pool.size();
    constexpr std::size_t kTxLen = 14;  // "TX" + 12 hex
    if (txBase + rows * kTxLen > 0xFFFFFFFFull) {
      std::fprintf(stderr, "dataset too large for 32-bit .ftxb string offsets\n");
      std::fclose(f);
      return 1;
    }
    ftv::FtxbHeader h{};
    std::memcpy(h.magic, ftv::kFtxbMagic, 4);
    h.version = ftv::kFtxbVersion;
    h.record_count = rows;
    h.records_offset = sizeof(ftv::FtxbHeader);
    h.pool_offset = h.records_offset + rows * sizeof(ftv::FtxbRecord);
    h.pool_size = txBase + rows * kTxLen;
    Writer w(f);
    w.put_bytes(&h, sizeof(h));
    for (std::uint64_t row = 0; row < rows; ++row) {
      const Tx t = gen.next(row);
      ftv::FtxbRecord r{};
      r.timestamp_ms = t.ts_ms;
      r.amount_minor = t.amount_minor;
      r.src_off = poolOff[t.src];
      r.src_len = poolLen[t.src];
      r.dst_off = poolOff[t.dst];
      r.dst_len = poolLen[t.dst];
      r.tx_id_off = static_cast<std::uint32_t>(txBase + row * kTxLen);
      r.tx_id_len = kTxLen;
      r.channel = static_cast<std::uint8_t>(t.channel);
      r.currency = ftv::pack_code4(t.currency);
      r.src_jurisdiction = ftv::pack_code8(t.src_juris);
      r.dst_jurisdiction = ftv::pack_code8(t.dst_juris);
      r.chain = ftv::pack_code8(t.chain);
      w.put_bytes(&r, sizeof(r));
    }
    w.put_bytes(pool.data(), pool.size());
    for (std::uint64_t row = 0; row < rows; ++row) {
      std::snprintf(txBuf, sizeof(txBuf), "TX%012llx", static_cast<unsigned long long>(mix(row + seed) & 0xFFFFFFFFFFFFull));
      w.put_bytes(txBuf, kTxLen);
    }
    w.flush();
  }
  std::fclose(f);
  std::fprintf(stderr, "gen_synthetic: wrote %llu rows (%llu deliberately malformed), %llu entities -> %s\n",
               static_cast<unsigned long long>(rows), static_cast<unsigned long long>(malformed),
               static_cast<unsigned long long>(entities), out.c_str());
  return 0;
}
