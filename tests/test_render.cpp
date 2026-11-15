// Offscreen render test: draws real engine frames of a small deterministic
// dataset through a windowless GL context and checks the output against the
// palette rules:
//   * the background is within +-2 of #050608 in every channel;
//   * the frame holds at most 4 hue clusters (neutral ink + the warm hues);
//   * the emissive attachment is lit only on the active whale arcs and
//     flashes, and is black when no whale is in flight.
// Exits with 77 (ctest SKIP) when no OpenGL 4.5 context can be created.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>

#include "app/camera_presets.hpp"
#include "app/render_config.hpp"
#include "core/thread_pool.hpp"
#include "ingest/ingest.hpp"
#include "layout/force_solver.hpp"
#include "layout/layout_engine.hpp"
#include "layout/layout_params.hpp"
#include "model/edge_aggregation.hpp"
#include "model/globe.hpp"
#include "model/graph_builder.hpp"
#include "model/hub_taxonomy.hpp"
#include "model/whales.hpp"
#include "render/camera.hpp"
#include "render/gl_context.hpp"
#include "render/renderer.hpp"
#include "test_framework.hpp"
#include "test_util.hpp"

namespace ftv::test {
std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
int& failure_count() {
  static int n = 0;
  return n;
}
}  // namespace ftv::test

namespace {

using namespace ftv;

constexpr int kWidth = 960;
constexpr int kHeight = 540;

// Deterministic transactions across every hub, with a spread of amounts so
// each currency has whale candidates.
std::string make_dataset() {
  static const char* const kJur[] = {"US", "GB", "DE", "CH", "JP", "SG", "HK", "KY"};
  static const char* const kCcy[] = {"USD", "EUR", "GBP", "JPY"};
  static const char* const kChain[] = {"ETH", "BTC", "SOL", "TRON"};
  std::string csv =
      "tx_id,timestamp_utc,src_account,dst_account,amount,currency,channel,src_jurisdiction,dst_jurisdiction,chain\n";
  std::uint32_t s = 12345u;
  auto next = [&s]() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  };
  char line[256];
  for (int i = 0; i < 6000; ++i) {
    const std::uint32_t a = next() % 900, b = next() % 900;
    const std::uint32_t ja = a % 8, jb = b % 8;
    const bool chain = (i % 9) == 0;
    const std::uint32_t amount = 10 + next() % 5000 + ((next() % 200) == 0 ? 2000000 : 0);
    std::snprintf(line, sizeof(line), "R%d,2024-03-%02dT%02d:%02d:00Z,ACC-%u,ACC-%u,%u.00,%s,%s,%s,%s,%s\n", i,
                  1 + i % 28, (i / 60) % 24, i % 60, a, b, amount, kCcy[next() % 4], chain ? "ONCHAIN" : "SWIFT",
                  kJur[ja], kJur[jb], chain ? kChain[next() % 4] : "");
    csv += line;
  }
  return csv;
}

struct Scene {
  RenderSettings rs;
  std::optional<HubTaxonomy> tax;
  IngestResult ingest;
  Graph graph;
  WhaleSet whales;
};

struct Rgb {
  int r, g, b;
};

void to_hsv(int r8, int g8, int b8, float& h, float& s, float& v) {
  const float r = static_cast<float>(r8) / 255.0f, g = static_cast<float>(g8) / 255.0f,
              b = static_cast<float>(b8) / 255.0f;
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
  v = mx;
  s = mx > 0.0f ? d / mx : 0.0f;
  if (d <= 0.0f) {
    h = 0.0f;
  } else if (mx == r) {
    h = 60.0f * std::fmod((g - b) / d, 6.0f);
  } else if (mx == g) {
    h = 60.0f * ((b - r) / d + 2.0f);
  } else {
    h = 60.0f * ((r - g) / d + 4.0f);
  }
  if (h < 0.0f) h += 360.0f;
}

// Distance from p to the segment ab.
float seg_dist(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
  const glm::vec2 ab = b - a;
  const float l2 = glm::dot(ab, ab);
  const float t = l2 > 0.0f ? std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
  return glm::length(p - (a + ab * t));
}

}  // namespace

int main() {
  const std::string src = FTVIZ_SOURCE_DIR;
  std::string err;
  Scene sc;
  sc.tax = HubTaxonomy::load(src + "/config/hubs.json", err);
  auto rs = load_render_settings(src + "/config/render.json", err);
  auto theme = load_theme(src + "/config/theme.json", err);
  if (!sc.tax || !rs || !theme) {
    std::fprintf(stderr, "config: %s\n", err.c_str());
    return 1;
  }
  sc.rs = *rs;
  sc.rs.theme = *theme;
  sc.rs.window_width = kWidth;
  sc.rs.window_height = kHeight;

  ThreadPool pool(1);
  ftv::test::TempFile data("render.csv", make_dataset());
  IngestOptions io;
  io.path = data.path();
  if (!run_ingest(io, pool, sc.ingest, err)) {
    std::fprintf(stderr, "ingest: %s\n", err.c_str());
    return 1;
  }
  EdgeList edges = aggregate_edges(sc.ingest, pool);
  sc.graph = build_graph(sc.ingest, std::move(edges), *sc.tax, pool);
  const Theme& th = sc.rs.theme;
  sc.whales = build_whales(sc.ingest, sc.graph, sc.rs.replay_seconds, th.whale_flight_s + th.whale_flash_s);
  ForceSolver solver(sc.graph, *sc.tax, LayoutParams::from_json(sc.tax->layout_json()), pool);
  for (int i = 0; i < 60; ++i) solver.step();
  LayoutEngine engine(solver, sc.graph.node_count());
  engine.publish_now();

  GlWindow ctx;
  if (!ctx.create_offscreen(false, err)) {
    std::fprintf(stderr, "SKIP: no OpenGL 4.5 context (%s)\n", err.c_str());
    return 77;
  }
  int ran = 0;
  {
    Renderer renderer(sc.graph, *sc.tax, sc.whales, sc.rs, ctx.caps(), src + "/shaders");
    if (!renderer.init(err)) {
      std::fprintf(stderr, "renderer: %s\n", err.c_str());
      return 1;
    }
    engine.snapshots().acquire_latest();
    renderer.prime_positions(engine.snapshots().front());
    const NodePositionFn pos = [&renderer](std::uint32_t n) { return renderer.node_world_position(n); };

    OrbitCamera camera;
    camera.configure(sc.rs.camera_fov_deg, sc.rs.camera_near, sc.rs.camera_damping);
    const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);

    auto render_at = [&](const std::string& preset, float time, glm::mat4& viewProj) {
      ViewSetup v;
      std::string e;
      if (!camera_preset(preset, *sc.tax, sc.whales, pos, sc.rs, v, e)) {
        std::fprintf(stderr, "preset: %s\n", e.c_str());
        return false;
      }
      camera.set_immediate(v.target, v.distance, v.yaw, v.pitch);
      FrameParams p;
      for (int f = 0; f < 3; ++f) {
        renderer.begin_frame(engine.snapshots());
        p.view = camera.view();
        p.proj = camera.projection(aspect);
        p.camera_pos = camera.position();
        p.time = (time >= 0.0f ? time : v.time) - static_cast<float>(2 - f) / 60.0f;
        p.near_plane = camera.near_plane();
        p.width = kWidth;
        p.height = kHeight;
        p.show_hud = false;
        p.present = false;
        renderer.draw_frame(p);
      }
      viewProj = p.proj * p.view;
      return true;
    };

    // ---- frame 1: whale preset, whales in flight -------------------------------
    {
      std::fprintf(stderr, "[ RUN  ] render: palette and emissive confinement (whale preset)\n");
      const int before = ftv::test::failure_count();
      glm::mat4 vp(1.0f);
      CHECK(sc.whales.events.size() > 0);
      ViewSetup vs;
      std::string e;
      CHECK(camera_preset("whale", *sc.tax, sc.whales, pos, sc.rs, vs, e));
      const float t = vs.time;
      CHECK(render_at("whale", t, vp));

      std::vector<std::uint8_t> rgba;
      int w = 0, h = 0;
      CHECK(renderer.read_output_rgba8(rgba, w, h));
      CHECK(w == kWidth && h == kHeight);

      // Background: the most common colour.
      std::map<std::uint32_t, int> hist;
      for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
        ++hist[(std::uint32_t{rgba[i]} << 16) | (std::uint32_t{rgba[i + 1]} << 8) | rgba[i + 2]];
      const auto mode = std::max_element(hist.begin(), hist.end(),
                                         [](const auto& a, const auto& b) { return a.second < b.second; });
      const Rgb bg{static_cast<int>(mode->first >> 16), static_cast<int>((mode->first >> 8) & 255),
                   static_cast<int>(mode->first & 255)};
      std::fprintf(stderr, "  background %d,%d,%d (%d px)\n", bg.r, bg.g, bg.b, mode->second);
      CHECK(std::abs(bg.r - 5) <= 2 && std::abs(bg.g - 6) <= 2 && std::abs(bg.b - 8) <= 2);
      CHECK(mode->second > kWidth * kHeight / 4);

      // Hue clusters: neutral pixels form one cluster; chromatic pixels are
      // binned by hue (10 degree bins, bins under 0.2% of the chromatic
      // pixels ignored) and adjacent occupied bins merged.
      int bins[36] = {};
      int chromatic = 0, neutral = 0;
      for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        float hh, ss, vv;
        to_hsv(rgba[i], rgba[i + 1], rgba[i + 2], hh, ss, vv);
        if (vv < 0.08f) continue;  // background and near-black
        if (ss < 0.25f) {
          ++neutral;
        } else {
          ++bins[static_cast<int>(hh / 10.0f) % 36];
          ++chromatic;
        }
      }
      const int minCount = std::max(1, chromatic / 500);
      int runs = 0, occupied = 0;
      for (int b = 0; b < 36; ++b) {
        const bool occ = bins[b] >= minCount, prevOcc = bins[(b + 35) % 36] >= minCount;
        occupied += occ ? 1 : 0;
        runs += (occ && !prevOcc) ? 1 : 0;
      }
      if (occupied == 36) runs = 1;
      const int clusters = (neutral > 0 ? 1 : 0) + runs;
      std::fprintf(stderr, "  %d neutral + %d chromatic px, %d hue clusters\n", neutral, chromatic, clusters);
      CHECK(clusters <= 4);

      // Emissive confinement: every lit emissive pixel lies on an active
      // whale's arc (any part of it: the trail is a sub-range) or within its
      // destination flash.
      std::vector<float> em;
      CHECK(renderer.read_emissive(em, w, h));
      std::vector<std::vector<glm::vec2>> paths;
      std::vector<glm::vec2> flashes;
      auto project = [&](const glm::vec3& p, glm::vec2& out) {
        const glm::vec4 c = vp * glm::vec4(p, 1.0f);
        if (c.w <= 1e-3f) return false;
        out = {(c.x / c.w * 0.5f + 0.5f) * static_cast<float>(w), (0.5f - c.y / c.w * 0.5f) * static_cast<float>(h)};
        return true;
      };
      int active = 0;
      for (const WhaleEvent& ev : sc.whales.events) {
        const float age = t - ev.start;
        const float head = age / th.whale_flight_s;
        const bool flying = head > 0.0f && head - th.whale_trail_s / th.whale_flight_s < 1.0f;
        const bool flashing = age >= th.whale_flight_s && age < th.whale_flight_s + th.whale_flash_s;
        if (!flying && !flashing) continue;
        ++active;
        const glm::vec3 a = renderer.node_world_position(ev.src), b = renderer.node_world_position(ev.dst);
        std::vector<glm::vec2> path;
        for (int k = 0; k <= 256; ++k) {
          glm::vec2 q;
          if (project(arc_point(a, b, static_cast<float>(k) / 256.0f, sc.rs.globe), q)) path.push_back(q);
        }
        paths.push_back(std::move(path));
        glm::vec2 f;
        if (project(b, f)) flashes.push_back(f);
      }
      CHECK(active > 0);
      const float ui = sc.rs.ui_scale;
      const float ribbonTol = (0.5f * th.whale_width_px[2] + 2.0f) * ui + 1.5f;
      const float flashTol = (12.0f + 2.0f) * ui + 2.0f;
      int lit = 0, stray = 0;
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          const float* px = &em[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4];
          if (std::max({px[0], px[1], px[2]}) <= 1e-3f) continue;
          ++lit;
          const glm::vec2 p(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
          bool ok = false;
          for (const glm::vec2& f : flashes) ok = ok || glm::length(p - f) <= flashTol;
          for (const auto& path : paths) {
            for (std::size_t k = 1; k < path.size() && !ok; ++k) ok = seg_dist(p, path[k - 1], path[k]) <= ribbonTol;
            if (ok) break;
          }
          if (!ok) ++stray;
        }
      }
      std::fprintf(stderr, "  %d active whales, %d lit emissive px, %d outside whale arcs/flashes\n", active, lit, stray);
      CHECK(lit > 0);
      CHECK(stray == 0);
      std::fprintf(stderr, "[ %s ] render: palette and emissive confinement (whale preset)\n",
                   ftv::test::failure_count() == before ? " OK " : "FAIL");
      ++ran;
    }

    // ---- frame 2: a replay time with no whale in flight -> emissive is black ----
    {
      std::fprintf(stderr, "[ RUN  ] render: emissive is empty when no whale is active\n");
      const int before = ftv::test::failure_count();
      // First replay time at which no event is in flight or flashing.
      const float life = th.whale_flight_s + th.whale_flash_s + 0.05f;
      float quiet = -1.0f;
      for (float t = 0.05f; t < sc.whales.replay_seconds && quiet < 0.0f; t += 0.05f) {
        bool busy = false;
        for (const WhaleEvent& ev : sc.whales.events) busy = busy || (t >= ev.start - 0.05f && t < ev.start + life);
        if (!busy) quiet = t;
      }
      CHECK(quiet > 0.0f);
      glm::mat4 vp(1.0f);
      CHECK(render_at("globe", quiet, vp));
      std::vector<float> em;
      int w = 0, h = 0;
      CHECK(renderer.read_emissive(em, w, h));
      float peak = 0.0f;
      for (float c : em) peak = std::max(peak, c);
      std::fprintf(stderr, "  replay t=%.3f s, emissive peak %.5f, active whales %u\n", static_cast<double>(quiet),
                   static_cast<double>(peak), renderer.counters().active_whales);
      CHECK(peak <= 1e-3f);
      CHECK(renderer.counters().active_whales == 0);
      std::fprintf(stderr, "[ %s ] render: emissive is empty when no whale is active\n",
                   ftv::test::failure_count() == before ? " OK " : "FAIL");
      ++ran;
    }
  }
  ctx.destroy();
  std::fprintf(stderr, "\n%d tests, %d failed checks\n", ran, ftv::test::failure_count());
  return ftv::test::failure_count() == 0 ? 0 : 1;
}
