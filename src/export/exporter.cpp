#include "export/exporter.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace ftv {

namespace fs = std::filesystem;

namespace {

void append_fixed(std::string& out, double v, int decimals) {
  char buf[64];
  if (!(v == v)) v = 0.0;  // NaN never reaches a file
  const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, decimals);
  out.append(buf, r.ptr);
}

void append_u64(std::string& out, std::uint64_t v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, r.ptr);
}

// RFC 4180 field: quoted when it holds a comma, quote or line break.
void append_csv(std::string& out, std::string_view s) {
  if (s.find_first_of(",\"\r\n") == std::string_view::npos) {
    out.append(s);
    return;
  }
  out.push_back('"');
  for (char c : s) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
}

void append_json_string(std::string& out, std::string_view s) {
  out.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += b;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
void civil(std::int64_t days, int& y, unsigned& m, unsigned& d) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
}

void split(std::int64_t ms, int& y, unsigned& mo, unsigned& d, unsigned& h, unsigned& mi, unsigned& s, unsigned& milli) {
  std::int64_t sec = ms >= 0 ? ms / 1000 : (ms - 999) / 1000;
  milli = static_cast<unsigned>(ms - sec * 1000);
  std::int64_t days = sec >= 0 ? sec / 86400 : (sec - 86399) / 86400;
  const auto rem = static_cast<unsigned>(sec - days * 86400);
  h = rem / 3600;
  mi = (rem / 60) % 60;
  s = rem % 60;
  civil(days, y, mo, d);
}

std::string_view feed_chain(FeedId f, Asset a) {
  switch (f) {
    case FeedId::Coinbase: return "COINBASE";
    case FeedId::Kraken: return "KRAKEN";
    case FeedId::Binance: return "BINANCE";
    case FeedId::Fiat: return "FIAT";
    default: return asset_name(a);
  }
}

const std::string& hub_id(const ExportInput& in, std::size_t h) {
  static const std::string unknown = "UNKNOWN";
  return h < in.hub_ids.size() ? in.hub_ids[h] : unknown;
}

}  // namespace

std::string iso8601_ms(std::int64_t ms) {
  int y;
  unsigned mo, d, h, mi, s, milli;
  split(ms, y, mo, d, h, mi, s, milli);
  char b[40];
  std::snprintf(b, sizeof(b), "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", y, mo, d, h, mi, s, milli);
  return b;
}

std::string iso8601_s(std::int64_t seconds) {
  int y;
  unsigned mo, d, h, mi, s, milli;
  split(seconds * 1000, y, mo, d, h, mi, s, milli);
  char b[32];
  std::snprintf(b, sizeof(b), "%04d-%02u-%02uT%02u:%02u:%02uZ", y, mo, d, h, mi, s);
  return b;
}

std::string iso8601_basic(std::int64_t ms) {
  int y;
  unsigned mo, d, h, mi, s, milli;
  split(ms, y, mo, d, h, mi, s, milli);
  char b[32];
  std::snprintf(b, sizeof(b), "%04d%02u%02uT%02u%02u%02uZ", y, mo, d, h, mi, s);
  return b;
}

std::string export_hub_flows_csv(const ExportInput& in) {
  std::string out = "src_hub,dst_hub,currency,usd_volume,tx_count,source_label\r\n";
  if (!in.snapshot) return out;
  const std::size_t H = in.snapshot->hub_count;
  for (std::size_t s = 0; s < H; ++s) {
    for (std::size_t d = 0; d < H; ++d) {
      for (std::size_t a = 0; a < kAssetCount; ++a) {
        for (std::size_t l = 0; l < kLabelCount; ++l) {
          const std::size_t idx = LiveModel::flow_cell(H, s, d, static_cast<Asset>(a), static_cast<SourceLabel>(l));
          if (idx >= in.snapshot->flows.size()) continue;
          const FlowCell& c = in.snapshot->flows[idx];
          if (c.usd == 0.0 && c.tx == 0) continue;
          append_csv(out, hub_id(in, s));
          out.push_back(',');
          append_csv(out, hub_id(in, d));
          out.push_back(',');
          out.append(asset_name(static_cast<Asset>(a)));
          out.push_back(',');
          append_fixed(out, c.usd, 2);
          out.push_back(',');
          append_u64(out, c.tx);
          out.push_back(',');
          out.append(label_name(static_cast<SourceLabel>(l)));
          out += "\r\n";
        }
      }
    }
  }
  return out;
}

std::string export_hub_flows_json(const ExportInput& in) {
  std::string out = "{\n  \"hubs\": [";
  for (std::size_t h = 0; h < in.hub_ids.size(); ++h) {
    if (h) out += ", ";
    append_json_string(out, in.hub_ids[h]);
  }
  out += "],\n  \"flows\": {";
  bool firstSrc = true;
  if (in.snapshot) {
    const std::size_t H = in.snapshot->hub_count;
    for (std::size_t s = 0; s < H; ++s) {
      bool firstDst = true;
      for (std::size_t d = 0; d < H; ++d) {
        bool firstCell = true;
        for (std::size_t a = 0; a < kAssetCount; ++a) {
          for (std::size_t l = 0; l < kLabelCount; ++l) {
            const std::size_t idx = LiveModel::flow_cell(H, s, d, static_cast<Asset>(a), static_cast<SourceLabel>(l));
            if (idx >= in.snapshot->flows.size()) continue;
            const FlowCell& c = in.snapshot->flows[idx];
            if (c.usd == 0.0 && c.tx == 0) continue;
            if (firstDst && firstCell) {
              out += firstSrc ? "\n    " : ",\n    ";
              append_json_string(out, hub_id(in, s));
              out += ": {";
              firstSrc = false;
            }
            if (firstCell) {
              out += firstDst ? "\n      " : ",\n      ";
              append_json_string(out, hub_id(in, d));
              out += ": [";
              firstDst = false;
            } else {
              out += ",";
            }
            out += "\n        {\"currency\": ";
            append_json_string(out, asset_name(static_cast<Asset>(a)));
            out += ", \"usd_volume\": ";
            append_fixed(out, c.usd, 2);
            out += ", \"tx_count\": ";
            append_u64(out, c.tx);
            out += ", \"source_label\": ";
            append_json_string(out, label_name(static_cast<SourceLabel>(l)));
            out += "}";
            firstCell = false;
          }
        }
        if (!firstCell) out += "\n      ]";
      }
      if (!firstDst) out += "\n    }";
    }
  }
  out += firstSrc ? "}\n}\n" : "\n  }\n}\n";
  return out;
}

std::string export_whales_csv(const ExportInput& in) {
  std::string out = "ts_utc,chain,asset,usd_value,tier,src,dst,source_label\r\n";
  for (std::size_t i = 0; i < in.whale_count; ++i) {
    const WhaleRecord& w = in.whales[i];
    out += iso8601_ms(w.ts_ms);
    out.push_back(',');
    out.append(feed_chain(w.feed, w.asset));
    out.push_back(',');
    out.append(asset_name(w.asset));
    out.push_back(',');
    append_fixed(out, w.usd_value, 2);
    out.push_back(',');
    append_u64(out, w.tier);
    out.push_back(',');
    append_csv(out, hub_id(in, w.src_hub));
    out.push_back(',');
    append_csv(out, hub_id(in, w.dst_hub));
    out.push_back(',');
    out.append(label_name(w.label));
    out += "\r\n";
  }
  return out;
}

std::string export_volume_csv(const ExportInput& in) {
  // One row per (second, feed) with activity. A feed whose label changed
  // within a second (e.g. LIVE -> REPLAY) contributes one row per label, so
  // the feed column carries the label: "BTC" for LIVE, "BTC (REPLAY)" etc.
  std::string out = "ts_utc,feed,tx_count,usd_volume\r\n";
  for (std::size_t i = 0; i < in.volume_count; ++i) {
    const VolumeRecord& v = in.volumes[i];
    out += iso8601_s(v.second);
    out.push_back(',');
    std::string feed(feed_name(v.feed));
    if (v.label != SourceLabel::Live) feed += std::string(" (") + std::string(label_name(v.label)) + ")";
    append_csv(out, feed);
    out.push_back(',');
    append_u64(out, v.tx_count);
    out.push_back(',');
    append_fixed(out, v.usd_volume, 2);
    out += "\r\n";
  }
  return out;
}

std::string export_session_json(const ExportInput& in) {
  std::string o = "{\n";
  o += "  \"format\": \"ftviz-export/1\",\n  \"version\": ";
  append_json_string(o, in.version);
  o += ",\n  \"git_sha\": ";
  append_json_string(o, in.git_sha);
  o += ",\n  \"start_utc\": ";
  append_json_string(o, iso8601_ms(in.start_ms));
  o += ",\n  \"end_utc\": ";
  append_json_string(o, iso8601_ms(in.end_ms));
  o += ",\n  \"mode\": ";
  append_json_string(o, in.mode);
  o += ",\n  \"replay_capture\": ";
  if (in.capture.empty()) {
    o += "null";
  } else {
    append_json_string(o, in.capture);
  }
  o += ",\n  \"labels\": {\n";
  o += "    \"LIVE\": \"received from a public endpoint during this session\",\n";
  o += "    \"DELAYED\": \"real data fetched deliberately behind the chain tip\",\n";
  o += "    \"MODELED\": \"derived from published aggregates, not observed\",\n";
  o += "    \"REPLAY\": \"played back from a capture file, not live\"\n  },\n";
  o += "  \"feeds\": [";
  for (std::size_t i = 0; i < in.feeds.size(); ++i) {
    const ExportFeed& f = in.feeds[i];
    o += i ? ",\n    {" : "\n    {";
    o += "\"id\": ";
    append_json_string(o, f.id);
    o += ", \"url\": ";
    if (f.url.empty()) {
      o += "null";
    } else {
      append_json_string(o, f.url);
    }
    o += ", \"configured_label\": ";
    append_json_string(o, label_name(f.configured_label));
    o += ", \"current_label\": ";
    append_json_string(o, label_name(f.current_label));
    o += ", \"state\": ";
    append_json_string(o, f.state);
    o += ", \"events\": ";
    append_u64(o, f.events);
    o += ", \"drops\": ";
    append_u64(o, f.drops);
    o += ", \"usd_total\": ";
    append_fixed(o, f.usd_total, 2);
    o += ",\n     \"state_history\": [";
    for (std::size_t k = 0; k < f.history.size(); ++k) {
      const FeedTransition& t = f.history[k];
      o += k ? ", {" : "{";
      o += "\"at_utc\": ";
      append_json_string(o, iso8601_ms(t.at_ms));
      o += ", \"state\": ";
      append_json_string(o, kFeedStateNames[static_cast<std::size_t>(t.state) % 5]);
      o += ", \"label\": ";
      append_json_string(o, t.replay ? "REPLAY" : label_name(f.configured_label));
      o += "}";
    }
    o += "]}";
  }
  o += "\n  ],\n  \"modeled_sources\": [";
  for (std::size_t i = 0; i < in.corridors.size(); ++i) {
    const Corridor& c = in.corridors[i];
    o += i ? ",\n    {" : "\n    {";
    o += "\"currency\": ";
    append_json_string(o, asset_name(c.currency));
    o += ", \"src_hub\": ";
    append_json_string(o, hub_id(in, c.src_hub));
    o += ", \"dst_hub\": ";
    append_json_string(o, hub_id(in, c.dst_hub));
    o += ", \"usd_per_day\": ";
    append_fixed(o, c.usd_per_day, 0);
    o += ", \"source_url\": ";
    append_json_string(o, c.source_url);
    o += ", \"as_of_date\": ";
    append_json_string(o, c.as_of_date);
    o += ", \"label\": \"MODELED\"}";
  }
  o += in.corridors.empty() ? "],\n" : "\n  ],\n";
  o += "  \"counters\": {\"whales\": ";
  append_u64(o, in.whale_count);
  o += ", \"volume_rows\": ";
  append_u64(o, in.volume_count);
  o += ", \"whale_journal_overflow\": ";
  append_u64(o, in.snapshot ? in.snapshot->whale_overflow : 0);
  o += ", \"volume_journal_overflow\": ";
  append_u64(o, in.snapshot ? in.snapshot->volume_overflow : 0);
  std::uint64_t drops = 0;
  for (const ExportFeed& f : in.feeds) drops += f.drops;
  o += ", \"drops_total\": ";
  append_u64(o, drops);
  o += "}\n}\n";
  return o;
}

bool write_file_atomic(const std::string& path, const std::string& text, std::string& error) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(text.data(), static_cast<std::streamsize>(text.size())) || !f.flush()) {
      error = "cannot write " + tmp;
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);  // atomic replace on POSIX; replaces an existing file on Windows too
#if defined(_WIN32)
  // Windows refuses to replace a file that another process (a spreadsheet,
  // a viewer polling the export) has open without FILE_SHARE_DELETE. Such
  // readers come and go, so retry for up to ~2 s before giving up.
  for (int attempt = 0; ec && attempt < 400; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ec.clear();
    fs::rename(tmp, path, ec);
  }
#endif
  if (ec) {
    error = "cannot rename " + tmp + ": " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool write_export(const std::string& rootDir, const ExportInput& in, std::string& outDir, std::string& error) {
  std::error_code ec;
  fs::create_directories(rootDir, ec);
  if (ec) {
    error = "cannot create " + rootDir + ": " + ec.message();
    return false;
  }
  const std::string base = (fs::path(rootDir) / ("ftviz_export_" + iso8601_basic(in.end_ms))).string();
  std::string dir = base;
  for (int k = 2; fs::exists(dir, ec) && k < 1000; ++k) dir = base + "_" + std::to_string(k);
  if (!fs::create_directory(dir, ec) || ec) {
    error = "cannot create " + dir;
    return false;
  }
  const std::pair<const char*, std::string> files[] = {
      {"hub_flows.csv", export_hub_flows_csv(in)},
      {"hub_flows.json", export_hub_flows_json(in)},
      {"whales.csv", export_whales_csv(in)},
      {"volume_tps.csv", export_volume_csv(in)},
      {"session.json", export_session_json(in)},  // last: its presence marks a complete export
  };
  for (const auto& [name, text] : files) {
    if (!write_file_atomic((fs::path(dir) / name).string(), text, error)) return false;
  }
  outDir = dir;
  return true;
}

// ---------------------------------------------------------------------------
// ExportService
// ---------------------------------------------------------------------------

ExportService::ExportService(std::string rootDir, BuildFn build, FinishFn finish)
    : root_(std::move(rootDir)), build_(std::move(build)), finish_(std::move(finish)) {}

ExportService::~ExportService() { stop(); }

void ExportService::start() {
  if (thread_.joinable()) return;
  thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void ExportService::stop() {
  if (!thread_.joinable()) return;
  thread_.request_stop();
  requests_.fetch_add(1, std::memory_order_release);
  requests_.notify_one();
  thread_.join();
}

void ExportService::request() noexcept {
  requests_.fetch_add(1, std::memory_order_release);
  requests_.notify_one();
}

void ExportService::publish_result(bool ok, const std::string& text) noexcept {
  const std::uint64_t s = result_seq_.load(std::memory_order_relaxed);
  result_seq_.store(s + 1, std::memory_order_relaxed);  // odd: write in progress
  std::atomic_thread_fence(std::memory_order_release);
  char buf[512] = {};
  std::memcpy(buf, text.data(), std::min(text.size(), sizeof(buf) - 1));
  for (std::size_t w = 0; w < result_words_.size(); ++w) {
    std::uint64_t v = 0;
    std::memcpy(&v, buf + w * 8, 8);
    result_words_[w].store(v, std::memory_order_relaxed);
  }
  result_ok_.store(ok, std::memory_order_relaxed);
  result_seq_.store(s + 2, std::memory_order_release);
}

std::uint64_t ExportService::last_result(char* text, std::size_t cap, bool& ok) const noexcept {
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::uint64_t s0 = result_seq_.load(std::memory_order_acquire);
    if (s0 & 1u) continue;
    char buf[512];
    for (std::size_t w = 0; w < result_words_.size(); ++w) {
      const std::uint64_t v = result_words_[w].load(std::memory_order_relaxed);
      std::memcpy(buf + w * 8, &v, 8);
    }
    const bool okv = result_ok_.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (result_seq_.load(std::memory_order_relaxed) != s0) continue;
    buf[sizeof(buf) - 1] = 0;
    if (cap) {
      const std::size_t n = std::min(cap - 1, std::strlen(buf));
      std::memcpy(text, buf, n);
      text[n] = 0;
    }
    ok = okv;
    return s0 / 2;
  }
  if (cap) text[0] = 0;
  ok = false;
  return 0;
}

void ExportService::run(std::stop_token st) {
  // Starts from zero, not from the current count: a request made before this
  // thread first ran must still be served.
  std::uint32_t seen = 0;
  while (!st.stop_requested()) {
    requests_.wait(seen, std::memory_order_acquire);
    if (st.stop_requested()) break;
    // Every request up to now is served by this one export; requests that
    // arrive while it runs bump the counter again and cause exactly one more.
    seen = requests_.load(std::memory_order_acquire);
    busy_.store(true, std::memory_order_release);
    ExportInput in;
    std::string dir, err;
    bool ok = false;
    if (build_(in)) {
      if (test_delay_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(test_delay_ms_));
      ok = write_export(root_, in, dir, err);
    } else {
      err = "no model snapshot yet";
    }
    if (finish_) finish_();
    if (ok) last_dir_ = dir;
    publish_result(ok, ok ? "EXPORTED -> " + dir : "EXPORT FAILED: " + err);
    completed_.fetch_add(1, std::memory_order_acq_rel);
    busy_.store(false, std::memory_order_release);
  }
}

}  // namespace ftv
