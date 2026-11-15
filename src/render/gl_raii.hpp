#pragma once
// RAII wrappers for every GL object type used by the renderer. All are
// move-only; destruction releases the GL name. Creation uses Direct State
// Access entry points (glCreate*), never bind-to-create.
//
// These must only be created, used and destroyed on the render thread that
// owns the GL context.

#include <utility>

#include "render/gl.hpp"

namespace ftv::gl {

template <class Traits>
class Handle {
 public:
  Handle() = default;
  explicit Handle(GLuint id) noexcept : id_(id) {}
  ~Handle() { reset(); }
  Handle(Handle&& o) noexcept : id_(std::exchange(o.id_, 0)) {}
  Handle& operator=(Handle&& o) noexcept {
    if (this != &o) {
      reset();
      id_ = std::exchange(o.id_, 0);
    }
    return *this;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  void reset() noexcept {
    if (id_) Traits::destroy(id_);
    id_ = 0;
  }
  [[nodiscard]] GLuint id() const noexcept { return id_; }
  explicit operator bool() const noexcept { return id_ != 0; }

 private:
  GLuint id_ = 0;
};

struct BufferTraits {
  static void destroy(GLuint id) noexcept { glDeleteBuffers(1, &id); }
};
struct VertexArrayTraits {
  static void destroy(GLuint id) noexcept { glDeleteVertexArrays(1, &id); }
};
struct TextureTraits {
  static void destroy(GLuint id) noexcept { glDeleteTextures(1, &id); }
};
struct FramebufferTraits {
  static void destroy(GLuint id) noexcept { glDeleteFramebuffers(1, &id); }
};
struct QueryTraits {
  static void destroy(GLuint id) noexcept { glDeleteQueries(1, &id); }
};
struct ProgramTraits {
  static void destroy(GLuint id) noexcept { glDeleteProgram(id); }
};
struct ShaderTraits {
  static void destroy(GLuint id) noexcept { glDeleteShader(id); }
};

using Buffer = Handle<BufferTraits>;
using VertexArray = Handle<VertexArrayTraits>;
using Texture = Handle<TextureTraits>;
using Framebuffer = Handle<FramebufferTraits>;
using Query = Handle<QueryTraits>;
using Program = Handle<ProgramTraits>;
using Shader = Handle<ShaderTraits>;

inline Buffer create_buffer() {
  GLuint id = 0;
  glCreateBuffers(1, &id);
  return Buffer(id);
}
inline VertexArray create_vertex_array() {
  GLuint id = 0;
  glCreateVertexArrays(1, &id);
  return VertexArray(id);
}
inline Texture create_texture(GLenum target) {
  GLuint id = 0;
  glCreateTextures(target, 1, &id);
  return Texture(id);
}
inline Framebuffer create_framebuffer() {
  GLuint id = 0;
  glCreateFramebuffers(1, &id);
  return Framebuffer(id);
}
inline Query create_query(GLenum target) {
  GLuint id = 0;
  glCreateQueries(target, 1, &id);
  return Query(id);
}

// Fence sync object (GLsync is a pointer, not a GLuint name).
class Fence {
 public:
  Fence() = default;
  ~Fence() { reset(); }
  Fence(Fence&& o) noexcept : sync_(std::exchange(o.sync_, nullptr)) {}
  Fence& operator=(Fence&& o) noexcept {
    if (this != &o) {
      reset();
      sync_ = std::exchange(o.sync_, nullptr);
    }
    return *this;
  }
  Fence(const Fence&) = delete;
  Fence& operator=(const Fence&) = delete;

  void insert() noexcept {
    reset();
    sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  }
  // Blocks (up to timeout) until the GPU passed the fence. Returns false on
  // timeout or error. An empty fence counts as signalled.
  bool wait(GLuint64 timeoutNs) noexcept {
    if (!sync_) return true;
    const GLenum r = glClientWaitSync(sync_, GL_SYNC_FLUSH_COMMANDS_BIT, timeoutNs);
    return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
  }
  // Non-blocking poll.
  bool signaled() noexcept {
    if (!sync_) return true;
    const GLenum r = glClientWaitSync(sync_, 0, 0);
    return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
  }
  void reset() noexcept {
    if (sync_) glDeleteSync(sync_);
    sync_ = nullptr;
  }
  [[nodiscard]] bool valid() const noexcept { return sync_ != nullptr; }

 private:
  GLsync sync_ = nullptr;
};

// A persistently mapped, coherent buffer created with glNamedBufferStorage.
// Splitting it into regions (rings) is the caller's job.
class PersistentBuffer {
 public:
  PersistentBuffer() = default;
  bool create(GLsizeiptr bytes, GLbitfield access) {
    buffer_ = create_buffer();
    const GLbitfield flags = access | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glNamedBufferStorage(buffer_.id(), bytes, nullptr, flags);
    ptr_ = glMapNamedBufferRange(buffer_.id(), 0, bytes, flags);
    size_ = bytes;
    return ptr_ != nullptr;
  }
  ~PersistentBuffer() {
    if (buffer_ && ptr_) glUnmapNamedBuffer(buffer_.id());
  }
  PersistentBuffer(PersistentBuffer&& o) noexcept
      : buffer_(std::move(o.buffer_)), ptr_(std::exchange(o.ptr_, nullptr)), size_(std::exchange(o.size_, 0)) {}
  PersistentBuffer& operator=(PersistentBuffer&& o) noexcept {
    if (this != &o) {
      if (buffer_ && ptr_) glUnmapNamedBuffer(buffer_.id());
      buffer_ = std::move(o.buffer_);
      ptr_ = std::exchange(o.ptr_, nullptr);
      size_ = std::exchange(o.size_, 0);
    }
    return *this;
  }
  PersistentBuffer(const PersistentBuffer&) = delete;
  PersistentBuffer& operator=(const PersistentBuffer&) = delete;

  [[nodiscard]] GLuint id() const noexcept { return buffer_.id(); }
  [[nodiscard]] void* data() const noexcept { return ptr_; }
  [[nodiscard]] GLsizeiptr size() const noexcept { return size_; }

 private:
  Buffer buffer_;
  void* ptr_ = nullptr;
  GLsizeiptr size_ = 0;
};

constexpr GLsizeiptr align_up(GLsizeiptr v, GLsizeiptr a) noexcept { return (v + a - 1) / a * a; }

}  // namespace ftv::gl
