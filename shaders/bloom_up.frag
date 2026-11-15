#version 450 core
// Dual-filter (Kawase) upsample: eight taps around the coarser level, plus
// the same-size level of the down chain so every scale contributes.
in vec2 v_uv;
layout(binding = 0) uniform sampler2D u_src;   // coarser level (up chain or last down level)
layout(binding = 1) uniform sampler2D u_base;  // down-chain level of the output size
uniform vec2 u_texel;                          // 1 / coarser size
layout(location = 0) out vec4 o_color;
void main() {
  vec2 h = u_texel * 0.5;
  vec4 s = texture(u_src, v_uv + vec2(-h.x * 2.0, 0.0));
  s += texture(u_src, v_uv + vec2(-h.x, h.y)) * 2.0;
  s += texture(u_src, v_uv + vec2(0.0, h.y * 2.0));
  s += texture(u_src, v_uv + vec2(h.x, h.y)) * 2.0;
  s += texture(u_src, v_uv + vec2(h.x * 2.0, 0.0));
  s += texture(u_src, v_uv + vec2(h.x, -h.y)) * 2.0;
  s += texture(u_src, v_uv + vec2(0.0, -h.y * 2.0));
  s += texture(u_src, v_uv + vec2(-h.x, -h.y)) * 2.0;
  o_color = s / 12.0 + texture(u_base, v_uv);
}
