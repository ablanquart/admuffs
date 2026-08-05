// SPDX-License-Identifier: MIT
// ir_transmitter.h - sends IR commands via LIRC (`irsend`) or kernel rc-core
// (`ir-ctl`). Shelling out avoids a hard link-time dependency on liblirc and
// works with the standard Raspberry Pi OS gpio-ir stack used by the ANAVI HAT.
#pragma once

#include <string>

namespace admuffs {

struct IrCommand;  // from ir_database.h

class IrTransmitter {
public:
    enum class Backend { Lirc, IrCtl, DryRun };

    // backend: "irsend" | "ir-ctl" | "dryrun".  device: /dev/lircN for ir-ctl.
    bool init(const std::string& backend, const std::string& device);

    // Transmit a command. For LIRC uses the command's remote/button; for ir-ctl
    // uses the raw pulse list (falls back to dry-run log if unavailable).
    bool send(const IrCommand& cmd);

    Backend backend() const { return backend_; }
    const char* backend_name() const;

private:
    Backend backend_ = Backend::DryRun;
    std::string device_ = "/dev/lirc0";

    bool send_lirc(const IrCommand& cmd);
    bool send_irctl(const IrCommand& cmd);
};

}  // namespace admuffs
