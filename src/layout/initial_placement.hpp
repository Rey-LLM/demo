#pragma once
// Deterministic initial placement: position = anchor + radius * f(key64),
// where f maps the 64-bit hash onto a point inside the unit ball using a
// Fibonacci-sphere direction (low 20 bits) and a cube-root radial mapping
// (next 24 bits) so points are uniform in volume. No randomness involved.

#include <cstdint>

namespace ftv {

struct Float3 {
  float x, y, z;
};

Float3 unit_ball_point(std::uint64_t key64) noexcept;

}  // namespace ftv
