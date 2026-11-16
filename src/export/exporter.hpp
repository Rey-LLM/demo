#pragma once
// Analytical export: one directory per export,
//   <dir>/ftviz_export_<UTC ISO 8601 basic>/
//     session.json    version, git SHA, start/end UTC, mode, feeds with state
//                     history and source labels, drop counters
//     hub_flows.csv   src_hub,dst_hub,currency,usd_volume,tx_count,source_label
//     hub_flows.json  the same matrix, nested by source and destination hub
//     whales.csv      ts_utc,chain,asset,usd_value,tier,src,dst,source_label
//     volume_tps.csv  ts_utc,feed,tx_count,usd_volume (1-second buckets)
//     tape.csv        the whale & liquidation tape (its 10,000-row window)
//     liquidations.csv the liquidation rows of that window
// session.json also records the search filters, the sun time used for the
// terminator, the trading-session states, the RPC endpoint pools with their
// published limits, and the endpoint rotation history.
// RFC 4180 CSV, UTF-8, ISO 8601 UTC timestamps, fixed decimals via
// std::to_chars. Every file is written to a temporary name and renamed into
// place, so a reader never sees a partial file.
//
// ExportService runs the export on its own thread. Rendering and ingest
// never wait on it: request() is a relaxed counter bump plus a notify, and
// requests made while an export runs are coalesced into one more export.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "ingest/live/feed_state.hpp"
#include "model/live_model.hpp"

namespace ftv {

struct ExportFeed {
  std::string id;
  std::string url;              // empty for the modeled feed
  SourceLabel configured_label = SourceLabel::Live;
  SourceLabel current_label = SourceLabel::Live;
  std::string state;            // CONNECTING, LIVE, ... or MODELED / REPLAY in replay mode
  std::uint64_t events = 0;
  std::uint64_t drops = 0;
  double usd_total = 0.0;
  std::vector<FeedTransition> history;
};

struct ExportSession {
  std::string id, city, tz, state, next_state, source, as_of;
  std::int64_t next_change_ms = 0;  // 0: unknown
};

struct ExportEndpoint {
  std::string url, rpc_url, limit, limit_source, as_of;
  double max_rps = 0.0;
  bool active = false;
};

struct ExportPool {
  std::string feed;
  std::vector<ExportEndpoint> endpoints;
};

struct ExportRotation {
  std::int64_t at_ms = 0;
  std::string feed;
  unsigned from = 0, to = 0;
  std::string reason;
};

// Search filters in effect when the export was requested.
struct ExportFilters {
  double min_usd = 0.0;
  unsigned min_tier = 0;
  std::vector<std::string> venues;  // feed ids
  bool liquidations_only = false;
};

struct ExportInput {
  std::string version;
  std::string git_sha;
  std::string mode;             // live | replay | dataset
  std::string capture;          // replay capture path and SYNTHETIC/RECORDED note (empty if none)
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::vector<std::string> hub_ids;
  std::vector<ExportFeed> feeds;
  std::vector<Corridor> corridors;
  const LiveSnapshot* snapshot = nullptr;
  const WhaleRecord* whales = nullptr;
  std::size_t whale_count = 0;
  const VolumeRecord* volumes = nullptr;
  std::size_t volume_count = 0;
  // v4
  std::vector<TapeEntry> tape;  // oldest first
  ExportFilters filters;
  std::int64_t sun_ms = 0;       // 0: not recorded
  bool sun_fixed = false;        // --sun-time
  double subsolar_lat = 0.0, subsolar_lon = 0.0;
  std::vector<ExportSession> sessions;
  std::vector<ExportPool> pools;
  std::vector<ExportRotation> rotations;
};

// File contents (pure; used by the writer and the golden-file tests).
std::string export_session_json(const ExportInput& in);
std::string export_hub_flows_csv(const ExportInput& in);
std::string export_hub_flows_json(const ExportInput& in);
std::string export_whales_csv(const ExportInput& in);
std::string export_volume_csv(const ExportInput& in);
std::string export_tape_csv(const ExportInput& in);
std::string export_liquidations_csv(const ExportInput& in);

// "2026-11-15T04:40:12.345Z" (ms) / "2026-11-15T04:40:12Z" (s) / "20261115T044012Z".
std::string iso8601_ms(std::int64_t ms);
std::string iso8601_s(std::int64_t seconds);
std::string iso8601_basic(std::int64_t ms);

// Writes `text` to `path` through `path`.tmp + rename.
bool write_file_atomic(const std::string& path, const std::string& text, std::string& error);

// Writes the seven files. On success `outDir` is the created directory.
bool write_export(const std::string& rootDir, const ExportInput& in, std::string& outDir, std::string& error);

class ExportService {
 public:
  // `build` fills an ExportInput (called on the exporter thread; it may pin
  // snapshots and journals). `finish` releases whatever `build` pinned.
  using BuildFn = std::function<bool(ExportInput&)>;
  using FinishFn = std::function<void()>;

  ExportService(std::string rootDir, BuildFn build, FinishFn finish);
  ~ExportService();
  ExportService(const ExportService&) = delete;
  ExportService& operator=(const ExportService&) = delete;

  void start();
  void stop();
  // Any thread, never blocks.
  void request() noexcept;

  // Result of the latest finished export, read without locking (seqlock).
  // Returns the generation (0: none yet); `text` gets "EXPORTED -> <path>"
  // or "EXPORT FAILED: <reason>".
  std::uint64_t last_result(char* text, std::size_t cap, bool& ok) const noexcept;
  [[nodiscard]] std::uint64_t completed() const noexcept { return completed_.load(std::memory_order_acquire); }
  [[nodiscard]] bool busy() const noexcept { return busy_.load(std::memory_order_acquire); }
  [[nodiscard]] const std::string& last_dir() const noexcept { return last_dir_; }  // exporter thread / after stop

  // Test hook: extra time spent inside each export.
  void set_test_delay_ms(int ms) noexcept { test_delay_ms_ = ms; }

 private:
  void run(std::stop_token st);
  void publish_result(bool ok, const std::string& text) noexcept;

  std::string root_;
  BuildFn build_;
  FinishFn finish_;
  std::jthread thread_;
  std::atomic<std::uint32_t> requests_{0};   // bumped per request; the thread waits on it
  std::atomic<bool> busy_{false};
  std::atomic<std::uint64_t> completed_{0};
  std::string last_dir_;
  int test_delay_ms_ = 0;

  // Seqlock-protected result text.
  mutable std::atomic<std::uint64_t> result_seq_{0};
  std::array<std::atomic<std::uint64_t>, 64> result_words_{};  // 512 bytes of text, relaxed word copies
  std::atomic<bool> result_ok_{false};
};

}  // namespace ftv
