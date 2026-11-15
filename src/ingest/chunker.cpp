#include "ingest/chunker.hpp"

#include <algorithm>
#include <cstring>

namespace ftv {

std::vector<Chunk> make_line_chunks(std::span<const char> data, std::size_t begin, std::size_t targetBytes) {
  std::vector<Chunk> chunks;
  if (targetBytes == 0) targetBytes = 16u << 20;
  const std::size_t n = data.size();
  std::size_t pos = std::min(begin, n);
  chunks.reserve(n / targetBytes + 2);
  while (pos < n) {
    std::size_t end = std::min(n, pos + targetBytes);
    if (end < n) {
      // Snap forward to just past the next newline.
      const void* nl = std::memchr(data.data() + end, '\n', n - end);
      end = nl ? static_cast<std::size_t>(static_cast<const char*>(nl) - data.data()) + 1 : n;
    }
    chunks.push_back(Chunk{pos, end - pos, static_cast<std::uint32_t>(chunks.size())});
    pos = end;
  }
  return chunks;
}

std::vector<Chunk> make_record_chunks(std::uint64_t recordsOffset, std::uint64_t recordCount, std::size_t recordSize,
                                      std::size_t targetBytes) {
  std::vector<Chunk> chunks;
  if (recordSize == 0) return chunks;
  const std::uint64_t perChunk = std::max<std::uint64_t>(1, targetBytes / recordSize);
  for (std::uint64_t r = 0; r < recordCount; r += perChunk) {
    const std::uint64_t cnt = std::min(perChunk, recordCount - r);
    chunks.push_back(Chunk{recordsOffset + r * recordSize, cnt * recordSize, static_cast<std::uint32_t>(chunks.size())});
  }
  return chunks;
}

}  // namespace ftv
