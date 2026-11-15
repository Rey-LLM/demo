#include "render/gl_context.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "core/log.hpp"

#if defined(FTVIZ_HAVE_EGL)
// libEGL is loaded at run time (dlopen), so the binary does not depend on it:
// a machine without EGL still runs ftviz and falls back to a hidden window.
#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#endif

namespace ftv {

namespace {

#if defined(FTVIZ_HAVE_EGL)
struct EglApi {
  PFNEGLGETPROCADDRESSPROC GetProcAddress = nullptr;
  PFNEGLINITIALIZEPROC Initialize = nullptr;
  PFNEGLTERMINATEPROC Terminate = nullptr;
  PFNEGLBINDAPIPROC BindAPI = nullptr;
  PFNEGLCHOOSECONFIGPROC ChooseConfig = nullptr;
  PFNEGLCREATECONTEXTPROC CreateContext = nullptr;
  PFNEGLDESTROYCONTEXTPROC DestroyContext = nullptr;
  PFNEGLMAKECURRENTPROC MakeCurrent = nullptr;
  bool ok = false;

  EglApi() {
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    auto sym = [lib](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
      return fn != nullptr;
    };
    ok = sym(GetProcAddress, "eglGetProcAddress") && sym(Initialize, "eglInitialize") &&
         sym(Terminate, "eglTerminate") && sym(BindAPI, "eglBindAPI") && sym(ChooseConfig, "eglChooseConfig") &&
         sym(CreateContext, "eglCreateContext") && sym(DestroyContext, "eglDestroyContext") &&
         sym(MakeCurrent, "eglMakeCurrent");
  }
};

const EglApi& egl() {
  static const EglApi api;  // never unloaded: GL function pointers point into the driver
  return api;
}

GLADapiproc egl_proc(const char* name) { return reinterpret_cast<GLADapiproc>(egl().GetProcAddress(name)); }
#endif

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
#if defined(FTVIZ_HAVE_EGL)
  if (egl_display_) {
    auto dpy = static_cast<EGLDisplay>(egl_display_);
    egl().MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_context_) egl().DestroyContext(dpy, static_cast<EGLContext>(egl_context_));
    egl().Terminate(dpy);
  }
#endif
  egl_display_ = nullptr;
  egl_context_ = nullptr;
  if (glfw_initialised_) glfwTerminate();
  glfw_initialised_ = false;
}

bool GlWindow::create(int width, int height, const char* title, bool vsync, bool debugContext,
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
    glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, width > 0 ? GLFW_TRUE : GLFW_FALSE);
    window_ = glfwCreateWindow(width > 0 ? width : 64, height > 0 ? height : 64, title, nullptr, nullptr);
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
  return load_and_check(glfwGetProcAddress, debugContext, error);
}

bool GlWindow::create_offscreen(bool debugContext, std::string& error) {
#if defined(FTVIZ_HAVE_EGL)
  const EglApi& E = egl();
  auto getPlatformDisplay =
      E.ok ? reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(E.GetProcAddress("eglGetPlatformDisplayEXT")) : nullptr;
  EGLDisplay dpy = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                                      : EGL_NO_DISPLAY;
  EGLint major = 0, minor = 0;
  if (dpy != EGL_NO_DISPLAY && E.Initialize(dpy, &major, &minor)) {
    egl_display_ = dpy;
    if (E.BindAPI(EGL_OPENGL_API)) {
      const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
      EGLConfig cfg = nullptr;
      EGLint n = 0;
      E.ChooseConfig(dpy, cfgAttr, &cfg, 1, &n);
      const int versions[][2] = {{4, 6}, {4, 5}, {4, 3}};
      for (const auto& v : versions) {
        const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION,
                                  v[0],
                                  EGL_CONTEXT_MINOR_VERSION,
                                  v[1],
                                  EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                  EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                  EGL_CONTEXT_OPENGL_DEBUG,
                                  debugContext ? EGL_TRUE : EGL_FALSE,
                                  EGL_NONE};
        EGLContext ctx = E.CreateContext(dpy, n > 0 ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttr);
        if (ctx != EGL_NO_CONTEXT) {
          egl_context_ = ctx;
          break;
        }
      }
      if (egl_context_ && E.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, static_cast<EGLContext>(egl_context_))) {
        FTV_LOG_INFO("offscreen context: surfaceless EGL %d.%d", major, minor);
        const bool ok = load_and_check(egl_proc, debugContext, error);
        caps_.has_default_framebuffer = false;
        return ok;
      }
    }
    destroy();
  }
  FTV_LOG_INFO("surfaceless EGL unavailable; using a hidden GLFW window for offscreen rendering");
#endif
  return create(0, 0, "ftviz", false, debugContext, error);
}

bool GlWindow::load_and_check(GLADloadfunc loader, bool debugContext, std::string& error) {
  const int glVersion = gladLoadGL(loader);
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
  GLint colorSamples = 1, depthSamples = 1;
  glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &colorSamples);
  glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &depthSamples);
  caps_.max_samples = std::max(1, std::min(colorSamples, depthSamples));
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
