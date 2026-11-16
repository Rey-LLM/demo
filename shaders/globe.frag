#version 450 core
// Pass 1: globe. Ray-casts the sphere per pixel, writes its depth (so the far
// hemisphere occludes nodes and arcs) and draws the 15-degree graticule with
// analytic anti-aliasing: the distance to the nearest grid line, measured in
// pixels through screen-space derivatives, gives the coverage of a 1 px line.
// The limb gets a faint 1 px ink hairline.
//
// v4: day/night. The base colour of the sphere (Day Tint on the sunlit side,
// Obsidian at night) is mixed in the composite pass, display-referred, so
// the theme hexes come out exact. This pass adds the city lights on the
// night side: a log-toned BC4 map (NASA Black Marble) in Night Lights, HDR
// at most u_lights.w (1.5), written to the scene attachment only (never
// bloomed), zero where d >= 0.5, d = smoothstep(-t, t, N.sun).
#include "common.glsl"

layout(binding = 3) uniform sampler2D u_city;  // R: log-toned radiance [0, 1]

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
  vec3 rgb = u_amber.rgb * a + u_ink.rgb * limb;

  // City lights. Texture coordinates from lat/lon; the gradient of u uses
  // whichever longitude field has no seam at this pixel, so the mip choice
  // stays correct across the antimeridian.
  // Lights only where d < 0.5 (the dark half of the twilight band and the
  // night side), fading in from the terminator.
  float night = 1.0 - smoothstep(0.0, 0.5, day_factor(n));
  if (u_rim.y > 0.5 && night > 0.0) {
    const float kTwoPi = 6.28318531;
    vec2 uv = vec2(lon / kTwoPi + 0.5, 0.5 - lat / 3.14159265);
    vec2 dx = vec2(dFdx(lon), dFdx(lat)), dy = vec2(dFdy(lon), dFdy(lat));
    vec2 dxAlt = vec2(dFdx(lonAlt), dx.y), dyAlt = vec2(dFdy(lonAlt), dy.y);
    if (abs(dxAlt.x) + abs(dyAlt.x) < abs(dx.x) + abs(dy.x)) {
      dx = dxAlt;
      dy = dyAlt;
    }
    vec2 k = vec2(1.0 / kTwoPi, -1.0 / 3.14159265);
    float city = textureGrad(u_city, uv, dx * k, dy * k).r;
    // Lights fade toward the limb with the same N.V ramp as the graticule.
    rgb += u_lights.rgb * (u_lights.w * city * night * smoothstep(0.0, 0.25, ndv));
  }
  o_color = vec4(rgb, a + limb);

  vec4 clip = u_view_proj * vec4(hit, 1.0);
  gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
