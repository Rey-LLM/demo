#pragma once
// Small, non-hot file helpers (config, shader and asset loading). Paths may
// name a file on disk or a resource compiled into the binary
// ("embedded:/config/hubs.json", see core/embedded_fs.hpp).

#include <optional>
#include <string>

namespace ftv {

// Whole file as bytes (binary-safe despite the name).
std::optional<std::string> read_text_file(const std::string& path);
bool file_exists(const std::string& path);

}  // namespace ftv
