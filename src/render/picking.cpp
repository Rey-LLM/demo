#include "render/picking.hpp"

#include <algorithm>
#include <cstring>

namespace ftv {

bool Picker::create() {
  // 4-byte readback target, persistently mapped for reading.
  return pbo_.create(sizeof(std::uint32_t), GL_MAP_READ_BIT);
}

void Picker::resize(int width, int height) {
  if (width == width_ && height == height_ && fbo_) return;
  width_ = width;
  height_ = height;
  fbo_ = gl::create_framebuffer();
  id_tex_ = gl::create_texture(GL_TEXTURE_2D);
  depth_tex_ = gl::create_texture(GL_TEXTURE_2D);
  glTextureStorage2D(id_tex_.id(), 1, GL_R32UI, std::max(1, width), std::max(1, height));
  glTextureStorage2D(depth_tex_.id(), 1, GL_DEPTH_COMPONENT32F, std::max(1, width), std::max(1, height));
  glNamedFramebufferTexture(fbo_.id(), GL_COLOR_ATTACHMENT0, id_tex_.id(), 0);
  glNamedFramebufferTexture(fbo_.id(), GL_DEPTH_ATTACHMENT, depth_tex_.id(), 0);
  glNamedFramebufferDrawBuffer(fbo_.id(), GL_COLOR_ATTACHMENT0);
  glNamedFramebufferReadBuffer(fbo_.id(), GL_COLOR_ATTACHMENT0);
}

void Picker::request(int x, int y) noexcept {
  req_x_ = x;
  req_y_ = y;
  want_render_ = true;
}

void Picker::begin_pass() noexcept {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id());
  glViewport(0, 0, width_, height_);
  const GLuint zero = 0;
  const GLfloat depth0 = 0.0f;  // reverse-Z clear
  glClearNamedFramebufferuiv(fbo_.id(), GL_COLOR, 0, &zero);
  glClearNamedFramebufferfv(fbo_.id(), GL_DEPTH, 0, &depth0);
}

void Picker::end_pass() noexcept {
  const int x = std::clamp(req_x_, 0, std::max(0, width_ - 1));
  const int y = std::clamp(height_ - 1 - req_y_, 0, std::max(0, height_ - 1));  // GL origin is bottom-left
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_.id());
  glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);  // async into the PBO
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  fence_.insert();
  want_render_ = false;
  in_flight_ = true;
}

bool Picker::poll(std::optional<std::uint32_t>& result) noexcept {
  if (!in_flight_ || !fence_.signaled()) return false;
  std::uint32_t v = 0;
  std::memcpy(&v, pbo_.data(), sizeof(v));  // coherent mapping: visible once the fence signalled
  in_flight_ = false;
  fence_.reset();
  result = v == 0 ? std::nullopt : std::optional<std::uint32_t>(v - 1);
  return true;
}

}  // namespace ftv
