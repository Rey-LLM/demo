#version 450 core
#include "common.glsl"
in vec4 v_color;
in float v_side;
in float v_t;
layout(location = 0) out vec4 o_color;
void main() {
  // Soft edges across the ribbon and slow pulses flowing from source to
  // target to show direction.
  float edge = 1.0 - v_side * v_side;
  float flow = 0.75 + 0.25 * sin(v_t * 40.0 - u_camera.w * 3.0);
  float a = v_color.a * edge * flow;
  o_color = vec4(v_color.rgb * a, a);
}
