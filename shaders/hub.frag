#version 450 core
#include "common.glsl"

in vec2 v_px;
in vec2 v_tex;
flat in float v_fade;
flat in uint v_kind;
flat in float v_scale;
flat in float v_cell;
flat in float v_boost;

layout(binding = 1) uniform sampler2D u_font;  // 1-row glyph atlas, linear filtering
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_emissive;  // marker glow (bloomed); alpha 0 keeps it additive

const float kCellW = 6.0;
const float kCellH = 8.0;

// Box-filtered magnification of the bitmap font: blend only within one
// device pixel of a texel edge, so glyphs stay sharp at any size. Samples
// outside this glyph's cell read as empty.
float glyph_coverage(vec2 t) {
  vec2 uv = floor(t) + 0.5 + clamp((fract(t) - 0.5) * v_scale, -0.5, 0.5);
  if (uv.x < v_cell || uv.x > v_cell + kCellW || uv.y < 0.0 || uv.y > kCellH) return 0.0;
  uv = clamp(uv, vec2(v_cell + 0.5, 0.5), vec2(v_cell + kCellW - 0.5, kCellH - 0.5));
  return texture(u_font, uv / vec2(textureSize(u_font, 0))).r;
}

void main() {
  float a;
  float glow = 0.0;
  if (v_kind == 0u) {
    float ui = u_ink.w;
    float d = length(v_px);
    float ring = clamp(0.5 * u_shape0.z * ui + 0.5 - abs(d - u_shape0.w * ui), 0.0, 1.0);
    float dotCov = clamp(0.5 * u_shape1.x * ui + 0.5 - d, 0.0, 1.0);
    a = min(max(ring * u_alpha1.z * v_boost, dotCov * u_alpha1.z), 1.0);
    // Each hub glows like a city at night: a small warm core that the bloom
    // spreads, breathing slowly on the live clock.
    float sigma = 2.2 * ui;
    float pulse = 0.85 + 0.15 * sin(u_atmo1.z * 1.7);
    glow = 1.4 * pulse * v_boost * exp(-d * d / (2.0 * sigma * sigma)) * v_fade;
  } else {
    // Ink, plus a one-texel obsidian halo (the scene's zero) so a label stays
    // legible where it crosses bright arcs.
    float ink = glyph_coverage(v_tex);
    float halo = 0.0;
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) halo = max(halo, glyph_coverage(v_tex + vec2(x, y)));
    float inkA = ink * u_alpha1.w * v_fade;
    float haloA = halo * 0.8 * v_fade;
    if (max(inkA, haloA) <= 0.0) discard;
    o_color = vec4(u_ink.rgb * inkA, max(inkA, haloA));
    o_emissive = vec4(0.0);
    return;
  }
  a *= v_fade;
  if (a <= 0.0 && glow <= 0.004) discard;
  o_color = vec4(u_ink.rgb * a, a);
  o_emissive = vec4(u_gold.rgb * glow, 0.0);
}
