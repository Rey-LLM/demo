#pragma once
// Minimal printf-style logging to stderr. Stateless: there is no logger object
// and no global mutable configuration. Debug-level messages compile away in
// release builds. Never call from per-row or per-frame hot loops.

#include <cstdarg>

namespace ftv {

enum class LogLevel { Debug, Info, Warn, Error };

#if defined(__GNUC__) || defined(__clang__)
#define FTV_PRINTF_LIKE(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define FTV_PRINTF_LIKE(fmt_idx, arg_idx)
#endif

void log_write(LogLevel level, const char* fmt, ...) FTV_PRINTF_LIKE(2, 3);

}  // namespace ftv

#define FTV_LOG_INFO(...) ::ftv::log_write(::ftv::LogLevel::Info, __VA_ARGS__)
#define FTV_LOG_WARN(...) ::ftv::log_write(::ftv::LogLevel::Warn, __VA_ARGS__)
#define FTV_LOG_ERROR(...) ::ftv::log_write(::ftv::LogLevel::Error, __VA_ARGS__)
#ifndef NDEBUG
#define FTV_LOG_DEBUG(...) ::ftv::log_write(::ftv::LogLevel::Debug, __VA_ARGS__)
#else
#define FTV_LOG_DEBUG(...) ((void)0)
#endif
