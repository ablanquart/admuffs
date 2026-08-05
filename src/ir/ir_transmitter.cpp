// SPDX-License-Identifier: MIT
#include "ir/ir_transmitter.h"
#include "ir/ir_database.h"
#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace admuffs {

namespace {
// Shell-free exec (fork/execvp): no metacharacter parsing, so even a
// maliciously crafted ir_remote / device string cannot inject a command.
bool run(const std::vector<std::string>& argv) {
    return run_argv(argv) == 0;
}
bool have(const char* prog) {
    return run_argv({"/bin/sh", "-c",
                     std::string("command -v ") + prog + " >/dev/null 2>&1"}) == 0;
}
}  // namespace

const char* IrTransmitter::backend_name() const {
    switch (backend_) {
        case Backend::Lirc:   return "lirc(irsend)";
        case Backend::IrCtl:  return "ir-ctl";
        case Backend::DryRun: return "dryrun";
    }
    return "?";
}

bool IrTransmitter::init(const std::string& backend, const std::string& device) {
    device_ = device;
    if (backend == "irsend" || backend == "lirc") backend_ = Backend::Lirc;
    else if (backend == "ir-ctl" || backend == "irctl") backend_ = Backend::IrCtl;
    else backend_ = Backend::DryRun;

    if (backend_ == Backend::Lirc && !have("irsend")) {
        LOG_WARN("irsend not found; IR will run in dry-run mode. Install 'lirc'.");
        backend_ = Backend::DryRun;
    }
    if (backend_ == Backend::IrCtl && !have("ir-ctl")) {
        LOG_WARN("ir-ctl not found; IR will run in dry-run mode. Install 'v4l-utils'.");
        backend_ = Backend::DryRun;
    }
    LOG_INFO("IR transmitter backend: %s (device %s)", backend_name(), device_.c_str());
    return true;
}

bool IrTransmitter::send_lirc(const IrCommand& cmd) {
    if (!cmd.has_lirc()) {
        LOG_WARN("IR: command '%s' has no LIRC mapping", cmd.name.c_str());
        return false;
    }
    return run({"irsend", "SEND_ONCE", cmd.lirc_remote, cmd.lirc_button});
}

bool IrTransmitter::send_irctl(const IrCommand& cmd) {
    if (!cmd.has_raw()) {
        LOG_WARN("IR: command '%s' has no raw pulse data for ir-ctl", cmd.name.c_str());
        return false;
    }
    // ir-ctl --device /dev/lirc0 --send=<file> ; we pass pulses inline via a
    // temporary send file (format: "carrier N" then "pulse/space" durations).
    char tmpl[] = "/tmp/admuffs_ir_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return false;
    FILE* f = fdopen(fd, "w");
    fprintf(f, "carrier %d\n", cmd.carrier_hz);
    for (size_t i = 0; i < cmd.pulses.size(); ++i)
        fprintf(f, "%s %d\n", (i % 2 == 0 ? "pulse" : "space"), cmd.pulses[i]);
    fclose(f);

    bool ok = run({"ir-ctl", "--device", device_, std::string("--send=") + tmpl});
    remove(tmpl);
    return ok;
}

bool IrTransmitter::send(const IrCommand& cmd) {
    switch (backend_) {
        case Backend::Lirc:  return send_lirc(cmd);
        case Backend::IrCtl: return send_irctl(cmd);
        case Backend::DryRun:
            LOG_INFO("[dryrun] IR send '%s' (remote=%s button=%s raw=%zu)",
                     cmd.name.c_str(), cmd.lirc_remote.c_str(),
                     cmd.lirc_button.c_str(), cmd.pulses.size());
            return true;
    }
    return false;
}

}  // namespace admuffs
