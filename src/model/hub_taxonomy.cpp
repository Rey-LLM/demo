#include "model/hub_taxonomy.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/file_util.hpp"
#include "core/log.hpp"
#include "ingest/types.hpp"

namespace ftv {

namespace {

bool parse_vec(const JsonValue* v, float* out, std::size_t n) {
  if (!v || !v->is_array() || v->items().size() != n) return false;
  for (std::size_t i = 0; i < n; ++i) {
    if (!v->items()[i].is_number()) return false;
    out[i] = static_cast<float>(v->items()[i].as_number());
  }
  return true;
}

std::optional<HubType> parse_type(const std::string& s) {
  if (s == "FIAT_CENTER") return HubType::FiatCenter;
  if (s == "CHAIN_CLOUD") return HubType::ChainCloud;
  if (s == "FALLBACK") return HubType::Fallback;
  return std::nullopt;
}

}  // namespace

std::optional<HubTaxonomy> HubTaxonomy::load(const std::string& path, std::string& error) {
  auto text = read_text_file(path);
  if (!text) {
    error = "cannot read hub config '" + path + "'";
    return std::nullopt;
  }
  auto root = parse_json(*text, error);
  if (!root) {
    error = path + ": " + error;
    return std::nullopt;
  }
  return from_json(*root, error);
}

std::optional<HubTaxonomy> HubTaxonomy::from_json(const JsonValue& root, std::string& error) {
  HubTaxonomy tax;
  if (const JsonValue* lay = root.find("layout")) {
    tax.layout_json_ = *lay;
    tax.ring_.fiat_ring_radius = static_cast<float>(lay->number_or("fiat_ring_radius", 1000.0));
    tax.ring_.fiat_phase_deg = static_cast<float>(lay->number_or("fiat_phase_deg", 90.0));
    tax.ring_.chain_ring_radius = static_cast<float>(lay->number_or("chain_ring_radius", 700.0));
    tax.ring_.chain_ring_height = static_cast<float>(lay->number_or("chain_ring_height", 600.0));
    tax.ring_.chain_phase_deg = static_cast<float>(lay->number_or("chain_phase_deg", -1.0));
    tax.ring_.fallback_height = static_cast<float>(lay->number_or("fallback_height", -600.0));
  } else {
    tax.layout_json_ = JsonValue::make_object();
  }

  const JsonValue* hubs = root.find("hubs");
  if (!hubs || !hubs->is_array() || hubs->items().empty()) {
    error = "hub config must contain a non-empty \"hubs\" array";
    return std::nullopt;
  }
  std::vector<bool> explicitAnchor;
  for (const JsonValue& h : hubs->items()) {
    HubDef d;
    d.id = h.string_or("id", "");
    if (d.id.empty()) {
      error = "every hub needs an \"id\"";
      return std::nullopt;
    }
    d.display_name = h.string_or("display_name", d.id);
    auto type = parse_type(h.string_or("type", "FIAT_CENTER"));
    if (!type) {
      error = "hub '" + d.id + "' has an invalid type (FIAT_CENTER | CHAIN_CLOUD | FALLBACK)";
      return std::nullopt;
    }
    d.type = *type;
    d.radius = static_cast<float>(h.number_or("radius", 200.0));
    d.k_anchor = static_cast<float>(h.number_or("k_anchor", 0.02));
    float c[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    if (const JsonValue* col = h.find("color")) {
      if (!parse_vec(col, c, 4) && !parse_vec(col, c, 3)) {
        error = "hub '" + d.id + "' color must be [r,g,b] or [r,g,b,a]";
        return std::nullopt;
      }
    }
    d.color = glm::vec4(c[0], c[1], c[2], c[3]);
    float a[3] = {0, 0, 0};
    const bool hasAnchor = parse_vec(h.find("anchor"), a, 3);
    d.anchor = glm::vec3(a[0], a[1], a[2]);
    if (d.type == HubType::ChainCloud) {
      const std::string chain = h.string_or("chain", "");
      d.chain_code = pack_code8(chain);
      if (d.chain_code == 0) {
        error = "CHAIN_CLOUD hub '" + d.id + "' needs a \"chain\" id of 1..8 characters";
        return std::nullopt;
      }
    }
    if (d.radius <= 0.0f || d.k_anchor < 0.0f) {
      error = "hub '" + d.id + "' needs radius > 0 and k_anchor >= 0";
      return std::nullopt;
    }
    for (const auto& prev : tax.hubs_) {
      if (prev.id == d.id) {
        error = "duplicate hub id '" + d.id + "'";
        return std::nullopt;
      }
    }
    tax.hubs_.push_back(std::move(d));
    explicitAnchor.push_back(hasAnchor);
  }

  // Exactly one fallback hub; synthesise OFFSHORE_UNRESOLVED if absent.
  std::size_t fallbacks = 0;
  for (std::size_t i = 0; i < tax.hubs_.size(); ++i) {
    if (tax.hubs_[i].type == HubType::Fallback) {
      if (fallbacks++ == 0) tax.fallback_ = static_cast<std::uint16_t>(i);
    }
  }
  if (fallbacks > 1) {
    error = "only one FALLBACK hub is allowed";
    return std::nullopt;
  }
  if (fallbacks == 0) {
    HubDef d;
    d.id = "OFFSHORE_UNRESOLVED";
    d.display_name = "Offshore / unresolved";
    d.type = HubType::Fallback;
    d.radius = 160.0f;
    d.color = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f);
    tax.fallback_ = static_cast<std::uint16_t>(tax.hubs_.size());
    tax.hubs_.push_back(d);
    explicitAnchor.push_back(false);
  }
  if (tax.hubs_.size() > kMaxHubs) {
    error = "too many hubs (max 255)";
    return std::nullopt;
  }

  // Default arrangement for hubs without explicit anchors.
  std::size_t nFiat = 0, nChain = 0;
  for (const auto& h : tax.hubs_) {
    nFiat += h.type == HubType::FiatCenter;
    nChain += h.type == HubType::ChainCloud;
  }
  const float twoPi = 2.0f * std::numbers::pi_v<float>;
  const float fiatPhase = tax.ring_.fiat_phase_deg * std::numbers::pi_v<float> / 180.0f;
  const float chainPhase =
      tax.ring_.chain_phase_deg >= 0.0f
          ? tax.ring_.chain_phase_deg * std::numbers::pi_v<float> / 180.0f
          : fiatPhase + (nFiat > 0 ? 0.5f * twoPi / static_cast<float>(nFiat) : 0.0f);
  std::size_t iFiat = 0, iChain = 0;
  for (std::size_t i = 0; i < tax.hubs_.size(); ++i) {
    HubDef& h = tax.hubs_[i];
    if (h.type == HubType::FiatCenter) {
      const float ang = fiatPhase + twoPi * static_cast<float>(iFiat++) / static_cast<float>(std::max<std::size_t>(1, nFiat));
      if (!explicitAnchor[i]) h.anchor = glm::vec3(tax.ring_.fiat_ring_radius * std::cos(ang), 0.0f,
                                                  tax.ring_.fiat_ring_radius * std::sin(ang));
    } else if (h.type == HubType::ChainCloud) {
      const float ang = chainPhase + twoPi * static_cast<float>(iChain++) / static_cast<float>(std::max<std::size_t>(1, nChain));
      if (!explicitAnchor[i]) h.anchor = glm::vec3(tax.ring_.chain_ring_radius * std::cos(ang), tax.ring_.chain_ring_height,
                                                  tax.ring_.chain_ring_radius * std::sin(ang));
    } else if (!explicitAnchor[i]) {
      h.anchor = glm::vec3(0.0f, tax.ring_.fallback_height, 0.0f);
    }
  }
  for (std::size_t i = 0; i < tax.hubs_.size(); ++i) {
    for (std::size_t j = i + 1; j < tax.hubs_.size(); ++j) {
      const glm::vec3 d = tax.hubs_[i].anchor - tax.hubs_[j].anchor;
      const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
      if (dist < 1.2f * (tax.hubs_[i].radius + tax.hubs_[j].radius)) {
        FTV_LOG_WARN("hubs %s and %s overlap (distance %.1f, radii %.1f + %.1f)", tax.hubs_[i].id.c_str(),
                     tax.hubs_[j].id.c_str(), static_cast<double>(dist), static_cast<double>(tax.hubs_[i].radius),
                     static_cast<double>(tax.hubs_[j].radius));
      }
    }
  }

  for (std::size_t i = 0; i < tax.hubs_.size(); ++i) {
    if (tax.hubs_[i].type == HubType::ChainCloud) {
      tax.chain_map_.emplace_back(tax.hubs_[i].chain_code, static_cast<std::uint16_t>(i));
    }
  }
  if (const JsonValue* jm = root.find("jurisdictions")) {
    if (!jm->is_object()) {
      error = "\"jurisdictions\" must be an object mapping codes to hub ids";
      return std::nullopt;
    }
    for (const auto& kv : jm->members()) {
      const std::uint64_t code = pack_code8(kv.first);
      if (code == 0 || !kv.second.is_string()) {
        error = "invalid jurisdiction mapping entry '" + kv.first + "'";
        return std::nullopt;
      }
      auto hub = tax.find(kv.second.as_string());
      if (!hub) {
        error = "jurisdiction '" + kv.first + "' maps to unknown hub '" + kv.second.as_string() + "'";
        return std::nullopt;
      }
      tax.jurisdiction_map_.emplace_back(code, *hub);
    }
  }
  std::sort(tax.jurisdiction_map_.begin(), tax.jurisdiction_map_.end());
  std::sort(tax.chain_map_.begin(), tax.chain_map_.end());
  for (std::size_t i = 1; i < tax.jurisdiction_map_.size(); ++i) {
    if (tax.jurisdiction_map_[i].first == tax.jurisdiction_map_[i - 1].first) {
      error = "duplicate jurisdiction code in mapping";
      return std::nullopt;
    }
  }
  return tax;
}

std::uint16_t HubTaxonomy::hub_for_location(std::uint64_t code) const noexcept {
  const bool chain = (code & kChainDomainBit) != 0;
  const std::uint64_t c = code & ~kChainDomainBit;
  const auto& table = chain ? chain_map_ : jurisdiction_map_;
  auto it = std::lower_bound(table.begin(), table.end(), std::make_pair(c, std::uint16_t{0}));
  if (it != table.end() && it->first == c) return it->second;
  return fallback_;
}

std::optional<std::uint16_t> HubTaxonomy::find(const std::string& id) const noexcept {
  for (std::size_t i = 0; i < hubs_.size(); ++i) {
    if (hubs_[i].id == id) return static_cast<std::uint16_t>(i);
  }
  return std::nullopt;
}

}  // namespace ftv
