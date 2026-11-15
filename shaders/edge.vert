#version 450 core
// Pass 3: edges. Endpoints are fetched by index from the node-position SSBO,
// so edges never duplicate positions.
//  * intra pass: GL_LINES, 2 vertices per instance (straight segment);
//  * inter pass: GL_LINE_STRIP, 8 vertices per instance: source -> exit port
//    -> (corridor Bezier samples) -> entry port -> target, i.e. the 3-segment
//    route whose middle leg follows the bundle corridor.
// With GPU culling the instance index goes through the compact visible list;
// in the direct fallback it is the edge index itself and culling happens here.
#include "common.glsl"

struct Edge {
  uint src;
  uint dst;
  uint attr;
  float weight;
};
layout(std430, binding = 1) readonly buffer Edges { Edge edges[]; };
layout(std430, binding = 5) readonly buffer Visible { uint visible[]; };
layout(std430, binding = 4) readonly buffer Highlight { uint highlight_bits[]; };

uniform uint u_inter;   // 0: intra (2 verts), 1: inter (8 verts)
uniform uint u_direct;  // 1: no compute culling (GL 4.3 fallback path)
uniform uint u_first;   // first edge index in direct mode

out vec4 v_color;

void main() {
  uint e = u_direct != 0u ? (u_first + uint(gl_InstanceID)) : visible[gl_InstanceID];
  Edge ed = edges[e];
  vec3 a = node_position(ed.src);
  vec3 b = node_position(ed.dst);
  uint selected = u_counts.y;
  bool sel = selected != 0u && (ed.src + 1u == selected || ed.dst + 1u == selected);

  vec3 p;
  if (u_inter == 0u) {
    p = gl_VertexID == 0 ? a : b;
  } else {
    int v = gl_VertexID;
    if (v == 0) {
      p = a;
    } else if (v == 7) {
      p = b;
    } else {
      vec3 c0, c1, c2, c3;
      corridor_controls(attr_hub_src(ed.attr), attr_hub_dst(ed.attr), attr_palette(ed.attr), c0, c1, c2, c3);
      p = bezier(c0, c1, c2, c3, float(v - 1) / 5.0);
    }
  }

  bool keep = true;
  if (u_direct != 0u && !sel) {
    if (u_inter == 0u) {
      keep = !segment_outside(u_view_proj * vec4(a, 1.0), u_view_proj * vec4(b, 1.0));
    } else {
      float d = min(distance(u_camera.xyz, a), distance(u_camera.xyz, b));
      keep = d <= u_params1.x;
    }
  }

  uint colorIndex = u_params1.z > 0.5 ? 128u + attr_channel(ed.attr) : attr_palette(ed.attr);
  vec3 rgb = palette_color(colorIndex).rgb;
  float base = u_inter == 0u ? u_params0.y : u_params0.z;
  // Alpha scales with the log-normalised weight stored per edge.
  float alpha = base * (0.25 + 0.75 * ed.weight);
  if (selected != 0u) alpha *= sel ? 0.0 : 0.35;  // dim the rest while a node is selected
  if (sel) {
    rgb = mix(rgb, vec3(1.0), 0.5);
    alpha = 0.9;
  }
  v_color = vec4(rgb, keep ? alpha : 0.0);
  gl_Position = keep ? u_view_proj * vec4(p, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);  // degenerate outside clip
}
