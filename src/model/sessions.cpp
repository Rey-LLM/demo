#include "model/sessions.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "core/file_util.hpp"
#include "core/json.hpp"
#include "model/solar.hpp"

namespace ftv {

namespace {

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept { return a >= 0 ? a / b : -((-a + b - 1) / b); }

bool parse_hhmm(const std::string& s, std::int32_t& out) {
  if (s.size() != 5 || s[2] != ':') return false;
  const int h = std::atoi(s.substr(0, 2).c_str()), m = std::atoi(s.substr(3, 2).c_str());
  if (h < 0 || h > 24 || m < 0 || m > 59) return false;
  out = h * 60 + m;
  return true;
}

bool parse_date_days(const std::string& s, std::int32_t& out) {
  int y = 0, m = 0, d = 0;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  out = static_cast<std::int32_t>(days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d)));
  return true;
}

// Weekday of a day number (0 = Sunday ... 6 = Saturday); 1970-01-01 was a Thursday.
int weekday(std::int64_t days) noexcept { return static_cast<int>(((days % 7) + 11) % 7); }

}  // namespace

bool expand_zone_offsets(const std::string& tz, int fromYear, int toYear, std::vector<std::int64_t>& begins,
                         std::vector<std::int32_t>& offsets, std::string& error) {
  using namespace std::chrono;
  begins.clear();
  offsets.clear();
  const time_zone* zone = nullptr;
  try {
    zone = locate_zone(tz);
  } catch (const std::exception& e) {
    error = "unknown time zone '" + tz + "' (" + e.what() + ")";
    return false;
  }
  const sys_seconds end{seconds{days_from_civil(toYear + 1, 1, 1) * 86400}};
  sys_seconds t{seconds{days_from_civil(fromYear, 1, 1) * 86400}};
  begins.push_back(t.time_since_epoch().count());
  while (t < end) {
    const sys_info info = zone->get_info(t);
    offsets.push_back(static_cast<std::int32_t>(info.offset.count()));
    if (info.end >= end || info.end <= t) break;
    t = info.end;
    begins.push_back(t.time_since_epoch().count());
  }
  if (offsets.size() < begins.size()) offsets.push_back(offsets.empty() ? 0 : offsets.back());
  return !offsets.empty();
}

std::int32_t SessionCalendar::utc_offset_s(std::size_t i, std::int64_t utcMs) const noexcept {
  const ExchangeSession& ex = ex_[i];
  if (ex.offsets.empty()) return 0;
  const std::int64_t s = floor_div(utcMs, 1000);
  const auto it = std::upper_bound(ex.offset_begins.begin(), ex.offset_begins.end(), s);
  const std::size_t k = it == ex.offset_begins.begin() ? 0 : static_cast<std::size_t>(it - ex.offset_begins.begin()) - 1;
  return ex.offsets[std::min(k, ex.offsets.size() - 1)];
}

SessionStatus SessionCalendar::state(std::size_t i, std::int64_t utcMs) const noexcept {
  SessionStatus st;
  if (i >= ex_.size()) return st;
  const ExchangeSession& ex = ex_[i];
  const std::int32_t off = utc_offset_s(i, utcMs);
  st.utc_offset_s = off;
  const std::int64_t localMs = utcMs + static_cast<std::int64_t>(off) * 1000;
  const std::int64_t day = floor_div(localMs, 86400000);
  const std::int64_t minuteMs = localMs - day * 86400000;  // ms since local midnight

  auto trading_day = [&](std::int64_t d) {
    const int wd = weekday(d);
    if (wd == 0 || wd == 6) return false;
    return !std::binary_search(ex.holidays.begin(), ex.holidays.end(), static_cast<std::int32_t>(d));
  };
  auto is_holiday = [&](std::int64_t d) {
    const int wd = weekday(d);
    return wd != 0 && wd != 6 && std::binary_search(ex.holidays.begin(), ex.holidays.end(), static_cast<std::int32_t>(d));
  };
  // Local ms of a day's boundaries -> UTC ms (the offset at that instant is
  // taken from the current one; sessions never straddle a DST change).
  auto to_utc = [&](std::int64_t d, std::int32_t minute) {
    const std::int64_t local = d * 86400000 + static_cast<std::int64_t>(minute) * 60000;
    const std::int64_t guess = local - static_cast<std::int64_t>(off) * 1000;
    return local - static_cast<std::int64_t>(utc_offset_s(i, guess)) * 1000;
  };

  const bool lunch = ex.lunch_end > ex.lunch_begin;
  const std::int64_t m = minuteMs;
  auto at = [](std::int32_t minute) { return static_cast<std::int64_t>(minute) * 60000; };
  if (trading_day(day)) {
    if (m < at(ex.pre_open)) {
      st.state = SessionState::Closed;
      st.next_state = ex.pre_open < ex.open ? SessionState::Pre : SessionState::Open;
      st.next_change_ms = to_utc(day, ex.pre_open);
      return st;
    }
    if (m < at(ex.open)) {
      st.state = SessionState::Pre;
      st.next_state = SessionState::Open;
      st.next_change_ms = to_utc(day, ex.open);
      return st;
    }
    if (lunch && m >= at(ex.lunch_begin) && m < at(ex.lunch_end)) {
      st.state = SessionState::Lunch;
      st.next_state = SessionState::Open;
      st.next_change_ms = to_utc(day, ex.lunch_end);
      return st;
    }
    if (m < at(ex.close)) {
      st.state = SessionState::Open;
      const bool beforeLunch = lunch && m < at(ex.lunch_begin);
      st.next_state = beforeLunch ? SessionState::Lunch : SessionState::Closed;
      st.next_change_ms = to_utc(day, beforeLunch ? ex.lunch_begin : ex.close);
      return st;
    }
  }
  st.state = is_holiday(day) ? SessionState::Holiday : SessionState::Closed;
  // Next trading day's pre-open (bounded search: at most two weeks ahead).
  for (std::int64_t d = day + 1; d <= day + 14; ++d) {
    if (trading_day(d)) {
      st.next_state = ex.pre_open < ex.open ? SessionState::Pre : SessionState::Open;
      st.next_change_ms = to_utc(d, ex.pre_open);
      break;
    }
  }
  return st;
}

double SessionCalendar::hub_factor(std::size_t hub, std::int64_t utcMs) const noexcept {
  const int e = exchange_of_hub(hub);
  if (e < 0) return 1.0;
  return intensity[static_cast<std::size_t>(state(static_cast<std::size_t>(e), utcMs).state)];
}

SessionCalendar make_session_calendar(std::vector<ExchangeSession> exchanges, std::size_t hubCount) {
  SessionCalendar cal;
  cal.hub_map_.fill(-1);
  for (std::size_t i = 0; i < exchanges.size(); ++i) {
    ExchangeSession& ex = exchanges[i];
    std::sort(ex.holidays.begin(), ex.holidays.end());
    if (ex.offsets.empty()) {
      std::string err;
      int y0 = 2024, y1 = 2030;
      if (!ex.holiday_years.empty()) {
        y0 = *std::min_element(ex.holiday_years.begin(), ex.holiday_years.end()) - 1;
        y1 = *std::max_element(ex.holiday_years.begin(), ex.holiday_years.end()) + 1;
      }
      (void)expand_zone_offsets(ex.tz, y0, y1, ex.offset_begins, ex.offsets, err);
    }
    if (ex.hub < hubCount && ex.hub < cal.hub_map_.size() && cal.hub_map_[ex.hub] < 0) {
      cal.hub_map_[ex.hub] = static_cast<int>(i);
    }
  }
  cal.ex_ = std::move(exchanges);
  return cal;
}

std::optional<SessionCalendar> load_sessions(const std::string& path, const HubTaxonomy& taxonomy, std::string& error) {
  const auto text = read_text_file(path);
  if (!text) {
    error = "cannot open " + path;
    return std::nullopt;
  }
  const auto doc = parse_json(*text, error);
  if (!doc) {
    error = path + ": " + error;
    return std::nullopt;
  }
  std::vector<ExchangeSession> list;
  const JsonValue* arr = doc->find("exchanges");
  if (!arr || !arr->is_array()) {
    error = path + ": missing \"exchanges\" array";
    return std::nullopt;
  }
  for (const JsonValue& j : arr->items()) {
    ExchangeSession ex;
    ex.id = j.string_or("id", "");
    ex.name = j.string_or("name", ex.id);
    ex.city = j.string_or("city", ex.id);
    ex.tz = j.string_or("tz", "");
    ex.source = j.string_or("source", "");
    ex.as_of = j.string_or("as_of", "");
    const auto hub = taxonomy.find(j.string_or("hub", ""));
    if (ex.id.empty() || ex.tz.empty() || !hub) {
      error = path + ": exchange '" + ex.id + "' needs id, tz and a known hub";
      return std::nullopt;
    }
    if (ex.source.empty() || ex.as_of.empty()) {
      error = path + ": exchange '" + ex.id + "' needs source and as_of";
      return std::nullopt;
    }
    ex.hub = *hub;
    const std::string open = j.string_or("open", ""), close = j.string_or("close", "");
    const std::string pre = j.string_or("pre_open", open);
    if (!parse_hhmm(open, ex.open) || !parse_hhmm(close, ex.close) || !parse_hhmm(pre, ex.pre_open) ||
        ex.pre_open > ex.open || ex.open >= ex.close) {
      error = path + ": exchange '" + ex.id + "': bad pre_open/open/close (HH:MM, pre_open <= open < close)";
      return std::nullopt;
    }
    if (const JsonValue* l = j.find("lunch"); l && l->is_array() && l->items().size() == 2) {
      if (!l->items()[0].is_string() || !l->items()[1].is_string() ||
          !parse_hhmm(l->items()[0].as_string(), ex.lunch_begin) ||
          !parse_hhmm(l->items()[1].as_string(), ex.lunch_end) || ex.lunch_begin < ex.open ||
          ex.lunch_end > ex.close || ex.lunch_begin >= ex.lunch_end) {
        error = path + ": exchange '" + ex.id + "': bad lunch break";
        return std::nullopt;
      }
    }
    if (const JsonValue* h = j.find("holidays"); h && h->is_object()) {
      for (const auto& [year, dates] : h->members()) {
        const int y = std::atoi(year.c_str());
        if (y < 1970 || !dates.is_array()) {
          error = path + ": exchange '" + ex.id + "': holidays must map a year to a date list";
          return std::nullopt;
        }
        ex.holiday_years.push_back(y);
        for (const JsonValue& d : dates.items()) {
          std::int32_t days = 0;
          if (!d.is_string() || !parse_date_days(d.as_string(), days)) {
            error = path + ": exchange '" + ex.id + "': bad holiday date";
            return std::nullopt;
          }
          ex.holidays.push_back(days);
        }
      }
    }
    int y0 = 2024, y1 = 2030;
    if (!ex.holiday_years.empty()) {
      y0 = *std::min_element(ex.holiday_years.begin(), ex.holiday_years.end()) - 1;
      y1 = *std::max_element(ex.holiday_years.begin(), ex.holiday_years.end()) + 1;
    }
    if (!expand_zone_offsets(ex.tz, y0, y1, ex.offset_begins, ex.offsets, error)) {
      error = path + ": exchange '" + ex.id + "': " + error;
      return std::nullopt;
    }
    list.push_back(std::move(ex));
  }
  SessionCalendar cal = make_session_calendar(std::move(list), taxonomy.size());
  cal.as_of = doc->string_or("as_of", "");
  if (const JsonValue* in = doc->find("intensity"); in && in->is_object()) {
    for (std::size_t s = 0; s < cal.intensity.size(); ++s) {
      cal.intensity[s] = std::clamp(in->number_or(kSessionStateNames[s], cal.intensity[s]), 0.0, 4.0);
    }
  }
  try {
    cal.tzdb_version = std::chrono::get_tzdb().version;
  } catch (...) {
    cal.tzdb_version = "unknown";
  }
  return cal;
}

void format_session_chip(const ExchangeSession& ex, const SessionStatus& st, std::int64_t utcMs, char* out,
                         std::size_t cap) noexcept {
  const char* name = kSessionStateNames[static_cast<std::size_t>(st.state) % 5];
  const std::int64_t left = st.next_change_ms > utcMs ? (st.next_change_ms - utcMs) / 60000 : 0;
  const char* what = st.next_state == SessionState::Closed ? "close"
                     : st.next_state == SessionState::Lunch ? "lunch"
                     : st.next_state == SessionState::Pre ? "pre-open"
                                                          : "open";
  if (st.next_change_ms > utcMs && left < 100 * 60) {
    std::snprintf(out, cap, "%-9.9s %-7s %02lld:%02lld to %s", ex.city.c_str(), name, static_cast<long long>(left / 60),
                  static_cast<long long>(left % 60), what);
  } else if (st.next_change_ms > utcMs) {
    std::snprintf(out, cap, "%-9.9s %-7s %lldd %02lldh to %s", ex.city.c_str(), name,
                  static_cast<long long>(left / 1440), static_cast<long long>((left / 60) % 24), what);
  } else {
    std::snprintf(out, cap, "%-9.9s %s", ex.city.c_str(), name);
  }
}

}  // namespace ftv
