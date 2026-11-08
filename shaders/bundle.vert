#version 450 core
// Pass 2: corridor bundles. Each instance is a ribbon along the hub-to-hub
// cubic Bezier, evaluated here. The strip has 2 vertices per curve sample
// (4 * SEGMENTS vertices => 2 * SEGMENTS samples). The ribbon is expanded in
// screen space perpendicular to the projected tangent, so its width is in
// pixels regardless of distance.
#include "common.glsl"

struct Bundle {
  uvec4 ids;     // x hub_a, y hub_b, z palette slot, w channel (unused)
  vec4 params;   // x thickness [0..1], y brightness [0..1]
};
layout(std430, binding = 3) readonly buffer Bundles { Bundle bundles[]; };

uniform int u_segments;

out vec4 v_color;
out float v_side;
out float v_t;

void main() {
  Bundle bd = bundles[gl_InstanceID];
  int samples = 2 * u_segments;
  int s = gl_VertexID >> 1;
  float side = (gl_VertexID & 1) == 0 ? -1.0 : 1.0;
  float t = float(s) / float(samples - 1);

  vec3 c0, c1, c2, c3;
  corridor_controls(bd.ids.x, bd.ids.y, bd.ids.z, c0, c1, c2, c3);
  vec3 p = bezier(c0, c1, c2, c3, t);
  vec3 tangent = bezier_tangent(c0, c1, c2, c3, t);

  vec4 clip = u_view_proj * vec4(p, 1.0);
  vec4 clip2 = u_view_proj * vec4(p + normalize(tangent) * 10.0, 1.0);
  if (clip.w <= u_params1.w || clip2.w <= u_params1.w) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // behind the camera: degenerate
    v_color = vec4(0.0);
    v_side = 0.0;
    v_t = t;
    return;
  }
  vec2 sp = clip.xy / clip.w * u_viewport.xy;
  vec2 sp2 = clip2.xy / clip2.w * u_viewport.xy;
  vec2 dir = sp2 - sp;
  dir = length(dir) < 1e-4 ? vec2(1.0, 0.0) : normalize(dir);
  vec2 normal = vec2(-dir.y, dir.x);
  // Taper at the ports so the ribbon emerges from the hub surface.
  float taper = smoothstep(0.0, 0.08, t) * smoothstep(1.0, 0.92, t);
  float widthPx = u_params0.w * (0.25 + bd.params.x) * (0.35 + 0.65 * taper);
  clip.xy += normal * side * widthPx * u_viewport.zw * clip.w;  // px -> NDC (x2 / viewport, half-width)

  uint colorIndex = u_params1.z > 0.5 ? 128u + bd.ids.w : bd.ids.z;
  vec3 rgb = palette_color(colorIndex).rgb;
  float alpha = 0.12 + 0.48 * bd.params.y;
  if (u_counts.y != 0u) alpha *= 0.4;
  v_color = vec4(rgb, alpha);
  v_side = side;
  v_t = t;
  gl_Position = clip;
}
