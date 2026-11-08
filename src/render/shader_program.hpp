#pragma once
// Shader loading: reads GLSL from the shader directory, expands
// `#include "file"` directives (startup only), compiles, links, and prints the
// complete info log on any failure.

#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

#include "render/gl_raii.hpp"

namespace ftv {

struct ShaderStage {
  GLenum type;
  const char* file;
};

std::optional<gl::Program> build_program(const std::string& shaderDir, std::initializer_list<ShaderStage> stages,
                                         std::string& error);

}  // namespace ftv
