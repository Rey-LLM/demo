#pragma once
// PNG output for --screenshot (stb_image_write, deterministic for identical pixels).

#include <cstdint>
#include <string>
#include <vector>

namespace ftv {

// Writes 8-bit RGB (alpha dropped), rows top to bottom.
bool write_png_rgb(const std::string& path, const std::vector<std::uint8_t>& rgba, int width, int height);

}  // namespace ftv
