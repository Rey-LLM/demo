#version 450 core
in vec2 v_uv;
flat in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() {
  float d2 = dot(v_uv, v_uv);
  if (d2 > 1.0) discard;
  // Gaussian falloff; additive blending accumulates particles into volume.
  float a = v_color.a * exp(-4.0 * d2);
  o_color = vec4(v_color.rgb * a, a);
}
