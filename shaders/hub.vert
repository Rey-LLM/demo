#version 450 core
// Pass 1: hub clouds. Every instance is one soft billboard particle generated
// procedurally from gl_InstanceID (no CPU geometry):
//   particle 0 of each hub    -> large emissive anchor marker,
//   CHAIN_CLOUD particles     -> volumetric cloud (dense core, wispy edge),
//   FIAT_CENTER particles     -> glow halo hugging the hub sphere,
//   FALLBACK particles        -> dim grey haze.
#include "common.glsl"

out vec2 v_uv;
flat out vec4 v_color;

const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
  uint perHub = max(u_counts.w, 1u);
  uint hub = uint(gl_InstanceID) / perHub;
  uint k = uint(gl_InstanceID) % perHub;
  vec3 anchor = hubs[hub].anchor_radius.xyz;
  float R = hubs[hub].anchor_radius.w;
  float type = hubs[hub].params.x;
  vec4 col = hubs[hub].color;
  float glow = u_params1.y;

  uint seed = uint(gl_InstanceID) * 3u + 17u;
  float u1 = hash01(seed), u2 = hash01(seed + 1u), u3 = hash01(seed + 2u);
  // Uniform direction on the sphere.
  float z = 1.0 - 2.0 * u1;
  float phi = 6.2831853 * u2;
  vec3 dir = vec3(sqrt(max(0.0, 1.0 - z * z)) * cos(phi), z, sqrt(max(0.0, 1.0 - z * z)) * sin(phi));

  vec3 center;
  float size;
  float alpha;
  if (k == 0u) {
    center = anchor;
    size = 0.16 * R;
    alpha = 0.9 * glow;
    col.rgb = mix(col.rgb, vec3(1.0), 0.35) * 1.6;
  } else if (type > 0.5 && type < 1.5) {
    // Chain cloud: radius skewed toward the core, slow swirl around y.
    float r = 1.25 * R * pow(u3, 0.55);
    float swirl = u_camera.w * 0.05 * (1.0 - r / (1.25 * R));
    float c = cos(swirl), s = sin(swirl);
    vec3 p = dir * r;
    p = vec3(c * p.x - s * p.z, p.y * 0.8, s * p.x + c * p.z);
    center = anchor + p;
    size = R * (0.06 + 0.10 * hash01(seed + 5u));
    alpha = 0.10 * glow;
  } else if (type < 0.5) {
    float r = R * (0.95 + 0.35 * u3);
    center = anchor + dir * r;
    size = R * (0.05 + 0.06 * hash01(seed + 5u));
    alpha = 0.05 * glow;
  } else {
    float r = R * (0.4 + 0.9 * u3);
    center = anchor + dir * r;
    size = R * 0.08;
    alpha = 0.04 * glow;
  }

  vec2 corner = kCorners[gl_VertexID];
  vec4 vc = u_view * vec4(center, 1.0);
  vc.xy += corner * size;
  v_uv = corner;
  v_color = vec4(col.rgb, alpha);
  gl_Position = u_proj * vc;
}
