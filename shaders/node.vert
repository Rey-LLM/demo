#version 450 core
// Pass 4: nodes as camera-facing billboards that the fragment shader turns
// into analytic spheres. Position comes from the SSBO by gl_InstanceID; the
// static attributes are instanced vertex attributes (divisor 1).
#include "common.glsl"

layout(location = 0) in uint a_hub;
layout(location = 1) in uint a_flags;
layout(location = 2) in float a_size;
layout(location = 3) in uint a_color_index;

layout(std430, binding = 4) readonly buffer Highlight { uint highlight_bits[]; };

out vec3 v_view_pos;            // fragment position on the billboard (view space)
flat out vec3 v_center;         // sphere centre (view space)
flat out float v_radius;
flat out vec4 v_color;
flat out uint v_state;          // bit0 highlighted, bit1 selected, bit2 bridge, bit3 point sprite, bit4 dimmed
out vec2 v_uv;

const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
  uint id = uint(gl_InstanceID);
  vec3 center = node_position(id);
  vec4 vc = u_view * vec4(center, 1.0);
  float r = a_size * u_params0.x;

  uint state = 0u;
  if ((highlight_bits[id >> 5] & (1u << (id & 31u))) != 0u) state |= 1u;
  if (u_counts.y == id + 1u) state |= 2u;
  if ((a_flags & 1u) != 0u) state |= 4u;
  if (u_counts.y != 0u && (state & 3u) == 0u) state |= 16u;

  // Keep far-away nodes at least ~1.5 px wide so the clusters stay readable:
  // below that we draw a flat disc (point sprite) instead of a ray-cast sphere.
  float depth = max(-vc.z, u_params1.w);
  float pixelRadius = r * u_proj[1][1] * 0.5 * u_viewport.y / depth;
  float minPx = 1.5;
  float scale = 1.0;
  if (pixelRadius < minPx) {
    scale = minPx / max(pixelRadius, 1e-4);
    state |= 8u;
  }
  if ((state & 3u) != 0u) scale *= 1.6;

  vec2 corner = kCorners[gl_VertexID];
  // The billboard sits on the sphere's front plane (view z + radius) so every
  // ray-cast hit lies at or behind it. With reverse-Z the written depth is
  // therefore <= the interpolated depth, which node.frag declares with
  // layout(depth_less): the GREATER early depth test stays valid and occluded
  // impostor fragments are rejected before shading. The 1.4x margin covers
  // the perspective silhouette of the sphere.
  float rr = r * ((state & 3u) != 0u ? 1.6 : 1.0);
  vec3 pos = vc.xyz + vec3(corner * r * scale * 1.4, rr);
  v_view_pos = pos;
  v_center = vc.xyz;
  v_radius = rr;
  v_uv = corner * 1.4;
  vec4 hubColor = hubs[a_color_index].color;
  v_color = hubColor;
  v_state = state;
  gl_Position = u_proj * vec4(pos, 1.0);
}
