#include "core/file_util.hpp"

#include <cstdio>
#include <filesystem>

#include "core/embedded_fs.hpp"

namespace ftv {

std::optional<std::string> read_text_file(const std::string& path) {
  if (is_embedded_path(path)) {
    const EmbeddedFile* e = find_embedded(path);
    if (!e) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(e->data), e->size);
  }
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return std::nullopt;
  std::string out;
  char buf[16384];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

bool file_exists(const std::string& path) {
  if (is_embedded_path(path)) return find_embedded(path) != nullptr;
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

}  // namespace ftv
