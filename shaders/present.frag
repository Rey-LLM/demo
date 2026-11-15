#version 450 core
// Copies the sRGB output target to the default framebuffer. Sampling decodes
// to linear; an sRGB-capable back buffer re-encodes on write, otherwise the
// shader encodes (u_encode = 1), so both paths store the same bytes.
in vec2 v_uv;
layout(binding = 0) uniform sampler2D u_output;
uniform int u_encode;
layout(location = 0) out vec4 o_color;
void main() {
  vec3 c = texelFetch(u_output, ivec2(gl_FragCoord.xy), 0).rgb;
  if (u_encode != 0) c = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
  o_color = vec4(c, 1.0);
}
