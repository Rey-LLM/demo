// Shared declarations, expanded into each shader by the loader's #include.
// Binding map (see DESIGN_NOTES.md):
//   UBO  0: Frame            SSBO 0: node positions (current ring region)
//   SSBO 1: edges            SSBO 2: hubs           SSBO 3: corridor bundles
//   SSBO 4: highlight bits   SSBO 5/6: visible edge lists  SSBO 7: indirect cmds
//   SSBO 8: HUD glyph instances

layout(std140, binding = 0) uniform Frame {
  mat4 u_view;
  mat4 u_proj;
  mat4 u_view_proj;
  vec4 u_camera;    // xyz = camera position, w = time (s)
  vec4 u_viewport;  // w, h, 1/w, 1/h
  vec4 u_params0;   // x node size scale, y edge alpha, z inter-edge alpha, w corridor width (px)
  vec4 u_params1;   // x inter-hub detail distance, y glow strength, z colour mode, w near plane
  uvec4 u_counts;   // x node count, y selected node + 1 (0 = none), z hub count, w particles per hub
};

layout(std430, binding = 0) readonly buffer NodePositions { float node_pos[]; };

vec3 node_position(uint i) { return vec3(node_pos[3u * i], node_pos[3u * i + 1u], node_pos[3u * i + 2u]); }

struct Hub {
  vec4 anchor_radius;  // xyz anchor, w radius
  vec4 color;
  vec4 params;         // x type (0 fiat, 1 chain, 2 fallback), y k_anchor, z log population
};
layout(std430, binding = 2) readonly buffer Hubs { Hub hubs[]; };

layout(binding = 0) uniform sampler1D u_palette;

vec4 palette_color(uint index) { return texelFetch(u_palette, int(index & 255u), 0); }

// Edge attribute packing (see renderer.cpp):
//   bits 0-7 currency palette slot, 8-11 channel, 12-15 flags, 16-23 src hub, 24-31 dst hub
uint attr_palette(uint a) { return a & 255u; }
uint attr_channel(uint a) { return (a >> 8) & 15u; }
uint attr_flags(uint a) { return (a >> 12) & 15u; }
uint attr_hub_src(uint a) { return (a >> 16) & 255u; }
uint attr_hub_dst(uint a) { return (a >> 24) & 255u; }

uint hash_u32(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float hash01(uint x) { return float(hash_u32(x) >> 8) * (1.0 / 16777216.0); }

// Hub-to-hub corridor as a cubic Bezier from the source hub's exit port to
// the target hub's entry port. Bundles of different currencies get separate
// lanes; the lateral offset uses cross(dir, up), which flips sign with the
// direction, so A->B and B->A corridors never overlap.
void corridor_controls(uint ha, uint hb, uint lane, out vec3 p0, out vec3 p1, out vec3 p2, out vec3 p3) {
  vec3 A = hubs[ha].anchor_radius.xyz;
  vec3 B = hubs[hb].anchor_radius.xyz;
  float Ra = hubs[ha].anchor_radius.w;
  float Rb = hubs[hb].anchor_radius.w;
  vec3 d = B - A;
  float len = max(length(d), 1e-3);
  d /= len;
  vec3 side = cross(d, vec3(0.0, 1.0, 0.0));
  side = length(side) < 1e-3 ? vec3(1.0, 0.0, 0.0) : normalize(side);
  float laneOffset = 18.0 + (float(lane % 7u) - 3.0) * 9.0;
  vec3 lateral = side * laneOffset;
  p0 = A + d * Ra * 0.95 + lateral * 0.35;
  p3 = B - d * Rb * 0.95 + lateral * 0.35;
  float span = length(p3 - p0);
  vec3 lift = vec3(0.0, 0.12 * span, 0.0);
  p1 = mix(p0, p3, 0.25) + lateral + lift;
  p2 = mix(p0, p3, 0.75) + lateral + lift;
}

vec3 bezier(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t) {
  float u = 1.0 - t;
  return u * u * u * p0 + 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * p3;
}

vec3 bezier_tangent(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t) {
  float u = 1.0 - t;
  return 3.0 * u * u * (p1 - p0) + 6.0 * u * t * (p2 - p1) + 3.0 * t * t * (p3 - p2);
}

// Conservative clip-space rejection of a segment: culled only if both ends
// are outside the same frustum plane (reverse-Z: inside means 0 <= z <= w).
bool segment_outside(vec4 a, vec4 b) {
  if (a.x < -a.w && b.x < -b.w) return true;
  if (a.x > a.w && b.x > b.w) return true;
  if (a.y < -a.w && b.y < -b.w) return true;
  if (a.y > a.w && b.y > b.w) return true;
  if (a.z > a.w && b.z > b.w) return true;  // both closer than the near plane
  return false;
}
