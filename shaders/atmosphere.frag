#version 450 core
// Thin Fresnel rim on a shell at R * 1.015: pow(1 - N.V, exponent) in amber
// at alpha <= 0.22, with an analytically anti-aliased outer edge. A crisp
// edge, not a haze. Writes the scene attachment only (the emissive
// attachment is masked off), so the rim never blooms.
// v4: the rim lives on the day-side limb (scaled by the day factor of the
// shell normal, a 15 % floor keeps the night limb readable) and takes a warm
// Night Lights tint inside the twilight band around the terminator.
#include "common.glsl"

in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  vec2 ndc = v_uv * 2.0 - 1.0;
  vec4 pa = u_inv_view_proj * vec4(ndc, 1.0, 1.0);
  vec4 pb = u_inv_view_proj * vec4(ndc, 0.5, 1.0);
  vec3 ro = u_camera.xyz;
  vec3 rd = normalize(pb.xyz / pb.w - pa.xyz / pa.w);
  float Ra = u_globe.x * u_atmo1.y;
  float b = dot(ro, rd);
  // Distance from the globe centre to the ray (its closest approach).
  float closest = sqrt(max(dot(ro, ro) - b * b, 0.0));
  if (b > 0.0 || closest > Ra + 2.0 * fwidth(closest) + 1.0) discard;
  float c = dot(ro, ro) - Ra * Ra;
  float disc = max(b * b - c, 0.0);
  vec3 hit = ro + rd * (-b - sqrt(disc));
  vec3 n = normalize(hit);
  float ndv = clamp(dot(n, -rd), 0.0, 1.0);
  float rim = pow(1.0 - ndv, u_atmo1.x);
  float edge = clamp((Ra - closest) / max(fwidth(closest), 1e-4) + 0.5, 0.0, 1.0);
  float ns = dot(n, u_sun.xyz);
  float day = day_factor(n);
  float a = u_atmo.w * rim * edge * mix(0.15, 1.0, day);
  if (a <= 1e-4) discard;
  float band = 1.0 - smoothstep(0.0, 2.5 * u_sun.w, abs(ns));  // 1 on the terminator
  vec3 col = mix(u_atmo.rgb, u_lights.rgb, u_rim.x * band);
  o_color = vec4(col * a, a);
}
