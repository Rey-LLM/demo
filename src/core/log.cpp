#include "core/log.hpp"

#include <chrono>
#include <cstdio>

namespace ftv {

void log_write(LogLevel level, const char* fmt, ...) {
  static constexpr const char* kTags[] = {"debug", " info", " warn", "error"};
  // Build the whole line in a stack buffer and emit it with a single fwrite so
  // concurrent log lines from different threads do not interleave.
  char line[1024];
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const double secs = std::chrono::duration<double>(now).count();
  int n = std::snprintf(line, sizeof(line), "[%10.3f %s] ", secs, kTags[static_cast<int>(level)]);
  if (n < 0) return;
  va_list args;
  va_start(args, fmt);
  const int m = std::vsnprintf(line + n, sizeof(line) - static_cast<std::size_t>(n) - 1, fmt, args);
  va_end(args);
  std::size_t len = static_cast<std::size_t>(n) + (m > 0 ? static_cast<std::size_t>(m) : 0u);
  if (len > sizeof(line) - 2) len = sizeof(line) - 2;
  line[len++] = '\n';
  std::fwrite(line, 1, len, stderr);
}

}  // namespace ftv
