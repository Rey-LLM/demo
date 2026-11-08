#include "app/render_config.hpp"

#include <algorithm>
#include <cmath>

#include "core/file_util.hpp"

namespace ftv {

namespace {

float num(const JsonValue& j, const char* key, float fallback, float lo, float hi) {
  const double v = j.number_or(key, fallback);
  if (!std::isfinite(v)) return fallback;
  return std::clamp(static_cast<float>(v), lo, hi);
}

std::uint32_t count(const JsonValue& j, const char* key, std::uint32_t fallback, std::uint32_t hi) {
  const double v = j.number_or(key, fallback);
  if (!std::isfinite(v) || v < 0.0) return fallback;
  return static_cast<std::uint32_t>(std::min(v, static_cast<double>(hi)));
}

}  // namespace

RenderSettings render_settings_from_json(const JsonValue& j) {
  RenderSettings s;
  s.window_width = static_cast<int>(num(j, "window_width", static_cast<float>(s.window_width), 320, 16384));
  s.window_height = static_cast<int>(num(j, "window_height", static_cast<float>(s.window_height), 240, 16384));
  s.vsync = j.bool_or("vsync", s.vsync);
  s.msaa_samples = static_cast<int>(num(j, "msaa_samples", static_cast<float>(s.msaa_samples), 1, 16));
  s.max_visible_edges = count(j, "max_visible_edges", s.max_visible_edges, 2000000u);  // spec cap: 2M
  s.intra_edge_lod_percentile = num(j, "intra_edge_lod_percentile", s.intra_edge_lod_percentile, 0.0f, 0.999f);
  s.max_inter_hub_edges = count(j, "max_inter_hub_edges", s.max_inter_hub_edges, 2000000u);
  s.inter_hub_detail_distance = num(j, "inter_hub_detail_distance", s.inter_hub_detail_distance, 0.0f, 1e6f);
  s.gpu_culling = j.bool_or("gpu_culling", s.gpu_culling);
  if (const JsonValue* bg = j.find("background"); bg && bg->is_array() && bg->items().size() >= 3) {
    for (int i = 0; i < 3; ++i) {
      s.background[i] = std::clamp(static_cast<float>(bg->items()[static_cast<std::size_t>(i)].as_number(s.background[i])), 0.0f, 1.0f);
    }
  }
  s.node_size_scale = num(j, "node_size_scale", s.node_size_scale, 0.05f, 50.0f);
  s.edge_alpha = num(j, "edge_alpha", s.edge_alpha, 0.0f, 1.0f);
  s.inter_edge_alpha = num(j, "inter_edge_alpha", s.inter_edge_alpha, 0.0f, 1.0f);
  s.corridor_width_px = num(j, "corridor_width_px", s.corridor_width_px, 0.5f, 64.0f);
  s.hub_particles_per_hub = count(j, "hub_particles_per_hub", s.hub_particles_per_hub, 65536u);
  if (s.hub_particles_per_hub < 1) s.hub_particles_per_hub = 1;
  s.hub_glow_strength = num(j, "hub_glow_strength", s.hub_glow_strength, 0.0f, 10.0f);
  s.camera_fov_deg = num(j, "camera_fov_deg", s.camera_fov_deg, 10.0f, 120.0f);
  s.camera_near = num(j, "camera_near", s.camera_near, 0.01f, 100.0f);
  s.camera_damping = num(j, "camera_damping", s.camera_damping, 0.1f, 100.0f);
  s.camera_start_distance = num(j, "camera_start_distance", s.camera_start_distance, 5.0f, 20000.0f);
  s.camera_start_pitch_deg = num(j, "camera_start_pitch_deg", s.camera_start_pitch_deg, -85.0f, 85.0f);
  return s;
}

std::optional<RenderSettings> load_render_settings(const std::string& path, std::string& error) {
  const auto text = read_text_file(path);
  if (!text) {
    error = "cannot read " + path;
    return std::nullopt;
  }
  std::string perr;
  const auto root = parse_json(*text, perr);
  if (!root) {
    error = path + ": " + perr;
    return std::nullopt;
  }
  if (!root->is_object()) {
    error = path + ": top-level value must be an object";
    return std::nullopt;
  }
  return render_settings_from_json(*root);
}

}  // namespace ftv
