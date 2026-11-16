#pragma once
// Night-side city lights (assets/citylights.bin, built by
// tools/build_citylights.py from NASA Black Marble): one BC4
// (GL_COMPRESSED_RED_RGTC1) equirectangular map, already log-tone mapped to
// [0, 1]. Row 0 is latitude +90, column 0 is longitude -180.
//
// citylights.bin (little endian):
//   char[4] "FTNL", u32 version (1), u32 width, u32 height, u32 levels,
//   per mip level l (w = width >> l, h = height >> l, both >= 4):
//     (w / 4) * (h / 4) BC4 blocks of 8 bytes, row-major.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ftv {

struct CityLights {
  std::uint32_t width = 0, height = 0, levels = 0;
  std::vector<std::uint8_t> bc4;  // all levels, level 0 first
  // Byte offset and size of level l inside bc4.
  [[nodiscard]] std::size_t level_offset(std::uint32_t l) const noexcept;
  [[nodiscard]] static std::size_t level_bytes(std::uint32_t w, std::uint32_t h, std::uint32_t l) noexcept;
};

std::optional<CityLights> load_city_lights(const std::string& path, std::string& error);
bool parse_city_lights(const std::uint8_t* data, std::size_t size, CityLights& out, std::string& error);
std::vector<std::uint8_t> serialize_city_lights(const CityLights& lights);

// Encodes one 4x4 block of 8-bit values as BC4 (exhaustive over both endpoint
// modes is not needed: min/max endpoints, 8-value interpolation).
void encode_bc4_block(const std::uint8_t px[16], std::uint8_t out[8]) noexcept;
// Decodes one BC4 block (tests).
void decode_bc4_block(const std::uint8_t in[8], std::uint8_t px[16]) noexcept;

}  // namespace ftv
