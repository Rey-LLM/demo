#pragma once
// The live layer of a run: feed configuration, the bundled or given replay
// capture, the feed adapters (--live), the model thread and the exporter.
// Shared by the interactive viewer and the screenshot mode.

#include <memory>
#include <string>

#include "export/exporter.hpp"
#include "ingest/live/live_runtime.hpp"
#include "model/capture.hpp"
#include "model/feed_config.hpp"
#include "model/hub_taxonomy.hpp"

namespace ftv {

class Renderer;

struct LiveSessionOptions {
  ModelMode mode = ModelMode::Dataset;
  std::string feeds_path;     // config/feeds.json
  std::string capture_path;   // --replay PATH (empty: feeds.json replay_capture)
  std::string base_dir;       // directory relative config paths resolve against
  std::string export_dir;     // --export DIR (empty: current directory)
};

class LiveSession {
 public:
  static std::unique_ptr<LiveSession> create(const LiveSessionOptions& opts, const HubTaxonomy& taxonomy,
                                             std::string& error);
  ~LiveSession();

  // Starts the adapters (Live mode), the model thread and the exporter.
  // Screenshot mode does not call this and steps the model itself.
  void start();
  void stop();

  [[nodiscard]] ModelMode mode() const noexcept { return opts_.mode; }
  ModelThread& model() noexcept { return *model_; }
  [[nodiscard]] const LiveFeeds* feeds() const noexcept { return feeds_.get(); }
  [[nodiscard]] const FeedsConfig& config() const noexcept { return config_; }
  [[nodiscard]] const Capture* capture() const noexcept { return capture_loaded_ ? &capture_ : nullptr; }
  [[nodiscard]] const std::string& capture_path() const noexcept { return capture_path_; }
  ExportService& exporter() noexcept { return *exporter_; }

  // Render thread: moves pending VisualEvents into the renderer's live arc
  // slots (round robin over the fixed slots; never allocates).
  void pump_visuals(Renderer& renderer, float liveClock) noexcept;

  // Prints one line per feed (state, label, counters) to stdout.
  void print_feed_report() const;

 private:
  LiveSession() = default;
  bool build_export(ExportInput& in);
  void finish_export();

  LiveSessionOptions opts_;
  const HubTaxonomy* taxonomy_ = nullptr;
  FeedsConfig config_;
  Capture capture_;
  bool capture_loaded_ = false;
  std::string capture_path_;
  std::unique_ptr<LiveFeeds> feeds_;
  std::unique_ptr<ModelThread> model_;
  std::unique_ptr<ExportService> exporter_;
  const LiveSnapshot* pinned_ = nullptr;  // exporter thread
  bool gate_held_ = false;
  std::uint32_t next_slot_ = 0;
  std::uint32_t seek_seen_ = 0;
};

}  // namespace ftv
