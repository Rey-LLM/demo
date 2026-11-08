#pragma once
// Small, non-hot file helpers (config and shader loading).

#include <optional>
#include <string>

namespace ftv {

std::optional<std::string> read_text_file(const std::string& path);
bool file_exists(const std::string& path);

}  // namespace ftv
