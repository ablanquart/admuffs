// SPDX-License-Identifier: MIT
// ir_record.h - record IR codes from the user's real remote via the pHAT's
// receiver, and persist them as per-key overrides that beat the bundled DB.
#pragma once

#include <string>

namespace admuffs {

struct RecordResult {
    bool ok = false;
    std::string error;
    std::string key;       // KEY_* recorded
    int pulses = 0;        // durations captured
    std::string device;    // RX device used
};

// Path of the user's recorded-key override file (lives next to the config).
std::string recorded_keys_path();

// Find the first /dev/lircN that can RECEIVE (LIRC_GET_FEATURES). Empty if none.
std::string find_rx_device();

// Blocking capture (~up to 9 s): waits for one keypress on the RX device,
// parses the pulse train, and saves it under `key` in recorded_keys_path().
// Recorded keys override the IR database for whatever TV is configured.
RecordResult record_ir_key(const std::string& key);

}  // namespace admuffs
