#pragma once
// Command / search bar ("/" or Ctrl+K). Pure logic: the text buffer, the
// grammar, fuzzy matching and the active filters. The app draws it and acts
// on the chosen result; tests drive it directly. No allocation after
// construction: the query and the results live in fixed arrays.
//
// Grammar (case-insensitive, one term per query):
//   london | tokyo | ny ...      fly to a hub         (fuzzy on hub names and aliases)
//   eth | btc | sol | tron       fly to a chain cloud (fuzzy on chain names)
//   >10m | >250k | >5000         filter: USD value at least that much
//   whale:t2                     filter: whale tier at least 2 (t1..t3)
//   venue:okx                    filter: one venue (fuzzy on venue names)
//   liq                          filter: liquidations only
//   addr:0x...                   find an entity in the graph (exact address), else tape rows with matching fragments
//   tx:...                       find a transaction on the tape (exact hash)
// Filters combine (AND) and are shown as removable chips; the same filter
// kind replaces the previous one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ingest/live/live_event.hpp"

namespace ftv {

enum class CommandKind : std::uint8_t { None = 0, FlyHub, FlyChain, FilterMinUsd, FilterTier, FilterVenue, FilterLiq,
                                        FindAddress, FindTx };

struct CommandResult {
  CommandKind kind = CommandKind::None;
  std::int32_t hub = -1;      // FlyHub / FlyChain
  double value = 0.0;         // FilterMinUsd (USD), FilterTier (1..3)
  std::uint32_t feed_mask = 0;  // FilterVenue: feeds of that venue
  int score = 0;              // fuzzy score (higher is better)
  char label[48] = {};        // shown in the result list
  char arg[96] = {};          // FindAddress / FindTx: the query text after the prefix
};

enum class FilterKind : std::uint8_t { MinUsd = 0, Tier, Venue, Liq, Count };

struct TapeFilter {
  double min_usd = 0.0;
  std::uint8_t min_tier = 0;     // 0: any
  std::uint32_t feed_mask = 0;   // 0: any venue
  bool liq_only = false;
  [[nodiscard]] bool active(FilterKind k) const noexcept {
    switch (k) {
      case FilterKind::MinUsd: return min_usd > 0.0;
      case FilterKind::Tier: return min_tier > 0;
      case FilterKind::Venue: return feed_mask != 0;
      case FilterKind::Liq: return liq_only;
      default: return false;
    }
  }
  [[nodiscard]] bool any() const noexcept { return min_usd > 0.0 || min_tier > 0 || feed_mask != 0 || liq_only; }
  // Does a row/arc with these properties pass?
  [[nodiscard]] bool pass(double usd, std::uint8_t tier, FeedId feed, bool liquidation) const noexcept {
    if (usd < min_usd) return false;
    if (min_tier > 0 && tier < min_tier) return false;
    if (feed_mask != 0 && !(feed_mask & (1u << static_cast<unsigned>(feed)))) return false;
    if (liq_only && !liquidation) return false;
    return true;
  }
  void clear(FilterKind k) noexcept;
  // Chip text, e.g. ">10M", "WHALE T2", "VENUE OKX", "LIQ" (empty when inactive).
  void chip(FilterKind k, char* out, std::size_t cap) const noexcept;
};

struct HubName {
  std::string_view id;       // NEW_YORK
  std::string_view display;  // New York
  bool chain = false;
};

class CommandBar {
 public:
  static constexpr std::size_t kMaxQuery = 95;
  static constexpr std::size_t kMaxResults = 6;
  static constexpr std::size_t kMaxHubs = 32;

  // Hub names in hubs.json order (copied as views: the strings must outlive the bar).
  void set_hubs(const HubName* hubs, std::size_t n) noexcept;

  void open() noexcept;
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  // Editing (UTF-8 input is accepted as ASCII only; other code points are ignored).
  void insert(std::uint32_t codepoint) noexcept;
  void backspace() noexcept;
  void move_selection(int delta) noexcept;
  void set_text(std::string_view text) noexcept;  // tests / programmatic

  [[nodiscard]] std::string_view text() const noexcept { return {query_.data(), len_}; }
  [[nodiscard]] std::size_t result_count() const noexcept { return count_; }
  [[nodiscard]] const CommandResult& result(std::size_t i) const noexcept { return results_[i]; }
  [[nodiscard]] std::size_t selection() const noexcept { return sel_; }
  // The chosen result (Enter). Filters are applied to `filter()` here; the
  // caller handles fly-to and find. Closes the bar.
  CommandResult accept() noexcept;

  [[nodiscard]] const TapeFilter& filter() const noexcept { return filter_; }
  TapeFilter& filter() noexcept { return filter_; }

  // Recomputes the results for the current text.
  void update() noexcept;

 private:
  void add(const CommandResult& r) noexcept;

  std::array<char, kMaxQuery + 1> query_{};
  std::size_t len_ = 0;
  bool open_ = false;
  std::array<CommandResult, kMaxResults> results_{};
  std::size_t count_ = 0, sel_ = 0;
  std::array<HubName, kMaxHubs> hubs_{};
  std::size_t hub_count_ = 0;
  TapeFilter filter_;
};

// Fuzzy match of `query` against `name` (case-insensitive subsequence):
// < 0 when not a match; higher is better (prefix and word-start bonuses).
int fuzzy_score(std::string_view query, std::string_view name) noexcept;

// ">10m", "10M", "250k", "1.5b", "5000" -> USD. False on a malformed number.
bool parse_usd_amount(std::string_view s, double& out) noexcept;

// Venue name -> feed bit mask (BINANCE: spot + USD-M liquidations).
std::uint32_t venue_feed_mask(std::string_view venue) noexcept;

}  // namespace ftv
