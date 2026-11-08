#include "app/cli.hpp"

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
      "Usage: %s --input <transactions.csv|.ftxb> [options]\n"
      "\n"
      "Input / configuration\n"
      "  --input PATH           CSV with header or packed .ftxb file (required)\n"
      "  --format auto|csv|ftxb input format (default: auto-detect)\n"
      "  --hubs PATH            hub taxonomy (default: config/hubs.json next to the binary)\n"
      "  --render PATH          render settings (default: config/render.json next to the binary)\n"
      "  --shaders DIR          GLSL directory (default: shaders/ next to the binary)\n"
      "\n"
      "Threads / ingest\n"
      "  --threads N            layout/merge threads incl. the coordinator (default: all cores headless,\n"
      "                         all cores but one interactive, so the render thread keeps a core)\n"
      "  --parser-threads N     T_ingest workers (default: max(1, hw-2))\n"
      "  --chunk-mb N           ingest chunk size in MB (default 16)\n"
      "  --log-malformed K      print the first K malformed row offsets (default 8, max 16)\n"
      "\n"
      "Modes\n"
      "  --headless             ingest + layout benchmark without a window\n"
      "  --layout-iters N       headless: run N iterations (default: until converged, max 5000)\n"
      "  --prelayout N          run N layout iterations before opening the window\n"
      "  --screenshot FILE.ppm  render, save the frame as binary PPM and exit\n"
      "  --frames N             exit after N frames and print frame-time statistics\n"
      "\n"
      "Window / view\n"
      "  --width W --height H   window size (default from render.json)\n"
      "  --vsync | --no-vsync   override render.json\n"
      "  --msaa N               MSAA samples (1 = off)\n"
      "  --hub N                start focused on hub N (1-based)\n"
      "  --select NODE          start with node NODE (dense index) selected and framed\n"
      "  --camera Y,P,D         start camera yaw/pitch in degrees and distance\n"
      "  --no-detail            start with individual inter-hub edges hidden (toggle: E)\n"
      "  --paused               start with the layout paused (toggle: L)\n"
      "  --no-hud               start with the HUD hidden (toggle: H)\n"
      "  --no-gl-debug          do not request a debug context\n"
      "  --no-alloc-assert      Debug builds: warn instead of abort on per-frame allocations\n"
      "\n"
      "Controls: left-drag orbit, right/middle-drag pan, wheel zoom, click select,\n"
      "  1-9 fly to hub, 0 overview, F frame selection, E inter-hub edge detail,\n"
      "  L pause/resume layout, R reheat layout, C colour by currency/channel,\n"
      "  H toggle HUD, Esc clear selection / quit.\n",
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
    } else if (a == "--input" || a == "-i") {
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
    } else if (a == "--hubs") {
      if (!str(o.hubs_path)) return false;
    } else if (a == "--render") {
      if (!str(o.render_path)) return false;
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
  if (!o.help && o.input.empty()) {
    error = "--input is required";
    return false;
  }
  if (o.width < 0 || o.height < 0 || o.width > 16384 || o.height > 16384) {
    error = "invalid window size";
    return false;
  }
  return true;
}

}  // namespace ftv
