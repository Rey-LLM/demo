#version 450 core
// Final pass: scene + emissive + bloom (from the emissive chain only), ACES
// fitted tonemap over the display-referred background, vignette. The target
// is sRGB with GL_FRAMEBUFFER_SRGB enabled, so the output stays linear here.
in vec2 v_uv;
layout(binding = 0) uniform sampler2D u_scene;     // resolved RGBA16F
layout(binding = 1) uniform sampler2D u_emissive;  // resolved RGBA16F
layout(binding = 2) uniform sampler2D u_bloom;     // half resolution
uniform vec3 u_background;  // linear
uniform float u_exposure;
uniform float u_bloom_strength;
uniform float u_vignette;   // darkening at the corners (<= 0.03)
layout(location = 0) out vec4 o_color;

vec3 aces_fitted(vec3 x) {
  // Narkowicz 2015 fit of the ACES reference rendering transform.
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
  vec3 hdr = texture(u_scene, v_uv).rgb + texture(u_emissive, v_uv).rgb +
             texture(u_bloom, v_uv).rgb * u_bloom_strength;
  vec3 c = u_background + aces_fitted(hdr * u_exposure);
  vec2 q = v_uv * 2.0 - 1.0;
  c *= 1.0 - u_vignette * clamp(dot(q, q) * 0.5, 0.0, 1.0);
  o_color = vec4(c, 1.0);
}
