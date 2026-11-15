#version 450 core
// Pass 1: globe. Ray-casts the sphere per pixel, writes its depth (so the far
// hemisphere occludes nodes and arcs) and draws the 15-degree graticule with
// analytic anti-aliasing: the distance to the nearest grid line, measured in
// pixels through screen-space derivatives, gives the coverage of a 1 px line.
// The limb gets a faint 1 px ink hairline.
#include "common.glsl"

in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  vec2 ndc = v_uv * 2.0 - 1.0;
  // Reverse-Z infinite projection: ndc.z = 1 is the near plane, 0.5 lies twice as far.
  vec4 pa = u_inv_view_proj * vec4(ndc, 1.0, 1.0);
  vec4 pb = u_inv_view_proj * vec4(ndc, 0.5, 1.0);
  vec3 ro = u_camera.xyz;
  vec3 rd = normalize(pb.xyz / pb.w - pa.xyz / pa.w);
  float R = u_globe.x;
  float b = dot(ro, rd);
  float c = dot(ro, ro) - R * R;
  float disc = b * b - c;
  if (disc < 0.0) discard;
  float t = -b - sqrt(disc);
  if (t <= 0.0) discard;
  vec3 hit = ro + rd * t;
  vec3 n = hit / R;

  float lat = asin(clamp(n.y, -1.0, 1.0));
  float lon = atan(n.x, n.z);
  float lonAlt = atan(-n.x, -n.z);  // same field with its seam on the opposite meridian
  float sp = u_gold.w;
  float dLat = abs(fract(lat / sp + 0.5) - 0.5) * sp;
  float dLon = abs(fract(lon / sp + 0.5) - 0.5) * sp;
  float wLat = max(fwidth(lat), 1e-7);
  float wLon = max(min(fwidth(lon), fwidth(lonAlt)), 1e-7);
  const float lineW = 1.0;  // hairline: 1 device px
  float covLat = clamp(0.5 * lineW + 0.5 - dLat / wLat, 0.0, 1.0);
  float covLon = clamp(0.5 * lineW + 0.5 - dLon / wLon, 0.0, 1.0);
  // Meridians converge at the poles; fade them out above 75 degrees.
  covLon *= smoothstep(1.5708, 1.309, abs(lat));
  float cov = max(covLat, covLon);

  // Fade toward the limb (N.V -> 0).
  float ndv = max(dot(n, -rd), 0.0);
  float a = u_amber.w * cov * smoothstep(0.0, 0.45, ndv);
  // A 1 px ink hairline on the limb: the globe is as dark as the background,
  // so this is what shows where it occludes the arcs behind it.
  const float kLimbAlpha = 0.30;
  float limb = kLimbAlpha * clamp(1.0 - ndv / max(fwidth(ndv), 1e-6), 0.0, 1.0);
  o_color = vec4(u_amber.rgb * a + u_ink.rgb * limb, a + limb);

  vec4 clip = u_view_proj * vec4(hit, 1.0);
  gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
