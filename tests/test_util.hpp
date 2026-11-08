#pragma once
// Shared helpers for tests that need on-disk inputs.

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace ftv::test {

class TempFile {
 public:
  TempFile(std::string_view name, std::string_view contents) {
    path_ = (std::filesystem::temp_directory_path() / (std::string("ftviz_test_") + std::string(name))).string();
    std::FILE* f = std::fopen(path_.c_str(), "wb");
    if (f) {
      std::fwrite(contents.data(), 1, contents.size(), f);
      std::fclose(f);
    }
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

}  // namespace ftv::test
