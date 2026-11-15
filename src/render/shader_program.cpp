#include "render/shader_program.hpp"

#include <vector>

#include "core/file_util.hpp"
#include "core/log.hpp"

namespace ftv {

namespace {

bool expand_includes(const std::string& dir, const std::string& file, int depth, std::string& out, std::string& error) {
  if (depth > 8) {
    error = "#include nesting too deep in " + file;
    return false;
  }
  auto text = read_text_file(dir + "/" + file);
  if (!text) {
    error = "cannot read shader '" + dir + "/" + file + "'";
    return false;
  }
  std::size_t pos = 0;
  while (pos < text->size()) {
    std::size_t eol = text->find('\n', pos);
    if (eol == std::string::npos) eol = text->size();
    const std::string line = text->substr(pos, eol - pos);
    const std::size_t inc = line.find("#include");
    if (inc != std::string::npos && line.find_first_not_of(" \t") == inc) {
      const std::size_t q1 = line.find('"', inc);
      const std::size_t q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
      if (q2 == std::string::npos) {
        error = "malformed #include in " + file;
        return false;
      }
      if (!expand_includes(dir, line.substr(q1 + 1, q2 - q1 - 1), depth + 1, out, error)) return false;
    } else {
      out.append(line);
      out.push_back('\n');
    }
    pos = eol + 1;
  }
  return true;
}

const char* stage_name(GLenum t) {
  switch (t) {
    case GL_VERTEX_SHADER: return "vertex";
    case GL_FRAGMENT_SHADER: return "fragment";
    case GL_COMPUTE_SHADER: return "compute";
    default: return "shader";
  }
}

}  // namespace

std::optional<gl::Program> build_program(const std::string& shaderDir, std::initializer_list<ShaderStage> stages,
                                         std::string& error) {
  // Sources declare `#version 450 core`. On a 4.3/4.4 context (the documented
  // fallback) the version line is rewritten to 430: everything the shaders use
  // (SSBOs, std430, compute, binding qualifiers, conservative depth) is core
  // in GLSL 4.30.
  GLint major = 0, minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  const bool downgrade = major == 4 && minor < 5;

  std::vector<gl::Shader> shaders;
  for (const ShaderStage& st : stages) {
    std::string src;
    if (!expand_includes(shaderDir, st.file, 0, src, error)) return std::nullopt;
    if (downgrade) {
      const std::size_t v = src.find("#version 450 core");
      if (v != std::string::npos) src.replace(v, 17, "#version 430 core");
    }
    gl::Shader sh(glCreateShader(st.type));
    const char* p = src.c_str();
    glShaderSource(sh.id(), 1, &p, nullptr);
    glCompileShader(sh.id());
    GLint ok = 0;
    glGetShaderiv(sh.id(), GL_COMPILE_STATUS, &ok);
    if (!ok) {
      GLint len = 0;
      glGetShaderiv(sh.id(), GL_INFO_LOG_LENGTH, &len);
      std::string log(static_cast<std::size_t>(len > 1 ? len : 1), '\0');
      glGetShaderInfoLog(sh.id(), len, nullptr, log.data());
      error = std::string("failed to compile ") + stage_name(st.type) + " shader '" + st.file + "':\n" + log;
      return std::nullopt;
    }
    shaders.push_back(std::move(sh));
  }
  gl::Program prog(glCreateProgram());
  for (const auto& sh : shaders) glAttachShader(prog.id(), sh.id());
  glLinkProgram(prog.id());
  for (const auto& sh : shaders) glDetachShader(prog.id(), sh.id());
  GLint ok = 0;
  glGetProgramiv(prog.id(), GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetProgramiv(prog.id(), GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<std::size_t>(len > 1 ? len : 1), '\0');
    glGetProgramInfoLog(prog.id(), len, nullptr, log.data());
    std::string names;
    for (const auto& st : stages) names += std::string(st.file) + " ";
    error = "failed to link program [" + names + "]:\n" + log;
    return std::nullopt;
  }
  return prog;
}

}  // namespace ftv
