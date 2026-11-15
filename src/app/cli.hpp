#pragma once
// Command-line options for the ftviz binary.

#include <cstddef>
#include <string>

#include "ingest/ingest.hpp"

namespace ftv {

struct CliOptions {
  std::string input;        // --input PATH (CSV or .ftxb)
  InputFormat format = InputFormat::Auto;
  std::string hubs_path;    // --hubs (default: config/hubs.json next to the binary)
  std::string render_path;  // --render (default: config/render.json next to the binary)
  std::string theme_path;   // --theme (default: config/theme.json next to the binary)
  std::string shader_dir;   // --shaders (default: shaders/ next to the binary)

  unsigned threads = 0;          // --threads: total layout/merge concurrency (0 = auto)
  unsigned parser_threads = 0;   // --parser-threads (0 = max(1, hw - 2))
  std::size_t chunk_mb = 16;     // --chunk-mb
  unsigned log_malformed = 8;    // --log-malformed K

  bool headless = false;          // --headless: ingest + layout benchmark, no window
  unsigned layout_iters = 0;      // --layout-iters N (headless; 0 = until converged, max 5000)
  unsigned prelayout = 0;         // --prelayout N: iterations before the window opens
  std::string screenshot;         // --screenshot out.png: render offscreen (no window), write, exit
  unsigned warmup_frames = 4;     // --warmup-frames N: frames rendered before the screenshot is taken
  unsigned frames = 0;            // --frames N: exit after N frames and print frame statistics

  int width = 0;                  // --width / --height override render.json
  int height = 0;
  int vsync = -1;                 // --vsync / --no-vsync
  int msaa = -1;                  // --msaa N
  bool gl_debug = true;           // --no-gl-debug disables the debug context
  bool alloc_assert = true;       // --no-alloc-assert: warn instead of abort (Debug builds)
  bool inter_detail = true;       // --no-detail starts with individual inter-hub edges off
  bool start_paused = false;      // --paused starts with the layout paused
  bool show_hud = true;           // --no-hud starts with the HUD hidden (toggle: H)
  int fly_hub = -1;               // --hub N: start focused on hub N (1-based, like the keys)
  long long select_node = -1;     // --select N: start with node N selected (1-hop highlight)
  std::string camera_preset;      // --camera NAME (globe, london, hub:<ID>, whale, graticule)
  float start_time = -1.0f;       // --time S: replay clock at start (< 0: preset or 0)
  float ui_scale = 0.0f;          // --ui-scale S (0: render.json)
  float cam_yaw_deg = 0.0f;       // --camera yaw,pitch,distance (degrees, degrees, world units)
  float cam_pitch_deg = -1000.0f; // < -900 => render.json default
  float cam_distance = 0.0f;      // 0 => render.json default
  bool live = false;               // --live: start the feed adapters (config/feeds.json)
  bool replay = false;             // --replay [FILE.ftxcap]: play a capture (default: the bundled one)
  std::string replay_path;
  std::string feeds_path;          // --feeds (default: config/feeds.json next to the binary)
  std::string export_dir;          // --export DIR: where exports are written (default: current directory)
  float export_after = -1.0f;      // --export-after S: export once after S seconds, then exit
  std::string pin_hub;             // --pin HUB_ID: start with that hub's panel pinned
  bool overlay = false;            // --overlay: screenshots include the live HUD layer (no timing text)
  bool help = false;
};

// Returns false with `error` set for invalid arguments.
bool parse_cli(int argc, char** argv, CliOptions& out, std::string& error);
void print_usage(const char* argv0);

}  // namespace ftv
