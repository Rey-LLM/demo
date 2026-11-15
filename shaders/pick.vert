#version 450 core
// Pass 5: picking. Same billboard geometry as node.vert; outputs the
// instance id (+1, so 0 means background) to an R32UI target.
#include "common.glsl"

layout(location = 2) in float a_size;

out vec3 v_view_pos;
flat out vec3 v_center;
flat out float v_radius;
flat out uint v_id;
out vec2 v_uv;
flat out uint v_point;

const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
  uint id = uint(gl_InstanceID);
  vec4 vc = u_view * vec4(node_position(id), 1.0);
  float r = a_size * u_params0.x;
  float depth = max(-vc.z, u_params1.w);
  float pixelRadius = r * u_proj[1][1] * 0.5 * u_viewport.y / depth;
  // Picking is a bit more generous than drawing: at least 4 px.
  float scale = pixelRadius < 4.0 ? 4.0 / max(pixelRadius, 1e-4) : 1.0;
  vec2 corner = kCorners[gl_VertexID];
  vec3 pos = vc.xyz + vec3(corner * r * scale * 1.4, r);  // front plane, as in node.vert
  v_view_pos = pos;
  v_center = vc.xyz;
  v_radius = r;
  v_id = id + 1u;
  v_uv = corner * 1.4;
  v_point = scale > 1.0 ? 1u : 0u;
  gl_Position = u_proj * vec4(pos, 1.0);
}
