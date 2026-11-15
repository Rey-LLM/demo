// Export: golden files, CSV/JSON round trip, atomic writes, request coalescing.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/json.hpp"
#include "export/exporter.hpp"
#include "test_framework.hpp"

#ifndef FTVIZ_SOURCE_DIR
#define FTVIZ_SOURCE_DIR "."
#endif

using namespace ftv;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A small fixed session: three hubs (one id needs CSV quoting), flows under
// three labels, two whales, a feed that went OFFLINE -> REPLAY -> LIVE.
struct Session {
  std::unique_ptr<LiveSnapshot> snap = std::make_unique<LiveSnapshot>();
  std::vector<WhaleRecord> whales;
  std::vector<VolumeRecord> volumes;
  ExportInput in;
  Session() {
    const std::int64_t t0 = 1794717600000;  // 2026-11-15T04:40:00Z
    snap->hub_count = 3;
    snap->session_start_ms = t0;
    snap->now_ms = t0 + 12345;
    auto cell = [&](std::size_t s, std::size_t d, Asset a, SourceLabel l) -> FlowCell& {
      return snap->flows[LiveModel::flow_cell(3, s, d, a, l)];
    };
    cell(0, 1, Asset::BTC, SourceLabel::Live) = {1234567.891, 42};
    cell(0, 1, Asset::BTC, SourceLabel::Replay) = {1000.5, 3};
    cell(1, 0, Asset::ETH, SourceLabel::Delayed) = {0.125, 1};
    cell(0, 2, Asset::GBP, SourceLabel::Modeled) = {11284722.22, 0};
    cell(2, 2, Asset::SOL, SourceLabel::Live) = {99.999, 7};
    whales = {
        {t0 + 1500, 2600000.0, FeedId::Coinbase, Asset::BTC, SourceLabel::Live, 3, 0, 1},
        {t0 + 9001, 310000.55, FeedId::BtcMempool, Asset::BTC, SourceLabel::Replay, 1, 1, 1},
    };
    volumes = {
        {t0 / 1000, FeedId::BtcMempool, SourceLabel::Live, 0, 6, 12000.5},
        {t0 / 1000, FeedId::Fiat, SourceLabel::Modeled, 0, 0, 60895833.33},
        {t0 / 1000 + 1, FeedId::BtcMempool, SourceLabel::Replay, 0, 2, 100.0},
        {t0 / 1000 + 1, FeedId::Sol, SourceLabel::Delayed, 0, 960, 4321.0},
    };
    snap->whale_count = whales.size();
    snap->volume_count = volumes.size();
    in.version = "2.0.0";
    in.git_sha = "0123456789ab";
    in.mode = "live";
    in.capture = "assets/replay/default.ftxcap (SYNTHETIC)";
    in.start_ms = t0;
    in.end_ms = t0 + 12345;
    in.hub_ids = {"NEW_YORK", "LONDON", "OFF\"SHORE, X"};
    ExportFeed btc;
    btc.id = "BTC";
    btc.url = "wss://ws.blockchain.info/inv";
    btc.configured_label = SourceLabel::Live;
    btc.current_label = SourceLabel::Live;
    btc.state = "LIVE";
    btc.events = 77;
    btc.drops = 5;
    btc.usd_total = 12100.5;
    btc.history = {{t0, FeedState::Connecting, false}, {t0 + 400, FeedState::Offline, false},
                   {t0 + 15500, FeedState::Offline, true}, {t0 + 20000, FeedState::Live, false}};
    ExportFeed fiat;
    fiat.id = "FIAT";
    fiat.configured_label = fiat.current_label = SourceLabel::Modeled;
    fiat.state = "MODELED";
    fiat.usd_total = 60895833.33;
    in.feeds = {btc, fiat};
    Corridor c;
    c.currency = Asset::GBP;
    c.src_hub = 0;
    c.dst_hub = 1;
    c.usd_per_day = 9.75e11;
    c.share_pct = 13.0;
    c.source_url = "https://www.bis.org/publications/202210-commentary-otc-derivatives";
    c.as_of_date = "2022-04-30";
    in.corridors = {c};
    in.snapshot = snap.get();
    in.whales = whales.data();
    in.whale_count = whales.size();
    in.volumes = volumes.data();
    in.volume_count = volumes.size();
  }
};

// RFC 4180 reader (quotes, doubled quotes, CRLF).
std::vector<std::vector<std::string>> read_csv(const std::string& text, bool& crlfOnly) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  crlfOnly = true;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted) {
      if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
        field.push_back('"');
        ++i;
      } else if (c == '"') {
        quoted = false;
      } else {
        field.push_back(c);
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      row.push_back(std::move(field));
      field.clear();
    } else if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
      row.push_back(std::move(field));
      field.clear();
      rows.push_back(std::move(row));
      row.clear();
      ++i;
    } else if (c == '\n' || c == '\r') {
      crlfOnly = false;
    } else {
      field.push_back(c);
    }
  }
  if (!field.empty() || !row.empty()) crlfOnly = false;  // the last record must end with CRLF
  return rows;
}

fs::path temp_dir(const char* name) {
  const fs::path p = fs::temp_directory_path() / (std::string("ftviz_test_") + name);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

}  // namespace

TEST_CASE("export: ISO 8601 UTC timestamps") {
  CHECK_EQ(iso8601_ms(1794717612345), std::string("2026-11-15T04:40:12.345Z"));
  CHECK_EQ(iso8601_s(1794717612), std::string("2026-11-15T04:40:12Z"));
  CHECK_EQ(iso8601_basic(1794717612345), std::string("20261115T044012Z"));
  CHECK_EQ(iso8601_ms(0), std::string("1970-01-01T00:00:00.000Z"));
  CHECK_EQ(iso8601_s(951782400), std::string("2000-02-29T00:00:00Z"));
  CHECK_EQ(iso8601_ms(-1), std::string("1969-12-31T23:59:59.999Z"));
}

TEST_CASE("export: files match the golden files byte for byte") {
  Session s;
  const fs::path golden = fs::path(FTVIZ_SOURCE_DIR) / "tests" / "golden" / "export";
  const std::pair<const char*, std::string> files[] = {
      {"session.json", export_session_json(s.in)},     {"hub_flows.csv", export_hub_flows_csv(s.in)},
      {"hub_flows.json", export_hub_flows_json(s.in)}, {"whales.csv", export_whales_csv(s.in)},
      {"volume_tps.csv", export_volume_csv(s.in)},
  };
  for (const auto& [name, text] : files) {
    const std::string want = slurp(golden / name);
    if (want != text) {
      std::fprintf(stderr, "  golden mismatch: %s\n", name);
      // Left next to the temp dir for inspection.
      std::ofstream(fs::temp_directory_path() / (std::string("ftviz_actual_") + name), std::ios::binary) << text;
    }
    CHECK(!want.empty());
    CHECK(want == text);
  }
}

TEST_CASE("export: CSV round trip (RFC 4180, CRLF, quoting, fixed decimals)") {
  Session s;
  bool crlf = false;
  const auto flows = read_csv(export_hub_flows_csv(s.in), crlf);
  CHECK(crlf);
  REQUIRE(flows.size() == 6);
  CHECK_EQ(flows[0].size(), 6u);
  CHECK_EQ(flows[0][0], std::string("src_hub"));
  double total = 0.0;
  std::uint64_t tx = 0;
  bool sawQuoted = false, sawModeled = false, sawReplay = false, sawDelayed = false;
  for (std::size_t i = 1; i < flows.size(); ++i) {
    REQUIRE(flows[i].size() == 6);
    total += std::stod(flows[i][3]);
    tx += std::stoull(flows[i][4]);
    sawQuoted = sawQuoted || flows[i][1] == "OFF\"SHORE, X";
    sawModeled = sawModeled || flows[i][5] == "MODELED";
    sawReplay = sawReplay || flows[i][5] == "REPLAY";
    sawDelayed = sawDelayed || flows[i][5] == "DELAYED";
    CHECK(flows[i][3].find('e') == std::string::npos);  // fixed notation only
    CHECK(flows[i][3].size() > 3 && flows[i][3][flows[i][3].size() - 3] == '.');
  }
  CHECK(sawQuoted && sawModeled && sawReplay && sawDelayed);
  CHECK(std::fabs(total - (1234567.89 + 1000.50 + 0.13 + 11284722.22 + 100.00)) < 0.011);
  CHECK_EQ(tx, 53u);

  const auto whales = read_csv(export_whales_csv(s.in), crlf);
  CHECK(crlf);
  REQUIRE(whales.size() == 3);
  CHECK_EQ(whales[1][0], std::string("2026-11-15T04:40:01.500Z"));
  CHECK_EQ(whales[1][2], std::string("BTC"));
  CHECK_EQ(whales[1][3], std::string("2600000.00"));
  CHECK_EQ(whales[1][4], std::string("3"));
  CHECK_EQ(whales[1][5], std::string("NEW_YORK"));
  CHECK_EQ(whales[1][6], std::string("LONDON"));
  CHECK_EQ(whales[1][7], std::string("LIVE"));
  CHECK_EQ(whales[2][7], std::string("REPLAY"));

  const auto vol = read_csv(export_volume_csv(s.in), crlf);
  CHECK(crlf);
  REQUIRE(vol.size() == 5);
  CHECK_EQ(vol[1][0], std::string("2026-11-15T04:40:00Z"));
  CHECK_EQ(vol[1][1], std::string("BTC"));
  CHECK_EQ(vol[2][1], std::string("FIAT (MODELED)"));
  CHECK_EQ(vol[3][1], std::string("BTC (REPLAY)"));
  CHECK_EQ(vol[4][1], std::string("SOL (DELAYED)"));
  CHECK_EQ(vol[4][2], std::string("960"));
  CHECK_EQ(vol[4][3], std::string("4321.00"));
}

TEST_CASE("export: JSON files parse and carry the same figures") {
  Session s;
  std::string err;
  const auto session = parse_json(export_session_json(s.in), err);
  REQUIRE(session.has_value());
  CHECK_EQ(session->string_or("version", ""), std::string("2.0.0"));
  CHECK_EQ(session->string_or("git_sha", ""), std::string("0123456789ab"));
  CHECK_EQ(session->string_or("mode", ""), std::string("live"));
  CHECK_EQ(session->string_or("start_utc", ""), std::string("2026-11-15T04:40:00.000Z"));
  CHECK_EQ(session->string_or("end_utc", ""), std::string("2026-11-15T04:40:12.345Z"));
  const JsonValue* feeds = session->find("feeds");
  REQUIRE(feeds != nullptr);
  REQUIRE(feeds->items().size() == 2);
  const JsonValue& btc = feeds->items()[0];
  CHECK_EQ(btc.string_or("id", ""), std::string("BTC"));
  CHECK_EQ(btc.string_or("current_label", ""), std::string("LIVE"));
  CHECK_EQ(btc.number_or("drops", -1), 5.0);
  const JsonValue* hist = btc.find("state_history");
  REQUIRE(hist != nullptr);
  REQUIRE(hist->items().size() == 4);
  CHECK_EQ(hist->items()[2].string_or("state", ""), std::string("OFFLINE"));
  CHECK_EQ(hist->items()[2].string_or("label", ""), std::string("REPLAY"));
  CHECK_EQ(hist->items()[3].string_or("label", ""), std::string("LIVE"));
  CHECK_EQ(feeds->items()[1].string_or("current_label", ""), std::string("MODELED"));

  const auto flows = parse_json(export_hub_flows_json(s.in), err);
  REQUIRE(flows.has_value());
  const JsonValue* f = flows->find("flows");
  REQUIRE(f != nullptr);
  const JsonValue* ny = f->find("NEW_YORK");
  REQUIRE(ny != nullptr);
  const JsonValue* ln = ny->find("LONDON");
  REQUIRE(ln != nullptr);
  REQUIRE(ln->items().size() == 2);
  CHECK_EQ(ln->items()[0].number_or("usd_volume", 0), 1234567.89);
  CHECK_EQ(ln->items()[0].number_or("tx_count", 0), 42.0);
  CHECK_EQ(ln->items()[1].string_or("source_label", ""), std::string("REPLAY"));
  CHECK(ny->find("OFF\"SHORE, X") != nullptr);
}

TEST_CASE("export: atomic write leaves no temporary file and replaces whole files") {
  const fs::path dir = temp_dir("atomic");
  const std::string path = (dir / "a.csv").string();
  std::string err;
  REQUIRE(write_file_atomic(path, std::string(100000, 'x'), err));
  REQUIRE(write_file_atomic(path, "short\r\n", err));
  CHECK_EQ(slurp(path), std::string("short\r\n"));
  CHECK(!fs::exists(path + ".tmp"));
  // A reader polling during rewrites only ever sees one complete version.
  // (On Windows an open can fail while the file is being replaced; that is
  // not a torn read, so a failed open is skipped.)
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  const std::string a(50000, 'a'), b(70000, 'b');
  std::thread reader([&] {
    while (!stop.load()) {
      std::ifstream f(path, std::ios::binary);
      if (!f) continue;
      const std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (t != a && t != b && t != "short\r\n") bad.fetch_add(1);
    }
  });
  for (int i = 0; i < 200; ++i) CHECK(write_file_atomic(path, i % 2 ? a : b, err));
  stop = true;
  reader.join();
  CHECK_EQ(bad.load(), 0);
  // An unwritable target fails with a reason and creates nothing.
  CHECK(!write_file_atomic((dir / "missing" / "x.csv").string(), "x", err));
  CHECK(!err.empty());
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("export: write_export creates the directory with all five files") {
  Session s;
  const fs::path root = temp_dir("export");
  std::string out, err;
  REQUIRE(write_export(root.string(), s.in, out, err));
  CHECK_EQ(fs::path(out).filename().string(), std::string("ftviz_export_20261115T044012Z"));
  for (const char* n : {"session.json", "hub_flows.csv", "hub_flows.json", "whales.csv", "volume_tps.csv"}) {
    CHECK(fs::exists(fs::path(out) / n));
    CHECK(!fs::exists(fs::path(out) / (std::string(n) + ".tmp")));
  }
  CHECK_EQ(slurp(fs::path(out) / "whales.csv"), export_whales_csv(s.in));
  // A second export in the same second gets its own directory.
  std::string out2;
  REQUIRE(write_export(root.string(), s.in, out2, err));
  CHECK(out2 != out);
  // A root that cannot be created reports the failure.
  std::string out3;
  const fs::path file = root / "plain_file";
  std::ofstream(file) << "x";
  CHECK(!write_export((file / "sub").string(), s.in, out3, err));
  CHECK(!err.empty());
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("export: requests made during an export coalesce into one more; one runs at a time") {
  Session s;
  const fs::path root = temp_dir("coalesce");
  std::atomic<int> builds{0}, active{0}, maxActive{0}, finishes{0};
  {
    ExportService svc(
        root.string(),
        [&](ExportInput& in) {
          in = s.in;
          builds.fetch_add(1);
          const int a = active.fetch_add(1) + 1;
          int m = maxActive.load();
          while (a > m && !maxActive.compare_exchange_weak(m, a)) {
          }
          return true;
        },
        [&] {
          active.fetch_sub(1);
          finishes.fetch_add(1);
        });
    svc.set_test_delay_ms(150);
    svc.start();
    CHECK_EQ(svc.completed(), 0u);
    char text[512];
    bool ok = false;
    CHECK_EQ(svc.last_result(text, sizeof text, ok), 0u);

    const auto t0 = std::chrono::steady_clock::now();
    svc.request();
    while (!svc.busy() && svc.completed() == 0) std::this_thread::yield();
    for (int i = 0; i < 50; ++i) svc.request();  // all arrive while the first export runs
    const auto requestCost = std::chrono::steady_clock::now() - t0;
    while (svc.completed() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // a third export would show up here
    CHECK_EQ(svc.completed(), 2u);
    CHECK_EQ(builds.load(), 2);
    CHECK_EQ(finishes.load(), 2);
    CHECK_EQ(maxActive.load(), 1);
    CHECK(!svc.busy());
    // request() never waits for the export (150 ms each here).
    CHECK(std::chrono::duration<double>(requestCost).count() < 0.1);
    const std::uint64_t gen = svc.last_result(text, sizeof text, ok);
    CHECK_EQ(gen, 2u);
    CHECK(ok);
    CHECK(std::string(text).rfind("EXPORTED -> ", 0) == 0);
    CHECK(fs::exists(fs::path(std::string(text).substr(12)) / "session.json"));
    svc.stop();
  }
  // Failure is reported with a reason.
  {
    const fs::path file = root / "blocker";
    std::ofstream(file) << "x";
    ExportService svc((file / "nope").string(), [&](ExportInput& in) {
      in = s.in;
      return true;
    }, nullptr);
    svc.start();
    svc.request();
    while (svc.completed() < 1) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    char text[512];
    bool ok = true;
    svc.last_result(text, sizeof text, ok);
    CHECK(!ok);
    CHECK(std::string(text).rfind("EXPORT FAILED: ", 0) == 0);
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}
