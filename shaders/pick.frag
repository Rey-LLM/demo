#version 450 core
in vec2 v_px;
flat in float v_radius;
flat in uint v_id;
layout(location = 0) out uint o_id;
void main() {
  if (length(v_px) > v_radius) discard;
  o_id = v_id;
}
