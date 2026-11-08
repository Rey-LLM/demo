#include "render/hud.hpp"

#include <cstring>

#include "render/font.hpp"

namespace ftv {

bool HudRenderer::create(GLint ssboAlignment) {
  region_bytes_ = gl::align_up(static_cast<GLsizeiptr>(kMaxGlyphs * sizeof(Glyph)), ssboAlignment);
  if (!ring_.create(region_bytes_ * 3, GL_MAP_WRITE_BIT)) return false;
  const auto px = build_font_atlas();
  atlas_ = gl::create_texture(GL_TEXTURE_2D);
  glTextureStorage2D(atlas_.id(), 1, GL_R8, kGlyphCount * kCellW, kCellH);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTextureSubImage2D(atlas_.id(), 0, 0, 0, kGlyphCount * kCellW, kCellH, GL_RED, GL_UNSIGNED_BYTE, px.data());
  glTextureParameteri(atlas_.id(), GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTextureParameteri(atlas_.id(), GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  vao_ = gl::create_vertex_array();
  return true;
}

void HudRenderer::begin(unsigned slot) noexcept {
  slot_ = slot;
  count_ = 0;
}

float HudRenderer::text(float x, float y, float scale, std::uint32_t rgba, std::string_view s) noexcept {
  auto* base = reinterpret_cast<Glyph*>(static_cast<char*>(ring_.data()) + region_bytes_ * slot_);
  const float adv = static_cast<float>(kCellW) * scale;
  for (char c : s) {
    unsigned code = static_cast<unsigned char>(c);
    if (code >= 'a' && code <= 'z') code -= 32;  // font is upper case only
    if (code < 32 || code > 126) code = '?';
    if (code != ' ' && count_ < kMaxGlyphs) {
      base[count_++] = Glyph{x, y, static_cast<float>(kCellW) * scale, static_cast<float>(kCellH) * scale, code, rgba, 0, 0};
    }
    x += adv;
  }
  return x;
}

void HudRenderer::panel(float x, float y, float w, float h, std::uint32_t rgba) noexcept {
  if (count_ >= kMaxGlyphs) return;
  auto* base = reinterpret_cast<Glyph*>(static_cast<char*>(ring_.data()) + region_bytes_ * slot_);
  base[count_++] = Glyph{x, y, w, h, 127u, rgba, 0, 0};
}

void HudRenderer::draw(GLuint program) noexcept {
  if (count_ == 0) return;
  glUseProgram(program);
  glBindVertexArray(vao_.id());
  glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 8, ring_.id(), region_bytes_ * slot_,
                    static_cast<GLsizeiptr>(count_ * sizeof(Glyph)));
  glBindTextureUnit(1, atlas_.id());
  glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count_));
}

}  // namespace ftv
