#pragma once
// Renderer settings (config/render.json, parsed by app/render_config.cpp).

#include <cstdint>

namespace ftv {

struct RenderSettings {
  int window_width = 1920;
  int window_height = 1080;
  bool vsync = false;
  int msaa_samples = 4;
  std::uint32_t max_visible_edges = 2000000;
  float intra_edge_lod_percentile = 0.0f;  // drop intra edges below this weight percentile [0,1)
  std::uint32_t max_inter_hub_edges = 250000;
  float inter_hub_detail_distance = 900.0f;
  bool gpu_culling = true;
  float background[3] = {0.015f, 0.018f, 0.03f};
  float node_size_scale = 1.0f;
  float edge_alpha = 0.10f;
  float inter_edge_alpha = 0.35f;
  float corridor_width_px = 9.0f;
  std::uint32_t hub_particles_per_hub = 1536;
  float hub_glow_strength = 0.8f;
  float camera_fov_deg = 50.0f;
  float camera_near = 0.5f;
  float camera_damping = 8.0f;
  float camera_start_distance = 3600.0f;
  float camera_start_pitch_deg = 28.0f;
};

}  // namespace ftv
