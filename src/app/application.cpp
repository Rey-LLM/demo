#include "app/application.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <xxhash.h>

#include "app/hud_stats.hpp"
#include "app/interaction.hpp"
#include "app/live_hud.hpp"
#include "app/live_session.hpp"
#include "app/render_config.hpp"
#include "core/alloc_counter.hpp"
#include "core/embedded_fs.hpp"
#include "core/fixed_text.hpp"
#include "core/log.hpp"
#include "core/thread_pool.hpp"
#include "core/timing.hpp"
#include "ingest/ingest.hpp"
#include "layout/force_solver.hpp"
#include "layout/layout_engine.hpp"
#include "layout/layout_params.hpp"
#include "model/edge_aggregation.hpp"
#include "model/graph_builder.hpp"
#include "model/hub_taxonomy.hpp"
#include "model/cables.hpp"
#include "model/citylights.hpp"
#include "model/solar.hpp"
#include "model/whales.hpp"
#include "app/camera_presets.hpp"
#include "app/png_writer.hpp"
#include "render/camera.hpp"
#include "render/coastlines.hpp"
#include "render/font.hpp"
#include "render/gl_context.hpp"
#include "render/renderer.hpp"

#ifndef FTVIZ_SOURCE_DIR
#define FTVIZ_SOURCE_DIR "."
#endif

namespace ftv {

namespace {

// Earth layers loaded at startup (all optional: a missing asset only turns
// its layer off, with a warning).
struct EarthData {
  std::optional<CableSet> cables;
  RouteTable routes;
  std::optional<CityLights> lights;
  [[nodiscard]] EarthAssets assets() const noexcept {
    EarthAssets a;
    a.cables = cables ? &*cables : nullptr;
    a.routes = routes.points.empty() ? nullptr : &routes;
    a.lights = lights ? &*lights : nullptr;
    return a;
  }
};

std::int64_t wall_clock_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

glm::vec3 sun_vector(std::int64_t utcMs) noexcept {
  float v[3];
  sun_direction(subsolar_point(utcMs), v);
  return {v[0], v[1], v[2]};
}

// Screenshots must not depend on when they are taken: without --sun-time
// and without a model clock they use the March 2026 equinox at 12:00 UTC.
constexpr std::int64_t kDefaultScreenshotSunMs = 1774008000000;  // 2026-03-20T12:00:00Z

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Startup helpers (allocation is fine here)
// ---------------------------------------------------------------------------

std::string grp(std::uint64_t v) {
  FixedText<32> t;
  t.grouped(v);
  return std::string(t.view());
}

#if defined(FTVIZ_COUNT_ALLOCATIONS) && defined(GLAD_OPTION_GL_DEBUG)
// Debug builds load GL through glad's call wrappers. These hooks bracket every
// GL call as driver code for the allocation counter, so a driver's own C++
// allocations on the render thread (shader JIT and the like) are reported
// separately instead of being charged to our frame code (alloc_counter.hpp).
void gl_pre_call_hook(const char*, GLADapiproc, int, ...) { enter_foreign_code(); }
void gl_post_call_hook(void*, const char*, GLADapiproc, int, ...) { leave_foreign_code(); }
#endif

// An explicit option wins, then --data-dir/<rel>, then the copy compiled
// into the binary. Nothing next to the executable is ever required.
std::string resolve_path(const std::string& explicitPath, const std::string& dataDir, const char* rel) {
  if (!explicitPath.empty()) return explicitPath;
  if (!dataDir.empty()) return (fs::path(dataDir) / rel).string();
  return embedded_path(rel);
}

void print_ingest_report(const IngestResult& in, const std::string& path) {
  const IngestStats& s = in.stats;
  std::printf("[ingest] %s: %.1f MB, %s, %u parser threads, %zu chunks\n", path.c_str(),
              static_cast<double>(s.bytes) / (1024.0 * 1024.0), in.format == InputFormat::Ftxb ? "FTXB" : "CSV",
              s.parser_threads, s.chunk_count);
  std::printf("[ingest] rows ok %s, malformed %s, blank %s, amounts truncated %s\n", grp(s.rows_ok).c_str(),
              grp(s.rows_malformed).c_str(), grp(s.rows_blank).c_str(), grp(s.amounts_truncated).c_str());
  if (!s.malformed_offsets.empty()) {
    std::printf("[ingest] first malformed rows at byte offsets:");
    for (std::uint64_t off : s.malformed_offsets) std::printf(" %" PRIu64, off);
    std::printf("\n");
  }
  std::printf("[ingest] throughput %.3f M rows/s, %.1f MB/s (parse phase %.1f ms), wall %.1f ms\n",
              s.rows_per_sec() / 1e6, s.mb_per_sec(), s.parse_ms, s.wall_ms);
  std::printf("[ingest] unique entities %s, aggregated edges %s (dropped invalid %s)\n", grp(s.unique_entities).c_str(),
              grp(s.aggregated_edges).c_str(), grp(s.dropped_invalid_edges).c_str());
  std::printf("[ingest] phases (ms):");
  for (const PhaseTiming& p : s.phases) std::printf(" %s=%.1f", p.name, p.ms);
  std::printf("\n");
}

void print_model_report(const Graph& g, const HubTaxonomy& tax, const WhaleSet& whales, double buildMs) {
  std::printf("[model] graph %s nodes, %s edges (%s inter-hub, %.2f%%), %s bridges, %s dropped; build %.1f ms\n",
              grp(g.node_count()).c_str(), grp(g.edge_count()).c_str(), grp(g.inter_hub_edges).c_str(),
              g.edge_count() ? 100.0 * static_cast<double>(g.inter_hub_edges) / static_cast<double>(g.edge_count()) : 0.0,
              grp(g.bridge_count).c_str(), grp(g.dropped_edges).c_str(), buildMs);
  std::printf("[model] %zu currencies, %zu whale events (top %zu single transactions per currency), "
              "replay %.0f s\n",
              g.currencies.size(), whales.events.size(), WhaleTracker::kPerCurrency,
              static_cast<double>(whales.replay_seconds));
  std::printf("[model] hub populations:");
  for (std::size_t h = 0; h < tax.size(); ++h) {
    std::printf(" %s=%s", tax.hubs()[h].id.c_str(),
                grp(h < g.hub_population.size() ? g.hub_population[h] : 0u).c_str());
  }
  std::printf("\n");
}

// Structure metrics of a layout: how well nodes sit inside their hub.
struct ClusterQuality {
  double inside_fraction = 0.0;      // nodes within 1.2 R of their own anchor
  double nearest_own_fraction = 0.0; // nodes whose radius-normalised nearest anchor is their own hub
  double mean_norm_dist = 0.0;       // mean |p - anchor| / R
};

ClusterQuality measure_clusters(const ForceSolver& s, const Graph& g, const HubTaxonomy& tax) {
  ClusterQuality q;
  const std::size_t n = s.node_count();
  if (n == 0) return q;
  const auto& hubs = tax.hubs();
  std::uint64_t inside = 0, nearestOwn = 0;
  double sumNorm = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint16_t h = g.nodes.hub[i];
    const glm::vec3 p = s.position(i);
    const double d = glm::length(p - hubs[h].anchor) / hubs[h].radius;
    sumNorm += d;
    if (d <= 1.2) ++inside;
    std::size_t best = h;
    double bestD = d;
    for (std::size_t k = 0; k < hubs.size(); ++k) {
      const double dk = glm::length(p - hubs[k].anchor) / hubs[k].radius;
      if (dk < bestD) {
        bestD = dk;
        best = k;
      }
    }
    if (best == h) ++nearestOwn;
  }
  q.inside_fraction = static_cast<double>(inside) / static_cast<double>(n);
  q.nearest_own_fraction = static_cast<double>(nearestOwn) / static_cast<double>(n);
  q.mean_norm_dist = sumNorm / static_cast<double>(n);
  return q;
}

struct Percentiles {
  double mean = 0, p50 = 0, p95 = 0, max = 0;
};

Percentiles percentiles(std::vector<double> v) {
  Percentiles p;
  if (v.empty()) return p;
  double sum = 0;
  for (double x : v) sum += x;
  p.mean = sum / static_cast<double>(v.size());
  std::sort(v.begin(), v.end());
  p.p50 = v[v.size() / 2];
  p.p95 = v[std::min(v.size() - 1, static_cast<std::size_t>(static_cast<double>(v.size()) * 0.95))];
  p.max = v.back();
  return p;
}

// Returns the statistics of the last iteration run (default values if none).
IterationStats run_layout_iterations(ForceSolver& solver, unsigned count, bool untilConverged, const char* tag,
                                     std::vector<IterationStats>* record) {
  IterationStats last;
  if (count == 0) return last;
  Stopwatch wall;
  for (unsigned i = 0; i < count; ++i) {
    const IterationStats st = solver.step();
    last = st;
    if (record) record->push_back(st);
    if ((i + 1) % 100 == 0 || st.converged) {
      std::printf("[%s] iter %5" PRIu64 "  %7.2f ms (tree %.2f, repulsion %.2f, integrate %.2f)  T %.3f  disp %.4f%s\n",
                  tag, st.iteration, st.total_ms, st.tree_ms, st.repulsion_ms, st.integrate_ms,
                  static_cast<double>(st.temperature), st.mean_displacement, st.converged ? "  CONVERGED" : "");
      std::fflush(stdout);
    }
    if (untilConverged && st.converged) break;
  }
  std::printf("[%s] %" PRIu64 " iterations in %.1f s\n", tag, solver.iteration(), wall.seconds());
  return last;
}

// ---------------------------------------------------------------------------
// Headless benchmark
// ---------------------------------------------------------------------------

int run_headless(const CliOptions& cli, ForceSolver& solver, const Graph& graph, const HubTaxonomy& tax,
                 unsigned concurrency) {
  const bool untilConverged = cli.layout_iters == 0;
  const unsigned maxIters = untilConverged ? 5000u : cli.layout_iters;
  std::vector<IterationStats> rec;
  rec.reserve(maxIters);
  std::printf("[layout] %s nodes, %u threads (coordinator + pool), %s\n", grp(solver.node_count()).c_str(),
              concurrency, untilConverged ? "until converged (max 5000 iterations)" : "fixed iteration count");
  run_layout_iterations(solver, maxIters, untilConverged, "layout", &rec);

  std::vector<double> tot, tree, rep, integ;
  for (const IterationStats& s : rec) {
    tot.push_back(s.total_ms);
    tree.push_back(s.tree_ms);
    rep.push_back(s.repulsion_ms);
    integ.push_back(s.integrate_ms);
  }
  const auto pt = percentiles(tot), ptree = percentiles(tree), prep = percentiles(rep), pint = percentiles(integ);
  std::printf("[layout] per-iteration ms      mean     p50     p95     max\n");
  std::printf("[layout]   total            %7.2f %7.2f %7.2f %7.2f\n", pt.mean, pt.p50, pt.p95, pt.max);
  std::printf("[layout]   octree build     %7.2f %7.2f %7.2f %7.2f\n", ptree.mean, ptree.p50, ptree.p95, ptree.max);
  std::printf("[layout]   BH repulsion     %7.2f %7.2f %7.2f %7.2f\n", prep.mean, prep.p50, prep.p95, prep.max);
  std::printf("[layout]   springs+verlet   %7.2f %7.2f %7.2f %7.2f\n", pint.mean, pint.p50, pint.p95, pint.max);
  std::printf("[layout] converged: %s at iteration %" PRIu64 ", final T %.4f\n", solver.converged() ? "yes" : "no",
              solver.iteration(), static_cast<double>(solver.temperature()));
  std::printf("[layout] checksum %016" PRIx64 ", BH interactions/body %.0f (last iteration), kernel %s\n",
              solver.checksum(), solver.interactions_per_body(), solver.simd_avx() ? "AVX" : "SSE2/scalar");
  const ClusterQuality q = measure_clusters(solver, graph, tax);
  std::printf("[layout] structure: %.2f%% of nodes within 1.2R of their hub anchor, %.2f%% nearest to their own hub, "
              "mean |p-a|/R %.3f\n",
              q.inside_fraction * 100.0, q.nearest_own_fraction * 100.0, q.mean_norm_dist);
  return 0;
}

// ---------------------------------------------------------------------------
// Interactive mode
// ---------------------------------------------------------------------------

// Input events gathered by the GLFW callbacks between frames. Fixed-size, so
// event handling never allocates.
struct InputState {
  double cursor_x = 0.0, cursor_y = 0.0;
  double press_x = 0.0, press_y = 0.0;
  bool left_down = false;
  bool pressed = false;  // a left press since the last frame
  bool pan_down = false;
  double drag_travel = 0.0;
  float orbit_dx = 0.0f, orbit_dy = 0.0f;
  float pan_dx = 0.0f, pan_dy = 0.0f;
  float scroll = 0.0f;
  bool click = false;
  double click_x = 0.0, click_y = 0.0;
  std::array<int, 32> keys{};
  std::array<int, 32> mods{};
  unsigned key_count = 0;
};

InputState* input_of(GLFWwindow* w) { return static_cast<InputState*>(glfwGetWindowUserPointer(w)); }

void install_callbacks(GLFWwindow* w, InputState* in) {
  glfwSetWindowUserPointer(w, in);
  glfwSetMouseButtonCallback(w, [](GLFWwindow* win, int button, int action, int) {
    InputState* s = input_of(win);
    double x = 0, y = 0;
    glfwGetCursorPos(win, &x, &y);
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
      if (action == GLFW_PRESS) {
        s->left_down = true;
        s->pressed = true;
        s->press_x = x;
        s->press_y = y;
        s->drag_travel = 0.0;
      } else if (action == GLFW_RELEASE) {
        s->left_down = false;
        if (s->drag_travel < 4.0) {  // a click, not an orbit drag
          s->click = true;
          s->click_x = x;
          s->click_y = y;
        }
      }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT || button == GLFW_MOUSE_BUTTON_MIDDLE) {
      s->pan_down = action == GLFW_PRESS;
    }
  });
  glfwSetCursorPosCallback(w, [](GLFWwindow* win, double x, double y) {
    InputState* s = input_of(win);
    const auto dx = static_cast<float>(x - s->cursor_x);
    const auto dy = static_cast<float>(y - s->cursor_y);
    s->cursor_x = x;
    s->cursor_y = y;
    if (s->left_down) {
      s->orbit_dx += dx;
      s->orbit_dy += dy;
      s->drag_travel += std::fabs(dx) + std::fabs(dy);
    }
    if (s->pan_down) {
      s->pan_dx += dx;
      s->pan_dy += dy;
    }
  });
  glfwSetScrollCallback(w, [](GLFWwindow* win, double, double yoff) {
    input_of(win)->scroll += static_cast<float>(yoff);
  });
  glfwSetKeyCallback(w, [](GLFWwindow* win, int key, int, int action, int mods) {
    InputState* s = input_of(win);
    // Arrow keys repeat (timeline stepping); everything else acts on press.
    const bool arrow = key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT;
    if ((action == GLFW_PRESS || (arrow && action == GLFW_REPEAT)) && s->key_count < s->keys.size()) {
      s->mods[s->key_count] = mods;
      s->keys[s->key_count++] = key;
    }
  });
  double x = 0, y = 0;
  glfwGetCursorPos(w, &x, &y);
  in->cursor_x = x;
  in->cursor_y = y;
}

struct FrameRecord {
  float frame_ms;
  float cpu_ms;
  float upload_ms;  // layout snapshot copy into the position ring (0 when none arrived)
};

// Replay clock: loops over [0, replay_seconds). Space pauses it.
float wrap_time(float t, float period) {
  if (period <= 0.0f) return t;
  t = std::fmod(t, period);
  return t < 0.0f ? t + period : t;
}

class Viewer {
 public:
  Viewer(const CliOptions& cli, const RenderSettings& rs, const std::string& shaderDir, const Graph& graph,
         const HubTaxonomy& tax, const WhaleSet& whales, const CoastlineSet& coast, const EarthData& earth,
         const IngestResult& ingest, LayoutEngine& engine, LiveSession* live, std::string inputName)
      : cli_(cli),
        rs_(rs),
        shader_dir_(shaderDir),
        graph_(graph),
        tax_(tax),
        whales_(whales),
        coast_(coast),
        earth_(earth),
        engine_(engine),
        live_(live),
        title_("ftviz - " + inputName),
        hud_stats_(graph, tax, ingest, inputName),
        live_hud_(tax, live ? live->mode() : ModelMode::Dataset, live ? live->feeds() : nullptr,
                  live ? &live->config() : nullptr) {}

  int run();

 private:
  bool create_gpu_side();
  void destroy_gpu_side();
  void handle_input(int fbw, int fbh);
  void handle_key(int key, int mods);
  void frame_selection();
  void fly_to_preset(int index);
  void overview();
  void set_paused(bool paused);
  void seek_relative(std::int64_t deltaMs);
  void seek_to_cursor(const LiveHudLayout& l, double fbX);
  void request_export();
  void refresh_live_hud();
  void print_frame_summary() const;

  const CliOptions& cli_;
  RenderSettings rs_;
  std::string shader_dir_;
  const Graph& graph_;
  const HubTaxonomy& tax_;
  const WhaleSet& whales_;
  const CoastlineSet& coast_;
  const EarthData& earth_;
  std::int64_t model_now_ms_ = 0;  // live / replay model clock (UTC ms), 0 until the first snapshot
  LayoutEngine& engine_;
  LiveSession* live_;
  std::string title_;

  GlWindow window_;
  std::unique_ptr<Renderer> renderer_;
  InputState input_;
  OrbitCamera camera_;
  HudStats hud_stats_;
  LiveHud live_hud_;
  LiveHudLayout live_layout_;
  LiveHudInput live_in_;
  FixedText<512> toast_;
  double toast_left_ms_ = 0.0;
  std::uint64_t export_seen_ = 0;
  bool export_pending_exit_ = false;
  int hovered_hub_ = -1;
  int pinned_hub_ = -1;
  bool live_hud_dirty_ = true;
  bool paused_ = false;       // Space: ingest application, layout and clocks
  bool scrubbing_ = false;
  bool flying_ = false;       // 1-5 preset flight in progress
  float fly_t_ = 0.0f;
  CameraPose fly_from_, fly_to_;
  float live_clock_ = 0.0f;
  glm::mat4 inv_view_proj_{1.0f};
  FrameStats frame_stats_;
  bool show_hud_ = true;
  bool inter_detail_ = true;
  bool replay_paused_ = false;
  bool selection_changed_ = true;
  std::uint64_t foreign_allocs_ = 0;  // driver allocations inside GL calls (Debug counting only)
  std::vector<FrameRecord> records_;
};

bool Viewer::create_gpu_side() {
  std::string err;
  if (!window_.create(rs_.window_width, rs_.window_height, title_.c_str(), rs_.vsync, cli_.gl_debug, err)) {
    FTV_LOG_ERROR("%s", err.c_str());
    return false;
  }
#if defined(FTVIZ_COUNT_ALLOCATIONS) && defined(GLAD_OPTION_GL_DEBUG)
  gladSetGLPreCallback(&gl_pre_call_hook);
  gladSetGLPostCallback(&gl_post_call_hook);
#endif
  install_callbacks(window_.handle(), &input_);
  renderer_ = std::make_unique<Renderer>(graph_, tax_, whales_, coast_, rs_, window_.caps(), shader_dir_);
  renderer_->set_earth(earth_.assets());
  if (!renderer_->init(err)) {
    FTV_LOG_ERROR("renderer initialisation failed:\n%s", err.c_str());
    return false;
  }
  // Take the newest snapshot (reader side of the triple buffer; the front
  // slot is reader-owned and stays valid) and copy it into every position
  // region, so a (re-)created renderer is correct even while the layout is
  // idle and will not publish again.
  engine_.snapshots().acquire_latest();
  renderer_->prime_positions(engine_.snapshots().front());
  selection_changed_ = true;
  return true;
}

void Viewer::destroy_gpu_side() {
  renderer_.reset();  // GL objects first, while the context still exists
  window_.destroy();
}

void Viewer::overview() {
  camera_.fly_to(glm::vec3(0.0f), rs_.camera_start_distance);
}

// Keys 1-5: an eased 1.2 s flight (direction slerp) to a view preset.
void Viewer::fly_to_preset(int index) {
  fly_from_ = CameraPose{camera_.target(), camera_.distance(), camera_.yaw(), camera_.pitch()};
  fly_to_ = view_preset(index, tax_, rs_);
  fly_t_ = 0.0f;
  flying_ = true;
}

void Viewer::set_paused(bool paused) {
  paused_ = paused;
  replay_paused_ = paused;
  engine_.set_paused(paused);
  if (live_) live_->model().set_paused(paused);
  live_hud_dirty_ = true;
}

void Viewer::seek_relative(std::int64_t deltaMs) {
  if (!live_ || live_->mode() != ModelMode::Replay) return;
  const std::int64_t D = live_->model().timeline_duration_ms();
  if (D <= 0) return;
  live_->model().request_seek(std::clamp<std::int64_t>(live_->model().timeline_ms() + deltaMs, 0, D - 1));
  live_hud_dirty_ = true;
}

void Viewer::seek_to_cursor(const LiveHudLayout& l, double fbX) {
  if (!live_ || live_->mode() != ModelMode::Replay || l.timeline.w <= 0.0f) return;
  const std::int64_t D = live_->model().timeline_duration_ms();
  const double f = std::clamp((fbX - static_cast<double>(l.timeline.x)) / static_cast<double>(l.timeline.w), 0.0, 1.0);
  live_->model().request_seek(std::min<std::int64_t>(static_cast<std::int64_t>(f * static_cast<double>(D)), D - 1));
  live_hud_dirty_ = true;
}

void Viewer::request_export() {
  if (live_) live_->exporter().request();
}

void Viewer::refresh_live_hud() {
  if (!live_) return;
  ModelThread& m = live_->model();
  live_in_.pinned_hub = pinned_hub_;
  live_in_.timeline_visible = live_->mode() == ModelMode::Replay;
  live_in_.timeline_pos_s = static_cast<double>(m.timeline_ms()) / 1000.0;
  live_in_.timeline_len_s = static_cast<double>(m.timeline_duration_ms()) / 1000.0;
  live_in_.capture_synthetic = live_->capture() && live_->capture()->synthetic();
  live_in_.paused = paused_;
  if (const LiveSnapshot* snap = m.snapshots().acquire()) {
    live_hud_.refresh(*snap, live_in_);
    model_now_ms_ = snap->now_ms;
    m.snapshots().release(snap);
  }
}

void Viewer::frame_selection() {
  const auto sel = renderer_->selection();
  if (!sel) {
    overview();
    return;
  }
  const glm::vec3 c = renderer_->node_world_position(*sel);
  float extent = 0.0f;
  const auto& adj = graph_.adjacency;
  for (std::uint32_t k = adj.offset[*sel]; k < adj.offset[*sel + 1]; ++k) {
    extent = std::max(extent, glm::length(renderer_->node_world_position(adj.neighbor[k]) - c));
  }
  extent = std::clamp(extent, 5.0f, 2.0f * rs_.globe.radius);
  camera_.fly_to(c, extent / std::sin(camera_.fov_y() * 0.5f) * 1.1f);
}

void Viewer::handle_key(int key, int mods) {
  if (key >= GLFW_KEY_1 && key <= GLFW_KEY_5) {
    fly_to_preset(key - GLFW_KEY_1 + 1);
    return;
  }
  switch (key) {
    case GLFW_KEY_F:
      flying_ = false;
      frame_selection();
      break;
    case GLFW_KEY_E:
      request_export();
      break;
    case GLFW_KEY_D:
      inter_detail_ = !inter_detail_;
      break;
    case GLFW_KEY_LEFT:
      seek_relative((mods & GLFW_MOD_SHIFT) ? -10000 : -1000);
      break;
    case GLFW_KEY_RIGHT:
      seek_relative((mods & GLFW_MOD_SHIFT) ? 10000 : 1000);
      break;
    case GLFW_KEY_L:
      engine_.set_paused(!engine_.paused());
      break;
    case GLFW_KEY_R:
      engine_.reheat();
      break;
    case GLFW_KEY_H:
      show_hud_ = !show_hud_;
      break;
    case GLFW_KEY_SPACE:
      set_paused(!paused_);
      break;
    case GLFW_KEY_ESCAPE:
      if (pinned_hub_ >= 0) {
        pinned_hub_ = -1;
        live_hud_dirty_ = true;
      } else if (renderer_->selection()) {
        renderer_->set_selection(std::nullopt);
        selection_changed_ = true;
      } else {
        glfwSetWindowShouldClose(window_.handle(), GLFW_TRUE);
      }
      break;
    default:
      break;
  }
}

void Viewer::handle_input(int fbw, int fbh) {
  GLFWwindow* w = window_.handle();
  int ww = 1, wh = 1;
  glfwGetWindowSize(w, &ww, &wh);
  ww = std::max(ww, 1);
  wh = std::max(wh, 1);

  for (unsigned i = 0; i < input_.key_count; ++i) handle_key(input_.keys[i], input_.mods[i]);
  input_.key_count = 0;

  // Cursor coordinates are in window units; HUD and pick targets are in pixels.
  const double sx = static_cast<double>(fbw) / ww, sy = static_cast<double>(fbh) / wh;
  const double cx = input_.cursor_x * sx, cy = input_.cursor_y * sy;
  const bool overButton = live_ && show_hud_ && live_layout_.export_button.contains(cx, cy);
  const bool timelineOn = live_ && show_hud_ && live_->mode() == ModelMode::Replay;
  if (live_in_.export_hover != overButton) live_in_.export_hover = overButton;

  // Dragging that started on the timeline scrubs instead of orbiting.
  if (input_.pressed) {
    input_.pressed = false;
    scrubbing_ = timelineOn && live_layout_.timeline.contains(input_.press_x * sx, input_.press_y * sy);
  } else if (scrubbing_ && !input_.left_down && !input_.click) {
    scrubbing_ = false;
  }
  if (scrubbing_) {
    seek_to_cursor(live_layout_, cx);
    input_.orbit_dx = input_.orbit_dy = 0.0f;
  }

  if (input_.orbit_dx != 0.0f || input_.orbit_dy != 0.0f) {
    flying_ = false;
    camera_.orbit(-input_.orbit_dx * 0.005f, input_.orbit_dy * 0.005f);
  }
  if (input_.pan_dx != 0.0f || input_.pan_dy != 0.0f) {
    flying_ = false;
    camera_.pan(input_.pan_dx / static_cast<float>(wh), input_.pan_dy / static_cast<float>(wh));
  }
  if (input_.scroll != 0.0f) {
    flying_ = false;
    camera_.zoom(input_.scroll);
  }
  input_.orbit_dx = input_.orbit_dy = input_.pan_dx = input_.pan_dy = input_.scroll = 0.0f;

  // Hub hover: CPU ray-sphere test against the hub markers (last frame's camera).
  int hover = -1;
  if (!input_.left_down && !input_.pan_down && !overButton) {
    const Ray ray = pixel_ray(camera_.position(), inv_view_proj_, static_cast<float>(cx), static_cast<float>(cy),
                              static_cast<float>(fbw), static_cast<float>(fbh));
    hover = pick_hub(ray, tax_, rs_.globe.radius);
  }
  hovered_hub_ = hover;

  if (input_.click) {
    input_.click = false;
    const double px = input_.click_x * sx, py = input_.click_y * sy;
    if (scrubbing_) {
      scrubbing_ = false;  // the release that ends a scrub (already applied above)
    } else if (live_ && show_hud_ && live_layout_.export_button.contains(px, py)) {
      request_export();
    } else if (timelineOn && live_layout_.timeline.contains(px, py)) {
      seek_to_cursor(live_layout_, px);
    } else {
      const Ray ray = pixel_ray(camera_.position(), inv_view_proj_, static_cast<float>(px), static_cast<float>(py),
                                static_cast<float>(fbw), static_cast<float>(fbh));
      const int hub = pick_hub(ray, tax_, rs_.globe.radius);
      if (hub >= 0 && live_) {
        pinned_hub_ = pinned_hub_ == hub ? -1 : hub;  // click again to unpin
        live_hud_dirty_ = true;
      } else {
        // Nodes keep the GPU id buffer (R32UI) pick.
        renderer_->request_pick(static_cast<int>(px), static_cast<int>(py));
      }
    }
  }
}

void Viewer::print_frame_summary() const {
  if (records_.empty()) return;
  std::vector<double> fm, cm;
  // Skip the first frames: shader compilation / first uploads are not steady state.
  const std::size_t skip = std::min<std::size_t>(records_.size() / 4, 10);
  for (std::size_t i = skip; i < records_.size(); ++i) {
    fm.push_back(records_[i].frame_ms);
    cm.push_back(records_[i].cpu_ms);
  }
  const auto pf = percentiles(fm), pc = percentiles(cm);
  const GpuTimers& t = renderer_->timers();
  const RenderCounters& c = renderer_->counters();
  std::printf("[render] %zu frames measured (first %zu skipped)\n", fm.size(), skip);
  std::printf("[render] frame ms   mean %.2f  p50 %.2f  p95 %.2f  max %.2f  => %.1f FPS (mean)\n", pf.mean, pf.p50,
              pf.p95, pf.max, pf.mean > 0 ? 1000.0 / pf.mean : 0.0);
  std::printf("[render] CPU ms     mean %.3f  p50 %.3f  p95 %.3f  max %.3f (render thread, excl. fence wait + swap)\n",
              pc.mean, pc.p50, pc.p95, pc.max);
  std::vector<double> up;
  for (std::size_t i = skip; i < records_.size(); ++i) {
    if (records_[i].upload_ms > 0.0f) up.push_back(records_[i].upload_ms);
  }
  if (!up.empty()) {
    const auto pu = percentiles(up);
    std::printf("[render] snapshot copy on %zu of %zu frames (<= %zu KiB per frame), ms mean %.3f  p50 %.3f  max %.3f\n",
                up.size(), fm.size(), Renderer::kUploadBytesPerFrame >> 10, pu.mean, pu.p50, pu.max);
  }
  std::printf("[render] GPU ms total %.2f:", static_cast<double>(t.total_ms()));
  for (std::size_t p = 0; p < kGpuPassCount; ++p) {
    std::printf(" %s %.3f", kGpuPassNames[p], static_cast<double>(t.ms(static_cast<GpuPass>(p))));
  }
  std::printf("\n[render] draws/frame %u (scene %u, post %u), arcs %s (intra %s + inter %s), allocations %" PRIu64
              "%s\n",
              c.draw_calls, c.scene_draws, c.post_draws,
              grp(static_cast<std::uint64_t>(c.visible_intra) + c.visible_inter).c_str(),
              grp(c.visible_intra).c_str(), grp(c.visible_inter).c_str(), frame_stats_.allocs_total,
              frame_stats_.alloc_counting ? "" : " (counting only in Debug builds)");
  if (frame_stats_.alloc_counting) {
    std::printf("[render] driver allocations inside GL calls on the render thread: %" PRIu64
                " (third-party code, reported but not asserted)\n",
                foreign_allocs_);
  }
}

int Viewer::run() {
  if (!create_gpu_side()) return 1;

  camera_.configure(rs_.camera_fov_deg, rs_.camera_near, rs_.camera_damping);
  camera_.set_limits(rs_.globe.radius * 1.002f);
  float replayTime = cli_.start_time >= 0.0f ? cli_.start_time : 0.0f;
  {
    ViewSetup v;
    std::string err;
    const NodePositionFn pos = [this](std::uint32_t n) { return renderer_->node_world_position(n); };
    const std::string preset = !cli_.camera_preset.empty() ? cli_.camera_preset
                               : cli_.fly_hub >= 1 && static_cast<std::size_t>(cli_.fly_hub) <= tax_.size()
                                   ? "hub:" + tax_.hubs()[static_cast<std::size_t>(cli_.fly_hub - 1)].id
                                   : std::string("globe");
    if (!camera_preset(preset, tax_, whales_, pos, rs_, v, err)) {
      FTV_LOG_ERROR("%s", err.c_str());
      destroy_gpu_side();
      return 1;
    }
    if (cli_.camera_preset.empty() && cli_.cam_pitch_deg > -900.0f) {
      v.yaw = glm::radians(cli_.cam_yaw_deg);
      v.pitch = glm::radians(cli_.cam_pitch_deg);
      if (cli_.cam_distance > 0.0f) v.distance = cli_.cam_distance;
    }
    camera_.set_immediate(v.target, v.distance, v.yaw, v.pitch);
    if (!cli_.camera_preset.empty() && cli_.start_time < 0.0f) replayTime = v.time;
  }
  inter_detail_ = cli_.inter_detail;
  show_hud_ = cli_.show_hud;
  if (!cli_.pin_hub.empty()) {
    if (const auto h = tax_.find(cli_.pin_hub)) pinned_hub_ = static_cast<int>(*h);
  }
  if (live_) export_seen_ = live_->exporter().completed();
  double sinceStartMs = 0.0;
  bool exportAfterDone = cli_.export_after < 0.0f;
  if (cli_.select_node >= 0 && static_cast<std::size_t>(cli_.select_node) < graph_.node_count()) {
    renderer_->set_selection(static_cast<std::uint32_t>(cli_.select_node));
    frame_selection();
    camera_.update(1e3f);  // jump straight to the framed view
  }

  const unsigned frameLimit = cli_.frames;
  records_.reserve(frameLimit ? frameLimit : 1u);
  frame_stats_.alloc_counting = allocation_counting_enabled();

  Stopwatch clock;
  double sinceHudRefresh = 1e9;
  double frameEma = 16.0, cpuEma = 1.0, cpuPeak = 0.0;
  unsigned frame = 0;
  bool allocWarned = false;

  while (!glfwWindowShouldClose(window_.handle())) {
    // ---- frame body: must not allocate (asserted in Debug builds) ----------
    // The counted region spans the whole iteration: event polling (our GLFW
    // callbacks), input, pick readback, camera, HUD text and every GL call.
    // Only the exceptional paths that `continue` below (minimised window,
    // context re-creation) are excluded.
    const std::uint64_t alloc0 = thread_allocation_count();
    const std::uint64_t foreign0 = thread_foreign_allocation_count();
    glfwPollEvents();
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_.handle(), &fbw, &fbh);
    if (fbw <= 0 || fbh <= 0) {
      // Minimised: no GL work at all; sleep until something happens.
      glfwWaitEventsTimeout(0.1);
      clock.reset();
      continue;
    }
    if (window_.context_lost()) {
      FTV_LOG_ERROR("OpenGL context lost (GPU reset); re-creating the window and all GPU resources");
      destroy_gpu_side();
      if (!create_gpu_side()) return 1;
      continue;
    }

    const double dtMs = clock.lap_ms();
    const auto dt = static_cast<float>(std::min(dtMs, 250.0) / 1000.0);
    if (!replay_paused_) replayTime = wrap_time(replayTime + dt, rs_.replay_seconds);
    frameEma += (dtMs - frameEma) * 0.08;
    sinceStartMs += dtMs;

    Stopwatch cpu;
    live_layout_ = LiveHud::layout(fbw, fbh, hud_stats_.left_bottom(fbh));
    handle_input(fbw, fbh);
    std::optional<std::uint32_t> picked;
    if (renderer_->poll_pick(picked)) {
      renderer_->set_selection(picked);
      selection_changed_ = true;
    }
    if (flying_) {
      fly_t_ += dt / kPresetFlightSeconds;
      const CameraPose pose = ease_pose(fly_from_, fly_to_, fly_t_);
      camera_.set_immediate(pose.target, pose.distance, pose.yaw, pose.pitch);
      if (fly_t_ >= 1.0f) flying_ = false;
    }
    camera_.update(dt);

    // Live layer: clocks, whale arcs from the model thread, export results.
    if (live_) {
      ModelThread& model = live_->model();
      if (!paused_) live_clock_ += dt;
      if (live_->mode() == ModelMode::Replay) {
        // The live arc clock follows the replay timeline (also across seeks).
        const float pos = static_cast<float>(model.position_ms()) * 1e-3f;
        if (std::fabs(pos - live_clock_) > 0.15f) live_clock_ = pos;
      }
      live_->pump_visuals(*renderer_, live_clock_);
      if (!exportAfterDone && sinceStartMs >= static_cast<double>(cli_.export_after) * 1000.0) {
        exportAfterDone = true;
        export_pending_exit_ = true;
        request_export();
      }
      const std::uint64_t done = live_->exporter().completed();
      if (done != export_seen_) {
        export_seen_ = done;
        char text[512];
        bool ok = false;
        live_->exporter().last_result(text, sizeof text, ok);
        toast_.clear();
        for (const char* c = text; *c; ++c) {
          if (c[0] == '-' && c[1] == '>') {
            toast_.ch(kGlyphArrow);
            ++c;
          } else {
            toast_.ch(*c);
          }
        }
        toast_left_ms_ = 3000.0;
        live_in_.toast_ok = ok;
        std::printf("[export] %s\n", text);
        if (export_pending_exit_) glfwSetWindowShouldClose(window_.handle(), GLFW_TRUE);
      }
      toast_left_ms_ = std::max(0.0, toast_left_ms_ - dtMs);
      live_in_.toast = toast_left_ms_ > 0.0 ? toast_.view() : std::string_view();
    }

    renderer_->begin_frame(engine_.snapshots());

    sinceHudRefresh += dtMs;
    if (show_hud_) {
      if (sinceHudRefresh >= 250.0 || selection_changed_) {
        frame_stats_.fps = frameEma > 0.0 ? 1000.0 / frameEma : 0.0;
        frame_stats_.frame_ms = frameEma;
        frame_stats_.cpu_ms = cpuEma;
        frame_stats_.cpu_peak_ms = cpuPeak;
        frame_stats_.inter_detail = inter_detail_;
        frame_stats_.replay_paused = paused_;
        frame_stats_.replay_time = replayTime;
        hud_stats_.refresh(frame_stats_, *renderer_, engine_.status(), renderer_->selection());
        sinceHudRefresh = 0.0;
        cpuPeak = 0.0;
        selection_changed_ = false;
        live_hud_dirty_ = true;
      }
      hud_stats_.emit(renderer_->hud(), fbw, fbh);
      if (live_) {
        if (live_hud_dirty_) {
          refresh_live_hud();  // 4 Hz, and at once when the pin, pause or timeline changes
          live_hud_dirty_ = false;
        }
        live_hud_.emit(renderer_->hud(), live_layout_, live_in_);
      }
    }

    FrameParams p;
    const float aspect = static_cast<float>(fbw) / static_cast<float>(fbh);
    p.view = camera_.view();
    p.proj = camera_.projection(aspect);
    p.camera_pos = camera_.position();
    p.time = replayTime;
    p.near_plane = camera_.near_plane();
    p.width = fbw;
    p.height = fbh;
    p.inter_detail = inter_detail_;
    p.show_hud = show_hud_;
    p.live_time = live_clock_;
    p.hovered_hub = hovered_hub_;
    p.pinned_hub = pinned_hub_;
    // Terminator: --sun-time, else the model clock (wall clock LIVE, capture
    // time REPLAY), else the wall clock.
    p.sun_dir = sun_vector(cli_.sun_time_set ? cli_.sun_time_ms : model_now_ms_ > 0 ? model_now_ms_ : wall_clock_ms());
    inv_view_proj_ = glm::inverse(p.proj * p.view);
    renderer_->draw_frame(p);

    const double cpuMs = std::max(0.0, cpu.millis() - static_cast<double>(renderer_->counters().fence_wait_ms));
    const std::uint64_t allocs = thread_allocation_count() - alloc0;
    foreign_allocs_ += thread_foreign_allocation_count() - foreign0;
    // ---- end of frame body -------------------------------------------------

    cpuEma += (cpuMs - cpuEma) * 0.08;
    cpuPeak = std::max(cpuPeak, cpuMs);
    frame_stats_.allocs_last_frame = allocs;
    frame_stats_.allocs_total += allocs;
    if (allocs != 0 && frame_stats_.alloc_counting) {
      if (cli_.alloc_assert) {
        std::fprintf(stderr, "FATAL: %" PRIu64 " heap allocation(s) on the render thread during frame %u "
                             "(zero allocations per frame is required; --no-alloc-assert downgrades this)\n",
                     allocs, frame);
        std::abort();
      }
      if (!allocWarned) {
        FTV_LOG_WARN("%" PRIu64 " heap allocation(s) during frame %u", allocs, frame);
        allocWarned = true;
      }
    }

    if (frameLimit) {
      records_.push_back(
          FrameRecord{static_cast<float>(dtMs), static_cast<float>(cpuMs), renderer_->counters().upload_ms});
    }
    glfwSwapBuffers(window_.handle());
    ++frame;
    if (frameLimit && frame >= frameLimit) break;
  }

  if (frameLimit) print_frame_summary();
  if (live_) {
    live_->print_feed_report();
    if (frame_stats_.alloc_counting) {
      std::printf("[model] allocations on the model thread while ticking: %" PRIu64 "\n", live_->model().allocations());
    }
  }
  destroy_gpu_side();
  return 0;
}

// ---------------------------------------------------------------------------
// Screenshot mode: offscreen context, fixed time step, PNG output
// ---------------------------------------------------------------------------

int render_screenshot(const CliOptions& cli, const RenderSettings& rs, const std::string& shaderDir,
                      const Graph& graph, const HubTaxonomy& tax, const WhaleSet& whales, const CoastlineSet& coast,
                      const EarthData& earth, LayoutEngine& engine, LiveSession* live) {
  GlWindow ctx;
  std::string err;
  if (!ctx.create_offscreen(cli.gl_debug, err)) {
    FTV_LOG_ERROR("%s", err.c_str());
    return 1;
  }
  int code = 0;
  {
    Renderer renderer(graph, tax, whales, coast, rs, ctx.caps(), shaderDir);
    renderer.set_earth(earth.assets());
    if (!renderer.init(err)) {
      FTV_LOG_ERROR("renderer initialisation failed:\n%s", err.c_str());
      return 1;
    }
    engine.snapshots().acquire_latest();
    renderer.prime_positions(engine.snapshots().front());

    OrbitCamera camera;
    camera.configure(rs.camera_fov_deg, rs.camera_near, rs.camera_damping);
    ViewSetup v;
    const NodePositionFn pos = [&renderer](std::uint32_t n) { return renderer.node_world_position(n); };
    if (!camera_preset(cli.camera_preset.empty() ? std::string("globe") : cli.camera_preset, tax, whales, pos, rs, v,
                       err)) {
      FTV_LOG_ERROR("%s", err.c_str());
      return 1;
    }
    camera.set_immediate(v.target, v.distance, v.yaw, v.pitch);
    // Fixed simulated time: warm-up frames step 1/60 s and the captured frame
    // lands on the requested replay time, so the output is reproducible.
    const float target = cli.start_time >= 0.0f ? cli.start_time : v.time;
    const unsigned warm = cli.warmup_frames;
    const int w = rs.window_width, h = rs.window_height;
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    Stopwatch sw;
    // The live layer is stepped synchronously (no threads) in fixed 100 ms
    // steps up to the capture time, so the model state, the live whale arcs
    // and the HUD text are the same on every run.
    LiveHud liveHud(tax, live ? live->mode() : ModelMode::Dataset, nullptr, live ? &live->config() : nullptr);
    LiveHudInput hudIn;
    if (live) {
      const auto targetMs = static_cast<std::int64_t>(std::llround(static_cast<double>(target) * 1000.0));
      for (std::int64_t t = 0; t < targetMs; t += 100) {
        live->model().step(std::min<std::int64_t>(100, targetMs - t));
        live->pump_visuals(renderer, target);
      }
      if (!cli.pin_hub.empty()) {
        if (const auto hub = tax.find(cli.pin_hub)) hudIn.pinned_hub = static_cast<int>(*hub);
      }
      hudIn.timeline_visible = live->mode() == ModelMode::Replay;
      hudIn.timeline_pos_s = static_cast<double>(live->model().timeline_ms()) / 1000.0;
      hudIn.timeline_len_s = static_cast<double>(live->model().timeline_duration_ms()) / 1000.0;
      hudIn.capture_synthetic = live->capture() && live->capture()->synthetic();
      if (const LiveSnapshot* snap = live->model().snapshots().acquire()) {
        liveHud.refresh(*snap, hudIn);
        live->model().snapshots().release(snap);
      }
    }
    const bool overlay = cli.overlay && live;
    std::int64_t sunMs = kDefaultScreenshotSunMs;
    if (cli.sun_time_set) {
      sunMs = cli.sun_time_ms;
    } else if (live) {
      if (const LiveSnapshot* snap = live->model().snapshots().acquire()) {
        if (snap->now_ms > 0) sunMs = snap->now_ms;
        live->model().snapshots().release(snap);
      }
    }
    {
      char iso[32];
      format_iso8601_utc(sunMs, iso, sizeof(iso));
      std::printf("[screenshot] sun time %s\n", iso);
    }
    const LiveHudLayout hudLayout = LiveHud::layout(w, h, 10.0f * HudStats::text_scale(h));
    for (unsigned f = 0; f <= warm; ++f) {
      renderer.begin_frame(engine.snapshots());
      if (overlay) liveHud.emit(renderer.hud(), hudLayout, hudIn);
      FrameParams p;
      p.view = camera.view();
      p.proj = camera.projection(aspect);
      p.camera_pos = camera.position();
      p.time = target - static_cast<float>(warm - f) / 60.0f;
      p.live_time = p.time;
      p.pinned_hub = hudIn.pinned_hub;
      p.near_plane = camera.near_plane();
      p.width = w;
      p.height = h;
      p.inter_detail = cli.inter_detail;
      p.show_hud = overlay;
      p.present = false;
      p.sun_dir = sun_vector(sunMs);
      renderer.draw_frame(p);
    }
    std::vector<std::uint8_t> rgba;
    int ow = 0, oh = 0;
    if (!renderer.read_output_rgba8(rgba, ow, oh) || !write_png_rgb(cli.screenshot, rgba, ow, oh)) {
      FTV_LOG_ERROR("could not write screenshot %s", cli.screenshot.c_str());
      code = 1;
    } else {
      const std::uint64_t digest = XXH3_64bits(rgba.data(), rgba.size());
      std::printf("[screenshot] %s: %dx%d, %u warm-up frames, replay t=%.3f s, %zu whales in flight, "
                  "pixel digest %016" PRIx64 ", %.1f s\n",
                  cli.screenshot.c_str(), ow, oh, warm, static_cast<double>(target),
                  static_cast<std::size_t>(renderer.counters().active_whales), digest, sw.seconds());
    }
  }
  ctx.destroy();
  return code;
}

}  // namespace

// ---------------------------------------------------------------------------
// Program flow
// ---------------------------------------------------------------------------

int run_application(const CliOptions& cli, const std::string& exeDir) {
  Stopwatch total;
  std::string err;

  // ---- configuration ---------------------------------------------------------
  (void)exeDir;
  const std::string hubsPath = resolve_path(cli.hubs_path, cli.data_dir, "config/hubs.json");
  const std::string renderPath = resolve_path(cli.render_path, cli.data_dir, "config/render.json");
  const std::string themePath = resolve_path(cli.theme_path, cli.data_dir, "config/theme.json");
  const std::string shaderDir = resolve_path(cli.shader_dir, cli.data_dir, "shaders");
  const std::string feedsPath = resolve_path(cli.feeds_path, cli.data_dir, "config/feeds.json");
  const std::string assetsDir = resolve_path(std::string(), cli.data_dir, "assets");

  auto taxonomy = HubTaxonomy::load(hubsPath, err);
  if (!taxonomy) {
    FTV_LOG_ERROR("hub taxonomy: %s", err.c_str());
    return 1;
  }
  const LayoutParams layoutParams = LayoutParams::from_json(taxonomy->layout_json());
  RenderSettings rs;
  if (!cli.headless) {
    auto loaded = load_render_settings(renderPath, err);
    if (!loaded) {
      FTV_LOG_ERROR("render settings: %s", err.c_str());
      return 1;
    }
    rs = *loaded;
    auto theme = load_theme(themePath, err);
    if (!theme) {
      FTV_LOG_ERROR("theme: %s", err.c_str());
      return 1;
    }
    rs.theme = *theme;
    if (cli.width > 0) rs.window_width = cli.width;
    if (cli.height > 0) rs.window_height = cli.height;
    if (cli.vsync >= 0) rs.vsync = cli.vsync != 0;
    if (cli.msaa >= 1) rs.msaa_samples = std::min(cli.msaa, 16);
    if (cli.ui_scale > 0.0f) rs.ui_scale = cli.ui_scale;
  }
  std::printf("[config] hubs %s (%zu hubs), render %s, theme %s, shaders %s\n", hubsPath.c_str(), taxonomy->size(),
              cli.headless ? "-" : renderPath.c_str(), cli.headless ? "-" : themePath.c_str(),
              cli.headless ? "-" : shaderDir.c_str());

  // ---- threads ------------------------------------------------------------------
  // Interactive runs keep one core free for the render thread / driver; the
  // headless benchmark uses every core for the layout.
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  unsigned poolWorkers = 0;
  if (cli.threads > 0) {
    poolWorkers = cli.threads - 1;
  } else {
    poolWorkers = cli.headless ? default_pool_workers() : (hw > 2 ? hw - 2 : 1u);
  }
  ThreadPool pool(poolWorkers);

  // ---- ingest ---------------------------------------------------------------------
  IngestOptions io;
  io.path = cli.input;
  io.format = cli.format;
  io.parser_threads = cli.parser_threads;
  io.chunk_bytes = cli.chunk_mb << 20;
  io.log_malformed = cli.log_malformed;
  IngestResult ingest;
  const bool haveInput = !cli.input.empty();
  if (haveInput && !run_ingest(io, pool, ingest, err)) {
    FTV_LOG_ERROR("ingest failed: %s", err.c_str());
    return 1;
  }
  // Without --input the graph is empty: the globe, hubs, coastlines and the
  // live layer (feeds, modeled corridors, whales) are the whole picture.
  EdgeList edges = aggregate_edges(ingest, pool);
  if (haveInput) {
    print_ingest_report(ingest, cli.input);
  } else {
    std::printf("[ingest] no --input: globe and live layer only\n");
  }
  if (haveInput && ingest.entity_count() == 0) {
    FTV_LOG_ERROR("no valid transactions in %s; nothing to visualise", cli.input.c_str());
    return 1;
  }

  // ---- model ----------------------------------------------------------------------
  Stopwatch sw;
  Graph graph = build_graph(ingest, std::move(edges), *taxonomy, pool);
  const double buildMs = sw.lap_ms();
  const WhaleSet whales =
      build_whales(ingest, graph, rs.replay_seconds, rs.theme.whale_flight_s + rs.theme.whale_flash_s);
  print_model_report(graph, *taxonomy, whales, buildMs);

  // ---- layout -----------------------------------------------------------------------
  ForceSolver solver(graph, *taxonomy, layoutParams, pool);
  std::printf("[startup] ready to lay out after %.1f s\n", total.seconds());
  if (cli.headless) return run_headless(cli, solver, graph, *taxonomy, pool.concurrency());

  const IterationStats pre = run_layout_iterations(solver, cli.prelayout, false, "prelayout", nullptr);

  // ---- coastlines and the live layer ------------------------------------------------
  CoastlineSet coast;
  if (!load_coastlines(assetsDir, coast, err)) {
    FTV_LOG_ERROR("coastlines: %s", err.c_str());
    return 1;
  }
  std::printf("[assets] coastlines: %zu segments (110m)%s, Natural Earth, public domain\n", coast.coarse_count,
              coast.fine_count ? " + 50m close-zoom set" : "");
  EarthData earth;
  {
    std::string e;
    earth.cables = load_cables(assetsDir + "/cables.bin", e);
    if (!earth.cables) {
      FTV_LOG_WARN("submarine cables off: %s", e.c_str());
    } else {
      std::vector<HubSite> sites;
      for (const HubDef& d : taxonomy->hubs()) {
        sites.push_back(HubSite{d.lat_deg, d.lon_deg, d.type == HubType::FiatCenter});
      }
      earth.routes = build_routes(*earth.cables, sites);
      std::printf("[assets] cables: %zu cables, %zu segments; %zu MODELED hub routes in %.0f ms\n",
                  earth.cables->cables.size(), earth.cables->segment_count(), earth.routes.routed_pairs,
                  earth.routes.build_ms);
    }
    earth.lights = load_city_lights(assetsDir + "/citylights.bin", e);
    if (!earth.lights) {
      FTV_LOG_WARN("city lights off: %s", e.c_str());
    } else {
      std::printf("[assets] city lights: %ux%u BC4, %u mips (NASA Black Marble)\n", earth.lights->width,
                  earth.lights->height, earth.lights->levels);
    }
  }
  LiveSessionOptions lo;
  lo.mode = cli.live ? ModelMode::Live : cli.replay ? ModelMode::Replay : ModelMode::Dataset;
  lo.feeds_path = feedsPath;
  lo.capture_path = cli.replay_path;
  lo.base_dir = is_embedded_path(feedsPath) ? std::string() : fs::path(feedsPath).parent_path().parent_path().string();
  lo.export_dir = cli.export_dir;
  std::unique_ptr<LiveSession> live = LiveSession::create(lo, *taxonomy, err);
  if (!live) {
    if (lo.mode != ModelMode::Dataset) {
      FTV_LOG_ERROR("live layer: %s", err.c_str());
      return 1;
    }
    FTV_LOG_WARN("live layer disabled (modeled corridors, hub panel and export unavailable): %s", err.c_str());
  } else {
    std::printf("[live] mode %s, feeds %s\n", kModelModeNames[static_cast<std::size_t>(lo.mode)], feedsPath.c_str());
    live->set_arc_lifetime(rs.theme.whale_flight_s + rs.theme.whale_flash_s);
  }

  LayoutEngine engine(solver, graph.node_count());
  // With a prelayout, its last iteration seeds the HUD timings.
  engine.publish_now(cli.prelayout > 0 ? &pre : nullptr);
  if (!cli.screenshot.empty()) {
    // Screenshots capture the pre-laid-out state exactly (layout thread never starts).
    return render_screenshot(cli, rs, shaderDir, graph, *taxonomy, whales, coast, earth, engine, live.get());
  }
  if (cli.start_paused) engine.set_paused(true);
  engine.start();
  if (live) {
#if defined(FTVIZ_COUNT_ALLOCATIONS)
    live->model().set_alloc_counter(&thread_allocation_count);
#endif
    live->start();
  }

  const std::string inputName = haveInput ? fs::path(cli.input).filename().string() : std::string("live");
  int code = 0;
  {
    Viewer viewer(cli, rs, shaderDir, graph, *taxonomy, whales, coast, earth, ingest, engine, live.get(), inputName);
    code = viewer.run();
  }
  if (live) live->stop();
  engine.stop();
  return code;
}

}  // namespace ftv
