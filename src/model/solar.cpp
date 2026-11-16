#include "model/solar.hpp"

#include <cmath>
#include <cstdio>

namespace ftv {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

double wrap360(double x) noexcept {
  x = std::fmod(x, 360.0);
  return x < 0.0 ? x + 360.0 : x;
}
}  // namespace

std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

SubsolarPoint subsolar_point(std::int64_t utc_ms) noexcept {
  // Julian day (UTC; the ~69 s TT-UTC offset moves the sun by < 0.001 deg).
  const double jd = static_cast<double>(utc_ms) / 86400000.0 + 2440587.5;
  const double T = (jd - 2451545.0) / 36525.0;  // Julian centuries since J2000.0

  const double L0 = wrap360(280.46646 + T * (36000.76983 + T * 0.0003032));  // geometric mean longitude
  const double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);            // mean anomaly
  const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);      // orbit eccentricity
  const double Mr = M * kDeg;
  const double C = std::sin(Mr) * (1.914602 - T * (0.004817 + 0.000014 * T)) +
                   std::sin(2.0 * Mr) * (0.019993 - 0.000101 * T) + std::sin(3.0 * Mr) * 0.000289;
  const double trueLong = L0 + C;
  const double omega = (125.04 - 1934.136 * T) * kDeg;
  const double lambda = (trueLong - 0.00569 - 0.00478 * std::sin(omega)) * kDeg;  // apparent longitude
  const double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  const double eps = (eps0 + 0.00256 * std::cos(omega)) * kDeg;  // corrected obliquity

  SubsolarPoint p;
  p.lat_deg = std::asin(std::sin(eps) * std::sin(lambda)) / kDeg;

  const double y = std::tan(eps * 0.5) * std::tan(eps * 0.5);
  const double L0r = L0 * kDeg;
  const double eot = y * std::sin(2.0 * L0r) - 2.0 * e * std::sin(Mr) + 4.0 * e * y * std::sin(Mr) * std::cos(2.0 * L0r) -
                     0.5 * y * y * std::sin(4.0 * L0r) - 1.25 * e * e * std::sin(2.0 * Mr);
  p.eot_min = 4.0 * eot / kDeg;

  // Minutes since 00:00 UTC; the sun is overhead where true solar time is noon.
  const std::int64_t msDay = ((utc_ms % 86400000) + 86400000) % 86400000;
  const double minutes = static_cast<double>(msDay) / 60000.0;
  double lon = -(minutes + p.eot_min - 720.0) / 4.0;
  lon = std::fmod(lon + 180.0, 360.0);
  if (lon < 0.0) lon += 360.0;
  p.lon_deg = lon - 180.0;
  return p;
}

void sun_direction(const SubsolarPoint& p, float out[3]) noexcept {
  const double la = p.lat_deg * kDeg, lo = p.lon_deg * kDeg;
  out[0] = static_cast<float>(std::cos(la) * std::sin(lo));
  out[1] = static_cast<float>(std::sin(la));
  out[2] = static_cast<float>(std::cos(la) * std::cos(lo));
}

namespace {
bool digits(std::string_view s, std::size_t pos, std::size_t n, int& out) noexcept {
  if (pos + n > s.size()) return false;
  int v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}
}  // namespace

std::optional<std::int64_t> parse_iso8601_utc(std::string_view s) noexcept {
  int Y = 0, Mo = 0, D = 0, h = 0, mi = 0, sec = 0, ms = 0;
  if (!digits(s, 0, 4, Y) || s.size() < 16 || s[4] != '-' || !digits(s, 5, 2, Mo) || s[7] != '-' ||
      !digits(s, 8, 2, D) || (s[10] != 'T' && s[10] != 't' && s[10] != ' ') || !digits(s, 11, 2, h) || s[13] != ':' ||
      !digits(s, 14, 2, mi)) {
    return std::nullopt;
  }
  std::size_t pos = 16;
  if (pos < s.size() && s[pos] == ':') {
    if (!digits(s, pos + 1, 2, sec)) return std::nullopt;
    pos += 3;
    if (pos < s.size() && s[pos] == '.') {
      ++pos;
      int scale = 100;
      while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        ms += (s[pos] - '0') * scale;
        scale /= 10;
        ++pos;
      }
    }
  }
  if (pos < s.size()) {
    const std::string_view z = s.substr(pos);
    if (z != "Z" && z != "z" && z != "+00:00" && z != "+0000" && z != "-00:00") return std::nullopt;
  }
  static constexpr int kDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (Mo < 1 || Mo > 12 || D < 1 || h > 23 || mi > 59 || sec > 60) return std::nullopt;
  const bool leap = (Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0;
  if (D > kDays[Mo - 1] || (Mo == 2 && D == 29 && !leap)) return std::nullopt;
  const std::int64_t days = days_from_civil(Y, static_cast<unsigned>(Mo), static_cast<unsigned>(D));
  return ((days * 24 + h) * 60 + mi) * 60000 + static_cast<std::int64_t>(sec) * 1000 + ms;
}

void format_iso8601_utc(std::int64_t utc_ms, char* out, std::size_t cap) noexcept {
  std::int64_t z = utc_ms >= 0 ? utc_ms / 86400000 : (utc_ms - 86399999) / 86400000;
  const std::int64_t msDay = utc_ms - z * 86400000;
  // civil_from_days
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const long long yy = static_cast<long long>(y + (m <= 2 ? 1 : 0));
  const long long s = static_cast<long long>(msDay / 1000);
  std::snprintf(out, cap, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", yy, m, d, s / 3600, (s / 60) % 60, s % 60);
}

}  // namespace ftv
