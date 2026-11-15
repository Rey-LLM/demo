#include "render/coastlines.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ftv {

namespace {

bool read_u32(const std::vector<char>& b, std::size_t& at, std::uint32_t& v) {
  if (at + 4 > b.size()) return false;
  std::memcpy(&v, b.data() + at, 4);  // the asset is little-endian, like every supported target
  at += 4;
  return true;
}

}  // namespace

long long load_coastline_file(const std::string& path, std::vector<CoastSegment>& out, std::string& error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return -1;
  }
  const std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::size_t at = 0;
  std::uint32_t version = 0, polylines = 0, points = 0;
  if (b.size() < 16 || std::memcmp(b.data(), "FTCL", 4) != 0) {
    error = path + ": not an FTCL coastline file";
    return -1;
  }
  at = 4;
  if (!read_u32(b, at, version) || !read_u32(b, at, polylines) || !read_u32(b, at, points) || version != 1) {
    error = path + ": unsupported FTCL header";
    return -1;
  }
  const std::size_t need = at + (static_cast<std::size_t>(polylines) + 1) * 4 + static_cast<std::size_t>(points) * 8;
  if (b.size() != need) {
    error = path + ": truncated or oversized FTCL file";
    return -1;
  }
  std::vector<std::uint32_t> offsets(static_cast<std::size_t>(polylines) + 1);
  for (auto& o : offsets) read_u32(b, at, o);
  if (offsets.front() != 0 || offsets.back() != points) {
    error = path + ": inconsistent polyline offsets";
    return -1;
  }
  const char* pts = b.data() + at;
  auto point = [pts](std::uint32_t i, float& lat, float& lon) {
    std::memcpy(&lat, pts + static_cast<std::size_t>(i) * 8, 4);
    std::memcpy(&lon, pts + static_cast<std::size_t>(i) * 8 + 4, 4);
  };
  const std::size_t before = out.size();
  for (std::size_t p = 0; p < polylines; ++p) {
    if (offsets[p + 1] < offsets[p]) {
      error = path + ": decreasing polyline offsets";
      out.resize(before);
      return -1;
    }
    for (std::uint32_t i = offsets[p]; i + 1 < offsets[p + 1]; ++i) {
      CoastSegment s{};
      point(i, s.lat0, s.lon0);
      point(i + 1, s.lat1, s.lon1);
      out.push_back(s);
    }
  }
  return static_cast<long long>(out.size() - before);
}

bool load_coastlines(const std::string& dir, CoastlineSet& out, std::string& error) {
  out = CoastlineSet{};
  const long long coarse = load_coastline_file(dir + "/coastlines_110m.bin", out.segments, error);
  if (coarse < 0) return false;
  out.coarse_count = static_cast<std::size_t>(coarse);
  std::string ignored;
  const long long fine = load_coastline_file(dir + "/coastlines_50m.bin", out.segments, ignored);
  out.fine_count = fine > 0 ? static_cast<std::size_t>(fine) : 0;
  return true;
}

}  // namespace ftv
