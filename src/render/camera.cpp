#include "render/camera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace ftv {

void OrbitCamera::configure(float fovDeg, float nearPlane, float damping) noexcept {
  fov_ = glm::radians(fovDeg);
  near_ = nearPlane;
  damping_ = damping;
}

void OrbitCamera::set_immediate(const glm::vec3& target, float distance, float yawRad, float pitchRad) noexcept {
  target_ = goal_target_ = target;
  distance_ = goal_distance_ = distance;
  yaw_ = goal_yaw_ = yawRad;
  pitch_ = goal_pitch_ = pitchRad;
}

void OrbitCamera::orbit(float dYaw, float dPitch) noexcept {
  goal_yaw_ += dYaw;
  goal_pitch_ = std::clamp(goal_pitch_ + dPitch, -1.52f, 1.52f);
}

void OrbitCamera::pan(float dx, float dy) noexcept {
  const glm::mat4 v = view();
  const glm::vec3 right(v[0][0], v[1][0], v[2][0]);
  const glm::vec3 up(v[0][1], v[1][1], v[2][1]);
  const float scale = goal_distance_ * std::tan(fov_ * 0.5f) * 2.0f;
  goal_target_ += (-dx * right + dy * up) * scale;
}

void OrbitCamera::zoom(float steps) noexcept {
  goal_distance_ = std::clamp(goal_distance_ * std::pow(0.88f, steps), 5.0f, 20000.0f);
}

void OrbitCamera::fly_to(const glm::vec3& target, float distance) noexcept {
  goal_target_ = target;
  goal_distance_ = std::clamp(distance, 5.0f, 20000.0f);
}

void OrbitCamera::update(float dt) noexcept {
  // Frame-rate independent exponential smoothing toward the goal.
  const float a = 1.0f - std::exp(-damping_ * std::max(dt, 0.0f));
  target_ += (goal_target_ - target_) * a;
  distance_ += (goal_distance_ - distance_) * a;
  yaw_ += (goal_yaw_ - yaw_) * a;
  pitch_ += (goal_pitch_ - pitch_) * a;
}

glm::vec3 OrbitCamera::position() const noexcept {
  const float cp = std::cos(pitch_);
  return target_ + distance_ * glm::vec3(cp * std::sin(yaw_), std::sin(pitch_), cp * std::cos(yaw_));
}

glm::mat4 OrbitCamera::view() const noexcept { return glm::lookAt(position(), target_, glm::vec3(0, 1, 0)); }

glm::mat4 OrbitCamera::projection(float aspect) const noexcept {
  // Reverse-Z, infinite far plane, [0,1] clip depth:
  //   clip.z = near, clip.w = -z_view  =>  ndc.z = near / -z_view
  const float f = 1.0f / std::tan(fov_ * 0.5f);
  glm::mat4 p(0.0f);
  p[0][0] = f / std::max(aspect, 1e-6f);
  p[1][1] = f;
  p[2][3] = -1.0f;
  p[3][2] = near_;
  return p;
}

}  // namespace ftv
