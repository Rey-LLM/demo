#pragma once
// .ftxb packed binary transaction format (little-endian).
//
//   [0,64)            FtxbHeader
//   [64, 64+64*N)     N x FtxbRecord (fixed 64 bytes each)
//   [pool_offset, +pool_size)  string pool (raw identifier bytes, no NULs)
//
// String references are (offset, length) pairs relative to the start of the
// string pool. Records are fixed-size so chunks are formed by record count and
// can be parsed without any scanning.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace ftv {

static_assert(std::endian::native == std::endian::little, ".ftxb readers/writers assume a little-endian host");

inline constexpr char kFtxbMagic[4] = {'F', 'T', 'X', 'B'};
inline constexpr std::uint32_t kFtxbVersion = 1;

struct FtxbHeader {
  char magic[4];               // "FTXB"
  std::uint32_t version;       // kFtxbVersion
  std::uint64_t record_count;  // N
  std::uint64_t records_offset;  // always 64
  std::uint64_t pool_offset;   // byte offset of the string pool
  std::uint64_t pool_size;     // bytes in the string pool
  std::uint64_t reserved[3];
};
static_assert(sizeof(FtxbHeader) == 64);

struct FtxbRecord {
  std::int64_t timestamp_ms;   // 0  epoch milliseconds UTC
  std::int64_t amount_minor;   // 8  fixed-point minor units (scale = currency_scale(currency))
  std::uint32_t src_off;       // 16 string pool offset of src identifier
  std::uint32_t dst_off;       // 20
  std::uint32_t tx_id_off;     // 24
  std::uint16_t src_len;       // 28
  std::uint16_t dst_len;       // 30
  std::uint16_t tx_id_len;     // 32
  std::uint8_t channel;        // 34 ftv::Channel
  std::uint8_t reserved;       // 35 must be 0
  std::uint32_t currency;      // 36 pack_code4
  std::uint64_t src_jurisdiction;  // 40 pack_code8 (0 = unknown)
  std::uint64_t dst_jurisdiction;  // 48
  std::uint64_t chain;             // 56 pack_code8 (0 = fiat row)
};
static_assert(sizeof(FtxbRecord) == 64);

// Validates the header against the file size. Returns false with a message.
bool validate_ftxb_header(std::span<const char> file, FtxbHeader& out, std::string& error);

inline FtxbRecord load_ftxb_record(const char* p) noexcept {
  FtxbRecord r;
  std::memcpy(&r, p, sizeof(r));  // memcpy: no alignment or aliasing assumptions
  return r;
}

}  // namespace ftv
