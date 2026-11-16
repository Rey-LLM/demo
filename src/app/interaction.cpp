#include "app/interaction.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "model/globe.hpp"

namespace ftv {

Ray pixel_ray(const glm::vec3& eye, const glm::mat4& invViewProj, float px, float py, float width,
              float height) noexcept {
  const float x = (px + 0.5f) / std::max(width, 1.0f) * 2.0f - 1.0f;
  const float y = 1.0f - (py + 0.5f) / std::max(height, 1.0f) * 2.0f;
  // Reverse-Z: any depth in (0, 1] is a finite point on the pixel's ray.
  const glm::vec4 h = invViewProj * glm::vec4(x, y, 0.5f, 1.0f);
  Ray r;
  r.origin = eye;
  const glm::vec3 p = std::fabs(h.w) > 1e-12f ? glm::vec3(h) / h.w : glm::vec3(h);
  const glm::vec3 d = p - eye;
  const float len = glm::length(d);
  r.dir = len > 0.0f ? d / len : glm::vec3(0.0f, 0.0f, -1.0f);
  return r;
}

float ray_sphere(const Ray& r, const glm::vec3& centre, float radius) noexcept {
  const glm::vec3 oc = r.origin - centre;
  const float b = glm::dot(oc, r.dir);
  const float c = glm::dot(oc, oc) - radius * radius;
  const float disc = b * b - c;
  if (disc < 0.0f) return -1.0f;
  const float s = std::sqrt(disc);
  const float t0 = -b - s;
  if (t0 > 0.0f) return t0;
  const float t1 = -b + s;
  return t1 > 0.0f ? t1 : -1.0f;
}

int pick_hub(const Ray& r, const HubTaxonomy& taxonomy, float globeRadius, float minDeg) noexcept {
  const float tGlobe = ray_sphere(r, glm::vec3(0.0f), globeRadius);
  int best = -1;
  float bestT = 0.0f;
  const auto& hubs = taxonomy.hubs();
  for (std::size_t i = 0; i < hubs.size(); ++i) {
    const glm::vec3 c = geo_normal(hubs[i].lat_deg, hubs[i].lon_deg) * globeRadius;
    const float rad = globeRadius * std::sin(std::max(hubs[i].footprint_deg, minDeg) * kDegToRad);
    const float t = ray_sphere(r, c, rad);
    if (t < 0.0f) continue;
    if (tGlobe > 0.0f && t > tGlobe + rad) continue;  // on the far side of the globe
    if (best < 0 || t < bestT) {
      best = static_cast<int>(i);
      bestT = t;
    }
  }
  return best;
}

namespace {

CameraPose region(float latDeg, float lonDeg, float distance) noexcept {
  CameraPose p;
  p.target = glm::vec3(0.0f);
  p.distance = distance;
  p.yaw = lonDeg * kDegToRad;
  p.pitch = latDeg * kDegToRad;
  return p;
}

glm::vec3 direction(float yaw, float pitch) noexcept {
  const float cp = std::cos(pitch);
  return {cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
}

}  // namespace

CameraPose view_preset(int index, const HubTaxonomy& taxonomy, const RenderSettings& rs) noexcept {
  const float R = rs.globe.radius;
  auto hub = [&](const char* id, float lat, float lon) {
    const auto h = taxonomy.find(id);
    return h ? glm::vec2(taxonomy.hubs()[*h].lat_deg, taxonomy.hubs()[*h].lon_deg) : glm::vec2(lat, lon);
  };
  switch (index) {
    case 2: {
      const glm::vec2 ny = hub("NEW_YORK", 40.71f, -74.01f);
      return region(ny.x - 12.0f, ny.y - 12.0f, R * 2.7f);
    }
    case 3: {
      const glm::vec2 ln = hub("LONDON", 51.51f, -0.13f);
      return region(ln.x - 8.0f, ln.y + 6.0f, R * 2.2f);
    }
    case 4: {
      // Midpoint of Tokyo and Shanghai (31.23 N, 121.47 E; not a hub).
      const glm::vec2 tk = hub("TOKYO", 35.68f, 139.69f);
      return region((tk.x + 31.23f) * 0.5f - 6.0f, (tk.y + 121.47f) * 0.5f, R * 2.3f);
    }
    case 5: {
      // The chain clouds have no geography; frame the whole shell from the
      // side their mean direction faces, far enough to see the limb.
      glm::vec3 sum(0.0f);
      for (const HubDef& h : taxonomy.hubs()) {
        if (h.type == HubType::ChainCloud) sum += geo_normal(h.lat_deg, h.lon_deg);
      }
      if (glm::length(sum) < 1e-3f) sum = glm::vec3(0.0f, 0.3f, 1.0f);
      sum = glm::normalize(sum);
      CameraPose p;
      p.distance = R * 5.2f;
      p.yaw = std::atan2(sum.x, sum.z);
      p.pitch = std::asin(std::clamp(sum.y, -1.0f, 1.0f));
      return p;
    }
    default:
      return region(rs.camera_start_pitch_deg, rs.camera_start_yaw_deg, rs.camera_start_distance);
  }
}

CameraPose hub_pose(const HubDef& hub, const RenderSettings& rs) noexcept {
  const float R = rs.globe.radius;
  const bool fiat = hub.type == HubType::FiatCenter;
  return region(hub.lat_deg - (fiat ? 6.0f : 0.0f), hub.lon_deg, R * (fiat ? 2.3f : 2.8f));
}

CameraPose ease_pose(const CameraPose& a, const CameraPose& b, float t) noexcept {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  const float e = t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
  const glm::vec3 da = direction(a.yaw, a.pitch), db = direction(b.yaw, b.pitch);
  const float c = std::clamp(glm::dot(da, db), -1.0f, 1.0f);
  const float om = std::acos(c);
  glm::vec3 d;
  if (om < 1e-4f) {
    d = glm::normalize(da + (db - da) * e);
  } else if (om > 3.1410f) {
    // Antipodal: no unique great circle; go through yaw at the mean pitch.
    const float yaw = a.yaw + (b.yaw - a.yaw) * e, pitch = a.pitch + (b.pitch - a.pitch) * e;
    d = direction(yaw, pitch);
  } else {
    const float s = std::sin(om);
    d = (std::sin((1.0f - e) * om) * da + std::sin(e * om) * db) / s;
  }
  CameraPose p;
  p.target = a.target + (b.target - a.target) * e;
  p.distance = a.distance + (b.distance - a.distance) * e;
  p.yaw = std::atan2(d.x, d.z);
  p.pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
  return p;
}

}  // namespace ftv
