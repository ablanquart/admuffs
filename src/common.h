// SPDX-License-Identifier: MIT
// common.h - shared utilities: logging, small string/time helpers.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace admuffs {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Initialize logging. If `file` is non-empty, logs are also appended there.
void log_init(LogLevel level, const std::string& file = "");
void log_msg(LogLevel lvl, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define LOG_DEBUG(...) ::admuffs::log_msg(::admuffs::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  ::admuffs::log_msg(::admuffs::LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  ::admuffs::log_msg(::admuffs::LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::admuffs::log_msg(::admuffs::LogLevel::Error, __VA_ARGS__)

// In-memory ring of recent log lines (what the CLI prints), for the web
// remote's VIEW LOG panel. Returns lines with seq > after (oldest first) and
// is safe to call from any thread.
struct LogLine { uint64_t seq; std::string text; };
std::vector<LogLine> log_since(uint64_t after, size_t max_lines = 200);

// String helpers.
std::string trim(const std::string& s);
std::string to_lower(std::string s);
std::vector<std::string> split(const std::string& s, char delim);
std::string base64_encode(const std::string& in);

// Monotonic milliseconds since an arbitrary epoch (for timing/hysteresis).
uint64_t now_ms();

// Sleep helper.
void sleep_ms(int ms);

// Run a program via fork/execvp WITHOUT a shell (no metacharacter parsing) --
// the safe alternative to system() when any argument is user-influenced.
// argv[0] is the program; returns its exit code, or -1 on spawn failure.
int run_argv(const std::vector<std::string>& argv);

}  // namespace admuffs
