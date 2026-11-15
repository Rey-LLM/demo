#version 450 core
// Pass 2: nodes as flat discs, 1-2.5 px wide by log(activity). The layout
// position comes from the SSBO by gl_InstanceID and is mapped onto the globe
// through the node's hub; the static attributes are instanced vertex
// attributes (divisor 1).
#include "common.glsl"

layout(location = 0) in uint a_hub;
layout(location = 1) in uint a_flags;
layout(location = 2) in float a_activity;  // log-normalised [0, 1]

layout(std430, binding = 4) readonly buffer Highlight { uint highlight_bits[]; };

out vec2 v_px;              // fragment offset from the centre, device px
flat out float v_radius;    // disc radius, device px
flat out float v_alpha;

const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
  uint id = uint(gl_InstanceID);
  vec3 p = globe_position(node_layout_position(id), a_hub);
  vec4 clip = u_view_proj * vec4(p, 1.0);

  float act = clamp(a_activity, 0.0, 1.0);
  float act2 = act * act;  // most nodes are quiet: keep them near the alpha floor
  float diameter = mix(u_shape0.x, u_shape0.y, act);
  float alpha = mix(u_alpha0.x, u_alpha0.y, act2);
  bool highlighted = (highlight_bits[id >> 5] & (1u << (id & 31u))) != 0u;
  if (u_counts.y != 0u) {
    if (u_counts.y == id + 1u) {
      diameter = u_shape0.y + 1.5;
      alpha = 0.70;
    } else if (highlighted) {
      diameter = max(diameter, u_shape0.y);
      alpha = 0.60;
    } else {
      alpha *= 0.35;
    }
  }
  float radius = 0.5 * diameter * u_ink.w;
  vec2 corner = kCorners[gl_VertexID];
  float extent = radius + 1.0;  // one pixel of margin for the AA ramp
  v_px = corner * extent;
  v_radius = radius;
  v_alpha = alpha;
  if (clip.w <= u_shape1.w) {
    gl_Position = kCulled;
    return;
  }
  clip.xy += px_to_clip(corner * extent, clip.w);
  gl_Position = clip;
}
