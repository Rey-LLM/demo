#pragma once
// Named camera presets for --camera and the screenshot mode:
//   globe       full globe (render.json start yaw / pitch / distance)
//   london      close-up of the LONDON hub (any hub: hub:<ID>)
//   whale       a large inter-hub whale event, its head mid-flight
//   graticule   grazing view of the grid near the limb
// Presets also choose a replay time: the whale preset times its event; the
// others use the instant with the most whale events in flight.

#include <cstdint>
#include <functional>
#include <string>

#include <glm/vec3.hpp>

#include "model/hub_taxonomy.hpp"
#include "model/whales.hpp"
#include "render/render_settings.hpp"

namespace ftv {

struct ViewSetup {
  glm::vec3 target{0.0f};
  float distance = 3000.0f;
  float yaw = 0.0f;    // radians
  float pitch = 0.0f;  // radians
  float time = 0.0f;   // replay time (s)
};

using NodePositionFn = std::function<glm::vec3(std::uint32_t)>;

// Returns false with `error` set for an unknown preset or hub id.
bool camera_preset(const std::string& name, const HubTaxonomy& taxonomy, const WhaleSet& whales,
                   const NodePositionFn& nodePos, const RenderSettings& settings, ViewSetup& out, std::string& error);

// Replay time in [0, replay_seconds) with the most whale events in flight
// (ties: earliest). `keep` (optional) filters the events considered.
float busiest_replay_time(const WhaleSet& whales, const Theme& theme, const std::function<bool(const WhaleEvent&)>& keep);

// Close-up of one hub (allocation free; used by the 1-9 keys). Leaves time at 0.
ViewSetup hub_view(const HubDef& hub, const RenderSettings& settings) noexcept;

// Index of the event the "whale" preset frames (or -1 when there is none).
int whale_preset_event(const WhaleSet& whales, const NodePositionFn& nodePos);

}  // namespace ftv
