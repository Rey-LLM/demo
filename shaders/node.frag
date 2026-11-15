#version 450 core
#include "common.glsl"

in vec2 v_px;
flat in float v_radius;
flat in float v_alpha;
layout(location = 0) out vec4 o_color;

void main() {
  // Analytic coverage of a disc of radius v_radius px.
  float cov = clamp(v_radius + 0.5 - length(v_px), 0.0, 1.0);
  if (cov <= 0.0) discard;
  float a = v_alpha * cov;
  o_color = vec4(u_ink.rgb * a, a);  // premultiplied
}
