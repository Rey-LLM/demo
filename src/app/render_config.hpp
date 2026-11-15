#pragma once
// Loads config/render.json into RenderSettings (startup only).

#include <optional>
#include <string>

#include "core/json.hpp"
#include "render/render_settings.hpp"

namespace ftv {

// Missing keys keep their defaults; values are clamped to sane ranges.
RenderSettings render_settings_from_json(const JsonValue& root);
std::optional<RenderSettings> load_render_settings(const std::string& path, std::string& error);

}  // namespace ftv
