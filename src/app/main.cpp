// ftviz entry point: argument parsing and locating the binary's directory.
// Everything else lives in app/application.cpp.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "app/application.hpp"
#include "app/cli.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::string executable_dir(const char* argv0) {
  namespace fs = std::filesystem;
  std::error_code ec;
#if defined(_WIN32)
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path().string();
#elif defined(__linux__)
  const fs::path self = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) return self.parent_path().string();
  ec.clear();
#endif
  const fs::path p = fs::absolute(fs::path(argv0 ? argv0 : ""), ec);
  if (ec) return ".";
  return p.parent_path().string();
}

}  // namespace

int main(int argc, char** argv) {
  ftv::CliOptions options;
  std::string error;
  if (!ftv::parse_cli(argc, argv, options, error)) {
    std::fprintf(stderr, "error: %s\n\n", error.c_str());
    ftv::print_usage(argv[0]);
    return 2;
  }
  if (options.help) {
    ftv::print_usage(argv[0]);
    return 0;
  }
  return ftv::run_application(options, executable_dir(argv[0]));
}
