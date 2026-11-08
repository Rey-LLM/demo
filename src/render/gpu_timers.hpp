#pragma once
// GL_TIME_ELAPSED timer queries for every render pass, triple-buffered by
// frame slot. Results of slot s are read only after that slot's frame fence
// has signalled, so reading them never stalls the pipeline.

#include <array>
#include <cstdint>

#include "render/gl_raii.hpp"

namespace ftv {

enum class GpuPass : std::uint8_t { Cull = 0, Nodes, Hubs, Corridors, Edges, Pick, Hud, Count };
inline constexpr std::size_t kGpuPassCount = static_cast<std::size_t>(GpuPass::Count);
inline constexpr std::array<const char*, kGpuPassCount> kGpuPassNames = {"CULL",  "NODES", "HUBS", "CORRIDORS",
                                                                         "EDGES", "PICK",  "HUD"};

class GpuTimers {
 public:
  static constexpr unsigned kSlots = 3;

  void create();
  void begin(unsigned slot, GpuPass pass) noexcept;
  void end() noexcept;
  // Collects the results for `slot` (call after waiting on that slot's fence).
  void collect(unsigned slot) noexcept;

  [[nodiscard]] float ms(GpuPass pass) const noexcept { return smoothed_[static_cast<std::size_t>(pass)]; }
  [[nodiscard]] float total_ms() const noexcept;

 private:
  std::array<std::array<gl::Query, kGpuPassCount>, kSlots> queries_;
  std::array<std::array<bool, kGpuPassCount>, kSlots> issued_{};
  std::array<float, kGpuPassCount> smoothed_{};
  bool active_ = false;
};

}  // namespace ftv
