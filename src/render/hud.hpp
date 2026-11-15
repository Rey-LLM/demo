#pragma once
// HUD text renderer: glyph instances are written straight into a persistently
// mapped ring buffer region (one region per frame slot) and drawn with a
// single instanced quad draw. Zero allocations per frame.

#include <cstdint>
#include <string_view>

#include "render/gl_raii.hpp"

namespace ftv {

class HudRenderer {
 public:
  static constexpr std::uint32_t kMaxGlyphs = 16384;

  bool create(GLint ssboAlignment);
  void begin(unsigned slot) noexcept;
  // Draws text at pixel (x, y) (top-left). Returns the x after the last glyph.
  float text(float x, float y, float scale, std::uint32_t rgba, std::string_view s) noexcept;
  void panel(float x, float y, float w, float h, std::uint32_t rgba) noexcept;
  [[nodiscard]] static float line_height(float scale) noexcept { return 10.0f * scale; }

  // Binds region + atlas and issues the single instanced draw.
  void draw(GLuint program) noexcept;

 private:
  struct Glyph {
    float x, y, w, h;
    std::uint32_t code, rgba, pad0, pad1;
  };
  static_assert(sizeof(Glyph) == 32);

  gl::PersistentBuffer ring_;
  gl::Texture atlas_;
  gl::VertexArray vao_;
  GLsizeiptr region_bytes_ = 0;
  unsigned slot_ = 0;
  std::uint32_t count_ = 0;
};

}  // namespace ftv
