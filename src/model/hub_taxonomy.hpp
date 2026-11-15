#pragma once
// Data-driven macro-hub taxonomy loaded from config/hubs.json.
//
// Hub kinds: FIAT_CENTER (financial centres on the y=0 ring), CHAIN_CLOUD (one
// volumetric cloud per chain on the elevated ring) and FALLBACK
// (OFFSHORE_UNRESOLVED, below the centre). Anchors may be given explicitly or
// left out, in which case the default ring arrangement is computed.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/json.hpp"

namespace ftv {

enum class HubType : std::uint8_t { FiatCenter = 0, ChainCloud = 1, Fallback = 2 };

struct HubDef {
  std::string id;
  std::string display_name;
  HubType type = HubType::FiatCenter;
  glm::vec3 anchor{0.0f};
  float radius = 200.0f;
  glm::vec4 color{1.0f};
  float k_anchor = 0.02f;
  std::uint64_t chain_code = 0;  // pack_code8 of the chain id for CHAIN_CLOUD hubs
};

struct HubRingLayout {
  float fiat_ring_radius = 1000.0f;
  float fiat_phase_deg = 90.0f;
  float chain_ring_radius = 700.0f;
  float chain_ring_height = 600.0f;
  float chain_phase_deg = -1.0f;  // < 0 => half a fiat step, so clouds sit between fiat hubs
  float fallback_height = -600.0f;
};

class HubTaxonomy {
 public:
  static constexpr std::uint16_t kMaxHubs = 255;  // hub ids are packed into 8 bits on the GPU

  static std::optional<HubTaxonomy> load(const std::string& path, std::string& error);
  static std::optional<HubTaxonomy> from_json(const JsonValue& root, std::string& error);

  [[nodiscard]] const std::vector<HubDef>& hubs() const noexcept { return hubs_; }
  [[nodiscard]] std::size_t size() const noexcept { return hubs_.size(); }
  [[nodiscard]] std::uint16_t fallback() const noexcept { return fallback_; }
  [[nodiscard]] const HubRingLayout& ring() const noexcept { return ring_; }
  [[nodiscard]] const JsonValue& layout_json() const noexcept { return layout_json_; }

  // Maps a location code (packed jurisdiction, or packed chain | domain bit)
  // to a hub id. Unknown or empty codes map to the fallback hub.
  [[nodiscard]] std::uint16_t hub_for_location(std::uint64_t code) const noexcept;

  [[nodiscard]] std::optional<std::uint16_t> find(const std::string& id) const noexcept;

 private:
  std::vector<HubDef> hubs_;
  std::vector<std::pair<std::uint64_t, std::uint16_t>> jurisdiction_map_;  // sorted by code
  std::vector<std::pair<std::uint64_t, std::uint16_t>> chain_map_;         // sorted by code
  std::uint16_t fallback_ = 0;
  HubRingLayout ring_;
  JsonValue layout_json_;
};

}  // namespace ftv
