#pragma once
// Submarine cable geometry (assets/cables.bin, built by tools/build_cables.py)
// and the MODELED flow routing on it.
//
// cables.bin (little endian):
//   char[4] "FTCB", u32 version (1), u32 cable_count, u32 point_count,
//   u32 landing_count, u32 reserved
//   cable_count x { u32 first_point, u32 point_count, u8 corridor, u8[3] 0 }
//   point_count x { f32 lat_deg, f32 lon_deg }        polylines, DP-simplified
//   landing_count x { f32 lat_deg, f32 lon_deg }      landing points
// A cable is one polyline; a cable system with branches is several cables.
//
// Routing (load time only): vertices of all polylines, merged where they lie
// within 2 km of each other (shared landing points), form a graph whose
// edges are the polyline segments weighted by great-circle length. Each
// FIAT_CENTER hub is linked to the landing points within 800 km of it (or to
// its 3 nearest) by straight "backhaul" edges. Dijkstra from every hub gives
// one route per hub pair. Real traffic routing is not public: these routes
// are MODELED ("ROUTE · MODELED (cable geometry)") and are never presented as
// the path of any actual transaction.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ftv {

enum class CableCorridor : std::uint8_t { Other = 0, TransAtlantic, TransPacific, EuropeAsia, IntraAsia, Count };
inline constexpr const char* kCorridorNames[] = {"OTHER", "TRANS_ATLANTIC", "TRANS_PACIFIC", "EUROPE_ASIA",
                                                 "INTRA_ASIA"};

struct CablePolyline {
  std::uint32_t first = 0, count = 0;
  CableCorridor corridor = CableCorridor::Other;
};

struct CableSet {
  std::vector<CablePolyline> cables;
  std::vector<float> latlon;          // 2 per point
  std::vector<float> landings;        // 2 per landing point
  [[nodiscard]] std::size_t point_count() const noexcept { return latlon.size() / 2; }
  [[nodiscard]] std::size_t segment_count() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return cables.empty(); }
};

// Loads cables.bin from disk or the embedded file system.
std::optional<CableSet> load_cables(const std::string& path, std::string& error);
// Serialises a set (tests and tools).
std::vector<std::uint8_t> serialize_cables(const CableSet& set);
bool parse_cables(const std::uint8_t* data, std::size_t size, CableSet& out, std::string& error);

struct HubSite {
  double lat_deg = 0.0, lon_deg = 0.0;
  bool routable = true;  // FIAT_CENTER hubs; chain clouds and the fallback have no geography
};

// One modeled route: unit vectors on the sphere with the normalised
// cumulative arc length in w (0 at the source hub, 1 at the destination).
struct RoutePoint {
  float x, y, z, t;
};

struct RouteTable {
  std::size_t hub_count = 0;
  // Per ordered hub pair (src * hub_count + dst): first point and count
  // (count 0: no route, use the great-circle arc).
  std::vector<std::uint32_t> first, count;
  std::vector<RoutePoint> points;
  std::vector<double> length_km;  // per pair, 0 when unrouted
  std::size_t routed_pairs = 0;
  double build_ms = 0.0;
};

struct RouteOptions {
  double merge_km = 2.0;          // vertices closer than this are one graph node
  double backhaul_km = 800.0;     // hub -> landing links within this distance ...
  std::size_t backhaul_min = 3;   // ... or to this many nearest landing points
  double max_detour = 2.6;        // reject a route longer than this x great-circle distance
  std::size_t max_points = 512;   // per route (resampled by arc length when longer)
};

RouteTable build_routes(const CableSet& cables, const std::vector<HubSite>& hubs, const RouteOptions& opt = {});

// Great-circle distance in km (sphere, R = 6371.0088 km).
double great_circle_km(double lat1, double lon1, double lat2, double lon2) noexcept;

}  // namespace ftv
