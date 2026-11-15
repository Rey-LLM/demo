// Shared declarations, expanded into each scene shader by the loader's #include.
// Binding map (see DESIGN_NOTES.md):
//   UBO  0: Frame            SSBO 0: node positions (layout space, current ring region)
//   SSBO 1: edges            SSBO 2: hubs           SSBO 4: highlight bits
//   SSBO 8: HUD glyphs       SSBO 9: hub label glyphs   SSBO 10: whale events
// All colours arrive in linear RGB (theme hex values are converted at load).

layout(std140, binding = 0) uniform Frame {
  mat4 u_view;
  mat4 u_proj;
  mat4 u_view_proj;
  mat4 u_inv_view_proj;
  vec4 u_camera;     // xyz camera position, w replay time (s)
  vec4 u_viewport;   // w, h, 1/w, 1/h
  vec4 u_globe;      // x radius, y node altitude, z node relief, w arc lift (fractions of the radius)
  vec4 u_ink;        // rgb ink, w ui scale (device px per logical px)
  vec4 u_amber;      // rgb graticule, w graticule alpha
  vec4 u_gold;       // rgb whale emissive, w graticule spacing (rad)
  vec4 u_alpha0;     // node alpha min, node alpha max, intra arc alpha min, intra arc alpha max
  vec4 u_alpha1;     // inter arc alpha min, inter arc alpha max, hub ring alpha, label alpha
  vec4 u_shape0;     // node min px, node max px, ring width px, ring radius px
  vec4 u_shape1;     // dot px, label px, label tracking (em), near plane
  vec4 u_whale0;     // flight s, trail s, flash s, intensity min
  vec4 u_whale1;     // intensity max, width tier1, tier2, tier3 (px)
  uvec4 u_counts;    // x node count, y selected node + 1 (0 = none), z hub count, w whale count
};

layout(std430, binding = 0) readonly buffer NodePositions { float node_pos[]; };

vec3 node_layout_position(uint i) {
  return vec3(node_pos[3u * i], node_pos[3u * i + 1u], node_pos[3u * i + 2u]);
}

struct Hub {
  vec4 anchor_radius;  // layout anchor xyz, layout radius
  vec4 geo;            // unit normal on the globe, footprint (rad)
  vec4 east;           // tangent frame
  vec4 north;
};
layout(std430, binding = 2) readonly buffer Hubs { Hub hubs[]; };

// Mirrors ftv::globe_position (src/model/globe.hpp).
vec3 globe_position(vec3 p, uint hub) {
  Hub h = hubs[hub];
  vec3 d = (p - h.anchor_radius.xyz) / h.anchor_radius.w;
  float planar = length(d.xz);
  float k = planar > 1.6 ? 1.6 / planar : 1.0;
  vec3 dir = normalize(h.geo.xyz + (h.east.xyz * (d.x * k) + h.north.xyz * (d.z * k)) * h.geo.w);
  float alt = u_globe.y + u_globe.z * clamp(d.y, -1.0, 1.0);
  return dir * (u_globe.x * (1.0 + alt));
}

// Mirrors ftv::arc_point: lifted great-circle arc from a to b; `lift` scales
// the apex height (1.0 = u_globe.w per radian of separation).
vec3 arc_point_lift(vec3 a, vec3 b, float t, float lift) {
  float la = length(a), lb = length(b);
  vec3 ua = a / la, ub = b / lb;
  float c = clamp(dot(ua, ub), -0.9999, 1.0);
  float om = acos(c);
  vec3 u;
  if (om < 1e-4) {
    u = normalize(ua + (ub - ua) * t);
  } else {
    u = (sin((1.0 - t) * om) * ua + sin(t * om) * ub) / sin(om);
  }
  float r = la + (lb - la) * t + u_globe.x * u_globe.w * lift * om * sin(3.14159265 * t);
  return u * r;
}

vec3 arc_point(vec3 a, vec3 b, float t) { return arc_point_lift(a, b, t, 1.0); }

// Clip-space offset for `px` device pixels along a screen-space direction.
vec2 px_to_clip(vec2 px, float w) { return px * 2.0 * u_viewport.zw * w; }

// Position far outside the clip volume: collapses a primitive.
const vec4 kCulled = vec4(2.0, 2.0, 2.0, 1.0);

uint hash_u32(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
