#version 450 core
// One triangle covering the viewport: glDrawArrays(GL_TRIANGLES, 0, 3).
out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
