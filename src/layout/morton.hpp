#pragma once
// 30-bit 3D Morton codes (10 bits per axis). Octree depth is capped at 10
// levels; bodies that share a depth-10 cell simply share a (larger) leaf.

#include <cstdint>

namespace ftv {

inline constexpr unsigned kMortonBitsPerAxis = 10;
inline constexpr unsigned kMortonLevels = kMortonBitsPerAxis;

// Spreads the low 10 bits of v so there are two zero bits between each.
constexpr std::uint32_t morton_expand10(std::uint32_t v) noexcept {
  v &= 0x3FFu;
  v = (v | (v << 16)) & 0x030000FFu;
  v = (v | (v << 8)) & 0x0300F00Fu;
  v = (v | (v << 4)) & 0x030C30C3u;
  v = (v | (v << 2)) & 0x09249249u;
  return v;
}

constexpr std::uint32_t morton3(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
  return (morton_expand10(x) << 2) | (morton_expand10(y) << 1) | morton_expand10(z);
}

// Child octant digit of a code at tree level `level` (0 = root's children).
constexpr unsigned morton_digit(std::uint32_t code, unsigned level) noexcept {
  return (code >> (3u * (kMortonLevels - 1u - level))) & 7u;
}

}  // namespace ftv
