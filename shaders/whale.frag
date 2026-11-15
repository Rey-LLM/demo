#version 450 core
#include "common.glsl"

in float v_t;
in float v_side;
in vec2 v_px;
flat in float v_head;
flat in float v_trail;
flat in float v_halfw;
flat in float v_intensity;
flat in uint v_kind;

layout(location = 1) out vec4 o_emissive;

void main() {
  float e;
  if (v_kind == 0u) {
    // Trail: zero ahead of the head and beyond the trail, brightest at the head.
    float tail = v_head - v_trail;
    if (v_t > v_head || v_t < tail) discard;
    float f = clamp((v_t - tail) / v_trail, 0.0, 1.0);
    float cov = clamp(v_halfw + 0.5 - abs(v_side), 0.0, 1.0);
    e = v_intensity * f * f * cov;
  } else {
    // Single flash: a thin ring expanding from the destination plus a core.
    float d = length(v_px);
    float ring = clamp(1.0 * u_ink.w + 0.5 - abs(d - v_halfw), 0.0, 1.0);
    float core = clamp(1.5 * u_ink.w + 0.5 - d, 0.0, 1.0);
    e = v_intensity * max(ring, core);
  }
  if (e <= 0.0) discard;
  o_emissive = vec4(u_gold.rgb * e, 1.0);
}
