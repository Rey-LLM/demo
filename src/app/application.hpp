#pragma once
// Top-level program flow: configuration, ingest, model build, layout and the
// interactive render loop (or the headless benchmark).

#include <string>

#include "app/cli.hpp"

namespace ftv {

// Returns the process exit code. `exeDir` is the directory of the running
// binary (shaders/ and config/ are looked up there first).
int run_application(const CliOptions& options, const std::string& exeDir);

}  // namespace ftv
