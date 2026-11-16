// v4: solar position, trading sessions, RPC endpoint pools, the command bar,
// submarine-cable routing and the city-lights asset format.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "ingest/live/endpoint_pool.hpp"
#include "model/cables.hpp"
#include "model/citylights.hpp"
#include "model/sessions.hpp"
#include "model/solar.hpp"
#include "test_framework.hpp"
#include "ui/command_bar.hpp"

using namespace ftv;

namespace {

std::int64_t utc(const char* iso) {
  const auto ms = parse_iso8601_utc(iso);
  CHECK(ms.has_value());
  return ms.value_or(0);
}

double lon_diff(double a, double b) {
  double d = std::fmod(a - b + 540.0, 360.0) - 180.0;
  return std::fabs(d);
}

}  // namespace

// Reference: astropy 8.0.2, get_sun() transformed to ITRS (direction of the
// sun in Earth-fixed coordinates), at six instants across two years.
TEST_CASE("solar: subsolar point within 0.2 degrees of astropy on 6 dates") {
  struct Ref {
    const char* t;
    double lat, lon;
  };
  const Ref refs[] = {
      {"2024-03-20T03:06:00Z", 0.0, 135.3548},     {"2024-06-20T20:51:00Z", 23.4381, -132.3036},
      {"2024-12-21T09:20:00Z", -23.4384, 39.5588}, {"2025-02-11T12:00:00Z", -13.8465, 3.5467},
      {"2025-11-03T12:00:00Z", -15.2270, -4.1088}, {"2026-07-04T18:30:00Z", 22.8223, -96.3766},
  };
  for (const Ref& r : refs) {
    const SubsolarPoint p = subsolar_point(utc(r.t));
    const double dLat = std::fabs(p.lat_deg - r.lat), dLon = lon_diff(p.lon_deg, r.lon);
    if (dLat > 0.2 || dLon > 0.2) std::fprintf(stderr, "  %s: lat %.4f lon %.4f\n", r.t, p.lat_deg, p.lon_deg);
    CHECK(dLat <= 0.2);
    CHECK(dLon <= 0.2);
  }
}

TEST_CASE("solar: sun direction, ISO 8601 parsing and formatting") {
  float v[3];
  sun_direction(SubsolarPoint{0.0, 0.0, 0.0}, v);
  CHECK(std::fabs(v[2] - 1.0f) < 1e-6f && std::fabs(v[0]) < 1e-6f && std::fabs(v[1]) < 1e-6f);
  sun_direction(SubsolarPoint{0.0, 90.0, 0.0}, v);
  CHECK(std::fabs(v[0] - 1.0f) < 1e-6f);
  sun_direction(SubsolarPoint{90.0, 0.0, 0.0}, v);
  CHECK(std::fabs(v[1] - 1.0f) < 1e-6f);
  CHECK(utc("1970-01-01T00:00:00Z") == 0);
  CHECK(utc("2026-03-20T12:00:00Z") == 1774008000000);
  CHECK(utc("2026-03-20 12:00") == 1774008000000);
  CHECK(utc("2026-03-20T12:00:00.250+00:00") == 1774008000250);
  CHECK(!parse_iso8601_utc("2026-13-01T00:00Z"));
  CHECK(!parse_iso8601_utc("2026-03-20T12:00:00+01:00"));
  CHECK(!parse_iso8601_utc("yesterday"));
  char buf[32];
  format_iso8601_utc(1774008000000, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "2026-03-20T12:00:00Z") == 0);
}

namespace {
SessionCalendar test_calendar() {
  std::vector<ExchangeSession> ex(2);
  ex[0].id = "LSE";
  ex[0].city = "LONDON";
  ex[0].tz = "Europe/London";
  ex[0].hub = 1;
  ex[0].pre_open = 7 * 60 + 50;
  ex[0].open = 8 * 60;
  ex[0].close = 16 * 60 + 30;
  ex[0].holidays = {static_cast<std::int32_t>(days_from_civil(2026, 12, 25))};
  ex[0].holiday_years = {2026};
  ex[1].id = "TSE";
  ex[1].city = "TOKYO";
  ex[1].tz = "Asia/Tokyo";
  ex[1].hub = 4;
  ex[1].pre_open = 8 * 60;
  ex[1].open = 9 * 60;
  ex[1].close = 15 * 60 + 30;
  ex[1].lunch_begin = 11 * 60 + 30;
  ex[1].lunch_end = 12 * 60 + 30;
  ex[1].holiday_years = {2026};
  return make_session_calendar(std::move(ex), 13);
}
}  // namespace

TEST_CASE("sessions: states across DST, lunch, weekend and holiday") {
  const SessionCalendar cal = test_calendar();
  CHECK(cal.size() == 2);
  auto st = [&](std::size_t i, const char* t) { return cal.state(i, utc(t)).state; };
  // London, winter (GMT): 08:30 UTC = 08:30 local.
  CHECK(st(0, "2026-01-15T08:30:00Z") == SessionState::Open);
  CHECK(st(0, "2026-01-15T07:55:00Z") == SessionState::Pre);
  CHECK(st(0, "2026-01-15T07:00:00Z") == SessionState::Closed);
  CHECK(st(0, "2026-01-15T16:45:00Z") == SessionState::Closed);
  // DST starts 2026-03-29: Friday 07:30 UTC is 07:30 GMT (closed), Monday 07:30 UTC is 08:30 BST (open).
  CHECK(st(0, "2026-03-27T07:30:00Z") == SessionState::Closed);
  CHECK(st(0, "2026-03-30T07:30:00Z") == SessionState::Open);
  CHECK(cal.utc_offset_s(0, utc("2026-03-30T07:30:00Z")) == 3600);
  CHECK(cal.utc_offset_s(0, utc("2026-03-27T07:30:00Z")) == 0);
  // Saturday and a configured holiday (a Friday).
  CHECK(st(0, "2026-07-04T10:00:00Z") == SessionState::Closed);
  CHECK(st(0, "2026-12-25T10:00:00Z") == SessionState::Holiday);
  // Tokyo lunch: 03:00 UTC = 12:00 JST.
  CHECK(st(1, "2026-07-01T03:00:00Z") == SessionState::Lunch);
  CHECK(st(1, "2026-07-01T01:00:00Z") == SessionState::Open);
  CHECK(st(1, "2026-07-01T06:45:00Z") == SessionState::Closed);
  // Next change and the chip text (city and state padded into HUD columns).
  const SessionStatus s = cal.state(0, utc("2026-01-15T14:16:00Z"));
  CHECK(s.state == SessionState::Open);
  CHECK(s.next_state == SessionState::Closed);
  CHECK(s.next_change_ms == utc("2026-01-15T16:30:00Z"));
  char chip[64];
  format_session_chip(cal.exchange(0), s, utc("2026-01-15T14:16:00Z"), chip, sizeof(chip));
  if (std::string(chip) != "LONDON    OPEN    02:14 to close") std::fprintf(stderr, "  chip [%s]\n", chip);
  CHECK(std::string(chip) == "LONDON    OPEN    02:14 to close");
  // Hub mapping and modeled intensity.
  CHECK(cal.exchange_of_hub(1) == 0);
  CHECK(cal.exchange_of_hub(0) == -1);
  CHECK(cal.hub_factor(0, utc("2026-01-15T14:16:00Z")) == 1.0);
  CHECK(cal.hub_factor(1, utc("2026-01-15T14:16:00Z")) == cal.intensity[static_cast<std::size_t>(SessionState::Open)]);
}

namespace {
std::vector<EndpointConfig> three_endpoints() {
  std::vector<EndpointConfig> eps(3);
  for (int i = 0; i < 3; ++i) {
    eps[static_cast<std::size_t>(i)].url = "wss://e" + std::to_string(i) + ".example";
    eps[static_cast<std::size_t>(i)].max_rps = 2.0;
    eps[static_cast<std::size_t>(i)].burst = 2.0;
  }
  return eps;
}
}  // namespace

TEST_CASE("endpoint pool: token bucket holds the published limit; an empty bucket never rotates") {
  EndpointPool pool(three_endpoints());
  CHECK(pool.try_acquire(0));
  CHECK(pool.try_acquire(0));
  CHECK(!pool.try_acquire(0));   // at the limit: skipped ...
  CHECK(pool.active() == 0);     // ... and not spread to another endpoint
  CHECK(!pool.try_acquire(400)); // 0.8 tokens
  CHECK(pool.try_acquire(500));  // 1.0 token at 2 req/s
  // Over 10 s at most burst + 10 s x 2 req/s requests pass, all on endpoint 0.
  int passed = 0;
  for (std::int64_t t = 1000; t <= 11000; t += 10) passed += pool.try_acquire(t) ? 1 : 0;
  CHECK(passed <= 2 + 20 + 1);
  CHECK(passed >= 19);
  CHECK(pool.active() == 0);
  CHECK(pool.rotations() == 0);
}

TEST_CASE("endpoint pool: 429 and errors rotate with cooldowns and a history") {
  EndpointPool pool(three_endpoints(), 30000, 120000, 900000);
  CHECK(pool.on_rate_limited(1000) == 1);
  CHECK(!pool.healthy(0, 1000));
  CHECK(pool.health(0).rate_limited == 1);
  CHECK(pool.on_error(2000) == 2);
  CHECK(pool.on_error(3000) == 1);  // all cooling: endpoint 1 (32 s) ends before 0 (121 s) and 2 (33 s)
  CHECK(pool.wait_ms(3000) > 0);
  CHECK(pool.wait_ms(40000) == 0);
  CHECK(pool.rotations() == 3);
  CHECK(pool.history_count() == 4);
  CHECK(pool.history(1).reason == RotationReason::RateLimited);
  CHECK(pool.history(1).from == 0 && pool.history(1).to == 1);
  CHECK(pool.history(2).reason == RotationReason::Error);
  pool.on_success(40000, 120.0);
  CHECK(pool.health(1).latency_ewma_ms > 0.0);
  CHECK(pool.score(1) < pool.score(0));
}

namespace {
const HubName kHubs[] = {{"NEW_YORK", "New York", false}, {"LONDON", "London", false},
                         {"FRANKFURT", "Frankfurt", false}, {"ZURICH", "Zurich", false},
                         {"TOKYO", "Tokyo", false}, {"SINGAPORE", "Singapore", false},
                         {"HONG_KONG", "Hong Kong", false}, {"BLOCKCHAIN_ETH", "Ethereum", true},
                         {"BLOCKCHAIN_BTC", "Bitcoin", true}, {"BLOCKCHAIN_SOL", "Solana", true}};
}  // namespace

TEST_CASE("command bar: hubs, chains, filters, find, at most 6 results") {
  CommandBar bar;
  bar.set_hubs(kHubs, sizeof(kHubs) / sizeof(kHubs[0]));
  bar.open();
  CHECK(bar.is_open());
  bar.set_text("lond");
  CHECK(bar.result_count() >= 1);
  CHECK(bar.result(0).kind == CommandKind::FlyHub && bar.result(0).hub == 1);
  bar.set_text("eth");
  CHECK(bar.result_count() >= 1 && bar.result(0).kind == CommandKind::FlyChain && bar.result(0).hub == 7);
  bar.set_text("o");
  CHECK(bar.result_count() <= CommandBar::kMaxResults);
  bar.set_text(">10m");
  CHECK(bar.result_count() == 1 && bar.result(0).kind == CommandKind::FilterMinUsd);
  CHECK(bar.result(0).value == 1e7);
  CommandResult r = bar.accept();
  CHECK(r.kind == CommandKind::FilterMinUsd);
  CHECK(!bar.is_open());
  CHECK(bar.filter().min_usd == 1e7);
  bar.open();
  bar.set_text("whale:t2");
  bar.accept();
  CHECK(bar.filter().min_tier == 2);
  bar.open();
  bar.set_text("venue:okx");
  r = bar.accept();
  CHECK(r.kind == CommandKind::FilterVenue);
  CHECK(bar.filter().feed_mask == (1u << static_cast<unsigned>(FeedId::Okx)));
  CHECK(bar.filter().pass(2e7, 3, FeedId::Okx, false));
  CHECK(!bar.filter().pass(2e7, 3, FeedId::Coinbase, false));
  CHECK(!bar.filter().pass(5e6, 3, FeedId::Okx, false));
  CHECK(!bar.filter().pass(2e7, 1, FeedId::Okx, false));
  char chip[32];
  bar.filter().chip(FilterKind::MinUsd, chip, sizeof(chip));
  CHECK(std::string(chip) == ">10M");
  bar.filter().clear(FilterKind::MinUsd);
  CHECK(!bar.filter().active(FilterKind::MinUsd));
  CHECK(bar.filter().active(FilterKind::Tier));
  bar.open();
  bar.set_text("addr:0xAbC123");
  CHECK(bar.result_count() == 1 && bar.result(0).kind == CommandKind::FindAddress);
  CHECK(std::string(bar.result(0).arg) == "0xAbC123");
  bar.set_text("tx:5Kd9");
  CHECK(bar.result_count() == 1 && bar.result(0).kind == CommandKind::FindTx);
  bar.set_text(">12q");
  CHECK(bar.result_count() == 0);
  double usd = 0.0;
  CHECK(parse_usd_amount("250k", usd) && usd == 250000.0);
  CHECK(parse_usd_amount("1.5b", usd) && usd == 1.5e9);
  CHECK(!parse_usd_amount("k", usd));
  CHECK(fuzzy_score("ny", "New York") > fuzzy_score("ny", "Sydney"));
  CHECK(fuzzy_score("zz", "London") < 0);
}

namespace {
CableSet atlantic_cable() {
  CableSet c;
  // New York area -> mid-Atlantic -> Cornwall, then a second, unconnected cable in the Pacific.
  const float pts[] = {40.40f, -73.50f, 41.0f, -60.0f, 45.0f, -40.0f, 49.0f, -20.0f, 50.10f, -5.50f,
                       21.3f, -157.9f, 30.0f, -140.0f, 34.0f, -120.5f};
  c.latlon.assign(std::begin(pts), std::end(pts));
  c.cables.push_back({0, 5, CableCorridor::TransAtlantic});
  c.cables.push_back({5, 3, CableCorridor::TransPacific});
  c.landings = {40.40f, -73.50f, 50.10f, -5.50f};
  return c;
}
}  // namespace

TEST_CASE("cables: binary round trip and MODELED Dijkstra routes with a great-circle fallback") {
  const CableSet c = atlantic_cable();
  CHECK(c.segment_count() == 6);
  const std::vector<std::uint8_t> bin = serialize_cables(c);
  CableSet back;
  std::string err;
  CHECK(parse_cables(bin.data(), bin.size(), back, err));
  CHECK(back.cables.size() == 2 && back.latlon == c.latlon && back.landings == c.landings);
  CHECK(back.cables[0].corridor == CableCorridor::TransAtlantic);
  CHECK(!parse_cables(bin.data(), bin.size() - 3, back, err));

  // Hubs: New York, London, Tokyo, and a chain cloud without geography.
  std::vector<HubSite> hubs = {{40.71, -74.01, true}, {51.51, -0.13, true}, {35.68, 139.69, true}, {0, 0, false}};
  const RouteTable rt = build_routes(c, hubs);
  CHECK(rt.hub_count == 4);
  const std::size_t ny_ld = 0 * 4 + 1, ld_ny = 1 * 4 + 0, ny_tk = 0 * 4 + 2;
  std::fprintf(stderr, "  routes: ny-ld %u, ny-tk %u, routed %zu\n", rt.count[ny_ld], rt.count[ny_tk], rt.routed_pairs);
  CHECK(rt.count[ny_ld] >= 5);
  CHECK(rt.count[ld_ny] == rt.count[ny_ld]);
  CHECK(rt.count[ny_tk] == 0);  // no cable path: great circle
  CHECK(rt.count[0 * 4 + 3] == 0);
  const double gc = great_circle_km(40.71, -74.01, 51.51, -0.13);
  CHECK(rt.length_km[ny_ld] > gc);
  CHECK(rt.length_km[ny_ld] < 1.6 * gc);
  // Starts at New York, ends at London, w runs 0 .. 1 monotonically.
  const RoutePoint* p = &rt.points[rt.first[ny_ld]];
  const std::uint32_t n = rt.count[ny_ld];
  CHECK(p[0].t == 0.0f && std::fabs(p[n - 1].t - 1.0f) < 1e-6f);
  for (std::uint32_t i = 1; i < n; ++i) CHECK(p[i].t >= p[i - 1].t);
  const double la = 40.71 * 3.14159265358979 / 180.0, lo = -74.01 * 3.14159265358979 / 180.0;
  CHECK(std::fabs(p[0].x - std::cos(la) * std::sin(lo)) < 1e-4);
  CHECK(std::fabs(p[0].y - std::sin(la)) < 1e-4);
  CHECK(std::fabs(great_circle_km(0, 0, 0, 1) - 111.195) < 0.01);
}

TEST_CASE("city lights: BC4 round trip, mip chain format and validation") {
  std::uint8_t px[16], enc[8], dec[16];
  for (int i = 0; i < 16; ++i) px[i] = static_cast<std::uint8_t>(10 + i * 13);
  encode_bc4_block(px, enc);
  decode_bc4_block(enc, dec);
  const int tol = (px[15] - px[0]) / 14 + 1;
  for (int i = 0; i < 16; ++i) CHECK(std::abs(static_cast<int>(dec[i]) - static_cast<int>(px[i])) <= tol);
  std::memset(px, 77, sizeof(px));
  encode_bc4_block(px, enc);
  decode_bc4_block(enc, dec);
  for (int i = 0; i < 16; ++i) CHECK(dec[i] == 77);

  CityLights cl;
  cl.width = 16;
  cl.height = 8;
  cl.levels = 2;  // 16x8, 8x4
  CHECK(CityLights::level_bytes(16, 8, 0) == 8 * 8);
  CHECK(CityLights::level_bytes(16, 8, 1) == 2 * 8);
  cl.bc4.assign(CityLights::level_bytes(16, 8, 0) + CityLights::level_bytes(16, 8, 1), 0);
  CHECK(cl.level_offset(1) == 64);
  const auto bin = serialize_city_lights(cl);
  CityLights back;
  std::string err;
  CHECK(parse_city_lights(bin.data(), bin.size(), back, err));
  CHECK(back.width == 16 && back.height == 8 && back.levels == 2 && back.bc4.size() == 80);
  CHECK(!parse_city_lights(bin.data(), bin.size() - 1, back, err));
  auto bad = bin;
  bad[16] = 3;  // 3 levels: 16x8 -> 4x2 is below a block
  CHECK(!parse_city_lights(bad.data(), bad.size(), back, err));
  bad = bin;
  bad[0] = 'X';
  CHECK(!parse_city_lights(bad.data(), bad.size(), back, err));
}
