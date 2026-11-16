#pragma once
// Subsolar point from UTC time: the NOAA / Meeus low-precision solar position
// (solar declination plus the equation of time; error well under 0.1 degree
// between 1950 and 2050). Drives the day/night terminator on the globe.

#include <cstdint>
#include <optional>
#include <string_view>

namespace ftv {

struct SubsolarPoint {
  double lat_deg = 0.0;  // = solar declination
  double lon_deg = 0.0;  // east positive, [-180, 180)
  double eot_min = 0.0;  // equation of time (minutes)
};

// `utc_ms`: UTC epoch milliseconds.
SubsolarPoint subsolar_point(std::int64_t utc_ms) noexcept;

// Unit vector toward the sun in the globe frame used by the renderer
// (y = north pole, z = lon 0 at the equator, x = lon 90 E).
void sun_direction(const SubsolarPoint& p, float out[3]) noexcept;

// Parses an ISO 8601 UTC time: YYYY-MM-DDTHH:MM[:SS[.fff]][Z|+00:00]
// (also with a space for 'T'). Returns UTC epoch milliseconds.
std::optional<std::int64_t> parse_iso8601_utc(std::string_view s) noexcept;
// Formats UTC epoch ms as "YYYY-MM-DDTHH:MM:SSZ" into `out` (>= 21 bytes).
void format_iso8601_utc(std::int64_t utc_ms, char* out, std::size_t cap) noexcept;

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept;

}  // namespace ftv
