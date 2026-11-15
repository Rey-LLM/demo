#version 450 core
// Pass 6 (on click only): picking. Same disc placement as node.vert, at least
// 4 px wide; outputs the instance id (+1, so 0 means background).
#include "common.glsl"

layout(location = 0) in uint a_hub;

out vec2 v_px;
flat out float v_radius;
flat out uint v_id;

const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
  uint id = uint(gl_InstanceID);
  vec4 clip = u_view_proj * vec4(globe_position(node_layout_position(id), a_hub), 1.0);
  float radius = max(4.0, 0.5 * u_shape0.y * u_ink.w);
  vec2 corner = kCorners[gl_VertexID];
  v_px = corner * radius;
  v_radius = radius;
  v_id = id + 1u;
  if (clip.w <= u_shape1.w) {
    gl_Position = kCulled;
    return;
  }
  clip.xy += px_to_clip(corner * radius, clip.w);
  gl_Position = clip;
}
