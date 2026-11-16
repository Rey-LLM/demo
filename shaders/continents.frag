#version 450 core
// 1 px coastline with analytic coverage across the line.
#include "common.glsl"

in float v_side;
flat in float v_alpha;
flat in vec3 v_rgb;
layout(location = 0) out vec4 o_color;

void main() {
  float cov = clamp(0.5 + 0.5 - abs(v_side), 0.0, 1.0);  // half-width 0.5 px, 1 px ramp
  float a = v_alpha * cov;
  if (a <= 0.0) discard;
  o_color = vec4(v_rgb * a, a);  // premultiplied
}
