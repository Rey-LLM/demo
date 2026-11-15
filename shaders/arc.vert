#version 450 core
// Pass 3: transaction arcs, 1 px hairlines, additively blended into the
// RGBA16F scene target so overlapping flows read as brightness.
//  * intra-hub edges: GL_LINES, 2 vertices per instance (short chords on the
//    hub's disc), drawn only below a camera altitude;
//  * inter-hub edges: GL_LINE_STRIP, u_segments + 1 vertices per instance
//    along the lifted great circle between the endpoints.
// Endpoints are fetched by index from the node-position SSBO, so edges never
// duplicate positions. Opacity is depth-faded (1.0 -> 0.35 across the scene
// sphere) and vertices behind the globe are dropped by a ray-sphere test. Edges are sorted by weight within each range, so the
// instance count selects the heaviest prefix (the edge budget).
#include "common.glsl"

struct Edge {
  uint src;
  uint dst;
  uint attr;     // bits 0-7 flags, 16-23 source hub, 24-31 target hub
  float weight;  // shading weight [0, 1]: log-scale min-max within the currency, squared
};
layout(std430, binding = 1) readonly buffer Edges { Edge edges[]; };

uniform uint u_inter;     // 0: intra (GL_LINES), 1: inter (GL_LINE_STRIP)
uniform uint u_first;     // first edge index of the range
uniform uint u_segments;  // inter: segments per arc

out vec4 v_color;

void main() {
  Edge ed = edges[u_first + uint(gl_InstanceID)];
  vec3 a = globe_position(node_layout_position(ed.src), (ed.attr >> 16) & 255u);
  vec3 b = globe_position(node_layout_position(ed.dst), (ed.attr >> 24) & 255u);
  vec3 p;
  float alpha;
  if (u_inter == 0u) {
    p = gl_VertexID == 0 ? a : b;
    alpha = mix(u_alpha0.z, u_alpha0.w, ed.weight);
  } else {
    // Edges between the same pair of hubs would otherwise share one path and
    // add up to a saturated trunk: a per-edge lift in [0.55, 1.45] fans each
    // corridor into separate hairlines, and the alpha tapers toward the
    // endpoints where every arc of a hub converges.
    float t = float(gl_VertexID) / float(u_segments);
    float lift = 0.55 + 0.9 * float(hash_u32(u_first + uint(gl_InstanceID)) & 1023u) / 1023.0;
    p = arc_point_lift(a, b, t, lift);
    alpha = mix(u_alpha1.x, u_alpha1.y, ed.weight) * mix(0.25, 1.0, sin(3.14159265 * t));
  }
  uint selected = u_counts.y;
  if (selected != 0u) {
    bool sel = ed.src + 1u == selected || ed.dst + 1u == selected;
    alpha = sel ? 0.60 : alpha * 0.35;
  }
  // Depth cue and cheap analytic occlusion behind the globe (the depth
  // buffer handles pixels, this removes the whole vertex so line strips fade
  // out cleanly at the limb instead of ending in a stair-step).
  alpha *= depth_fade(p);
  if (occluded_by_globe(p)) alpha = 0.0;
  v_color = vec4(u_ink.rgb * alpha, alpha);  // premultiplied, additive
  gl_Position = u_view_proj * vec4(p, 1.0);
}
