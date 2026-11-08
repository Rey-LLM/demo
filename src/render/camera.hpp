#pragma once
// Orbit / pan / zoom camera with exponential damping and animated fly-to.
// Produces a reverse-Z infinite-far perspective projection (pair with
// glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE), depth cleared to 0 and
// glDepthFunc(GL_GREATER)): depth = near / -z_view, so precision is spread
// evenly in log space across the scene's large depth range.

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace ftv {

class OrbitCamera {
 public:
  void configure(float fovDeg, float nearPlane, float damping) noexcept;
  void set_immediate(const glm::vec3& target, float distance, float yawRad, float pitchRad) noexcept;

  void orbit(float dYaw, float dPitch) noexcept;
  void pan(float dx, float dy) noexcept;  // in screen fractions
  void zoom(float steps) noexcept;        // wheel steps; positive = closer
  void fly_to(const glm::vec3& target, float distance) noexcept;

  void update(float dt) noexcept;

  [[nodiscard]] glm::mat4 view() const noexcept;
  [[nodiscard]] glm::mat4 projection(float aspect) const noexcept;
  [[nodiscard]] glm::vec3 position() const noexcept;
  [[nodiscard]] glm::vec3 target() const noexcept { return target_; }
  [[nodiscard]] float distance() const noexcept { return distance_; }
  [[nodiscard]] float near_plane() const noexcept { return near_; }
  [[nodiscard]] float fov_y() const noexcept { return fov_; }

 private:
  // Current (smoothed) and goal values.
  glm::vec3 target_{0.0f}, goal_target_{0.0f};
  float distance_ = 3000.0f, goal_distance_ = 3000.0f;
  float yaw_ = 0.0f, goal_yaw_ = 0.0f;
  float pitch_ = 0.4f, goal_pitch_ = 0.4f;
  float fov_ = 0.87f;
  float near_ = 0.5f;
  float damping_ = 8.0f;
};

}  // namespace ftv
