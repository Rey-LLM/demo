#include "model/citylights.hpp"

#include <algorithm>
#include <cstring>

#include "core/file_util.hpp"

namespace ftv {

namespace {
template <class T>
void put(std::vector<std::uint8_t>& out, const T& v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}
}  // namespace

std::size_t CityLights::level_bytes(std::uint32_t w, std::uint32_t h, std::uint32_t l) noexcept {
  const std::size_t lw = std::max<std::uint32_t>(w >> l, 4), lh = std::max<std::uint32_t>(h >> l, 4);
  return (lw / 4) * (lh / 4) * 8;
}

std::size_t CityLights::level_offset(std::uint32_t l) const noexcept {
  std::size_t off = 0;
  for (std::uint32_t k = 0; k < l; ++k) off += level_bytes(width, height, k);
  return off;
}

bool parse_city_lights(const std::uint8_t* data, std::size_t size, CityLights& out, std::string& error) {
  if (size < 20 || std::memcmp(data, "FTNL", 4) != 0) {
    error = "not a city-lights file";
    return false;
  }
  std::uint32_t version = 0, w = 0, h = 0, levels = 0;
  std::memcpy(&version, data + 4, 4);
  std::memcpy(&w, data + 8, 4);
  std::memcpy(&h, data + 12, 4);
  std::memcpy(&levels, data + 16, 4);
  if (version != 1) {
    error = "unsupported city-lights version";
    return false;
  }
  if (w == 0 || h == 0 || w % 4 != 0 || h % 4 != 0 || w > 16384 || h > 8192) {
    error = "bad city-lights size";
    return false;
  }
  if (levels == 0 || levels > 14 || (std::min(w, h) >> (levels - 1)) < 4) {
    error = "bad city-lights mip count";
    return false;
  }
  std::size_t bytes = 0;
  for (std::uint32_t l = 0; l < levels; ++l) bytes += CityLights::level_bytes(w, h, l);
  if (size - 20 != bytes) {
    error = "city-lights payload size mismatch";
    return false;
  }
  out.width = w;
  out.height = h;
  out.levels = levels;
  out.bc4.assign(data + 20, data + 20 + bytes);
  return true;
}

std::optional<CityLights> load_city_lights(const std::string& path, std::string& error) {
  const auto bytes = read_text_file(path);  // binary-safe; disk or embedded
  if (!bytes) {
    error = "cannot read " + path;
    return std::nullopt;
  }
  CityLights c;
  if (!parse_city_lights(reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size(), c, error)) {
    error = path + ": " + error;
    return std::nullopt;
  }
  return c;
}

std::vector<std::uint8_t> serialize_city_lights(const CityLights& c) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), {'F', 'T', 'N', 'L'});
  put(out, std::uint32_t{1});
  put(out, c.width);
  put(out, c.height);
  put(out, c.levels);
  out.insert(out.end(), c.bc4.begin(), c.bc4.end());
  return out;
}

namespace {
void palette(std::uint8_t r0, std::uint8_t r1, std::uint8_t pal[8]) noexcept {
  pal[0] = r0;
  pal[1] = r1;
  if (r0 > r1) {
    for (int i = 1; i <= 6; ++i) pal[i + 1] = static_cast<std::uint8_t>(((7 - i) * r0 + i * r1 + 3) / 7);
  } else {
    for (int i = 1; i <= 4; ++i) pal[i + 1] = static_cast<std::uint8_t>(((5 - i) * r0 + i * r1 + 2) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
}
}  // namespace

void encode_bc4_block(const std::uint8_t px[16], std::uint8_t out[8]) noexcept {
  const auto [mn, mx] = std::minmax_element(px, px + 16);
  std::uint8_t r0 = *mx, r1 = *mn;
  std::uint8_t pal[8];
  if (r0 == r1) {
    // Flat block: r0 > r1 is required for the 8-value mode; any index 0 works.
    std::memset(out, 0, 8);
    out[0] = r0;
    out[1] = r0;
    return;
  }
  palette(r0, r1, pal);
  std::uint64_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0, bestErr = 1 << 30;
    for (int k = 0; k < 8; ++k) {
      const int e = std::abs(static_cast<int>(px[i]) - static_cast<int>(pal[k]));
      if (e < bestErr) {
        bestErr = e;
        best = k;
      }
    }
    bits |= static_cast<std::uint64_t>(best) << (3 * i);
  }
  out[0] = r0;
  out[1] = r1;
  for (int b = 0; b < 6; ++b) out[2 + b] = static_cast<std::uint8_t>(bits >> (8 * b));
}

void decode_bc4_block(const std::uint8_t in[8], std::uint8_t px[16]) noexcept {
  std::uint8_t pal[8];
  palette(in[0], in[1], pal);
  std::uint64_t bits = 0;
  for (int b = 0; b < 6; ++b) bits |= static_cast<std::uint64_t>(in[2 + b]) << (8 * b);
  for (int i = 0; i < 16; ++i) px[i] = pal[(bits >> (3 * i)) & 7u];
}

}  // namespace ftv
