#version 450 core
// Pass 6: HUD. One instanced quad per glyph (or solid panel), all in a single
// draw. Instances live in a persistently mapped ring buffer region.
#include "common.glsl"

struct Glyph {
  vec4 rect;    // x, y (top-left, px), w, h (px)
  uvec4 info;   // x glyph code, y packed RGBA8
};
layout(std430, binding = 8) readonly buffer Glyphs { Glyph glyphs[]; };

out vec2 v_uv;
flat out uint v_code;
flat out vec4 v_color;

void main() {
  Glyph g = glyphs[gl_InstanceID];
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vec2 px = g.rect.xy + corner * g.rect.zw;
  vec2 ndc = vec2(px.x * u_viewport.z * 2.0 - 1.0, 1.0 - px.y * u_viewport.w * 2.0);
  v_uv = corner;
  v_code = g.info.x;
  v_color = unpackUnorm4x8(g.info.y);
  gl_Position = vec4(ndc, 0.0, 1.0);
}
