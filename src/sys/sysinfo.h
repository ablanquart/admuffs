// SPDX-License-Identifier: MIT
// sysinfo.h - small host facts for the INFO panel + service management.
#pragma once

#include <string>

namespace admuffs {

std::string os_pretty_name();     // e.g. "Debian GNU/Linux 12 (bookworm)"
std::string hardware_model();     // e.g. "Raspberry Pi 4 Model B Rev 1.4"
std::string kernel_version();     // uname -r
std::string uptime_human();       // e.g. "3d 4h 12m"
// "active" | "inactive" | "failed" | "not-installed" | "unknown"
std::string service_state(const std::string& unit = "admuffs.service");

// Write + enable a systemd unit running this binary with --run.
// Returns true on success; err carries the reason otherwise. Needs root.
bool install_service(std::string& err);
bool uninstall_service(std::string& err);

}  // namespace admuffs
