#pragma once
// Resources compiled into the binary (shaders, config, coastlines, the
// replay capture). cmake/embed_resources.cmake turns every file listed in
// FTVIZ_EMBEDDED_FILES into a constexpr byte array; this is the lookup.
//
// Paths of the form "embedded:/<relative path>" name an embedded file, for
// example "embedded:/shaders/arc.vert". read_text_file() and file_exists()
// in core/file_util.hpp understand them, so every loader takes either a
// path on disk (an explicit --hubs, --shaders, ... override) or an embedded
// one, and the default is always the embedded copy.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ftv {

struct EmbeddedFile {
  std::string_view path;       // relative to the source tree, '/' separated
  const unsigned char* data;   // `size` bytes followed by one 0 byte
  std::size_t size;
};

inline constexpr std::string_view kEmbeddedScheme = "embedded:";

// Every embedded file (generated: embedded_index.cpp).
std::span<const EmbeddedFile> embedded_files() noexcept;

// True for "embedded:..." paths.
[[nodiscard]] bool is_embedded_path(std::string_view path) noexcept;
// "embedded:/" + rel.
[[nodiscard]] std::string embedded_path(std::string_view rel);
// The file named by an "embedded:" path or by a bare relative path
// ("config/hubs.json"); '\' separators and "./" segments are accepted.
// nullptr when there is no such file.
[[nodiscard]] const EmbeddedFile* find_embedded(std::string_view path) noexcept;

}  // namespace ftv
