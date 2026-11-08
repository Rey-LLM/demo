#include "render/gl_context.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.hpp"

namespace ftv {

namespace {

void glfw_error(int code, const char* desc) { FTV_LOG_ERROR("GLFW error %d: %s", code, desc); }

// Runs synchronously on the GL thread (GL_DEBUG_OUTPUT_SYNCHRONOUS), so the
// userParam state needs no synchronisation.
void GLAD_API_PTR gl_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei,
                                    const GLchar* message, const void* user) {
  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
  auto* state = static_cast<GlWindow::DebugState*>(const_cast<void*>(user));
  if (severity == GL_DEBUG_SEVERITY_HIGH) {
    std::fprintf(stderr, "[GL debug HIGH] source=0x%x type=0x%x id=%u: %s\n", source, type, id, message);
    std::fprintf(stderr, "FATAL: high-severity OpenGL debug message; aborting.\n");
    std::abort();
  }
  constexpr unsigned kMaxReports = 32;
  if (state && state->reported >= kMaxReports) {
    if (state->suppressed++ == 0) std::fprintf(stderr, "[GL debug] further LOW/MEDIUM messages suppressed\n");
    return;
  }
  if (state) ++state->reported;
  std::fprintf(stderr, "[GL debug %s] source=0x%x type=0x%x id=%u: %s\n",
               severity == GL_DEBUG_SEVERITY_MEDIUM ? "MEDIUM" : "LOW", source, type, id, message);
}

bool has_extension(const char* name) {
  GLint n = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &n);
  for (GLint i = 0; i < n; ++i) {
    const auto* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
    if (ext && std::strcmp(ext, name) == 0) return true;
  }
  return false;
}

}  // namespace

GlWindow::~GlWindow() { destroy(); }

void GlWindow::destroy() noexcept {
  if (window_) glfwDestroyWindow(window_);
  window_ = nullptr;
  if (glfw_initialised_) glfwTerminate();
  glfw_initialised_ = false;
}

bool GlWindow::create(int width, int height, const char* title, bool vsync, bool debugContext, bool hidden,
                      std::string& error) {
  glfwSetErrorCallback(glfw_error);
  if (!glfwInit()) {
    error = "glfwInit failed";
    return false;
  }
  glfw_initialised_ = true;

  // Probe 4.6 -> 4.5 -> 4.3. A refused version is expected on many drivers,
  // so GLFW errors are muted while probing and reported only if all fail.
  glfwSetErrorCallback(nullptr);
  const int versions[][2] = {{4, 6}, {4, 5}, {4, 3}};
  for (const auto& v : versions) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, v[0]);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, v[1]);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, debugContext ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_ROBUSTNESS, GLFW_LOSE_CONTEXT_ON_RESET);
    glfwWindowHint(GLFW_DEPTH_BITS, 0);  // we render into our own 32F depth target
    glfwWindowHint(GLFW_SAMPLES, 0);
    glfwWindowHint(GLFW_VISIBLE, hidden ? GLFW_FALSE : GLFW_TRUE);
    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (window_) break;
  }
  glfwSetErrorCallback(glfw_error);
  if (!window_) {
    const char* desc = nullptr;
    glfwGetError(&desc);
    error = std::string("could not create an OpenGL 4.3+ core profile context") + (desc ? ": " : "") +
            (desc ? desc : "");
    return false;
  }
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(vsync ? 1 : 0);

  const int glVersion = gladLoadGL(glfwGetProcAddress);
  if (!glVersion) {
    error = "failed to load OpenGL entry points";
    return false;
  }
  caps_.major = GLAD_VERSION_MAJOR(glVersion);
  caps_.minor = GLAD_VERSION_MINOR(glVersion);
  const bool gl45 = caps_.major > 4 || (caps_.major == 4 && caps_.minor >= 5);
  const bool gl43 = caps_.major > 4 || (caps_.major == 4 && caps_.minor >= 3);
  caps_.has_dsa = gl45 || has_extension("GL_ARB_direct_state_access");
  caps_.has_buffer_storage = gl45 || has_extension("GL_ARB_buffer_storage");
  caps_.has_clip_control = gl45 || has_extension("GL_ARB_clip_control");
  caps_.has_compute = gl43;
  caps_.has_robustness = gl45 || has_extension("GL_KHR_robustness");
  caps_.renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  caps_.vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
  caps_.version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &caps_.ssbo_alignment);
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &caps_.ubo_alignment);
  FTV_LOG_INFO("OpenGL %d.%d core: %s / %s", caps_.major, caps_.minor, caps_.vendor.c_str(), caps_.renderer.c_str());

  if (!gl43 || !caps_.has_dsa || !caps_.has_buffer_storage || !caps_.has_clip_control) {
    error = "OpenGL 4.5 (or 4.3 with ARB_direct_state_access, ARB_buffer_storage and ARB_clip_control) is required; got " +
            caps_.version;
    return false;
  }

  if (debugContext) {
    GLint flags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    if (flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
      glEnable(GL_DEBUG_OUTPUT);
      glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
      debug_state_ = DebugState{};
      glDebugMessageCallback(gl_debug_callback, &debug_state_);
      glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
      FTV_LOG_INFO("GL debug context enabled (HIGH severity messages abort)");
    }
  }
  return true;
}

bool GlWindow::context_lost() const noexcept {
  if (!caps_.has_robustness || !glad_glGetGraphicsResetStatus) return false;
  return glGetGraphicsResetStatus() != GL_NO_ERROR;
}

}  // namespace ftv
