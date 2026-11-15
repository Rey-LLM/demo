#version 450 core
// Pass 5: hub markers and labels in one instanced draw. Instances [0, H) are
// markers (1.25 px ring + 2 px dot), [H, H + G) are label glyphs (uppercase
// monospace, tracked). Everything is placed in screen space around the hub's
// projected position and fades out as the hub turns toward the limb.
#include "common.glsl"

struct LabelGlyph {
  uint hub;
  uint code;    // ASCII
  uint column;  // character index within the label
  uint place;   // bits 0-7 label side (0 E, 1 W, 2 N, 3 S), 8-15 label length
};
layout(std430, binding = 9) readonly buffer Labels { LabelGlyph labels[]; };

out vec2 v_px;          // marker: offset from the hub centre (device px)
out vec2 v_tex;         // glyph: position in atlas texels
flat out float v_fade;
flat out uint v_kind;   // 0 marker, 1 glyph
flat out float v_scale; // glyph: device px per atlas texel
flat out float v_cell;  // glyph: first atlas column of its cell

const int kCellW = 6;
const int kCellH = 8;

void main() {
  uint H = u_counts.z;
  uint inst = uint(gl_InstanceID);
  bool glyph = inst >= H;
  LabelGlyph g = glyph ? labels[inst - H] : LabelGlyph(inst, 0u, 0u, 0u);
  Hub h = hubs[g.hub];
  vec3 pos = h.geo.xyz * (u_globe.x * (1.0 + u_globe.y));
  float ndv = dot(h.geo.xyz, normalize(u_camera.xyz - pos));
  vec4 clip = u_view_proj * vec4(pos, 1.0);
  v_fade = smoothstep(0.05, 0.30, ndv);
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));  // 0..1
  v_px = vec2(0.0);
  v_tex = vec2(0.0);
  v_scale = 1.0;
  v_cell = 0.0;
  if (ndv <= 0.05 || clip.w <= u_shape1.w) {
    v_kind = 0u;
    gl_Position = kCulled;
    return;
  }
  float ui = u_ink.w;
  vec2 offset;
  if (!glyph) {
    v_kind = 0u;
    float extent = (u_shape0.w + u_shape0.z) * ui + 1.5;
    offset = (corner * 2.0 - 1.0) * extent;
    v_px = offset;
  } else {
    v_kind = 1u;
    float sizePx = u_shape1.y * ui;          // label size: one atlas cell tall
    float texel = sizePx / float(kCellH);
    float advance = float(kCellW) * texel + u_shape1.z * sizePx;  // cell + tracking
    // Clear the marker ring, or the hub's disc of nodes when that is larger on
    // screen (close-ups), so the label never sits on top of the cluster.
    vec3 rim = normalize(h.geo.xyz + h.east.xyz * h.geo.w) * (u_globe.x * (1.0 + u_globe.y));
    vec4 rimClip = u_view_proj * vec4(rim, 1.0);
    float discPx = rimClip.w > u_shape1.w
                       ? length((rimClip.xy / rimClip.w - clip.xy / clip.w) * 0.5 * u_viewport.xy)
                       : 0.0;
    float ring = max((u_shape0.w + u_shape0.z) * ui, discPx);
    float width = float(g.place >> 8) * advance - u_shape1.z * sizePx;
    uint side = g.place & 255u;
    vec2 origin;
    if (side == 1u)      origin = vec2(-ring - 7.0 * ui - width, -0.5 * sizePx);
    else if (side == 2u) origin = vec2(-0.5 * width, ring + 5.0 * ui);
    else if (side == 3u) origin = vec2(-0.5 * width, -ring - 5.0 * ui - sizePx);
    else                 origin = vec2(ring + 7.0 * ui, -0.5 * sizePx);
    // The quad covers the cell plus one texel on every side for the dark
    // halo. Screen y grows upward in clip space; atlas rows grow downward.
    vec2 t = corner * vec2(float(kCellW + 2), float(kCellH + 2)) - 1.0;
    offset = floor(origin + vec2(float(g.column) * advance, 0.0))  // pixel-aligned glyph grid
             + vec2(t.x, float(kCellH) - t.y) * texel;
    v_cell = float((int(g.code) - 32) * kCellW);
    v_tex = vec2(v_cell + t.x, t.y);
    v_scale = texel;
  }
  // Snap the anchor to the pixel grid so rings and glyphs stay crisp.
  vec2 screen = (clip.xy / clip.w * 0.5 + 0.5) * u_viewport.xy;
  screen = floor(screen) + 0.5;
  vec2 ndc = (screen + offset) * u_viewport.zw * 2.0 - 1.0;
  gl_Position = vec4(ndc, 0.0, 1.0);
}
