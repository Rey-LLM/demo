#include "ui/command_bar.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace ftv {

namespace {

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool starts_with_ci(std::string_view s, std::string_view p) noexcept {
  if (p.size() > s.size()) return false;
  for (std::size_t i = 0; i < p.size(); ++i) {
    if (lower(s[i]) != lower(p[i])) return false;
  }
  return true;
}

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

void copy_label(char* out, std::size_t cap, std::string_view s) noexcept {
  const std::size_t n = std::min(cap - 1, s.size());
  std::memcpy(out, s.data(), n);
  out[n] = '\0';
}

void format_usd_short(double usd, char* out, std::size_t cap) noexcept {
  if (usd >= 1e9) {
    std::snprintf(out, cap, "%gB", usd / 1e9);
  } else if (usd >= 1e6) {
    std::snprintf(out, cap, "%gM", usd / 1e6);
  } else if (usd >= 1e3) {
    std::snprintf(out, cap, "%gK", usd / 1e3);
  } else {
    std::snprintf(out, cap, "%g", usd);
  }
}

struct Alias {
  std::string_view alias;
  std::string_view hub;
};
// Common short names (fuzzy matching handles the rest).
constexpr Alias kAliases[] = {
    {"ny", "NEW_YORK"}, {"nyc", "NEW_YORK"}, {"new york", "NEW_YORK"}, {"ldn", "LONDON"}, {"fra", "FRANKFURT"},
    {"zrh", "ZURICH"},  {"tyo", "TOKYO"},    {"sgp", "SINGAPORE"},      {"sg", "SINGAPORE"}, {"hk", "HONG_KONG"},
    {"sha", "SHANGHAI"}, {"btc", "BLOCKCHAIN_BTC"}, {"bitcoin", "BLOCKCHAIN_BTC"}, {"eth", "BLOCKCHAIN_ETH"},
    {"ether", "BLOCKCHAIN_ETH"}, {"sol", "BLOCKCHAIN_SOL"}, {"trx", "BLOCKCHAIN_TRON"}, {"tron", "BLOCKCHAIN_TRON"},
};

struct Venue {
  std::string_view name;
  std::uint32_t mask;
};
constexpr std::uint32_t bit(FeedId f) { return 1u << static_cast<unsigned>(f); }
constexpr Venue kVenues[] = {
    {"binance", bit(FeedId::Binance) | bit(FeedId::BinanceLiq)},
    {"okx", bit(FeedId::Okx)},
    {"coinbase", bit(FeedId::Coinbase)},
    {"kraken", bit(FeedId::Kraken)},
    {"btc", bit(FeedId::BtcMempool)},
    {"eth", bit(FeedId::Eth)},
    {"sol", bit(FeedId::Sol)},
};

}  // namespace

int fuzzy_score(std::string_view q, std::string_view name) noexcept {
  if (q.empty()) return -1;
  // Subsequence match; consecutive and word-start matches score higher.
  int score = 0;
  std::size_t j = 0;
  bool prevMatched = false;
  for (std::size_t i = 0; i < name.size() && j < q.size(); ++i) {
    const char c = lower(name[i]);
    if (c == lower(q[j]) || (q[j] == ' ' && (c == '_' || c == ' '))) {
      const bool wordStart = i == 0 || name[i - 1] == '_' || name[i - 1] == ' ' || name[i - 1] == '-';
      score += 10 + (prevMatched ? 8 : 0) + (wordStart ? 6 : 0);
      prevMatched = true;
      ++j;
    } else {
      prevMatched = false;
      score -= 1;
    }
  }
  if (j < q.size()) return -1;
  if (starts_with_ci(name, q)) score += 30;
  if (q.size() == name.size() && starts_with_ci(name, q)) score += 40;
  return std::max(score, 0);
}

bool parse_usd_amount(std::string_view s, double& out) noexcept {
  s = trim(s);
  if (!s.empty() && (s.front() == '>' || s.front() == '$')) s.remove_prefix(1);
  if (!s.empty() && s.front() == '=') s.remove_prefix(1);
  if (!s.empty() && s.front() == '$') s.remove_prefix(1);
  if (s.empty()) return false;
  double mult = 1.0;
  const char last = lower(s.back());
  if (last == 'k' || last == 'm' || last == 'b') {
    mult = last == 'k' ? 1e3 : last == 'm' ? 1e6 : 1e9;
    s.remove_suffix(1);
  }
  double v = 0.0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !(v >= 0.0)) return false;
  out = v * mult;
  return true;
}

std::uint32_t venue_feed_mask(std::string_view venue) noexcept {
  int best = -1;
  std::uint32_t mask = 0;
  for (const Venue& v : kVenues) {
    const int s = fuzzy_score(venue, v.name);
    if (s > best) {
      best = s;
      mask = v.mask;
    }
  }
  return best >= 0 ? mask : 0u;
}

void TapeFilter::clear(FilterKind k) noexcept {
  switch (k) {
    case FilterKind::MinUsd: min_usd = 0.0; break;
    case FilterKind::Tier: min_tier = 0; break;
    case FilterKind::Venue: feed_mask = 0; break;
    case FilterKind::Liq: liq_only = false; break;
    default: break;
  }
}

void TapeFilter::chip(FilterKind k, char* out, std::size_t cap) const noexcept {
  out[0] = '\0';
  if (!active(k)) return;
  switch (k) {
    case FilterKind::MinUsd: {
      char v[24];
      format_usd_short(min_usd, v, sizeof v);
      std::snprintf(out, cap, ">%s", v);
      break;
    }
    case FilterKind::Tier: std::snprintf(out, cap, "WHALE T%u", static_cast<unsigned>(min_tier)); break;
    case FilterKind::Venue: {
      const char* name = "?";
      for (const Venue& v : kVenues) {
        if (v.mask == feed_mask) {
          name = v.name.data();
          break;
        }
      }
      char up[16] = {};
      for (std::size_t i = 0; i + 1 < sizeof up && name[i]; ++i) up[i] = name[i] >= 'a' && name[i] <= 'z' ? static_cast<char>(name[i] - 32) : name[i];
      std::snprintf(out, cap, "VENUE %s", up);
      break;
    }
    case FilterKind::Liq: std::snprintf(out, cap, "LIQ"); break;
    default: break;
  }
}

void CommandBar::set_hubs(const HubName* hubs, std::size_t n) noexcept {
  hub_count_ = std::min(n, kMaxHubs);
  for (std::size_t i = 0; i < hub_count_; ++i) hubs_[i] = hubs[i];
}

void CommandBar::open() noexcept {
  open_ = true;
  len_ = 0;
  query_[0] = '\0';
  update();
}

void CommandBar::close() noexcept {
  open_ = false;
  count_ = 0;
  sel_ = 0;
}

void CommandBar::insert(std::uint32_t cp) noexcept {
  if (cp < 0x20 || cp > 0x7E || len_ >= kMaxQuery) return;
  query_[len_++] = static_cast<char>(cp);
  query_[len_] = '\0';
  update();
}

void CommandBar::backspace() noexcept {
  if (len_ == 0) return;
  query_[--len_] = '\0';
  update();
}

void CommandBar::set_text(std::string_view t) noexcept {
  len_ = std::min(t.size(), kMaxQuery);
  std::memcpy(query_.data(), t.data(), len_);
  query_[len_] = '\0';
  update();
}

void CommandBar::move_selection(int delta) noexcept {
  if (count_ == 0) return;
  const int n = static_cast<int>(count_);
  sel_ = static_cast<std::size_t>(((static_cast<int>(sel_) + delta) % n + n) % n);
}

void CommandBar::add(const CommandResult& r) noexcept {
  // Keep the best kMaxResults by score (stable for equal scores).
  if (count_ < kMaxResults) {
    results_[count_++] = r;
  } else if (r.score > results_[count_ - 1].score) {
    results_[count_ - 1] = r;
  } else {
    return;
  }
  for (std::size_t i = count_ - 1; i > 0 && results_[i].score > results_[i - 1].score; --i) {
    std::swap(results_[i], results_[i - 1]);
  }
}

void CommandBar::update() noexcept {
  count_ = 0;
  sel_ = 0;
  const std::string_view q = trim(text());
  if (q.empty()) return;

  // Prefixed terms.
  auto after = [&](std::string_view prefix, std::string_view& rest) {
    if (!starts_with_ci(q, prefix)) return false;
    rest = trim(q.substr(prefix.size()));
    return true;
  };
  std::string_view rest;
  if (after("addr:", rest)) {
    CommandResult r;
    r.kind = CommandKind::FindAddress;
    r.score = 1000;
    copy_label(r.arg, sizeof r.arg, rest);
    std::snprintf(r.label, sizeof r.label, "FIND ADDRESS %.30s", r.arg);
    add(r);
    return;
  }
  if (after("tx:", rest)) {
    CommandResult r;
    r.kind = CommandKind::FindTx;
    r.score = 1000;
    copy_label(r.arg, sizeof r.arg, rest);
    std::snprintf(r.label, sizeof r.label, "FIND TX %.36s", r.arg);
    add(r);
    return;
  }
  if (after("whale:", rest)) {
    if (rest.size() == 2 && lower(rest[0]) == 't' && rest[1] >= '1' && rest[1] <= '3') {
      CommandResult r;
      r.kind = CommandKind::FilterTier;
      r.value = rest[1] - '0';
      r.score = 1000;
      std::snprintf(r.label, sizeof r.label, "FILTER  WHALE TIER >= %c", rest[1]);
      add(r);
    }
    return;
  }
  if (after("venue:", rest)) {
    for (const Venue& v : kVenues) {
      const int s = rest.empty() ? 1 : fuzzy_score(rest, v.name);
      if (s < 0) continue;
      CommandResult r;
      r.kind = CommandKind::FilterVenue;
      r.feed_mask = v.mask;
      r.score = s;
      std::snprintf(r.label, sizeof r.label, "FILTER  VENUE %.*s", static_cast<int>(v.name.size()), v.name.data());
      for (char* c = r.label; *c; ++c) *c = *c >= 'a' && *c <= 'z' ? static_cast<char>(*c - 32) : *c;
      add(r);
    }
    return;
  }
  if (q.front() == '>') {
    double usd = 0.0;
    if (parse_usd_amount(q, usd)) {
      CommandResult r;
      r.kind = CommandKind::FilterMinUsd;
      r.value = usd;
      r.score = 1000;
      char v[24];
      format_usd_short(usd, v, sizeof v);
      std::snprintf(r.label, sizeof r.label, "FILTER  VALUE > $%s", v);
      add(r);
    }
    return;
  }
  if (q.size() >= 3 && starts_with_ci(q, "liq")) {
    CommandResult r;
    r.kind = CommandKind::FilterLiq;
    r.score = 900;
    std::snprintf(r.label, sizeof r.label, "FILTER  LIQUIDATIONS ONLY");
    add(r);
  }

  // Hubs and chain clouds: fuzzy on id, display name and aliases.
  for (std::size_t i = 0; i < hub_count_; ++i) {
    const HubName& h = hubs_[i];
    int best = std::max(fuzzy_score(q, h.display), fuzzy_score(q, h.id));
    for (const Alias& a : kAliases) {
      if (a.hub == h.id) {
        const int s = fuzzy_score(q, a.alias);
        if (s >= 0) best = std::max(best, s + (q.size() == a.alias.size() ? 60 : 0));
      }
    }
    if (best < 0) continue;
    CommandResult r;
    r.kind = h.chain ? CommandKind::FlyChain : CommandKind::FlyHub;
    r.hub = static_cast<std::int32_t>(i);
    r.score = best;
    std::snprintf(r.label, sizeof r.label, "%s %.*s", h.chain ? "CHAIN" : "HUB  ", static_cast<int>(h.display.size()),
                  h.display.data());
    add(r);
  }
}

CommandResult CommandBar::accept() noexcept {
  CommandResult r;
  if (count_ > 0) r = results_[std::min(sel_, count_ - 1)];
  switch (r.kind) {
    case CommandKind::FilterMinUsd: filter_.min_usd = r.value; break;
    case CommandKind::FilterTier: filter_.min_tier = static_cast<std::uint8_t>(r.value); break;
    case CommandKind::FilterVenue: filter_.feed_mask = r.feed_mask; break;
    case CommandKind::FilterLiq: filter_.liq_only = true; break;
    default: break;
  }
  close();
  return r;
}

}  // namespace ftv
