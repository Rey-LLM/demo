#include "app/png_writer.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace ftv {

bool write_png_rgb(const std::string& path, const std::vector<std::uint8_t>& rgba, int width, int height) {
  if (width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
    return false;
  }
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
  for (std::size_t i = 0, n = rgb.size() / 3; i < n; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
  }
  stbi_write_png_compression_level = 9;
  return stbi_write_png(path.c_str(), width, height, 3, rgb.data(), width * 3) != 0;
}

}  // namespace ftv
