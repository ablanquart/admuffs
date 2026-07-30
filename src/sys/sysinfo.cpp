// SPDX-License-Identifier: MIT
#include "sys/sysinfo.h"
#include "common.h"

#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace admuffs {

namespace {
std::string first_line(const std::string& path) {
    std::ifstream in(path);
    std::string s;
    std::getline(in, s);
    // device-tree strings are NUL-terminated
    size_t z = s.find('\0');
    if (z != std::string::npos) s = s.substr(0, z);
    return trim(s);
}

std::string run_capture(const char* cmd) {
    FILE* p = popen(cmd, "r");
    if (!p) return "";
    char buf[256] = {0};
    std::string out;
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return trim(out);
}
}  // namespace

std::string os_pretty_name() {
    std::ifstream in("/etc/os-release");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string v = line.substr(12);
            if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
    return "unknown OS";
}

std::string hardware_model() {
    std::string m = first_line("/proc/device-tree/model");   // Pi and friends
    if (!m.empty()) return m;
    // x86 dev boxes
    std::string v = first_line("/sys/class/dmi/id/product_name");
    return v.empty() ? "unknown hardware" : v;
}

std::string kernel_version() {
    struct utsname u;
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
    return "unknown";
}

std::string uptime_human() {
    std::ifstream in("/proc/uptime");
    double secs = 0;
    in >> secs;
    long s = (long)secs;
    char buf[64];
    if (s >= 86400) snprintf(buf, sizeof(buf), "%ldd %ldh %ldm", s/86400, (s%86400)/3600, (s%3600)/60);
    else if (s >= 3600) snprintf(buf, sizeof(buf), "%ldh %ldm", s/3600, (s%3600)/60);
    else snprintf(buf, sizeof(buf), "%ldm", s/60);
    return buf;
}

std::string service_state(const std::string& unit) {
    if (system("command -v systemctl >/dev/null 2>&1") != 0) return "unknown";
    std::string cmd = "systemctl is-active " + unit + " 2>/dev/null";
    std::string s = run_capture(cmd.c_str());
    if (s == "active" || s == "inactive" || s == "failed" || s == "activating")
        return s;
    // Distinguish "not installed" from plain inactive.
    cmd = "systemctl cat " + unit + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0 ? (s.empty() ? "inactive" : s) : "not-installed";
}

bool install_service(std::string& err) {
    if (geteuid() != 0) { err = "must run as root (sudo admuffs --install-service)"; return false; }

    char exe[512] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { err = "cannot resolve binary path"; return false; }

    // Run as the invoking user when installed via sudo (audio + lirc access
    // is group-based; root works too but least privilege is nicer).
    const char* sudo_user = getenv("SUDO_USER");
    std::string user = (sudo_user && *sudo_user) ? sudo_user : "root";

    std::ofstream unit("/etc/systemd/system/admuffs.service");
    if (!unit) { err = "cannot write /etc/systemd/system/admuffs.service"; return false; }
    unit << "[Unit]\n"
         << "Description=Admuffs - automatic TV commercial muting\n"
         << "After=network-online.target sound.target\n\n"
         << "[Service]\n"
         << "ExecStart=" << exe << " --run\n"
         << "Restart=always\n"
         << "RestartSec=3\n"
         << "User=" << user << "\n\n"
         << "[Install]\n"
         << "WantedBy=multi-user.target\n";
    unit.close();

    if (system("systemctl daemon-reload && systemctl enable --now admuffs.service") != 0) {
        err = "systemctl enable failed (see journalctl -u admuffs)";
        return false;
    }
    return true;
}

bool uninstall_service(std::string& err) {
    if (geteuid() != 0) { err = "must run as root"; return false; }
    if (system("systemctl disable --now admuffs.service 2>/dev/null") != 0)
        LOG_WARN("service was not active/enabled");
    remove("/etc/systemd/system/admuffs.service");
    if (system("systemctl daemon-reload") != 0) { err = "daemon-reload failed"; return false; }
    return true;
}

}  // namespace admuffs
