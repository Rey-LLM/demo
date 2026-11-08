#include "render/gpu_timers.hpp"

namespace ftv {

void GpuTimers::create() {
  for (auto& slot : queries_) {
    for (auto& q : slot) q = gl::create_query(GL_TIME_ELAPSED);
  }
}

void GpuTimers::begin(unsigned slot, GpuPass pass) noexcept {
  const auto p = static_cast<std::size_t>(pass);
  glBeginQuery(GL_TIME_ELAPSED, queries_[slot][p].id());
  issued_[slot][p] = true;
  active_ = true;
}

void GpuTimers::end() noexcept {
  if (!active_) return;
  glEndQuery(GL_TIME_ELAPSED);
  active_ = false;
}

void GpuTimers::collect(unsigned slot) noexcept {
  for (std::size_t p = 0; p < kGpuPassCount; ++p) {
    if (!issued_[slot][p]) {
      // Pass did not run in that frame (e.g. picking): decay toward zero.
      smoothed_[p] *= 0.9f;
      continue;
    }
    GLint available = 0;
    glGetQueryObjectiv(queries_[slot][p].id(), GL_QUERY_RESULT_AVAILABLE, &available);
    if (available) {
      GLuint64 ns = 0;
      glGetQueryObjectui64v(queries_[slot][p].id(), GL_QUERY_RESULT, &ns);
      const float ms = static_cast<float>(static_cast<double>(ns) * 1e-6);
      smoothed_[p] = smoothed_[p] == 0.0f ? ms : smoothed_[p] * 0.9f + ms * 0.1f;
    }
    issued_[slot][p] = false;
  }
}

float GpuTimers::total_ms() const noexcept {
  float t = 0.0f;
  for (float v : smoothed_) t += v;
  return t;
}

}  // namespace ftv
