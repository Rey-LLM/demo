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

std::optional<JsonValue> parse_file(const std::string& path, std::string& error) {
  const auto text = read_text_file(path);
  if (!text) {
    error = "cannot read " + path;
    return std::nullopt;
  }
  std::string perr;
  auto root = parse_json(*text, perr);
  if (!root) {
    error = path + ": " + perr;
    return std::nullopt;
  }
  if (!root->is_object()) {
    error = path + ": top-level value must be an object";
    return std::nullopt;
  }
  return root;
}

int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

float srgb_to_linear(float c) noexcept {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

bool parse_hex_color(const std::string& hex, float (&linear)[3]) noexcept {
  if (hex.size() != 7 || hex[0] != '#') return false;
  for (int i = 0; i < 3; ++i) {
    const int hi = hex_digit(hex[static_cast<std::size_t>(1 + 2 * i)]);
    const int lo = hex_digit(hex[static_cast<std::size_t>(2 + 2 * i)]);
    if (hi < 0 || lo < 0) return false;
    linear[i] = srgb_to_linear(static_cast<float>(hi * 16 + lo) / 255.0f);
  }
  return true;
}

RenderSettings render_settings_from_json(const JsonValue& j) {
  RenderSettings s;
  s.window_width = static_cast<int>(num(j, "window_width", static_cast<float>(s.window_width), 320, 16384));
  s.window_height = static_cast<int>(num(j, "window_height", static_cast<float>(s.window_height), 240, 16384));
  s.vsync = j.bool_or("vsync", s.vsync);
  s.msaa_samples = static_cast<int>(num(j, "msaa_samples", static_cast<float>(s.msaa_samples), 1, 16));
  s.ui_scale = num(j, "ui_scale", s.ui_scale, 0.5f, 4.0f);
  s.max_visible_edges = count(j, "max_visible_edges", s.max_visible_edges, 2000000u);
  s.max_inter_hub_edges = count(j, "max_inter_hub_edges", s.max_inter_hub_edges, 2000000u);
  s.max_intra_hub_edges = count(j, "max_intra_hub_edges", s.max_intra_hub_edges, 2000000u);
  s.intra_edge_max_altitude = num(j, "intra_edge_max_altitude", s.intra_edge_max_altitude, 0.0f, 1e6f);
  s.arc_segments = static_cast<int>(num(j, "arc_segments", static_cast<float>(s.arc_segments), 4, 128));
  s.globe.radius = num(j, "globe_radius", s.globe.radius, 10.0f, 1e5f);
  s.globe.node_altitude = num(j, "node_altitude", s.globe.node_altitude, 0.0f, 0.1f);
  s.globe.node_relief = num(j, "node_relief", s.globe.node_relief, 0.0f, 0.1f);
  s.globe.arc_lift = num(j, "arc_lift", s.globe.arc_lift, 0.0f, 2.0f);
  s.replay_seconds = num(j, "replay_seconds", s.replay_seconds, 10.0f, 86400.0f);
  s.camera_fov_deg = num(j, "camera_fov_deg", s.camera_fov_deg, 10.0f, 120.0f);
  s.camera_near = num(j, "camera_near", s.camera_near, 0.01f, 100.0f);
  s.camera_damping = num(j, "camera_damping", s.camera_damping, 0.1f, 100.0f);
  s.camera_start_distance = num(j, "camera_start_distance", s.camera_start_distance, 5.0f, 1e5f);
  s.camera_start_yaw_deg = num(j, "camera_start_yaw_deg", s.camera_start_yaw_deg, -360.0f, 360.0f);
  s.camera_start_pitch_deg = num(j, "camera_start_pitch_deg", s.camera_start_pitch_deg, -85.0f, 85.0f);
  return s;
}

std::optional<RenderSettings> load_render_settings(const std::string& path, std::string& error) {
  const auto root = parse_file(path, error);
  if (!root) return std::nullopt;
  return render_settings_from_json(*root);
}

// The locked palette: a theme may only use these tokens (v4).
//   Obsidian #050608, Amber #C8962E, Ink #D9DCE1, Neon Gold #FFC23D,
//   Night Lights #E8A54B, Day Tint #0B0E13.
bool palette_token(std::string hex) {
  for (char& c : hex) c = static_cast<char>(c >= 'a' && c <= 'f' ? c - 32 : c);
  static constexpr const char* kTokens[] = {"#050608", "#C8962E", "#D9DCE1", "#FFC23D", "#E8A54B", "#0B0E13"};
  for (const char* t : kTokens) {
    if (hex == t) return true;
  }
  return false;
}

std::optional<Theme> theme_from_json(const JsonValue& root, std::string& error) {
  Theme t;
  auto color = [&](const JsonValue* obj, const char* key, float (&out)[3], const char* what) {
    const std::string hex = obj ? obj->string_or(key, "") : std::string();
    if (!parse_hex_color(hex, out)) {
      error = std::string("theme: ") + what + " needs a \"#RRGGBB\" colour";
      return false;
    }
    if (!palette_token(hex)) {
      error = std::string("theme: ") + what + " " + hex +
              " is not a palette token (Obsidian, Amber, Ink, Neon Gold, Night Lights, Day Tint)";
      return false;
    }
    return true;
  };
  const JsonValue* grat = root.find("graticule");
  const JsonValue* ink = root.find("ink");
  const JsonValue* whale = root.find("whale");
  if (!color(&root, "background", t.background, "background")) return std::nullopt;
  if (!color(grat, "color", t.graticule, "graticule")) return std::nullopt;
  if (!color(ink, "color", t.ink, "ink")) return std::nullopt;
  if (!color(whale, "color", t.whale, "whale")) return std::nullopt;
  if (whale && whale->find("color_hot")) {
    if (!color(whale, "color_hot", t.whale_hot, "whale hot")) return std::nullopt;
  } else {
    for (int i = 0; i < 3; ++i) t.whale_hot[i] = t.whale[i];  // one colour for every tier
  }

  t.graticule_alpha = num(*grat, "alpha", t.graticule_alpha, 0.06f, 0.10f);
  t.graticule_spacing_deg = num(*grat, "spacing_deg", t.graticule_spacing_deg, 1.0f, 90.0f);

  t.node_alpha_min = num(*ink, "node_alpha_min", t.node_alpha_min, 0.04f, 0.70f);
  t.node_alpha_max = num(*ink, "node_alpha_max", t.node_alpha_max, t.node_alpha_min, 0.60f);
  t.node_min_px = num(*ink, "node_min_px", t.node_min_px, 1.0f, 2.5f);
  t.node_max_px = num(*ink, "node_max_px", t.node_max_px, t.node_min_px, 2.5f);
  t.intra_arc_alpha_min = num(*ink, "intra_arc_alpha_min", t.intra_arc_alpha_min, 0.0f, 0.039f);
  t.intra_arc_alpha_max = num(*ink, "intra_arc_alpha_max", t.intra_arc_alpha_max, t.intra_arc_alpha_min, 0.039f);
  t.inter_arc_alpha_min = num(*ink, "inter_arc_alpha_min", t.inter_arc_alpha_min, 0.03f, 0.10f);
  t.inter_arc_alpha_max = num(*ink, "inter_arc_alpha_max", t.inter_arc_alpha_max, t.inter_arc_alpha_min, 0.10f);
  t.hub_ring_alpha = num(*ink, "hub_ring_alpha", t.hub_ring_alpha, 0.04f, 0.70f);
  t.hub_ring_px = num(*ink, "hub_ring_px", t.hub_ring_px, 0.5f, 4.0f);
  t.hub_ring_radius_px = num(*ink, "hub_ring_radius_px", t.hub_ring_radius_px, 2.0f, 32.0f);
  t.hub_dot_px = num(*ink, "hub_dot_px", t.hub_dot_px, 0.5f, 8.0f);
  t.label_alpha = num(*ink, "label_alpha", t.label_alpha, 0.04f, 0.70f);
  t.label_px = num(*ink, "label_px", t.label_px, 6.0f, 48.0f);
  t.label_tracking_em = num(*ink, "label_tracking_em", t.label_tracking_em, 0.0f, 1.0f);

  t.whale_intensity_min = num(*whale, "intensity_min", t.whale_intensity_min, 4.0f, 8.0f);
  t.whale_intensity_max = num(*whale, "intensity_max", t.whale_intensity_max, t.whale_intensity_min, 8.0f);
  if (const JsonValue* w = whale->find("width_px"); w && w->is_array() && w->items().size() == 3) {
    for (std::size_t i = 0; i < 3; ++i) {
      t.whale_width_px[i] = std::clamp(static_cast<float>(w->items()[i].as_number(t.whale_width_px[i])), 1.0f, 16.0f);
    }
  }
  t.whale_flight_s = num(*whale, "flight_s", t.whale_flight_s, 0.2f, 30.0f);
  t.whale_trail_s = num(*whale, "trail_s", t.whale_trail_s, 0.05f, t.whale_flight_s);
  t.whale_flash_s = num(*whale, "flash_s", t.whale_flash_s, 0.05f, 10.0f);

  {
    const JsonValue* coast = root.find("coastline");
    if (!color(coast, "color", t.coast, "coastline")) return std::nullopt;
    t.coast_alpha = num(*coast, "alpha", t.coast_alpha, 0.18f, 0.28f);
    t.coast_night = num(*coast, "night_factor", t.coast_night, 0.0f, 1.0f);
    t.coast_close_altitude = num(*coast, "close_zoom_altitude", t.coast_close_altitude, 0.0f, 1e6f);
    const JsonValue* atmo = root.find("atmosphere");
    if (!color(atmo, "color", t.atmo, "atmosphere")) return std::nullopt;
    t.atmo_alpha = num(*atmo, "alpha", t.atmo_alpha, 0.0f, 0.22f);
    t.atmo_exponent = num(*atmo, "exponent", t.atmo_exponent, 4.0f, 6.0f);
    t.atmo_scale = num(*atmo, "scale", t.atmo_scale, 1.001f, 1.05f);
  }

  {
    const JsonValue* earth = root.find("earth");
    if (!color(earth, "day_tint", t.day_tint, "earth.day_tint")) return std::nullopt;
    if (!color(earth, "night_lights", t.night_lights, "earth.night_lights")) return std::nullopt;
    t.night_lights_max = num(*earth, "night_lights_max", t.night_lights_max, 0.0f, 1.5f);  // never beyond 1.5
    t.twilight = num(*earth, "twilight", t.twilight, 0.01f, 0.5f);
    t.cable_alpha = num(*earth, "cable_alpha", t.cable_alpha, 0.10f, 0.16f);
    t.rim_twilight = num(*earth, "rim_twilight", t.rim_twilight, 0.0f, 1.0f);
  }

  if (const JsonValue* post = root.find("post")) {
    t.bloom_strength = num(*post, "bloom_strength", t.bloom_strength, 0.0f, 4.0f);
    t.exposure = num(*post, "exposure", t.exposure, 0.1f, 8.0f);
    t.vignette = num(*post, "vignette", t.vignette, 0.0f, 0.03f);
  }
  return t;
}

std::optional<Theme> load_theme(const std::string& path, std::string& error) {
  const auto root = parse_file(path, error);
  if (!root) return std::nullopt;
  std::string perr;
  auto t = theme_from_json(*root, perr);
  if (!t) error = path + ": " + perr;
  return t;
}

}  // namespace ftv
