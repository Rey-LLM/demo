#include "app/camera_presets.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

#include "model/globe.hpp"

namespace ftv {

namespace {

void look_from(const glm::vec3& dir, float& yaw, float& pitch) {
  const glm::vec3 d = glm::normalize(dir);
  pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
  yaw = std::atan2(d.x, d.z);
}

float angle_between(const glm::vec3& a, const glm::vec3& b) {
  return std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f));
}

}  // namespace

float busiest_replay_time(const WhaleSet& whales, const Theme& theme,
                          const std::function<bool(const WhaleEvent&)>& keep) {
  const float span = theme.whale_flight_s * 0.9f;  // in flight (head visible), not just flashing
  float best = 0.0f;
  int bestCount = -1;
  for (float t = 0.0f; t < whales.replay_seconds; t += 0.05f) {
    int n = 0;
    for (const WhaleEvent& e : whales.events) {
      if (keep && !keep(e)) continue;
      const float age = t - e.start;
      if (age > theme.whale_flight_s * 0.1f && age < span) ++n;
    }
    if (n > bestCount) {
      bestCount = n;
      best = t;
    }
  }
  return best;
}

ViewSetup hub_view(const HubDef& d, const RenderSettings& settings) noexcept {
  const float R = settings.globe.radius;
  const float tanHalf = std::tan(settings.camera_fov_deg * kDegToRad * 0.5f);
  ViewSetup v;
  v.target = geo_normal(d.lat_deg, d.lon_deg) * R;
  // The hub's disc (footprint, plus strays up to 1.6x) fills ~70% of the height.
  v.distance = R * d.footprint_deg * kDegToRad * 1.6f / (0.7f * tanHalf);
  // Look down at the hub, tilted 32 degrees toward the equator.
  v.yaw = d.lon_deg * kDegToRad;
  v.pitch = (d.lat_deg - 32.0f) * kDegToRad;
  return v;
}

int whale_preset_event(const WhaleSet& whales, const NodePositionFn& nodePos) {
  // Prefer the earliest top-tier event between hubs 8-70 degrees apart (a
  // regional arc that fills the frame), then any top-tier event, then any.
  int fallback = -1, anyTop = -1;
  for (std::size_t i = 0; i < whales.events.size(); ++i) {
    const WhaleEvent& e = whales.events[i];
    if (fallback < 0) fallback = static_cast<int>(i);
    if (e.tier != 3) continue;
    if (anyTop < 0) anyTop = static_cast<int>(i);
    const float sep = angle_between(nodePos(e.src), nodePos(e.dst)) / kDegToRad;
    if (sep >= 8.0f && sep <= 70.0f) return static_cast<int>(i);
  }
  return anyTop >= 0 ? anyTop : fallback;
}

bool camera_preset(const std::string& name, const HubTaxonomy& taxonomy, const WhaleSet& whales,
                   const NodePositionFn& nodePos, const RenderSettings& settings, ViewSetup& out, std::string& error) {
  const float R = settings.globe.radius;
  const float tanHalf = std::tan(settings.camera_fov_deg * kDegToRad * 0.5f);
  const Theme& theme = settings.theme;

  if (name == "globe") {
    out.target = glm::vec3(0.0f);
    out.distance = settings.camera_start_distance;
    out.yaw = settings.camera_start_yaw_deg * kDegToRad;
    out.pitch = settings.camera_start_pitch_deg * kDegToRad;
    out.time = busiest_replay_time(whales, theme, nullptr);
    return true;
  }

  if (name == "london" || name.rfind("hub:", 0) == 0) {
    const std::string id = name == "london" ? std::string("LONDON") : name.substr(4);
    const auto h = taxonomy.find(id);
    if (!h) {
      error = "unknown hub id in --camera: " + id;
      return false;
    }
    out = hub_view(taxonomy.hubs()[*h], settings);
    out.time = busiest_replay_time(whales, theme, nullptr);
    return true;
  }

  if (name == "whale") {
    const int idx = whale_preset_event(whales, nodePos);
    if (idx < 0) {
      error = "--camera whale: the dataset has no whale events";
      return false;
    }
    const WhaleEvent& e = whales.events[static_cast<std::size_t>(idx)];
    const glm::vec3 a = nodePos(e.src), b = nodePos(e.dst);
    const glm::vec3 mid = arc_point(a, b, 0.5f, settings.globe);
    const float chord = glm::length(b - a);
    out.target = mid;
    out.distance = std::max(chord * 0.62f / tanHalf, 0.15f * R);
    float yaw = 0.0f, pitch = 0.0f;
    look_from(mid, yaw, pitch);
    out.yaw = yaw;
    out.pitch = pitch - 24.0f * kDegToRad;
    out.time = e.start + theme.whale_flight_s * 0.62f;  // head at 62% of the arc, trail behind it
    return true;
  }

  if (name == "graticule") {
    // Over the empty Southern Ocean, looking south across the converging
    // meridians toward the limb.
    // The camera hangs above 50S and looks past the pole, so the meridians
    // converge in frame and the limb crosses it.
    const glm::vec3 eye = geo_normal(-50.0f, 170.0f) * (2.4f * R);
    out.target = geo_normal(-58.0f, 170.0f) * R;
    out.distance = glm::length(eye - out.target);
    look_from(eye - out.target, out.yaw, out.pitch);
    out.time = busiest_replay_time(whales, theme, nullptr);
    return true;
  }

  error = "unknown camera preset '" + name + "' (globe, london, hub:<ID>, whale, graticule)";
  return false;
}

}  // namespace ftv
