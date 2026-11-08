#include "layout/initial_placement.hpp"

#include <cmath>
#include <numbers>

namespace ftv {

Float3 unit_ball_point(std::uint64_t key64) noexcept {
  constexpr std::uint32_t kFibBits = 20;
  constexpr double kFibN = static_cast<double>(1u << kFibBits);
  constexpr double kGoldenAngle = 2.39996322972865332;  // pi * (3 - sqrt(5))
  const auto i = static_cast<double>(key64 & ((1u << kFibBits) - 1));
  const auto r24 = static_cast<double>((key64 >> kFibBits) & 0xFFFFFFu);

  // Fibonacci sphere point i of N: z spaced uniformly, azimuth by golden angle.
  const double z = 1.0 - (2.0 * i + 1.0) / kFibN;
  const double rxy = std::sqrt(std::max(0.0, 1.0 - z * z));
  const double phi = std::fmod(i * kGoldenAngle, 2.0 * std::numbers::pi);
  // Cube-root radial mapping => uniform density inside the ball.
  const double r = std::cbrt((r24 + 0.5) / 16777216.0);
  return Float3{static_cast<float>(r * rxy * std::cos(phi)), static_cast<float>(r * z),
                static_cast<float>(r * rxy * std::sin(phi))};
}

}  // namespace ftv
