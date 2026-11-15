#pragma once
// GLFW window + OpenGL 4.5 core context creation (4.3 + ARB extensions as
// fallback), glad loading, capability checks and the debug callback.
// Offscreen mode creates no window: on Linux a surfaceless EGL context
// (EGL_MESA_platform_surfaceless, no display server needed) when the build
// found EGL, otherwise a hidden GLFW window whose default framebuffer is
// never used.

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
  bool has_default_framebuffer = true;
  GLint max_samples = 1;  // min(GL_MAX_COLOR_TEXTURE_SAMPLES, GL_MAX_DEPTH_TEXTURE_SAMPLES)  // false for a surfaceless offscreen context
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
  bool create(int width, int height, const char* title, bool vsync, bool debugContext, std::string& error);
  // Creates a context for offscreen rendering only (no visible window).
  bool create_offscreen(bool debugContext, std::string& error);
  void destroy() noexcept;

  [[nodiscard]] GLFWwindow* handle() const noexcept { return window_; }  // nullptr when offscreen via EGL
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
  bool load_and_check(GLADloadfunc loader, bool debugContext, std::string& error);

  GLFWwindow* window_ = nullptr;
  void* egl_display_ = nullptr;  // EGLDisplay / EGLContext, kept opaque to avoid EGL headers here
  void* egl_context_ = nullptr;
  DebugState debug_state_;
  GlCaps caps_;
  bool glfw_initialised_ = false;
};

}  // namespace ftv
