#pragma once
// RAII read-only memory mapping (mmap on POSIX, CreateFileMapping/MapViewOfFile
// on Windows). Move-only; the mapping lives as long as the object.

#include <cstddef>
#include <span>
#include <string>

namespace ftv {

class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile();
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  // Maps the whole file. Returns false and fills `error` on failure.
  bool open(const std::string& path, std::string& error);
  void close() noexcept;

  [[nodiscard]] const char* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool is_open() const noexcept { return opened_; }
  [[nodiscard]] std::span<const char> bytes() const noexcept { return {data_, size_}; }

 private:
  const char* data_ = nullptr;
  std::size_t size_ = 0;
  bool opened_ = false;
#if defined(_WIN32)
  void* file_ = nullptr;
  void* mapping_ = nullptr;
#endif
};

}  // namespace ftv
