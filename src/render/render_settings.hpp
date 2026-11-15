#pragma once
// Renderer settings: config/render.json (window, budgets, globe, camera,
// replay) and config/theme.json (palette and element styling). Both are
// parsed by app/render_config.cpp.

#include <cstdint>

#include "model/globe.hpp"

namespace ftv {

// Palette and element styling. Colours are linear RGB, converted from the
// sRGB hex values in theme.json at load time.
struct Theme {
  float background[3] = {0.0f, 0.0f, 0.0f};  // "Obsidian"   #050608
  float graticule[3] = {0.0f, 0.0f, 0.0f};   // "Amber Line" #C8962E
  float ink[3] = {0.0f, 0.0f, 0.0f};         // nodes, arcs, labels #D9DCE1
  float whale[3] = {0.0f, 0.0f, 0.0f};       // "Deep Amber" #FF9A1F: tier-1 whale emissive
  float whale_hot[3] = {0.0f, 0.0f, 0.0f};   // "White Gold" #FFF1C9: tier-3 whales and every arc head
  float graticule_alpha = 0.08f;             // 0.06 .. 0.10
  float graticule_spacing_deg = 15.0f;
  float node_alpha_min = 0.04f;
  float node_alpha_max = 0.60f;
  float node_min_px = 1.0f;
  float node_max_px = 2.5f;
  float intra_arc_alpha_min = 0.015f;        // intra-cluster edges stay below 0.04
  float intra_arc_alpha_max = 0.035f;
  float inter_arc_alpha_min = 0.03f;         // 0.03 .. 0.10
  float inter_arc_alpha_max = 0.10f;
  float hub_ring_alpha = 0.70f;
  float hub_ring_px = 1.25f;
  float hub_ring_radius_px = 6.0f;
  float hub_dot_px = 2.0f;
  float label_alpha = 0.65f;
  float label_px = 11.0f;
  float label_tracking_em = 0.14f;
  float coast[3] = {0.0f, 0.0f, 0.0f};       // amber blended toward graphite #3A3328 (linear)
  float coast_alpha = 0.22f;                 // 0.18 .. 0.28
  float coast_close_altitude = 1400.0f;      // 1:50m below this camera altitude, 1:110m above
  float atmo[3] = {0.0f, 0.0f, 0.0f};        // amber rim (linear)
  float atmo_alpha = 0.20f;                  // <= 0.22
  float atmo_exponent = 5.0f;                // 4 .. 6
  float atmo_scale = 1.015f;
  float whale_intensity_min = 4.0f;          // HDR multipliers, tier 1 .. tier 3
  float whale_intensity_max = 8.0f;
  float whale_width_px[3] = {2.0f, 3.0f, 4.0f};
  float whale_flight_s = 1.6f;
  float whale_trail_s = 0.4f;
  float whale_flash_s = 0.5f;
  float bloom_strength = 0.6f;
  float exposure = 1.0f;
  float vignette = 0.025f;                   // <= 0.03
};

struct RenderSettings {
  int window_width = 1920;
  int window_height = 1080;
  bool vsync = false;
  int msaa_samples = 8;
  float ui_scale = 1.0f;                     // device px per logical px (discs, rings, labels)
  std::uint32_t max_visible_edges = 2000000;
  std::uint32_t max_inter_hub_edges = 4000;    // additive hairlines: more saturates the corridors
  std::uint32_t max_intra_hub_edges = 30000;   // likewise inside the hub discs
  float intra_edge_max_altitude = 900.0f;    // intra-hub edges hidden above this camera altitude
  int arc_segments = 32;
  GlobeParams globe;
  float replay_seconds = 120.0f;
  float camera_fov_deg = 40.0f;
  float camera_near = 0.5f;
  float camera_damping = 8.0f;
  float camera_start_distance = 4300.0f;
  float camera_start_yaw_deg = -10.0f;
  float camera_start_pitch_deg = 28.0f;
  Theme theme;
};

}  // namespace ftv
