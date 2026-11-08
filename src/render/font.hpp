#pragma once
// Built-in 5x7 bitmap font (printable ASCII; lower case renders as upper
// case) baked into a single-row R8 atlas of 6x8 cells.

#include <cstdint>
#include <vector>

namespace ftv {

inline constexpr int kGlyphW = 5;
inline constexpr int kGlyphH = 7;
inline constexpr int kCellW = 6;
inline constexpr int kCellH = 8;
inline constexpr int kGlyphFirst = 32;
inline constexpr int kGlyphCount = 96;  // 32..127; 127 is a solid block (panels)

// Returns atlas pixels (kGlyphCount*kCellW x kCellH, one byte per pixel).
std::vector<std::uint8_t> build_font_atlas();

}  // namespace ftv
