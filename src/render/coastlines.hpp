#pragma once
// Natural Earth coastlines (public domain), converted offline by
// tools/build_coastlines.py into assets/coastlines_{110m,50m}.bin. Nothing is
// fetched at runtime.
//
// The loader flattens every polyline into independent segments, one
// {lat0, lon0, lat1, lon1} float4 (degrees) per segment: the continents pass
// draws one instance per segment and fetches its endpoints from an SSBO.

#include <cstddef>
#include <string>
#include <vector>

namespace ftv {

struct CoastSegment {
  float lat0, lon0, lat1, lon1;
};
static_assert(sizeof(CoastSegment) == 16);

struct CoastlineSet {
  std::vector<CoastSegment> segments;  // 1:110m first, then 1:50m
  std::size_t coarse_count = 0;        // segments of the 1:110m set
  std::size_t fine_count = 0;          // segments of the 1:50m set (0 when not loaded)
  [[nodiscard]] bool empty() const noexcept { return segments.empty(); }
};

// Appends the segments of one FTCL file. Returns the number of segments read,
// or -1 with `error` set for a missing or malformed file.
long long load_coastline_file(const std::string& path, std::vector<CoastSegment>& out, std::string& error);

// Loads <dir>/coastlines_110m.bin (required) and <dir>/coastlines_50m.bin
// (optional). Returns false with `error` set when the 1:110m file is missing.
bool load_coastlines(const std::string& assetDir, CoastlineSet& out, std::string& error);

}  // namespace ftv
