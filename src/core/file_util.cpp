#include "core/file_util.hpp"

#include <cstdio>
#include <filesystem>

namespace ftv {

std::optional<std::string> read_text_file(const std::string& path) {
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
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

}  // namespace ftv
