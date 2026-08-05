// SPDX-License-Identifier: MIT
// ir_database.h - loads the on-disk infrared code database.
//
// The database is a directory of JSON files (one per brand) plus an index.json.
// Each brand file lists models; each model maps logical command names
// ("KEY_MUTE", "KEY_VOLUMEUP", ...) to either a LIRC remote/button pair or a
// raw carrier+pulse sequence usable by `ir-ctl`.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace admuffs {

struct IrCommand {
    std::string name;          // logical name, e.g. "KEY_MUTE"
    std::string lirc_remote;   // LIRC remote name (for `irsend`)
    std::string lirc_button;   // LIRC button name  (for `irsend`)
    int carrier_hz = 38000;    // for raw ir-ctl transmission
    std::vector<int> pulses;   // raw pulse/space durations (us); optional
    bool has_raw() const { return !pulses.empty(); }
    bool has_lirc() const { return !lirc_remote.empty() && !lirc_button.empty(); }
};

struct IrTvProfile {
    std::string brand;             // "samsung"
    std::string model;             // "generic" or a specific model id
    std::string display_name;      // human label shown in the wizard
    std::string default_lirc_remote;
    std::map<std::string, IrCommand> commands;

    const IrCommand* command(const std::string& name) const {
        auto it = commands.find(name);
        return it == commands.end() ? nullptr : &it->second;
    }
};

// Resolve where the IR code database lives. If `configured` is non-empty and
// exists, it wins. Otherwise the standard locations are searched in order:
//   ./data/ir_codes                       (running from a source checkout)
//   /usr/local/share/admuffs/ir_codes      (make install default)
//   /usr/share/admuffs/ir_codes            (distro packaging)
// Returns the first directory that exists, else `configured`/the dev path.
std::string resolve_ir_db_dir(const std::string& configured);

class IrDatabase {
public:
    // Load every *.json listed in <dir>/index.json (or every *.json in dir if no
    // index). Returns false only if the directory cannot be read at all.
    bool load(const std::string& dir);

    std::vector<std::string> brands() const;
    std::vector<const IrTvProfile*> models_for(const std::string& brand) const;
    const IrTvProfile* find(const std::string& brand, const std::string& model) const;
    size_t size() const { return profiles_.size(); }

private:
    std::vector<IrTvProfile> profiles_;
    bool load_file(const std::string& path);
};

}  // namespace admuffs
