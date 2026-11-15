#version 450 core
// Dual-filter (Kawase) downsample: centre plus four diagonal taps at half-
// texel offsets of the source. Reads only the emissive chain.
in vec2 v_uv;
layout(binding = 0) uniform sampler2D u_src;
uniform vec2 u_texel;  // 1 / source size
layout(location = 0) out vec4 o_color;
void main() {
  vec2 h = u_texel * 0.5;
  vec4 s = texture(u_src, v_uv) * 4.0;
  s += texture(u_src, v_uv + vec2(-h.x, -h.y));
  s += texture(u_src, v_uv + vec2(h.x, -h.y));
  s += texture(u_src, v_uv + vec2(-h.x, h.y));
  s += texture(u_src, v_uv + vec2(h.x, h.y));
  o_color = s * 0.125;
}
