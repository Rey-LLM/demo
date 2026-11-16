#pragma once
// Exchange trading sessions (config/sessions.json): regular hours, lunch
// breaks and holidays per exchange, evaluated in the exchange's IANA time
// zone through the C++20 tzdb (std::chrono::locate_zone).
//
// load_sessions() runs once at startup: it resolves each zone and expands its
// UTC offsets into a small table of [begin, end) intervals covering the
// configured holiday years (+/- one year). Everything after that, state() and
// factor(), is a table lookup: no tzdb calls and no allocation, so the model
// and render threads can ask every frame.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "model/hub_taxonomy.hpp"

namespace ftv {

enum class SessionState : std::uint8_t { Pre = 0, Open, Lunch, Closed, Holiday, Count };
inline constexpr const char* kSessionStateNames[] = {"PRE", "OPEN", "LUNCH", "CLOSED", "HOLIDAY"};

struct SessionStatus {
  SessionState state = SessionState::Closed;
  std::int64_t next_change_ms = 0;  // UTC ms of the next state change (0 when unknown)
  SessionState next_state = SessionState::Closed;
  std::int32_t utc_offset_s = 0;    // zone offset at the queried instant
};

struct ExchangeSession {
  std::string id;         // "LSE"
  std::string name;       // "London Stock Exchange"
  std::string city;       // HUD chip label: "LONDON"
  std::string tz;         // IANA zone
  std::string source;     // holiday calendar URL
  std::string as_of;      // date the hours/holidays were retrieved
  std::uint16_t hub = 0;  // config/hubs.json index
  // Minutes after local midnight. lunch_begin == lunch_end: no lunch break.
  std::int32_t pre_open = 0, open = 0, close = 0, lunch_begin = 0, lunch_end = 0;
  std::vector<std::int32_t> holidays;  // local dates as days since 1970-01-01, sorted
  std::vector<int> holiday_years;      // years the holiday list covers
  // UTC offset table: offsets[i] applies from begins[i] (UTC s) until begins[i + 1].
  std::vector<std::int64_t> offset_begins;
  std::vector<std::int32_t> offsets;
};

class SessionCalendar {
 public:
  [[nodiscard]] std::size_t size() const noexcept { return ex_.size(); }
  [[nodiscard]] const ExchangeSession& exchange(std::size_t i) const noexcept { return ex_[i]; }
  [[nodiscard]] const std::vector<ExchangeSession>& exchanges() const noexcept { return ex_; }

  // State of exchange `i` at UTC ms `t`.
  [[nodiscard]] SessionStatus state(std::size_t i, std::int64_t utcMs) const noexcept;
  // Exchange serving hub `hub`, or -1.
  [[nodiscard]] int exchange_of_hub(std::size_t hub) const noexcept {
    return hub < hub_map_.size() ? hub_map_[hub] : -1;
  }
  // MODELED intensity multiplier for a hub at `t` (1.0 for hubs without an exchange).
  [[nodiscard]] double hub_factor(std::size_t hub, std::int64_t utcMs) const noexcept;
  // UTC offset (s) of exchange `i` at `t`, from the precomputed table.
  [[nodiscard]] std::int32_t utc_offset_s(std::size_t i, std::int64_t utcMs) const noexcept;

  std::string as_of;  // file-level retrieval date
  std::string tzdb_version;

  // Intensity multiplier per state (config "intensity").
  std::array<double, static_cast<std::size_t>(SessionState::Count)> intensity{0.4, 1.0, 0.5, 0.2, 0.1};

 private:
  friend std::optional<SessionCalendar> load_sessions(const std::string&, const HubTaxonomy&, std::string&);
  friend SessionCalendar make_session_calendar(std::vector<ExchangeSession>, std::size_t);
  std::vector<ExchangeSession> ex_;
  std::array<int, 32> hub_map_ = [] {
    std::array<int, 32> a{};
    a.fill(-1);
    return a;
  }();
};

std::optional<SessionCalendar> load_sessions(const std::string& path, const HubTaxonomy& taxonomy, std::string& error);

// Tests: builds a calendar from exchanges whose offset tables are already filled
// (or resolves them through the tzdb when empty).
SessionCalendar make_session_calendar(std::vector<ExchangeSession> exchanges, std::size_t hubCount);

// Expands `tz` into UTC offset intervals over [fromYear, toYear]. False when the zone is unknown.
bool expand_zone_offsets(const std::string& tz, int fromYear, int toYear, std::vector<std::int64_t>& begins,
                         std::vector<std::int32_t>& offsets, std::string& error);

// "LONDON  OPEN  02:14 to close" (no allocation; `out` >= 48 bytes).
void format_session_chip(const ExchangeSession& ex, const SessionStatus& st, std::int64_t utcMs, char* out,
                         std::size_t cap) noexcept;

}  // namespace ftv
