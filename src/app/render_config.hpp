#pragma once
// Loads config/render.json and config/theme.json into RenderSettings (startup only).

#include <optional>
#include <string>

#include "core/json.hpp"
#include "render/render_settings.hpp"

namespace ftv {

// Missing keys keep their defaults; values are clamped to sane ranges.
RenderSettings render_settings_from_json(const JsonValue& root);
std::optional<RenderSettings> load_render_settings(const std::string& path, std::string& error);

// Parses theme.json. Colours must be "#RRGGBB" (sRGB); they are converted to
// linear RGB. Alphas and intensities are clamped to the documented ranges.
std::optional<Theme> theme_from_json(const JsonValue& root, std::string& error);
std::optional<Theme> load_theme(const std::string& path, std::string& error);

// "#RRGGBB" -> linear RGB. Returns false for malformed input.
bool parse_hex_color(const std::string& hex, float (&linear)[3]) noexcept;
float srgb_to_linear(float c) noexcept;

}  // namespace ftv
