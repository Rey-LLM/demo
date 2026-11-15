#version 450 core
#include "common.glsl"

in float v_t;
in float v_side;
in vec2 v_px;
flat in float v_head;
flat in float v_trail;
flat in float v_halfw;
flat in float v_intensity;
flat in float v_heat;
flat in uint v_kind;
in float v_fade;

layout(location = 1) out vec4 o_emissive;

void main() {
  float e;
  float hot = v_heat;  // colour: deep amber (tier 1) .. white gold (tier 3), hotter at the head
  if (v_kind == 0u) {
    // Trail: zero ahead of the head and beyond the trail, brightest at the head.
    float tail = v_head - v_trail;
    if (v_t > v_head || v_t < tail) discard;
    float f = clamp((v_t - tail) / v_trail, 0.0, 1.0);
    float cov = clamp(v_halfw + 0.5 - abs(v_side), 0.0, 1.0);
    // Narrow Gaussian core across the ribbon (sigma = 0.35 half-widths) over a
    // dim body, plus a short, sharp head along the arc.
    float x = v_side / max(v_halfw, 0.5);
    float core = 0.30 + 0.70 * exp(-x * x / (2.0 * 0.35 * 0.35));
    float h = (v_head - v_t) / max(v_trail * 0.06, 1e-4);
    float head = 1.0 + 1.5 * exp(-h * h);
    e = v_intensity * f * f * cov * core * head;
    // The plasma cools along the trail: the head runs white-hot, the tail
    // keeps the tier colour. Core of the ribbon is hotter than its edge.
    hot = clamp(v_heat + 0.55 * exp(-h * h) + 0.25 * (core - 0.30), 0.0, 1.0);
  } else {
    // Single flash: a thin ring expanding from the destination plus a core.
    float d = length(v_px);
    float ring = clamp(1.0 * u_ink.w + 0.5 - abs(d - v_halfw), 0.0, 1.0);
    float core = clamp(1.5 * u_ink.w + 0.5 - d, 0.0, 1.0);
    e = v_intensity * max(ring, core);
    hot = clamp(v_heat + 0.5 * core, 0.0, 1.0);
  }
  e *= v_fade;
  if (e <= 0.0) discard;
  o_emissive = vec4(mix(u_gold.rgb, u_gold_hot.rgb, hot) * e, 1.0);
}
