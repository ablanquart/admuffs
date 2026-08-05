// SPDX-License-Identifier: MIT
#include "config.h"
#include "common.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace admuffs {

std::string Config::method_to_string(ControlMethod m) {
    switch (m) {
        case ControlMethod::Auto: return "auto";
        case ControlMethod::Api:  return "api";
        case ControlMethod::Ir:   return "ir";
    }
    return "auto";
}

ControlMethod Config::method_from_string(const std::string& s) {
    std::string v = to_lower(trim(s));
    if (v == "api") return ControlMethod::Api;
    if (v == "ir")  return ControlMethod::Ir;
    return ControlMethod::Auto;
}

bool Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;

    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }

    auto S = [&](const char* k, std::string& dst) {
        auto it = kv.find(k); if (it != kv.end()) dst = it->second;
    };
    auto I = [&](const char* k, int& dst) {
        auto it = kv.find(k); if (it != kv.end()) dst = std::atoi(it->second.c_str());
    };
    auto D = [&](const char* k, double& dst) {
        auto it = kv.find(k); if (it != kv.end()) dst = std::atof(it->second.c_str());
    };

    S("tv_brand", tv_brand);
    S("tv_model", tv_model);
    { std::string m; S("method", m); if (!m.empty()) method = method_from_string(m); }
    S("tv_ip", tv_ip);
    S("samsung_token", samsung_token);
    S("lg_client_key", lg_client_key);
    S("sony_psk", sony_psk);
    S("vizio_auth_token", vizio_auth_token);
    S("ir_backend", ir_backend);
    S("ir_device", ir_device);
    S("ir_remote", ir_remote);
    S("audio_device", audio_device);
    S("audio_tap", audio_tap);
    S("mute_mode", mute_mode);
    I("duck_steps", duck_steps);
    I("max_mute_s", max_mute_s);
    D("norm_target_db", norm_target_db);
    D("norm_tolerance_db", norm_tolerance_db);
    I("norm_interval_ms", norm_interval_ms);
    I("norm_max_range", norm_max_range);
    I("audio_rate", audio_rate);
    D("loudness_delta_db", loudness_delta_db);
    I("mute_debounce_ms", mute_debounce_ms);
    I("unmute_debounce_ms", unmute_debounce_ms);
    D("fire_threshold", fire_threshold);
    D("cal_normal_db", cal_normal_db);
    I("cal_normal_n", cal_normal_n);
    D("cal_commercial_db", cal_commercial_db);
    I("cal_commercial_n", cal_commercial_n);
    S("acr_provider", acr_provider);
    S("acr_host", acr_host);
    S("acr_key", acr_key);
    S("acr_secret", acr_secret);
    I("acr_interval_s", acr_interval_s);
    I("acr_sample_seconds", acr_sample_seconds);
    S("metadata_provider", metadata_provider);
    S("metadata_url", metadata_url);
    S("metadata_key", metadata_key);
    I("web_port", web_port);
    S("ir_db_dir", ir_db_dir);
    S("log_file", log_file);
    I("log_level", log_level);
    S("pin_salt", pin_salt);
    S("pin_hash", pin_hash);
    return true;
}

static void ensure_parent_dir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return;
    std::string dir = path.substr(0, slash);
    std::string acc;
    for (auto& part : split(dir, '/')) {
        if (part.empty()) { acc += "/"; continue; }
        acc += part + "/";
        mkdir(acc.c_str(), 0755);  // ignore EEXIST
    }
}

bool Config::save(const std::string& path) const {
    ensure_parent_dir(path);
    // Create the file owner-only from the very first byte: the config holds the
    // ACR secret and the PIN hash, so there must be no window in which it is
    // world-readable (a plain ofstream would create it 0644 until the chmod
    // below). Tightening umask makes the O_CREAT happen as 0600.
    mode_t old_umask = umask(S_IRWXG | S_IRWXO);
    std::ofstream out(path, std::ios::trunc);
    umask(old_umask);
    if (!out) { LOG_ERROR("cannot write config: %s", path.c_str()); return false; }

    out << "# admuffs configuration\n";
    out << "tv_brand=" << tv_brand << "\n";
    out << "tv_model=" << tv_model << "\n";
    out << "method=" << method_to_string(method) << "\n";
    out << "tv_ip=" << tv_ip << "\n";
    out << "samsung_token=" << samsung_token << "\n";
    out << "lg_client_key=" << lg_client_key << "\n";
    out << "sony_psk=" << sony_psk << "\n";
    out << "vizio_auth_token=" << vizio_auth_token << "\n";
    out << "\n# infrared\n";
    out << "ir_backend=" << ir_backend << "\n";
    out << "ir_device=" << ir_device << "\n";
    out << "ir_remote=" << ir_remote << "\n";
    out << "\n# audio / detection\n";
    out << "audio_device=" << audio_device << "\n";
    out << "audio_tap=" << audio_tap << "\n";
    out << "mute_mode=" << mute_mode << "\n";
    out << "duck_steps=" << duck_steps << "\n";
    out << "max_mute_s=" << max_mute_s << "\n";
    out << "norm_target_db=" << norm_target_db << "\n";
    out << "norm_tolerance_db=" << norm_tolerance_db << "\n";
    out << "norm_interval_ms=" << norm_interval_ms << "\n";
    out << "norm_max_range=" << norm_max_range << "\n";
    out << "audio_rate=" << audio_rate << "\n";
    out << "loudness_delta_db=" << loudness_delta_db << "\n";
    out << "mute_debounce_ms=" << mute_debounce_ms << "\n";
    out << "unmute_debounce_ms=" << unmute_debounce_ms << "\n";
    out << "fire_threshold=" << fire_threshold << "\n";
    out << "\n# loudness calibration (written by the web remote's Sample buttons)\n";
    out << "cal_normal_db=" << cal_normal_db << "\n";
    out << "cal_normal_n=" << cal_normal_n << "\n";
    out << "cal_commercial_db=" << cal_commercial_db << "\n";
    out << "cal_commercial_n=" << cal_commercial_n << "\n";
    out << "\n# external services (optional)\n";
    out << "acr_provider=" << acr_provider << "\n";
    out << "acr_host=" << acr_host << "\n";
    out << "acr_key=" << acr_key << "\n";
    out << "acr_secret=" << acr_secret << "\n";
    out << "acr_interval_s=" << acr_interval_s << "\n";
    out << "acr_sample_seconds=" << acr_sample_seconds << "\n";
    out << "metadata_provider=" << metadata_provider << "\n";
    out << "metadata_url=" << metadata_url << "\n";
    out << "metadata_key=" << metadata_key << "\n";
    out << "\n# web remote (0 disables)\n";
    out << "web_port=" << web_port << "\n";
    out << "\n# misc\n";
    out << "ir_db_dir=" << ir_db_dir << "\n";
    out << "log_file=" << log_file << "\n";
    out << "log_level=" << log_level << "\n";
    out << "\n# web auth (hashed PIN + salt; never the PIN itself)\n";
    out << "pin_salt=" << pin_salt << "\n";
    out << "pin_hash=" << pin_hash << "\n";
    out.close();
    // The config holds the ACR secret and the PIN hash -- restrict to owner.
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
    return static_cast<bool>(out);
}

std::string default_config_path() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else {
        const char* home = getenv("HOME");
        base = (home ? std::string(home) : ".") + "/.config";
    }
    return base + "/admuffs/admuffs.conf";
}

}  // namespace admuffs
