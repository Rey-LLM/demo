#include "app/application.hpp"

#include <algorithm>
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

#include "app/hud_stats.hpp"
#include "app/render_config.hpp"
#include "core/alloc_counter.hpp"
#include "core/fixed_text.hpp"
#include "core/log.hpp"
#include "core/thread_pool.hpp"
#include "core/timing.hpp"
#include "ingest/ingest.hpp"
#include "layout/force_solver.hpp"
#include "layout/layout_engine.hpp"
#include "layout/layout_params.hpp"
#include "model/corridors.hpp"
#include "model/edge_aggregation.hpp"
#include "model/graph_builder.hpp"
#include "model/hub_taxonomy.hpp"
#include "render/camera.hpp"
#include "render/gl_context.hpp"
#include "render/renderer.hpp"

#ifndef FTVIZ_SOURCE_DIR
#define FTVIZ_SOURCE_DIR "."
#endif

namespace ftv {

namespace {

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

// First existing path among: explicit option, <exe>/<rel>, <source>/<rel>.
std::string resolve_path(const std::string& explicitPath, const std::string& exeDir, const char* rel) {
  if (!explicitPath.empty()) return explicitPath;
  std::error_code ec;
  const fs::path candidates[] = {fs::path(exeDir) / rel, fs::path(FTVIZ_SOURCE_DIR) / rel, fs::path(rel)};
  for (const fs::path& c : candidates) {
    if (fs::exists(c, ec)) return c.string();
  }
  return (fs::path(exeDir) / rel).string();
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

void print_model_report(const Graph& g, const HubTaxonomy& tax, std::size_t bundles, double buildMs,
                        double corridorMs) {
  std::printf("[model] graph %s nodes, %s edges (%s inter-hub, %.2f%%), %s bridges, %s dropped; build %.1f ms\n",
              grp(g.node_count()).c_str(), grp(g.edge_count()).c_str(), grp(g.inter_hub_edges).c_str(),
              g.edge_count() ? 100.0 * static_cast<double>(g.inter_hub_edges) / static_cast<double>(g.edge_count()) : 0.0,
              grp(g.bridge_count).c_str(), grp(g.dropped_edges).c_str(), buildMs);
  std::printf("[model] %zu corridor bundles (%.1f ms), %zu currencies\n", bundles, corridorMs, g.currencies.size());
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
  bool pan_down = false;
  double drag_travel = 0.0;
  float orbit_dx = 0.0f, orbit_dy = 0.0f;
  float pan_dx = 0.0f, pan_dy = 0.0f;
  float scroll = 0.0f;
  bool click = false;
  double click_x = 0.0, click_y = 0.0;
  std::array<int, 32> keys{};
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
  glfwSetKeyCallback(w, [](GLFWwindow* win, int key, int, int action, int) {
    InputState* s = input_of(win);
    if (action == GLFW_PRESS && s->key_count < s->keys.size()) s->keys[s->key_count++] = key;
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

class Viewer {
 public:
  Viewer(const CliOptions& cli, const RenderSettings& rs, const std::string& shaderDir, const Graph& graph,
         const HubTaxonomy& tax, const std::vector<CorridorBundle>& bundles, const IngestResult& ingest,
         LayoutEngine& engine, std::string inputName)
      : cli_(cli),
        rs_(rs),
        shader_dir_(shaderDir),
        graph_(graph),
        tax_(tax),
        bundles_(bundles),
        engine_(engine),
        title_("ftviz - " + inputName),
        hud_stats_(graph, tax, ingest, inputName) {}

  int run();

 private:
  bool create_gpu_side();
  void destroy_gpu_side();
  void handle_input(int fbw, int fbh);
  void handle_key(int key);
  void frame_selection();
  void fly_to_hub(std::size_t hub);
  void print_frame_summary() const;

  const CliOptions& cli_;
  RenderSettings rs_;
  std::string shader_dir_;
  const Graph& graph_;
  const HubTaxonomy& tax_;
  const std::vector<CorridorBundle>& bundles_;
  LayoutEngine& engine_;
  std::string title_;

  GlWindow window_;
  std::unique_ptr<Renderer> renderer_;
  InputState input_;
  OrbitCamera camera_;
  HudStats hud_stats_;
  FrameStats frame_stats_;
  bool show_hud_ = true;
  bool inter_detail_ = true;
  bool color_by_channel_ = false;
  bool selection_changed_ = true;
  std::uint64_t foreign_allocs_ = 0;  // driver allocations inside GL calls (Debug counting only)
  std::vector<FrameRecord> records_;
};

bool Viewer::create_gpu_side() {
  std::string err;
  const bool hidden = false;  // even screenshot runs map the window: an unmapped window's back buffer is undefined
  if (!window_.create(rs_.window_width, rs_.window_height, title_.c_str(), rs_.vsync, cli_.gl_debug, hidden, err)) {
    FTV_LOG_ERROR("%s", err.c_str());
    return false;
  }
#if defined(FTVIZ_COUNT_ALLOCATIONS) && defined(GLAD_OPTION_GL_DEBUG)
  gladSetGLPreCallback(&gl_pre_call_hook);
  gladSetGLPostCallback(&gl_post_call_hook);
#endif
  install_callbacks(window_.handle(), &input_);
  renderer_ = std::make_unique<Renderer>(graph_, tax_, bundles_, rs_, window_.caps(), shader_dir_);
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

void Viewer::fly_to_hub(std::size_t hub) {
  if (hub >= tax_.size()) return;
  const HubDef& d = tax_.hubs()[hub];
  camera_.fly_to(d.anchor, d.radius / std::sin(camera_.fov_y() * 0.5f) * 1.35f);
}

void Viewer::frame_selection() {
  const auto sel = renderer_->selection();
  if (!sel) {
    camera_.fly_to(glm::vec3(0.0f), rs_.camera_start_distance);
    return;
  }
  const glm::vec3 c = renderer_->node_position(*sel);
  float extent = 0.0f;
  const auto& adj = graph_.adjacency;
  for (std::uint32_t k = adj.offset[*sel]; k < adj.offset[*sel + 1]; ++k) {
    extent = std::max(extent, glm::length(renderer_->node_position(adj.neighbor[k]) - c));
  }
  extent = std::clamp(extent, 15.0f, 2500.0f);
  camera_.fly_to(c, extent / std::sin(camera_.fov_y() * 0.5f) * 1.1f);
}

void Viewer::handle_key(int key) {
  if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) {
    fly_to_hub(static_cast<std::size_t>(key - GLFW_KEY_1));
    return;
  }
  switch (key) {
    case GLFW_KEY_0:
      camera_.fly_to(glm::vec3(0.0f), rs_.camera_start_distance);
      break;
    case GLFW_KEY_F:
      frame_selection();
      break;
    case GLFW_KEY_E:
      inter_detail_ = !inter_detail_;
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
    case GLFW_KEY_C:
      color_by_channel_ = !color_by_channel_;
      break;
    case GLFW_KEY_ESCAPE:
      if (renderer_->selection()) {
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

  for (unsigned i = 0; i < input_.key_count; ++i) handle_key(input_.keys[i]);
  input_.key_count = 0;

  if (input_.orbit_dx != 0.0f || input_.orbit_dy != 0.0f) {
    camera_.orbit(-input_.orbit_dx * 0.005f, input_.orbit_dy * 0.005f);
  }
  if (input_.pan_dx != 0.0f || input_.pan_dy != 0.0f) {
    camera_.pan(input_.pan_dx / static_cast<float>(wh), input_.pan_dy / static_cast<float>(wh));
  }
  if (input_.scroll != 0.0f) camera_.zoom(input_.scroll);
  input_.orbit_dx = input_.orbit_dy = input_.pan_dx = input_.pan_dy = input_.scroll = 0.0f;

  if (input_.click) {
    input_.click = false;
    // Cursor coordinates are in window units; the pick target is in pixels.
    const int px = static_cast<int>(input_.click_x * static_cast<double>(fbw) / ww);
    const int py = static_cast<int>(input_.click_y * static_cast<double>(fbh) / wh);
    renderer_->request_pick(px, py);
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
  std::printf("\n[render] draws/frame %u, visible edges %s (intra %s + inter %s), capacity %s, allocations %" PRIu64
              "%s\n",
              c.draw_calls, grp(static_cast<std::uint64_t>(c.visible_intra) + c.visible_inter).c_str(),
              grp(c.visible_intra).c_str(), grp(c.visible_inter).c_str(), grp(renderer_->visible_capacity()).c_str(),
              frame_stats_.allocs_total, frame_stats_.alloc_counting ? "" : " (counting only in Debug builds)");
  if (frame_stats_.alloc_counting) {
    std::printf("[render] driver allocations inside GL calls on the render thread: %" PRIu64
                " (third-party code, reported but not asserted)\n",
                foreign_allocs_);
  }
}

int Viewer::run() {
  if (!create_gpu_side()) return 1;

  camera_.configure(rs_.camera_fov_deg, rs_.camera_near, rs_.camera_damping);
  const float pitch = glm::radians(cli_.cam_pitch_deg > -900.0f ? cli_.cam_pitch_deg : rs_.camera_start_pitch_deg);
  const float dist = cli_.cam_distance > 0.0f ? cli_.cam_distance : rs_.camera_start_distance;
  camera_.set_immediate(glm::vec3(0.0f), dist, glm::radians(cli_.cam_yaw_deg), pitch);
  if (cli_.fly_hub >= 1 && static_cast<std::size_t>(cli_.fly_hub) <= tax_.size()) {
    const HubDef& d = tax_.hubs()[static_cast<std::size_t>(cli_.fly_hub - 1)];
    camera_.set_immediate(d.anchor, d.radius / std::sin(camera_.fov_y() * 0.5f) * 1.35f,
                          glm::radians(cli_.cam_yaw_deg), pitch);
  }
  inter_detail_ = cli_.inter_detail;
  show_hud_ = cli_.show_hud;
  if (cli_.select_node >= 0 && static_cast<std::size_t>(cli_.select_node) < graph_.node_count()) {
    renderer_->set_selection(static_cast<std::uint32_t>(cli_.select_node));
    frame_selection();
    camera_.update(1e3f);  // jump straight to the framed view
  }

  const bool screenshotMode = !cli_.screenshot.empty();
  const unsigned frameLimit = cli_.frames ? cli_.frames : (screenshotMode ? 6u : 0u);
  records_.reserve(frameLimit ? frameLimit : 1u);
  frame_stats_.alloc_counting = allocation_counting_enabled();

  Stopwatch clock;
  double sinceHudRefresh = 1e9;
  double frameEma = 16.0, cpuEma = 1.0, cpuPeak = 0.0;
  float animTime = 0.0f;
  unsigned frame = 0;
  bool allocWarned = false;
  int exitCode = 0;

  while (!glfwWindowShouldClose(window_.handle())) {
    // ---- frame body: must not allocate (asserted in Debug builds) ----------
    // The counted region spans the whole iteration: event polling (our GLFW
    // callbacks), input, pick readback, camera, HUD text and every GL call.
    // Only the exceptional paths that `continue` below (minimised window,
    // context re-creation) and the on-demand screenshot export are excluded.
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
    animTime += dt;
    frameEma += (dtMs - frameEma) * 0.08;

    Stopwatch cpu;
    handle_input(fbw, fbh);
    std::optional<std::uint32_t> picked;
    if (renderer_->poll_pick(picked)) {
      renderer_->set_selection(picked);
      selection_changed_ = true;
    }
    camera_.update(dt);

    renderer_->begin_frame(engine_.snapshots());

    sinceHudRefresh += dtMs;
    if (show_hud_) {
      if (sinceHudRefresh >= 250.0 || selection_changed_) {
        frame_stats_.fps = frameEma > 0.0 ? 1000.0 / frameEma : 0.0;
        frame_stats_.frame_ms = frameEma;
        frame_stats_.cpu_ms = cpuEma;
        frame_stats_.cpu_peak_ms = cpuPeak;
        frame_stats_.inter_detail = inter_detail_;
        frame_stats_.color_by_channel = color_by_channel_;
        hud_stats_.refresh(frame_stats_, *renderer_, engine_.status(), renderer_->selection());
        sinceHudRefresh = 0.0;
        cpuPeak = 0.0;
        selection_changed_ = false;
      }
      hud_stats_.emit(renderer_->hud(), fbw, fbh);
    }

    FrameParams p;
    const float aspect = static_cast<float>(fbw) / static_cast<float>(fbh);
    p.view = camera_.view();
    p.proj = camera_.projection(aspect);
    p.camera_pos = camera_.position();
    p.time = animTime;
    p.near_plane = camera_.near_plane();
    p.width = fbw;
    p.height = fbh;
    p.inter_detail = inter_detail_;
    p.color_by_channel = color_by_channel_;
    p.show_hud = show_hud_;
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
    if (screenshotMode && frame + 1 == frameLimit) {
      if (renderer_->save_screenshot(cli_.screenshot, fbw, fbh)) {
        std::printf("[render] screenshot written to %s (%dx%d)\n", cli_.screenshot.c_str(), fbw, fbh);
      } else {
        FTV_LOG_ERROR("could not write screenshot %s", cli_.screenshot.c_str());
        exitCode = 1;
      }
    }
    glfwSwapBuffers(window_.handle());
    ++frame;
    if (frameLimit && frame >= frameLimit) break;
  }

  if (frameLimit) print_frame_summary();
  destroy_gpu_side();
  return exitCode;
}

}  // namespace

// ---------------------------------------------------------------------------
// Program flow
// ---------------------------------------------------------------------------

int run_application(const CliOptions& cli, const std::string& exeDir) {
  Stopwatch total;
  std::string err;

  // ---- configuration ---------------------------------------------------------
  const std::string hubsPath = resolve_path(cli.hubs_path, exeDir, "config/hubs.json");
  const std::string renderPath = resolve_path(cli.render_path, exeDir, "config/render.json");
  const std::string shaderDir = resolve_path(cli.shader_dir, exeDir, "shaders");

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
    if (cli.width > 0) rs.window_width = cli.width;
    if (cli.height > 0) rs.window_height = cli.height;
    if (cli.vsync >= 0) rs.vsync = cli.vsync != 0;
    if (cli.msaa >= 1) rs.msaa_samples = std::min(cli.msaa, 16);
  }
  std::printf("[config] hubs %s (%zu hubs), render %s, shaders %s\n", hubsPath.c_str(), taxonomy->size(),
              cli.headless ? "-" : renderPath.c_str(), cli.headless ? "-" : shaderDir.c_str());

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
  if (!run_ingest(io, pool, ingest, err)) {
    FTV_LOG_ERROR("ingest failed: %s", err.c_str());
    return 1;
  }
  EdgeList edges = aggregate_edges(ingest, pool);
  print_ingest_report(ingest, cli.input);
  if (ingest.entity_count() == 0) {
    FTV_LOG_ERROR("no valid transactions in %s; nothing to visualise", cli.input.c_str());
    return 1;
  }

  // ---- model ----------------------------------------------------------------------
  Stopwatch sw;
  Graph graph = build_graph(ingest, std::move(edges), *taxonomy, pool);
  const double buildMs = sw.lap_ms();
  const std::vector<CorridorBundle> bundles = build_corridor_bundles(graph);
  const double corridorMs = sw.lap_ms();
  print_model_report(graph, *taxonomy, bundles.size(), buildMs, corridorMs);

  // ---- layout -----------------------------------------------------------------------
  ForceSolver solver(graph, *taxonomy, layoutParams, pool);
  std::printf("[startup] ready to lay out after %.1f s\n", total.seconds());
  if (cli.headless) return run_headless(cli, solver, graph, *taxonomy, pool.concurrency());

  const IterationStats pre = run_layout_iterations(solver, cli.prelayout, false, "prelayout", nullptr);

  LayoutEngine engine(solver, graph.node_count());
  // With a prelayout, its last iteration seeds the HUD timings, so frozen
  // (screenshot) runs report a measured iteration time instead of nothing.
  engine.publish_now(cli.prelayout > 0 ? &pre : nullptr);
  const bool screenshotMode = !cli.screenshot.empty();
  // Screenshots capture the pre-laid-out state exactly; otherwise T_layout
  // keeps refining in the background while the user explores.
  if (cli.start_paused) engine.set_paused(true);
  if (!screenshotMode) engine.start();

  const std::string inputName = fs::path(cli.input).filename().string();
  int code = 0;
  {
    Viewer viewer(cli, rs, shaderDir, graph, *taxonomy, bundles, ingest, engine, inputName);
    code = viewer.run();
  }
  engine.stop();
  return code;
}

}  // namespace ftv
