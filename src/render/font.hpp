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
inline constexpr int kGlyphCount = 101;  // 32..132; 127 is a solid block (panels)
// HUD symbols after the ASCII range (use as single chars in HUD strings).
inline constexpr char kGlyphDot = '\x80';    // filled dot: a feed that is LIVE
inline constexpr char kGlyphRing = '\x81';   // outlined ring: a feed on REPLAY
inline constexpr char kGlyphArrow = '\x82';  // right arrow
inline constexpr char kGlyphEllipsis = '\x83';
inline constexpr char kGlyphMidDot = '\x84';

// Returns atlas pixels (kGlyphCount*kCellW x kCellH, one byte per pixel).
std::vector<std::uint8_t> build_font_atlas();

}  // namespace ftv
