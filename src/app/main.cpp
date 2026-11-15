// ftviz entry point: argument parsing and locating the binary's directory.
// Everything else lives in app/application.cpp.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "app/application.hpp"
#include "app/cli.hpp"

#include "core/log.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

#if defined(FTVIZ_WIN32_GUI)
// The GUI-subsystem build opens no console on double-click. Started from a
// terminal, it attaches to that console so logs and --help still print.
bool attach_parent_console() {
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
  FILE* f = nullptr;
  freopen_s(&f, "CONOUT$", "w", stdout);
  freopen_s(&f, "CONOUT$", "w", stderr);
  return true;
}

void show_error_box(const char* text) {
  MessageBoxA(nullptr, text, "ftviz", MB_OK | MB_ICONERROR);
}
#endif

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
#if defined(FTVIZ_WIN32_GUI)
  const bool console = attach_parent_console();
#endif
  ftv::CliOptions options;
  std::string error;
  if (!ftv::parse_cli(argc, argv, options, error)) {
    std::fprintf(stderr, "error: %s\n\n", error.c_str());
    ftv::print_usage(argv[0]);
#if defined(FTVIZ_WIN32_GUI)
    if (!console) show_error_box(("Invalid arguments: " + error).c_str());
#endif
    return 2;
  }
  if (options.help) {
    ftv::print_usage(argv[0]);
    return 0;
  }
  const int code = ftv::run_application(options, executable_dir(argv[0]));
#if defined(FTVIZ_WIN32_GUI)
  if (code != 0 && !console) {
    const char* why = ftv::first_error_message();
    show_error_box((std::string("ftviz stopped with an error:\n\n") + (*why ? why : "see the log") +
                    "\n\nRun ftviz.exe from a terminal to see the full log.")
                       .c_str());
  }
#endif
  return code;
}
