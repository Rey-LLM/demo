#pragma once
// Globe placement shared by the CPU (camera presets, framing, tests) and the
// shaders (common.glsl mirrors these formulas line for line).
//
// Conventions: y is up; latitude 0 / longitude 0 lies on +z; longitude grows
// toward +x. A node is mapped from layout space onto the globe through its
// hub: d = (p - anchor) / radius is the node's offset inside the hub's layout
// ball; (d.x, d.z) becomes a tangent-plane offset on the hub's disc (angular
// radius = footprint) and d.y a small altitude relief.

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "model/hub_taxonomy.hpp"

namespace ftv {

struct GlobeParams {
  float radius = 1000.0f;
  float node_altitude = 0.0015f;  // base altitude of nodes, fraction of the radius
  float node_relief = 0.0010f;    // +- altitude from the layout's y offset
  float arc_lift = 0.22f;         // inter-hub arc apex height per radian of separation
};

inline constexpr float kDegToRad = 0.017453292519943295f;

struct HubFrame {
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  glm::vec3 east{1.0f, 0.0f, 0.0f};
  glm::vec3 north{0.0f, 1.0f, 0.0f};
  float footprint = 0.035f;  // radians
};

inline glm::vec3 geo_normal(float latDeg, float lonDeg) noexcept {
  const float la = latDeg * kDegToRad, lo = lonDeg * kDegToRad;
  return {std::cos(la) * std::sin(lo), std::sin(la), std::cos(la) * std::cos(lo)};
}

inline HubFrame hub_frame(const HubDef& h) noexcept {
  const float la = h.lat_deg * kDegToRad, lo = h.lon_deg * kDegToRad;
  HubFrame f;
  f.normal = geo_normal(h.lat_deg, h.lon_deg);
  f.east = {std::cos(lo), 0.0f, -std::sin(lo)};
  f.north = {-std::sin(la) * std::sin(lo), std::cos(la), -std::sin(la) * std::cos(lo)};
  f.footprint = h.footprint_deg * kDegToRad;
  return f;
}

// Layout-space position of a node of hub `h` -> world position on the globe.
inline glm::vec3 globe_position(const glm::vec3& p, const HubDef& h, const HubFrame& f, const GlobeParams& g) noexcept {
  glm::vec3 d = (p - h.anchor) / h.radius;
  float planar = std::sqrt(d.x * d.x + d.z * d.z);
  const float k = planar > 1.6f ? 1.6f / planar : 1.0f;  // keep strays on the hub's disc
  const glm::vec3 dir = glm::normalize(f.normal + (f.east * (d.x * k) + f.north * (d.z * k)) * f.footprint);
  const float alt = g.node_altitude + g.node_relief * std::clamp(d.y, -1.0f, 1.0f);
  return dir * (g.radius * (1.0f + alt));
}

// Point at parameter t in [0, 1] on the lifted great-circle arc a -> b.
inline glm::vec3 arc_point(const glm::vec3& a, const glm::vec3& b, float t, const GlobeParams& g) noexcept {
  const float la = glm::length(a), lb = glm::length(b);
  const glm::vec3 ua = a / la, ub = b / lb;
  const float c = std::clamp(glm::dot(ua, ub), -0.9999f, 1.0f);
  const float om = std::acos(c);
  glm::vec3 u;
  if (om < 1e-4f) {
    u = glm::normalize(ua + (ub - ua) * t);
  } else {
    const float s = std::sin(om);
    u = (std::sin((1.0f - t) * om) * ua + std::sin(t * om) * ub) / s;
  }
  const float r = la + (lb - la) * t + g.radius * g.arc_lift * om * std::sin(3.14159265f * t);
  return u * r;
}

}  // namespace ftv
