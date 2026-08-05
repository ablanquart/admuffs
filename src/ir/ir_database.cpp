// SPDX-License-Identifier: MIT
#include "ir/ir_database.h"
#include "common.h"

#include "json.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace admuffs {

namespace {
bool dir_exists(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
}  // namespace

std::string resolve_ir_db_dir(const std::string& configured) {
    if (dir_exists(configured)) return configured;
    static const char* kSearch[] = {
        "data/ir_codes",
        "/usr/local/share/admuffs/ir_codes",
        "/usr/share/admuffs/ir_codes",
    };
    for (const char* d : kSearch)
        if (dir_exists(d)) return d;
    // Nothing exists; return what we were given (or the dev path) so the
    // load() error message points somewhere sensible.
    return configured.empty() ? "data/ir_codes" : configured;
}

bool IrDatabase::load_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) { LOG_WARN("ir db: cannot open %s", path.c_str()); return false; }
    std::stringstream ss; ss << in.rdbuf();

    json j;
    try { j = json::parse(ss.str()); }
    catch (const std::exception& e) {
        LOG_WARN("ir db: parse error in %s: %s", path.c_str(), e.what());
        return false;
    }

    std::string brand = j.value("brand", "");
    if (brand.empty()) { LOG_WARN("ir db: %s missing 'brand'", path.c_str()); return false; }

    if (!j.contains("models") || !j["models"].is_array()) {
        LOG_WARN("ir db: %s missing 'models' array", path.c_str());
        return false;
    }

    for (const auto& m : j["models"]) {
        IrTvProfile p;
        p.brand = brand;
        p.model = m.value("model", "generic");
        p.display_name = m.value("display_name", brand + " " + p.model);
        p.default_lirc_remote = m.value("lirc_remote", "");

        if (m.contains("commands") && m["commands"].is_object()) {
            for (auto it = m["commands"].begin(); it != m["commands"].end(); ++it) {
                IrCommand c;
                c.name = it.key();
                const auto& v = it.value();
                if (v.is_string()) {
                    // Shorthand: "KEY_MUTE": "buttonName" -> uses default remote.
                    c.lirc_remote = p.default_lirc_remote;
                    c.lirc_button = v.get<std::string>();
                } else if (v.is_object()) {
                    c.lirc_remote = v.value("lirc_remote", p.default_lirc_remote);
                    c.lirc_button = v.value("lirc_button", "");
                    c.carrier_hz = v.value("carrier_hz", 38000);
                    if (v.contains("pulses") && v["pulses"].is_array())
                        c.pulses = v["pulses"].get<std::vector<int>>();
                }
                p.commands[c.name] = c;
            }
        }
        profiles_.push_back(std::move(p));
    }
    return true;
}

bool IrDatabase::load(const std::string& configured_dir) {
    profiles_.clear();
    std::string dir = resolve_ir_db_dir(configured_dir);

    // Prefer an explicit index.json listing files, else scan the directory.
    std::vector<std::string> files;
    std::string index = dir + "/index.json";
    std::ifstream idx(index);
    if (idx) {
        std::stringstream ss; ss << idx.rdbuf();
        try {
            json j = json::parse(ss.str());
            if (j.contains("files"))
                for (const auto& f : j["files"]) files.push_back(dir + "/" + f.get<std::string>());
        } catch (const std::exception& e) {
            LOG_WARN("ir db: bad index.json: %s", e.what());
        }
    }

    if (files.empty()) {
        DIR* d = opendir(dir.c_str());
        if (!d) { LOG_ERROR("ir db: cannot open dir %s", dir.c_str()); return false; }
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (name.size() > 5 && name.substr(name.size() - 5) == ".json" &&
                name != "index.json")
                files.push_back(dir + "/" + name);
        }
        closedir(d);
    }

    for (const auto& f : files) load_file(f);
    LOG_INFO("ir db: loaded %zu model profile(s) from %s", profiles_.size(), dir.c_str());
    return true;
}

std::vector<std::string> IrDatabase::brands() const {
    std::vector<std::string> out;
    for (const auto& p : profiles_) {
        bool found = false;
        for (const auto& b : out) if (b == p.brand) { found = true; break; }
        if (!found) out.push_back(p.brand);
    }
    return out;
}

std::vector<const IrTvProfile*> IrDatabase::models_for(const std::string& brand) const {
    std::vector<const IrTvProfile*> out;
    std::string b = to_lower(brand);
    for (const auto& p : profiles_) if (to_lower(p.brand) == b) out.push_back(&p);
    return out;
}

const IrTvProfile* IrDatabase::find(const std::string& brand, const std::string& model) const {
    std::string b = to_lower(brand), m = to_lower(model);
    for (const auto& p : profiles_)
        if (to_lower(p.brand) == b && to_lower(p.model) == m) return &p;
    // Fall back to the brand's generic profile if the exact model is absent.
    for (const auto& p : profiles_)
        if (to_lower(p.brand) == b && to_lower(p.model) == "generic") return &p;
    return nullptr;
}

}  // namespace admuffs
