#pragma once
// Splits a byte range into ~16 MB chunks whose boundaries are snapped forward
// to the next '\n', so no CSV record ever spans two chunks.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ftv {

struct Chunk {
  std::uint64_t offset = 0;  // absolute byte offset in the file
  std::uint64_t size = 0;
  std::uint32_t index = 0;
};

// Text chunks over [begin, data.size()).
std::vector<Chunk> make_line_chunks(std::span<const char> data, std::size_t begin, std::size_t targetBytes);

// Fixed-size record chunks for the binary format (records never split).
std::vector<Chunk> make_record_chunks(std::uint64_t recordsOffset, std::uint64_t recordCount, std::size_t recordSize,
                                      std::size_t targetBytes);

}  // namespace ftv
