#include "ingest/ftxb_format.hpp"

namespace ftv {

bool validate_ftxb_header(std::span<const char> file, FtxbHeader& out, std::string& error) {
  if (file.size() < sizeof(FtxbHeader)) {
    error = "file too small for an .ftxb header";
    return false;
  }
  std::memcpy(&out, file.data(), sizeof(out));
  if (std::memcmp(out.magic, kFtxbMagic, 4) != 0) {
    error = "bad .ftxb magic";
    return false;
  }
  if (out.version != kFtxbVersion) {
    error = "unsupported .ftxb version " + std::to_string(out.version);
    return false;
  }
  if (out.records_offset != sizeof(FtxbHeader)) {
    error = "unexpected .ftxb records offset";
    return false;
  }
  const std::uint64_t recordsEnd = out.records_offset + out.record_count * sizeof(FtxbRecord);
  if (out.record_count > (file.size() / sizeof(FtxbRecord)) || recordsEnd > file.size() ||
      out.pool_offset < recordsEnd || out.pool_offset > file.size() ||
      out.pool_size > file.size() - out.pool_offset) {
    error = ".ftxb header sections exceed the file size";
    return false;
  }
  return true;
}

}  // namespace ftv
