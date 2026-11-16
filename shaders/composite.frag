#version 450 core
// Final pass: scene + emissive + bloom (from the emissive chain only), ACES
// fitted tonemap over the display-referred background, vignette. The target
// is sRGB with GL_FRAMEBUFFER_SRGB enabled, so the output stays linear here.
//
// v4: the display-referred base is Obsidian off the globe and on its night
// side, Day Tint on its sunlit side. The globe is ray-cast again here (one
// sphere test per pixel) with analytic coverage at the limb, so the base
// colours are exact theme hexes and the edge is anti-aliased.
#include "common.glsl"

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
  vec3 base = u_background;
  {
    vec2 ndc = v_uv * 2.0 - 1.0;
    vec4 pa = u_inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec4 pb = u_inv_view_proj * vec4(ndc, 0.5, 1.0);
    vec3 ro = u_camera.xyz;
    vec3 rd = normalize(pb.xyz / pb.w - pa.xyz / pa.w);
    float R = u_globe.x;
    float b = dot(ro, rd);
    float closest = sqrt(max(dot(ro, ro) - b * b, 0.0));
    float cov = b < 0.0 ? clamp((R - closest) / max(fwidth(closest), 1e-6) + 0.5, 0.0, 1.0) : 0.0;
    if (cov > 0.0) {
      float disc = max(b * b - (dot(ro, ro) - R * R), 0.0);
      vec3 n = normalize(ro + rd * (-b - sqrt(disc)));
      base = mix(u_background, mix(u_background, u_day.rgb, day_factor(n)), cov);
    }
  }
  vec3 c = base + aces_fitted(hdr * u_exposure);
  vec2 q = v_uv * 2.0 - 1.0;
  c *= 1.0 - u_vignette * clamp(dot(q, q) * 0.5, 0.0, 1.0);
  o_color = vec4(c, 1.0);
}
