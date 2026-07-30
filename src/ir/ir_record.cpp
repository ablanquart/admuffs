// SPDX-License-Identifier: MIT
#include "ir/ir_record.h"
#include "config.h"
#include "common.h"

#include "json.hpp"

#include <fcntl.h>
#include <linux/lirc.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

using json = nlohmann::json;

namespace admuffs {

std::string recorded_keys_path() {
    std::string cfg = default_config_path();               // .../admuffs.conf
    size_t slash = cfg.find_last_of('/');
    return cfg.substr(0, slash + 1) + "recorded_keys.json";
}

std::string find_rx_device() {
    for (int i = 0; i < 4; ++i) {
        std::string dev = "/dev/lirc" + std::to_string(i);
        int fd = open(dev.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        uint32_t feats = 0;
        bool can_rx = ioctl(fd, LIRC_GET_FEATURES, &feats) == 0 &&
                      (feats & LIRC_CAN_REC_MODE2);
        close(fd);
        if (can_rx) return dev;
    }
    return "";
}

RecordResult record_ir_key(const std::string& key) {
    RecordResult r;
    r.key = key;

    // Sanity: only accept KEY_* style names (they become JSON keys + shell arg).
    if (key.size() < 5 || key.rfind("KEY_", 0) != 0 ||
        key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) {
        r.error = "invalid key name";
        return r;
    }

    if (system("command -v ir-ctl >/dev/null 2>&1") != 0) {
        r.error = "ir-ctl not installed (sudo apt install v4l-utils)";
        return r;
    }
    r.device = find_rx_device();
    if (r.device.empty()) {
        r.error = "no receive-capable /dev/lirc* device (run: admuffs --ir-check)";
        return r;
    }

    // Capture one burst. --one-shot stops at the first message on modern
    // ir-ctl; a plain timed capture is the fallback for older versions.
    std::string cmd = "timeout 9 ir-ctl -d " + r.device + " -r --one-shot 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { r.error = "cannot run ir-ctl"; return r; }
    std::vector<int> pulses;
    char line[128];
    while (fgets(line, sizeof(line), p)) {
        std::istringstream iss(line);
        std::string kind; long v = 0;
        if (!(iss >> kind >> v)) continue;
        if (kind == "timeout") break;
        if (kind == "pulse" || kind == "space") pulses.push_back((int)v);
    }
    pclose(p);

    if (pulses.size() < 6) {
        r.error = "nothing received -- point the remote at the pHAT's receiver "
                  "and press the button once (fresh batteries help)";
        return r;
    }
    if (pulses.size() % 2 == 0) pulses.pop_back();   // end on a mark

    // Merge into the override file: { "KEY_X": {carrier_hz, pulses}, ... }
    std::string path = recorded_keys_path();
    json j = json::object();
    {
        std::ifstream in(path);
        if (in) {
            try { std::stringstream ss; ss << in.rdbuf(); j = json::parse(ss.str()); }
            catch (...) { j = json::object(); }
        }
    }
    j[key] = {{"carrier_hz", 38000}, {"pulses", pulses}};
    {
        // config dir already exists (config was saved there)
        std::ofstream out(path, std::ios::trunc);
        if (!out) { r.error = "cannot write " + path; return r; }
        out << j.dump();
    }

    r.ok = true;
    r.pulses = (int)pulses.size();
    LOG_INFO("ir record: %s captured (%d durations) from %s -> %s",
             key.c_str(), r.pulses, r.device.c_str(), path.c_str());
    return r;
}

}  // namespace admuffs
