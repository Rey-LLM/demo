#version 450 core
in vec2 v_uv;
flat in uint v_code;
flat in vec4 v_color;
layout(binding = 1) uniform sampler2D u_font;
layout(location = 0) out vec4 o_color;
const int kCellW = 6;
const int kCellH = 8;
void main() {
  int g = int(v_code) - 32;
  ivec2 texel = ivec2(g * kCellW + int(v_uv.x * float(kCellW)), int(v_uv.y * float(kCellH)));
  texel.x = clamp(texel.x, g * kCellW, g * kCellW + kCellW - 1);
  texel.y = clamp(texel.y, 0, kCellH - 1);
  float on = texelFetch(u_font, texel, 0).r;
  if (on < 0.5) discard;
  o_color = v_color;
}
