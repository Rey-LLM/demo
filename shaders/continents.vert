#version 450 core
// Coastlines (Natural Earth 1:110m, 1:50m at close zoom): one instanced draw,
// one instance per segment, GL_TRIANGLE_STRIP with 4 vertices. Endpoints are
// fetched from SSBO 11 as lat/lon, projected onto the sphere at
// R * 1.0005 (clear of the globe's depth, no z-fighting) and expanded into a
// screen-space quad 1 px wide plus an anti-aliasing margin. Segments on the
// far hemisphere fade out with N.V; the globe's depth hides the rest.
#include "common.glsl"

struct Segment {
  vec4 latlon;  // lat0, lon0, lat1, lon1 (degrees)
};
layout(std430, binding = 11) readonly buffer Coast { Segment coast[]; };

uniform uint u_first;  // first segment of the active level of detail

out float v_side;       // signed distance from the line centre (px)
flat out float v_alpha;

vec3 geo(float latDeg, float lonDeg) {
  float la = radians(latDeg), lo = radians(lonDeg);
  return vec3(cos(la) * sin(lo), sin(la), cos(la) * cos(lo));
}

void main() {
  Segment s = coast[u_first + uint(gl_InstanceID)];
  vec3 na = geo(s.latlon.x, s.latlon.y);
  vec3 nb = geo(s.latlon.z, s.latlon.w);
  float R = u_globe.x * 1.0005;
  vec3 a = na * R, b = nb * R;
  bool second = gl_VertexID >= 2;
  vec3 p = second ? b : a;
  vec3 n = second ? nb : na;
  // Back-hemisphere fade: N.V from 0.12 down to 0 takes the segment out.
  float ndv = dot(n, normalize(u_camera.xyz - p));
  v_alpha = u_coast.w * smoothstep(0.0, 0.12, ndv);
  vec4 ca = u_view_proj * vec4(a, 1.0);
  vec4 cb = u_view_proj * vec4(b, 1.0);
  if (v_alpha <= 0.0 || ca.w <= u_shape1.w || cb.w <= u_shape1.w) {
    v_side = 0.0;
    gl_Position = kCulled;
    return;
  }
  vec2 sa = ca.xy / ca.w * u_viewport.xy;
  vec2 sb = cb.xy / cb.w * u_viewport.xy;
  vec2 dir = sb - sa;
  dir = length(dir) < 1e-5 ? vec2(1.0, 0.0) : normalize(dir);
  vec2 nrm = vec2(-dir.y, dir.x);
  float side = (gl_VertexID & 1) == 0 ? -1.0 : 1.0;
  const float kExtent = 1.5;  // half of 1 px plus a 1 px coverage ramp
  vec4 c = second ? cb : ca;
  c.xy += px_to_clip(nrm * side * kExtent, c.w);
  v_side = side * kExtent;
  gl_Position = c;
}
