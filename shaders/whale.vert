#version 450 core
// Pass 4: whale events, the only emissive element. One draw with 2W
// instances: [0, W) are arc ribbons (2 / 3 / 4 px by tier) with a travelling
// head and a short trail, [W, 2W) are the single flash at the destination.
// Inactive events collapse to an off-screen point, so the draw is the same
// every frame. Output goes to the emissive attachment only.
#include "common.glsl"

struct Whale {
  uvec4 nodes;   // x src node, y dst node, z src hub, w dst hub
  vec4 timing;   // x start (replay s), y tier (1..3)
};
layout(std430, binding = 10) readonly buffer Whales { Whale whales[]; };

uniform int u_segments;

out float v_t;          // arc parameter of this vertex
out float v_side;       // -1 .. 1 across the ribbon
out vec2 v_px;          // flash: offset from the centre (device px)
flat out float v_head;  // arc parameter of the head
flat out float v_trail; // trail length in arc parameter units
flat out float v_halfw; // ribbon half-width / flash radius (device px)
flat out float v_intensity;
flat out uint v_kind;   // 0 arc, 1 flash

const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
  uint W = u_counts.w;
  uint inst = uint(gl_InstanceID);
  bool flash = inst >= W;
  Whale w = whales[flash ? inst - W : inst];
  vec3 a = globe_position(node_layout_position(w.nodes.x), w.nodes.z);
  vec3 b = globe_position(node_layout_position(w.nodes.y), w.nodes.w);
  float flight = u_whale0.x, trailS = u_whale0.y, flashS = u_whale0.z;
  float age = u_camera.w - w.timing.x;
  uint tier = uint(clamp(w.timing.y, 1.0, 3.0));
  float peak = mix(u_whale0.w, u_whale1.x, float(tier - 1u) * 0.5);  // HDR intensity 4..8 by tier
  float ui = u_ink.w;
  v_t = 0.0;
  v_side = 0.0;
  v_px = vec2(0.0);
  v_head = age / flight;
  v_trail = trailS / flight;
  v_intensity = peak;

  if (!flash) {
    v_kind = 0u;
    bool live = v_head > 0.0 && v_head - v_trail < 1.0;
    int s = gl_VertexID >> 1;
    if (!live) {
      gl_Position = kCulled;
      return;
    }
    float side = (gl_VertexID & 1) == 0 ? -1.0 : 1.0;
    float t = float(s) / float(u_segments);
    float dt = 1.0 / float(u_segments);
    vec3 p = arc_point(a, b, t);
    vec3 q = arc_point(a, b, t < 1.0 ? t + dt : t - dt);
    vec4 cp = u_view_proj * vec4(p, 1.0);
    vec4 cq = u_view_proj * vec4(q, 1.0);
    if (cp.w <= u_shape1.w || cq.w <= u_shape1.w) {
      gl_Position = kCulled;
      return;
    }
    vec2 sp = cp.xy / cp.w * u_viewport.xy;
    vec2 sq = cq.xy / cq.w * u_viewport.xy;
    vec2 dir = sq - sp;
    dir = length(dir) < 1e-5 ? vec2(1.0, 0.0) : normalize(dir);
    vec2 nrm = vec2(-dir.y, dir.x);
    float widthPx = (tier == 3u ? u_whale1.w : tier == 2u ? u_whale1.z : u_whale1.y) * ui;
    v_halfw = 0.5 * widthPx;
    float extent = v_halfw + 1.0;  // margin for the AA ramp
    cp.xy += px_to_clip(nrm * side * extent, cp.w);
    v_t = t;
    v_side = side * extent;
    gl_Position = cp;
  } else {
    v_kind = 1u;
    float fa = age - flight;
    if (fa < 0.0 || fa >= flashS) {
      gl_Position = kCulled;
      return;
    }
    float k = fa / flashS;
    vec4 cb = u_view_proj * vec4(b, 1.0);
    if (cb.w <= u_shape1.w) {
      gl_Position = kCulled;
      return;
    }
    v_halfw = mix(2.0, 12.0, sqrt(k)) * ui;     // expanding ring radius
    v_intensity = peak * (1.0 - k) * (1.0 - k);  // decays to zero
    float extent = v_halfw + 2.0 * ui + 1.0;
    // The strip has 2(S+1) vertices; the ones past the quad repeat its last
    // corner so they add only degenerate triangles (collapsing them to an
    // off-screen point would stretch a triangle across the screen).
    vec2 corner = kCorners[min(gl_VertexID, 3)];
    v_px = corner * extent;
    cb.xy += px_to_clip(corner * extent, cb.w);
    gl_Position = cb;
  }
}
