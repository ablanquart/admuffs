// SPDX-License-Identifier: MIT
#include "common.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>
#include <deque>
#include <mutex>
#include <thread>
#include <chrono>

namespace admuffs {

namespace {
LogLevel g_level = LogLevel::Info;
FILE* g_file = nullptr;
std::mutex g_mtx;

// ring buffer of recent lines for the web log viewer
std::deque<LogLine> g_ring;
uint64_t g_seq = 0;
constexpr size_t kRingMax = 400;

const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}
}  // namespace

void log_init(LogLevel level, const std::string& file) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_level = level;
    if (g_file && g_file != stderr) { fclose(g_file); g_file = nullptr; }
    if (!file.empty()) {
        g_file = fopen(file.c_str(), "a");
    }
}

void log_msg(LogLevel lvl, const char* fmt, ...) {
    if (lvl < g_level) return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char ts[32];
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    std::lock_guard<std::mutex> lk(g_mtx);
    fprintf(stderr, "[%s] %s  %s\n", ts, level_tag(lvl), msg);
    if (g_file) {
        fprintf(g_file, "[%s] %s  %s\n", ts, level_tag(lvl), msg);
        fflush(g_file);
    }
    // mirror into the web log ring
    char lined[2200];
    snprintf(lined, sizeof(lined), "[%s] %s  %s", ts, level_tag(lvl), msg);
    g_ring.push_back({++g_seq, lined});
    while (g_ring.size() > kRingMax) g_ring.pop_front();
}

std::vector<LogLine> log_since(uint64_t after, size_t max_lines) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::vector<LogLine> out;
    for (const auto& l : g_ring)
        if (l.seq > after) {
            out.push_back(l);
            if (out.size() >= max_lines) break;
        }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(::tolower((unsigned char)c));
    return s;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::string base64_encode(const std::string& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(tbl[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(tbl[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int run_argv(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> a;
        a.reserve(argv.size() + 1);
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        _exit(127);   // exec failed
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

}  // namespace admuffs
