#include "core/embedded_fs.hpp"

namespace ftv {

bool is_embedded_path(std::string_view path) noexcept { return path.substr(0, kEmbeddedScheme.size()) == kEmbeddedScheme; }

std::string embedded_path(std::string_view rel) {
  while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) rel.remove_prefix(1);
  std::string out(kEmbeddedScheme);
  out += '/';
  out += rel;
  return out;
}

const EmbeddedFile* find_embedded(std::string_view path) noexcept {
  if (is_embedded_path(path)) path.remove_prefix(kEmbeddedScheme.size());
  // Normalise into a fixed buffer: '\' -> '/', drop leading separators and "./" segments.
  char buf[512];
  std::size_t n = 0;
  std::size_t i = 0;
  while (i < path.size()) {
    const char c = path[i] == '\\' ? '/' : path[i];
    const bool segStart = n == 0 || buf[n - 1] == '/';
    if (segStart && c == '/') {
      ++i;
      continue;
    }
    if (segStart && c == '.' && (i + 1 == path.size() || path[i + 1] == '/' || path[i + 1] == '\\')) {
      i += 2;
      continue;
    }
    if (n == sizeof(buf)) return nullptr;
    buf[n++] = c;
    ++i;
  }
  const std::string_view key(buf, n);
  for (const EmbeddedFile& f : embedded_files()) {
    if (f.path == key) return &f;
  }
  return nullptr;
}

}  // namespace ftv
