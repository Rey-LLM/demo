#include "app/cli.hpp"

#include "model/solar.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <system_error>

namespace ftv {

namespace {

template <class T>
bool parse_number(std::string_view s, T& out) {
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool parse_float(std::string_view s, float& out) {
  // std::from_chars for floating point is available in all supported
  // toolchains (GCC 12, Clang 15 + libstdc++, MSVC 2022).
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

}  // namespace

void print_usage(const char* argv0) {
  std::printf(
      "Usage: %s [--input <transactions.csv|.ftxb>] [options]\n"
      "\n"
      "With no options ftviz opens the globe and connects to every public feed in the\n"
      "embedded config/feeds.json. Shaders, config, coastlines and the replay capture are\n"
      "compiled into the binary; the options below only override them.\n"
      "\n"
      "Input / configuration\n"
      "  --input PATH           CSV with header or packed .ftxb file drawn as an entity graph\n"
      "                         (optional; alias --dataset, or pass the path as the only argument)\n"
      "  --format auto|csv|ftxb input format (default: auto-detect)\n"
      "  --data-dir DIR         read shaders/, config/ and assets/ from DIR instead of the binary\n"
      "  --no-user-config       ignore config overrides in %%APPDATA%%\\ftviz (Windows) or ~/.config/ftviz\n"
      "                         (order: embedded < user config dir < --data-dir < explicit option)\n"
      "  --hubs PATH            hub taxonomy (default: embedded config/hubs.json)\n"
      "  --render PATH          render settings (default: embedded config/render.json)\n"
      "  --theme PATH           palette and styling (default: embedded config/theme.json)\n"
      "  --shaders DIR          GLSL directory (default: embedded shaders/)\n"
      "\n"
      "Threads / ingest\n"
      "  --threads N            layout/merge threads incl. the coordinator (default: all cores headless,\n"
      "                         all cores but one interactive, so the render thread keeps a core)\n"
      "  --parser-threads N     T_ingest workers (default: max(1, hw-2))\n"
      "  --chunk-mb N           ingest chunk size in MB (default 16)\n"
      "  --log-malformed K      print the first K malformed row offsets (default 8, max 16)\n"
      "\n"
      "Live data\n"
      "  --live                 connect to the public feeds (the default); when no feed has been\n"
      "                         LIVE for 15 s (machine offline) the bundled capture plays as REPLAY\n"
      "  --no-live              no feed connections: only the MODELED fiat corridors (and --input)\n"
      "  --replay [FILE.ftxcap] play a capture on its timeline (default: the embedded synthetic one)\n"
      "  --feeds PATH           feed configuration (default: embedded config/feeds.json)\n"
      "  --export DIR           directory for exports (E key / EXPORT button; default: current directory)\n"
      "  --export-after S       write one export S seconds after start, then exit\n"
      "  --pin HUB_ID           start with that hub's panel pinned\n"
      "  --sun-time ISO8601     fix the day/night terminator at this UTC time (default: model clock)\n"
      "\n"
      "Modes\n"
      "  --headless             ingest + layout benchmark without a window\n"
      "  --layout-iters N       headless: run N iterations (default: until converged, max 5000)\n"
      "  --prelayout N          run N layout iterations before opening the window\n"
      "  --screenshot FILE.png  render offscreen (no window), write a PNG and exit\n"
      "  --warmup-frames N      screenshot: frames rendered before the capture (default 4)\n"
      "  --frames N             exit after N frames and print frame-time statistics\n"
      "\n"
      "Window / view\n"
      "  --width W --height H   window size (default from render.json)\n"
      "  --vsync | --no-vsync   override render.json\n"
      "  --msaa N               MSAA samples (1 = off)\n"
      "  --hub N                start focused on hub N (1-based)\n"
      "  --select NODE          start with node NODE (dense index) selected and framed\n"
      "  --camera PRESET        globe | london | hub:<ID> | whale | graticule\n"
      "  --camera Y,P,D         start camera yaw/pitch in degrees and distance\n"
      "  --time S               replay clock at start, seconds (default: preset, else 0)\n"
      "  --ui-scale S           device pixels per logical pixel for discs, rings, labels\n"
      "  --no-detail            start with individual inter-hub edges hidden (toggle: D)\n"
      "  --overlay              screenshot: draw the live HUD layer (feeds, hub panel, timeline)\n"
      "  --paused               start with the layout paused (toggle: L)\n"
      "  --no-hud               start with the HUD hidden (toggle: H)\n"
      "  --no-gl-debug          do not request a debug context\n"
      "  --no-alloc-assert      Debug builds: warn instead of abort on per-frame allocations\n"
      "\n"
      "Controls: left-drag orbit, right/middle-drag pan, wheel zoom, click a hub to pin its panel,\n"
      "  click a node to select it, 1-5 view presets (globe, Americas, Europe, Asia, chain shell),\n"
      "  E export, Space pause/resume (ingest, layout, clocks), Left/Right seek 1 s (Shift: 10 s),\n"
      "  drag the timeline to scrub, F frame selection, D inter-hub edge detail, L layout,\n"
      "  R reheat layout, H toggle HUD, Esc clear pin / selection, then quit.\n",
      argv0);
}

bool parse_cli(int argc, char** argv, CliOptions& o, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string_view& v) -> bool {
      if (i + 1 >= argc) {
        error = std::string("missing value for ") + std::string(a);
        return false;
      }
      v = argv[++i];
      return true;
    };
    auto uns = [&](unsigned& dst) -> bool {
      std::string_view v;
      if (!value(v)) return false;
      if (!parse_number(v, dst)) {
        error = std::string("invalid number for ") + std::string(a) + ": " + std::string(v);
        return false;
      }
      return true;
    };
    auto sint = [&](int& dst) -> bool {
      std::string_view v;
      if (!value(v)) return false;
      if (!parse_number(v, dst)) {
        error = std::string("invalid number for ") + std::string(a) + ": " + std::string(v);
        return false;
      }
      return true;
    };
    auto str = [&](std::string& dst) -> bool {
      std::string_view v;
      if (!value(v)) return false;
      dst.assign(v);
      return true;
    };

    if (a == "-h" || a == "--help") {
      o.help = true;
    } else if (a == "--input" || a == "-i" || a == "--dataset") {
      if (!str(o.input)) return false;
    } else if (a == "--format") {
      std::string_view v;
      if (!value(v)) return false;
      if (v == "auto") {
        o.format = InputFormat::Auto;
      } else if (v == "csv") {
        o.format = InputFormat::Csv;
      } else if (v == "ftxb") {
        o.format = InputFormat::Ftxb;
      } else {
        error = "unknown --format (expected auto, csv or ftxb)";
        return false;
      }
    } else if (a == "--data-dir") {
      if (!str(o.data_dir)) return false;
    } else if (a == "--hubs") {
      if (!str(o.hubs_path)) return false;
    } else if (a == "--render") {
      if (!str(o.render_path)) return false;
    } else if (a == "--theme") {
      if (!str(o.theme_path)) return false;
    } else if (a == "--warmup-frames") {
      if (!uns(o.warmup_frames)) return false;
    } else if (a == "--time" || a == "--ui-scale") {
      std::string_view v;
      if (!value(v)) return false;
      float f = 0.0f;
      if (!parse_float(v, f) || !(f >= 0.0f) || (a == "--ui-scale" && (f < 0.5f || f > 4.0f))) {
        error = std::string("invalid value for ") + std::string(a);
        return false;
      }
      (a == "--time" ? o.start_time : o.ui_scale) = f;
    } else if (a == "--shaders") {
      if (!str(o.shader_dir)) return false;
    } else if (a == "--threads") {
      if (!uns(o.threads)) return false;
    } else if (a == "--parser-threads") {
      if (!uns(o.parser_threads)) return false;
    } else if (a == "--chunk-mb") {
      unsigned mb = 0;
      if (!uns(mb)) return false;
      if (mb < 1 || mb > 1024) {
        error = "--chunk-mb must be in [1, 1024]";
        return false;
      }
      o.chunk_mb = mb;
    } else if (a == "--log-malformed") {
      if (!uns(o.log_malformed)) return false;
      if (o.log_malformed > 16) o.log_malformed = 16;
    } else if (a == "--headless") {
      o.headless = true;
    } else if (a == "--layout-iters") {
      if (!uns(o.layout_iters)) return false;
    } else if (a == "--prelayout") {
      if (!uns(o.prelayout)) return false;
    } else if (a == "--screenshot") {
      if (!str(o.screenshot)) return false;
    } else if (a == "--frames") {
      if (!uns(o.frames)) return false;
    } else if (a == "--width") {
      if (!sint(o.width)) return false;
    } else if (a == "--height") {
      if (!sint(o.height)) return false;
    } else if (a == "--vsync") {
      o.vsync = 1;
    } else if (a == "--no-vsync") {
      o.vsync = 0;
    } else if (a == "--msaa") {
      if (!sint(o.msaa)) return false;
    } else if (a == "--hub") {
      if (!sint(o.fly_hub)) return false;
    } else if (a == "--select") {
      std::string_view v;
      if (!value(v)) return false;
      if (!parse_number(v, o.select_node) || o.select_node < 0) {
        error = "invalid node index for --select";
        return false;
      }
    } else if (a == "--camera") {
      std::string_view v;
      if (!value(v)) return false;
      if (v.find(',') == std::string_view::npos) {
        o.camera_preset.assign(v);
        continue;
      }
      float vals[3] = {0, 0, 0};
      std::size_t start = 0;
      for (int k = 0; k < 3; ++k) {
        const std::size_t comma = k < 2 ? v.find(',', start) : v.size();
        if (comma == std::string_view::npos || !parse_float(v.substr(start, comma - start), vals[k])) {
          error = "--camera expects yaw,pitch,distance";
          return false;
        }
        start = comma + 1;
      }
      o.cam_yaw_deg = vals[0];
      o.cam_pitch_deg = vals[1];
      o.cam_distance = vals[2];
    } else if (a == "--live") {
      o.live = true;
      o.live_explicit = true;
    } else if (a == "--no-live") {
      o.live = false;
    } else if (a == "--replay") {
      o.replay = true;
      if (i + 1 < argc && argv[i + 1][0] != '-' && std::string_view(argv[i + 1]).ends_with(".ftxcap")) {
        o.replay_path = argv[++i];
      }
    } else if (a == "--feeds") {
      if (!str(o.feeds_path)) return false;
    } else if (a == "--export") {
      if (!str(o.export_dir)) return false;
    } else if (a == "--export-after") {
      std::string_view v;
      if (!value(v)) return false;
      if (!parse_float(v, o.export_after) || !(o.export_after >= 0.0f)) {
        error = "invalid value for --export-after";
        return false;
      }
    } else if (a == "--pin") {
      if (!str(o.pin_hub)) return false;
    } else if (a == "--no-user-config") {
      o.user_config = false;
    } else if (a == "--sun-time") {
      std::string_view v;
      if (!value(v)) return false;
      const auto ms = parse_iso8601_utc(v);
      if (!ms) {
        error = "invalid value for --sun-time (expected UTC ISO 8601, e.g. 2026-06-21T12:00:00Z)";
        return false;
      }
      o.sun_time_ms = *ms;
      o.sun_time_set = true;
    } else if (a == "--overlay") {
      o.overlay = true;
    } else if (a == "--no-detail") {
      o.inter_detail = false;
    } else if (a == "--paused") {
      o.start_paused = true;
    } else if (a == "--no-hud") {
      o.show_hud = false;
    } else if (a == "--no-gl-debug") {
      o.gl_debug = false;
    } else if (a == "--no-alloc-assert") {
      o.alloc_assert = false;
    } else if (!a.empty() && a[0] != '-' && o.input.empty()) {
      o.input.assign(a);  // positional input path
    } else {
      error = std::string("unknown option: ") + std::string(a);
      return false;
    }
  }
  if (o.headless && o.input.empty()) {
    error = "--headless needs --input";
    return false;
  }
  if (o.live_explicit && o.replay) {
    error = "--live and --replay are mutually exclusive";
    return false;
  }
  if (o.replay) o.live = false;
  if (o.width < 0 || o.height < 0 || o.width > 16384 || o.height > 16384) {
    error = "invalid window size";
    return false;
  }
  return true;
}

}  // namespace ftv
