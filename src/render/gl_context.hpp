#pragma once
// GLFW window + OpenGL 4.5 core context creation (4.3 + ARB extensions as
// fallback), glad loading, capability checks and the debug callback.

#include <string>

#include "render/gl.hpp"

namespace ftv {

struct GlCaps {
  int major = 0;
  int minor = 0;
  bool has_dsa = false;
  bool has_buffer_storage = false;
  bool has_clip_control = false;
  bool has_compute = false;
  bool has_robustness = false;
  GLint ssbo_alignment = 256;
  GLint ubo_alignment = 256;
  std::string renderer;
  std::string vendor;
  std::string version;
};

class GlWindow {
 public:
  GlWindow() = default;
  ~GlWindow();
  GlWindow(const GlWindow&) = delete;
  GlWindow& operator=(const GlWindow&) = delete;

  // Creates the window and context, loads GL, validates capabilities.
  bool create(int width, int height, const char* title, bool vsync, bool debugContext, bool hidden,
              std::string& error);
  void destroy() noexcept;

  [[nodiscard]] GLFWwindow* handle() const noexcept { return window_; }
  [[nodiscard]] const GlCaps& caps() const noexcept { return caps_; }

  // True if the driver reports a context reset (robustness).
  [[nodiscard]] bool context_lost() const noexcept;

  // State shared with the GL debug callback through its userParam pointer
  // (no globals): repeated LOW/MEDIUM messages are rate-limited.
  struct DebugState {
    unsigned reported = 0;
    unsigned suppressed = 0;
  };

 private:
  GLFWwindow* window_ = nullptr;
  DebugState debug_state_;
  GlCaps caps_;
  bool glfw_initialised_ = false;
};

}  // namespace ftv
