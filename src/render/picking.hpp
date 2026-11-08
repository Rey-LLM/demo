#pragma once
// On-demand GPU picking: the node pass is re-rendered into an R32UI target
// that stores gl_InstanceID + 1; one pixel is read back asynchronously into a
// persistently mapped pixel-pack buffer guarded by a fence, so the CPU never
// waits on the GPU (the result is consumed a frame or two later).

#include <cstdint>
#include <optional>

#include "render/gl_raii.hpp"

namespace ftv {

class Picker {
 public:
  bool create();
  // (Re)allocates the pick targets when the framebuffer size changes.
  void resize(int width, int height);

  void request(int x, int y) noexcept;  // window pixel coordinates, origin top-left
  [[nodiscard]] bool pending_render() const noexcept { return want_render_; }

  // Binds the pick framebuffer and clears it (call before the pick draw).
  void begin_pass() noexcept;
  // Issues the asynchronous 1-pixel readback (call after the pick draw).
  void end_pass() noexcept;

  // Returns the picked node (or nullopt for background) once the readback
  // fence signalled; returns false while nothing new is available.
  bool poll(std::optional<std::uint32_t>& result) noexcept;

 private:
  gl::Framebuffer fbo_;
  gl::Texture id_tex_;
  gl::Texture depth_tex_;
  gl::PersistentBuffer pbo_;
  gl::Fence fence_;
  int width_ = 0, height_ = 0;
  int req_x_ = 0, req_y_ = 0;
  bool want_render_ = false;
  bool in_flight_ = false;
};

}  // namespace ftv
