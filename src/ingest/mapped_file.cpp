#include "ingest/mapped_file.hpp"

#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace ftv {

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    close();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    opened_ = std::exchange(other.opened_, false);
#if defined(_WIN32)
    file_ = std::exchange(other.file_, nullptr);
    mapping_ = std::exchange(other.mapping_, nullptr);
#endif
  }
  return *this;
}

#if defined(_WIN32)

bool MappedFile::open(const std::string& path, std::string& error) {
  close();
  HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    error = "cannot open '" + path + "' (CreateFile error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(f, &sz)) {
    error = "cannot stat '" + path + "'";
    CloseHandle(f);
    return false;
  }
  file_ = f;
  size_ = static_cast<std::size_t>(sz.QuadPart);
  opened_ = true;
  if (size_ == 0) return true;  // empty file: nothing to map
  HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!m) {
    error = "CreateFileMapping failed for '" + path + "'";
    close();
    return false;
  }
  mapping_ = m;
  void* view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
  if (!view) {
    error = "MapViewOfFile failed for '" + path + "'";
    close();
    return false;
  }
  data_ = static_cast<const char*>(view);
  return true;
}

void MappedFile::close() noexcept {
  if (data_) UnmapViewOfFile(data_);
  if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
  if (file_) CloseHandle(static_cast<HANDLE>(file_));
  data_ = nullptr;
  mapping_ = nullptr;
  file_ = nullptr;
  size_ = 0;
  opened_ = false;
}

#else

bool MappedFile::open(const std::string& path, std::string& error) {
  close();
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    error = "cannot open '" + path + "': " + std::strerror(errno);
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    error = "cannot stat '" + path + "': " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  size_ = static_cast<std::size_t>(st.st_size);
  opened_ = true;
  if (size_ == 0) {
    ::close(fd);
    return true;
  }
  void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);  // the mapping keeps its own reference to the file
  if (p == MAP_FAILED) {
    error = "mmap failed for '" + path + "': " + std::strerror(errno);
    size_ = 0;
    opened_ = false;
    return false;
  }
  // Parsers stream through the file once front to back.
  ::madvise(p, size_, MADV_SEQUENTIAL | MADV_WILLNEED);
  data_ = static_cast<const char*>(p);
  return true;
}

void MappedFile::close() noexcept {
  if (data_) ::munmap(const_cast<char*>(data_), size_);
  data_ = nullptr;
  size_ = 0;
  opened_ = false;
}

#endif

}  // namespace ftv
