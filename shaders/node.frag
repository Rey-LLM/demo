#version 450 core
// Ray-sphere impostor: intersect the view ray with the sphere, shade the hit
// point and write its true depth (reverse-Z: depth = near / -z_view, which is
// exactly clip.z / clip.w of the hit point with our projection).
#include "common.glsl"

in vec3 v_view_pos;
flat in vec3 v_center;
flat in float v_radius;
flat in vec4 v_color;
flat in uint v_state;
in vec2 v_uv;

layout(location = 0) out vec4 o_color;
// Conservative depth: the hit point is never in front of the billboard (see
// node.vert), so the written depth never exceeds gl_FragCoord.z (reverse-Z).
layout(depth_less) out float gl_FragDepth;

void main() {
  vec3 base = v_color.rgb;
  if ((v_state & 4u) != 0u) base = mix(base, vec3(1.0, 0.85, 0.35), 0.45);  // bridge nodes: gold tint

  vec3 n;
  vec3 hit;
  if ((v_state & 8u) != 0u) {
    // Point-sprite mode for sub-pixel nodes: flat disc at the centre depth.
    if (dot(v_uv, v_uv) > 1.96) discard;
    hit = v_center;
    n = vec3(0.0, 0.0, 1.0);
  } else {
    vec3 d = normalize(v_view_pos);  // camera at the origin in view space
    float b = dot(d, v_center);
    float c = dot(v_center, v_center) - v_radius * v_radius;
    float disc = b * b - c;
    if (disc < 0.0) discard;
    float t = b - sqrt(disc);
    if (t < 0.0) t = b + sqrt(disc);  // camera inside the sphere
    hit = d * t;
    n = (hit - v_center) / v_radius;
  }

  vec3 L = normalize(vec3(0.35, 0.8, 0.45));
  float lambert = max(dot(n, L), 0.0);
  float rim = pow(1.0 - max(n.z, 0.0), 2.5);
  vec3 col = base * (0.30 + 0.75 * lambert) + base * rim * 0.6;
  if ((v_state & 2u) != 0u) {
    float pulse = 0.5 + 0.5 * sin(u_camera.w * 6.0);
    col = mix(col, vec3(1.0), 0.55 + 0.25 * pulse);
  } else if ((v_state & 1u) != 0u) {
    col = mix(col, vec3(1.0, 1.0, 0.9), 0.45);
  } else if ((v_state & 16u) != 0u) {
    col *= 0.28;
  }
  o_color = vec4(col, 1.0);

  vec4 clip = u_proj * vec4(hit, 1.0);
  gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
