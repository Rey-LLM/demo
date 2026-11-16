#include "model/cables.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>

#include "core/file_util.hpp"

namespace ftv {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kEarthKm = 6371.0088;

template <class T>
void put(std::vector<std::uint8_t>& out, const T& v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

struct Reader {
  const std::uint8_t* p;
  std::size_t left;
  template <class T>
  bool get(T& v) {
    if (left < sizeof(T)) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    left -= sizeof(T);
    return true;
  }
};

struct Vec3 {
  double x, y, z;
};
Vec3 unit(double latDeg, double lonDeg) noexcept {
  const double la = latDeg * kDeg, lo = lonDeg * kDeg;
  return {std::cos(la) * std::sin(lo), std::sin(la), std::cos(la) * std::cos(lo)};
}
double angle(const Vec3& a, const Vec3& b) noexcept {
  const double cx = a.y * b.z - a.z * b.y, cy = a.z * b.x - a.x * b.z, cz = a.x * b.y - a.y * b.x;
  const double c = std::sqrt(cx * cx + cy * cy + cz * cz);
  return std::atan2(c, a.x * b.x + a.y * b.y + a.z * b.z);
}
}  // namespace

double great_circle_km(double lat1, double lon1, double lat2, double lon2) noexcept {
  return angle(unit(lat1, lon1), unit(lat2, lon2)) * kEarthKm;
}

std::size_t CableSet::segment_count() const noexcept {
  std::size_t n = 0;
  for (const CablePolyline& c : cables) n += c.count > 1 ? c.count - 1 : 0;
  return n;
}

std::vector<std::uint8_t> serialize_cables(const CableSet& s) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), {'F', 'T', 'C', 'B'});
  put(out, std::uint32_t{1});
  put(out, static_cast<std::uint32_t>(s.cables.size()));
  put(out, static_cast<std::uint32_t>(s.point_count()));
  put(out, static_cast<std::uint32_t>(s.landings.size() / 2));
  put(out, std::uint32_t{0});
  for (const CablePolyline& c : s.cables) {
    put(out, c.first);
    put(out, c.count);
    put(out, static_cast<std::uint8_t>(c.corridor));
    put(out, std::uint8_t{0});
    put(out, std::uint16_t{0});
  }
  for (float f : s.latlon) put(out, f);
  for (float f : s.landings) put(out, f);
  return out;
}

bool parse_cables(const std::uint8_t* data, std::size_t size, CableSet& out, std::string& error) {
  Reader r{data, size};
  char magic[4];
  std::uint32_t version = 0, nc = 0, np = 0, nl = 0, reserved = 0;
  if (!r.get(magic) || std::memcmp(magic, "FTCB", 4) != 0 || !r.get(version) || version != 1 || !r.get(nc) ||
      !r.get(np) || !r.get(nl) || !r.get(reserved)) {
    error = "not a cables.bin v1 file";
    return false;
  }
  if (static_cast<std::uint64_t>(nc) * 12 + static_cast<std::uint64_t>(np) * 8 + static_cast<std::uint64_t>(nl) * 8 !=
      r.left) {
    error = "cables.bin: size does not match its header";
    return false;
  }
  out.cables.resize(nc);
  for (CablePolyline& c : out.cables) {
    std::uint8_t corridor = 0, z8 = 0;
    std::uint16_t z16 = 0;
    r.get(c.first);
    r.get(c.count);
    r.get(corridor);
    r.get(z8);
    r.get(z16);
    c.corridor = corridor < static_cast<std::uint8_t>(CableCorridor::Count) ? static_cast<CableCorridor>(corridor)
                                                                            : CableCorridor::Other;
    if (static_cast<std::uint64_t>(c.first) + c.count > np) {
      error = "cables.bin: polyline out of range";
      return false;
    }
  }
  out.latlon.resize(static_cast<std::size_t>(np) * 2);
  for (float& f : out.latlon) r.get(f);
  out.landings.resize(static_cast<std::size_t>(nl) * 2);
  for (float& f : out.landings) r.get(f);
  for (std::size_t i = 0; i + 1 < out.latlon.size(); i += 2) {
    if (!(std::fabs(out.latlon[i]) <= 90.0f) || !(std::fabs(out.latlon[i + 1]) <= 180.0f)) {
      error = "cables.bin: coordinate out of range";
      return false;
    }
  }
  return true;
}

std::optional<CableSet> load_cables(const std::string& path, std::string& error) {
  const auto bytes = read_text_file(path);  // binary-safe; disk or embedded
  if (!bytes) {
    error = "cannot read " + path;
    return std::nullopt;
  }
  CableSet s;
  if (!parse_cables(reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size(), s, error)) {
    error = path + ": " + error;
    return std::nullopt;
  }
  return s;
}

RouteTable build_routes(const CableSet& cables, const std::vector<HubSite>& hubs, const RouteOptions& opt) {
  const auto t0 = std::chrono::steady_clock::now();
  RouteTable rt;
  const std::size_t H = hubs.size();
  rt.hub_count = H;
  rt.first.assign(H * H, 0);
  rt.count.assign(H * H, 0);
  rt.length_km.assign(H * H, 0.0);
  if (cables.empty() || H == 0) return rt;

  // ---- graph nodes: polyline vertices merged on a ~merge_km grid ----------
  // Grid key: lat/lon quantised to cells of merge_km (longitude cells widen
  // toward the poles, which only merges a little more there).
  const double cellDeg = opt.merge_km / 111.2;
  std::unordered_map<std::uint64_t, std::uint32_t> cell;
  std::vector<Vec3> node;
  std::vector<double> nodeLat, nodeLon;
  auto key = [&](double lat, double lon) {
    const auto a = static_cast<std::int64_t>(std::floor((lat + 90.0) / cellDeg));
    const auto b = static_cast<std::int64_t>(std::floor((lon + 180.0) / cellDeg));
    return static_cast<std::uint64_t>(a) << 32 | static_cast<std::uint32_t>(b);
  };
  auto node_of = [&](double lat, double lon) -> std::uint32_t {
    const auto [it, inserted] = cell.try_emplace(key(lat, lon), static_cast<std::uint32_t>(node.size()));
    if (inserted) {
      node.push_back(unit(lat, lon));
      nodeLat.push_back(lat);
      nodeLon.push_back(lon);
    }
    return it->second;
  };
  struct Edge {
    std::uint32_t to;
    double w;  // radians
  };
  std::vector<std::vector<Edge>> adj;
  auto link = [&](std::uint32_t a, std::uint32_t b, double w) {
    if (a == b) return;
    if (adj.size() < node.size()) adj.resize(node.size());
    adj[a].push_back({b, w});
    adj[b].push_back({a, w});
  };
  for (const CablePolyline& c : cables.cables) {
    std::uint32_t prev = UINT32_MAX;
    for (std::uint32_t k = 0; k < c.count; ++k) {
      const std::size_t p = c.first + k;
      const std::uint32_t n = node_of(cables.latlon[2 * p], cables.latlon[2 * p + 1]);
      if (prev != UINT32_MAX) {
        adj.resize(node.size());
        link(prev, n, angle(node[prev], node[n]));
      }
      prev = n;
    }
  }
  // Landing points (merged into the same grid).
  std::vector<std::uint32_t> landing;
  for (std::size_t i = 0; i + 1 < cables.landings.size(); i += 2) {
    landing.push_back(node_of(cables.landings[i], cables.landings[i + 1]));
  }
  std::sort(landing.begin(), landing.end());
  landing.erase(std::unique(landing.begin(), landing.end()), landing.end());
  if (landing.empty()) {
    // No landing list: every polyline end is a landing.
    for (const CablePolyline& c : cables.cables) {
      if (c.count == 0) continue;
      const std::size_t a = c.first, b = c.first + c.count - 1;
      landing.push_back(node_of(cables.latlon[2 * a], cables.latlon[2 * a + 1]));
      landing.push_back(node_of(cables.latlon[2 * b], cables.latlon[2 * b + 1]));
    }
    std::sort(landing.begin(), landing.end());
    landing.erase(std::unique(landing.begin(), landing.end()), landing.end());
  }
  // Hub nodes and backhaul links.
  std::vector<std::uint32_t> hubNode(H, UINT32_MAX);
  for (std::size_t h = 0; h < H; ++h) {
    if (!hubs[h].routable) continue;
    hubNode[h] = static_cast<std::uint32_t>(node.size());
    node.push_back(unit(hubs[h].lat_deg, hubs[h].lon_deg));
    nodeLat.push_back(hubs[h].lat_deg);
    nodeLon.push_back(hubs[h].lon_deg);
    adj.resize(node.size());
    std::vector<std::pair<double, std::uint32_t>> near;
    for (std::uint32_t l : landing) {
      if (l >= adj.size() || adj[l].empty()) continue;  // a landing no cable reaches
      near.emplace_back(angle(node[hubNode[h]], node[l]), l);
    }
    std::sort(near.begin(), near.end());
    const double lim = opt.backhaul_km / kEarthKm;
    for (std::size_t k = 0; k < near.size(); ++k) {
      if (near[k].first > lim && k >= opt.backhaul_min) break;
      link(hubNode[h], near[k].second, near[k].first);
    }
  }
  adj.resize(node.size());

  // ---- Dijkstra from each routable hub ----------------------------------
  const std::size_t N = node.size();
  std::vector<double> dist(N);
  std::vector<std::uint32_t> prev(N);
  using QItem = std::pair<double, std::uint32_t>;
  for (std::size_t s = 0; s < H; ++s) {
    if (hubNode[s] == UINT32_MAX) continue;
    std::fill(dist.begin(), dist.end(), std::numeric_limits<double>::infinity());
    std::fill(prev.begin(), prev.end(), UINT32_MAX);
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> pq;
    dist[hubNode[s]] = 0.0;
    pq.push({0.0, hubNode[s]});
    while (!pq.empty()) {
      const auto [d, u] = pq.top();
      pq.pop();
      if (d > dist[u]) continue;
      // Paths may not pass through another hub's node (no transit via a city).
      if (u != hubNode[s] && std::find(hubNode.begin(), hubNode.end(), u) != hubNode.end()) continue;
      for (const Edge& e : adj[u]) {
        const double nd = d + e.w;
        if (nd < dist[e.to]) {
          dist[e.to] = nd;
          prev[e.to] = u;
          pq.push({nd, e.to});
        }
      }
    }
    for (std::size_t t = 0; t < H; ++t) {
      if (t == s || hubNode[t] == UINT32_MAX || !std::isfinite(dist[hubNode[t]])) continue;
      const double direct = angle(node[hubNode[s]], node[hubNode[t]]);
      if (dist[hubNode[t]] > opt.max_detour * direct + 0.02) continue;  // implausible detour: great circle instead
      std::vector<std::uint32_t> path;
      for (std::uint32_t v = hubNode[t]; v != UINT32_MAX; v = prev[v]) path.push_back(v);
      std::reverse(path.begin(), path.end());
      // Cumulative arc length, then resample to at most max_points.
      std::vector<double> cum(path.size(), 0.0);
      for (std::size_t k = 1; k < path.size(); ++k) cum[k] = cum[k - 1] + angle(node[path[k - 1]], node[path[k]]);
      const double total = cum.back();
      if (!(total > 0.0)) continue;
      const std::size_t pair = s * H + t;
      rt.first[pair] = static_cast<std::uint32_t>(rt.points.size());
      if (path.size() <= opt.max_points) {
        for (std::size_t k = 0; k < path.size(); ++k) {
          const Vec3& v = node[path[k]];
          rt.points.push_back({static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z),
                               static_cast<float>(cum[k] / total)});
        }
        rt.count[pair] = static_cast<std::uint32_t>(path.size());
      } else {
        std::size_t k = 0;
        for (std::size_t i = 0; i < opt.max_points; ++i) {
          const double target = total * static_cast<double>(i) / static_cast<double>(opt.max_points - 1);
          while (k + 1 < path.size() - 1 && cum[k + 1] < target) ++k;
          const double seg = cum[k + 1] - cum[k];
          const double f = seg > 0.0 ? std::clamp((target - cum[k]) / seg, 0.0, 1.0) : 0.0;
          const Vec3& a = node[path[k]];
          const Vec3& b = node[path[k + 1]];
          Vec3 v{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f};
          const double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
          rt.points.push_back({static_cast<float>(v.x / l), static_cast<float>(v.y / l), static_cast<float>(v.z / l),
                               static_cast<float>(target / total)});
        }
        rt.count[pair] = static_cast<std::uint32_t>(opt.max_points);
      }
      rt.length_km[pair] = total * kEarthKm;
      ++rt.routed_pairs;
    }
  }
  rt.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return rt;
}

}  // namespace ftv
