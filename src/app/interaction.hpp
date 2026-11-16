#pragma once
// Interaction helpers that are pure math (unit-tested without a GL context):
// CPU hub picking, the 1-5 camera presets and the eased camera flight.

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "model/hub_taxonomy.hpp"
#include "render/render_settings.hpp"

namespace ftv {

struct Ray {
  glm::vec3 origin{0.0f};
  glm::vec3 dir{0.0f, 0.0f, -1.0f};  // unit
};

// Ray through pixel (px, py) (origin top-left) for a camera at `eye` with the
// given inverse view-projection (reverse-Z, infinite far plane).
Ray pixel_ray(const glm::vec3& eye, const glm::mat4& invViewProj, float px, float py, float width, float height) noexcept;

// Nearest positive ray-sphere intersection distance, or a negative value.
float ray_sphere(const Ray& r, const glm::vec3& centre, float radius) noexcept;

// Hub under the ray, or -1. Each hub is a sphere around its marker (radius
// from its footprint, at least `minDeg` of arc); hubs behind the globe are
// rejected with a ray-sphere test against the globe itself.
int pick_hub(const Ray& r, const HubTaxonomy& taxonomy, float globeRadius, float minDeg = 1.6f) noexcept;

struct CameraPose {
  glm::vec3 target{0.0f};
  float distance = 3000.0f;
  float yaw = 0.0f;    // radians
  float pitch = 0.0f;  // radians
};

inline constexpr int kViewPresetCount = 5;
inline constexpr const char* kViewPresetNames[kViewPresetCount] = {"GLOBE", "AMERICAS / NEW YORK", "EUROPE / LONDON",
                                                                   "ASIA / TOKYO + SHANGHAI", "CHAIN-CLOUD SHELL"};
// Preset 1..5: Globe, Americas (New York), Europe (London), Asia (Tokyo +
// Shanghai), chain-cloud shell. Out-of-range indices return the globe view.
CameraPose view_preset(int index, const HubTaxonomy& taxonomy, const RenderSettings& settings) noexcept;

// Pose at parameter t in [0, 1] of the flight a -> b: cubic ease in/out, the
// viewing direction slerped on the unit sphere, target and distance blended.
// t <= 0 returns a and t >= 1 returns b exactly.
// Close view of one hub (search fly-to), looking down on it from slightly south.
CameraPose hub_pose(const HubDef& hub, const RenderSettings& settings) noexcept;

CameraPose ease_pose(const CameraPose& a, const CameraPose& b, float t) noexcept;

inline constexpr float kPresetFlightSeconds = 1.2f;

}  // namespace ftv
