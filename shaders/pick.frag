#version 450 core
#include "common.glsl"

in vec3 v_view_pos;
flat in vec3 v_center;
flat in float v_radius;
flat in uint v_id;
in vec2 v_uv;
flat in uint v_point;

layout(location = 0) out uint o_id;
layout(depth_less) out float gl_FragDepth;  // see node.frag

void main() {
  vec3 hit;
  if (v_point != 0u) {
    if (dot(v_uv, v_uv) > 1.96) discard;
    hit = v_center;
  } else {
    vec3 d = normalize(v_view_pos);
    float b = dot(d, v_center);
    float disc = b * b - (dot(v_center, v_center) - v_radius * v_radius);
    if (disc < 0.0) discard;
    hit = d * (b - sqrt(disc));
  }
  o_id = v_id;
  vec4 clip = u_proj * vec4(hit, 1.0);
  gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
