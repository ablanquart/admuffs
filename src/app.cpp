// SPDX-License-Identifier: MIT
#include "app.h"
#include "common.h"
#include "ir/ir_record.h"
#include "sensors/i2c_sensors.h"
#include "sys/sysinfo.h"
#include "version.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>

using json = nlohmann::json;

namespace admuffs {

App::App(Config cfg, std::string config_path)
    : cfg_(std::move(cfg)), config_path_(std::move(config_path)) {}

CredentialSaveFn App::credential_saver() {
    if (config_path_.empty()) return nullptr;
    return [this](const std::string& key, const std::string& value) {
        if (key == "samsung_token") cfg_.samsung_token = value;
        else if (key == "lg_client_key") cfg_.lg_client_key = value;
        else { LOG_WARN("unknown credential key '%s' (not persisted)", key.c_str()); return; }
        if (cfg_.save(config_path_))
            LOG_INFO("persisted new %s to %s", key.c_str(), config_path_.c_str());
        else
            LOG_WARN("could not persist %s to %s", key.c_str(), config_path_.c_str());
    };
}

bool App::init() {
    if (!db_.load(cfg_.ir_db_dir))
        LOG_WARN("IR database failed to load from %s", cfg_.ir_db_dir.c_str());

    tv_ = make_controller(cfg_, db_, credential_saver());
    if (!tv_) {
        LOG_ERROR("no TV controller could be built; check tv_brand/tv_model/method");
        return false;
    }
    if (!tv_->connect())
        LOG_WARN("TV controller connect() failed; commands may not reach the TV");
    LOG_INFO("TV control ready: %s", tv_->name().c_str());

    // One shared microphone pipeline feeds every audio-based source.
    if (!cfg_.audio_device.empty()) {
        audio_bus_ = std::make_shared<AudioBus>(cfg_.audio_device, cfg_.audio_rate);
        if (!audio_bus_->start()) audio_bus_.reset();
    } else {
        LOG_INFO("audio capture disabled (no audio_device)");
    }

    // Assemble detection sources; keep only the ones that start successfully.
    auto loudness = std::make_unique<LoudnessSource>(cfg_, audio_bus_);
    // Apply any existing calibration before the detector starts.
    if (cfg_.cal_normal_n > 0 && cfg_.cal_commercial_n > 0) {
        double gap = cfg_.cal_commercial_db - cfg_.cal_normal_db;
        double derived = std::clamp(gap * 0.6, 1.5, 12.0);
        loudness->set_delta_db(derived);
        LOG_INFO("loudness: calibrated threshold %.1f dB (normal %.1f dB x%d, "
                 "commercial %.1f dB x%d)", derived, cfg_.cal_normal_db,
                 cfg_.cal_normal_n, cfg_.cal_commercial_db, cfg_.cal_commercial_n);
    }

    std::vector<std::unique_ptr<DetectionSource>> candidates;
    candidates.push_back(std::move(loudness));
    candidates.push_back(std::make_unique<AcrSource>(cfg_, audio_bus_));
    candidates.push_back(std::make_unique<MetadataSource>(cfg_));

    for (auto& s : candidates) {
        if (s->start()) {
            LOG_INFO("detection source active: %s (weight %.2f)", s->name().c_str(), s->weight());
            sources_.push_back(std::move(s));
        }
    }
    // Keep a typed handle on the loudness source for live recalibration.
    for (auto& s : sources_)
        if (s->name() == "loudness(alsa)") loudness_ = static_cast<LoudnessSource*>(s.get());
    if (sources_.empty())
        LOG_WARN("no detection sources active; nothing will trigger a mute");

    fusion_ = std::make_unique<FusionEngine>(cfg_);
    return true;
}

bool App::room_tap() const { return to_lower(cfg_.audio_tap) != "upstream"; }

double App::measure_room_db(int seconds) const {
    if (!audio_bus_ || !audio_bus_->running()) return NAN;
    const size_t want = (size_t)audio_bus_->rate() * (size_t)seconds;
    std::vector<int16_t> buf;
    if (audio_bus_->latest(want, buf) < want / 2) return NAN;
    double sumsq = 0.0;
    for (int16_t s16 : buf) { double s = s16 / 32768.0; sumsq += s * s; }
    return 20.0 * std::log10(std::sqrt(sumsq / (double)buf.size()) + 1e-9);
}

void App::begin_suppression() {
    std::lock_guard<std::mutex> lk(tv_mtx_);
    if (to_lower(cfg_.mute_mode) == "volume_drop") {
        double before = room_tap() ? measure_room_db(2) : NAN;
        LOG_INFO("=> dropping TV volume (%d steps)", cfg_.drop_steps);
        for (int i = 0; i < cfg_.drop_steps; ++i) {
            tv_->send(TvCommand::VolumeDown);
            sleep_ms(150);
        }
        // With a room mic, measure how much the drop actually attenuated the
        // room and compensate the loudness detector so dropped audio is still
        // judged on the original scale (else the quiet we created reads as
        // "program resumed").
        if (room_tap() && loudness_ && !std::isnan(before)) {
            sleep_ms(2000);  // let post-drop audio fill the measurement window
            double after = measure_room_db(2);
            if (!std::isnan(after)) {
                double comp = std::clamp(before - after, 0.0, 20.0);
                loudness_->set_comp_db(comp);
                LOG_INFO("volume drop: measured %.1f dB attenuation; compensating", comp);
            }
        }
    } else {
        LOG_INFO("=> muting TV");
        tv_->send(TvCommand::Mute);
    }
    suppressing_ = true;
    suppress_since_ms_ = now_ms();
}

void App::end_suppression(const char* why) {
    std::lock_guard<std::mutex> lk(tv_mtx_);
    if (to_lower(cfg_.mute_mode) == "volume_drop") {
        LOG_INFO("=> restoring TV volume (%s)", why);
        for (int i = 0; i < cfg_.drop_steps; ++i) {
            tv_->send(TvCommand::VolumeUp);
            sleep_ms(150);
        }
        if (loudness_) loudness_->set_comp_db(0.0);
    } else {
        LOG_INFO("=> unmuting TV (%s)", why);
        tv_->send(TvCommand::Unmute);
    }
    suppressing_ = false;
    suppress_since_ms_ = 0;
}

AudioSampleResult App::set_norm_target() {
    AudioSampleResult r;
    if (!audio_bus_ || !audio_bus_->running()) {
        r.error = "no audio capture (set audio_device / plug in the mic)";
        return r;
    }
    double db = measure_room_db(2);
    if (std::isnan(db)) {
        r.error = "audio still warming up; try again in a few seconds";
        return r;
    }
    std::lock_guard<std::mutex> lk(cal_mtx_);
    cfg_.norm_target_db = db;
    norm_ema_db_ = db;
    norm_ema_init_ = true;
    r.ok = true;
    r.db = db;
    r.avg_db = db;
    r.n = 1;
    LOG_INFO("normalize: volume target set to %.1f dB (current room level)", db);
    if (!config_path_.empty() && !cfg_.save(config_path_))
        LOG_WARN("normalize: could not persist target to %s", config_path_.c_str());
    return r;
}

void App::normalize_tick() {
    // Idle until the user has set a target (real levels are negative dBFS).
    if (cfg_.norm_target_db >= -1.0) return;

    double lvl = measure_room_db(2);
    if (std::isnan(lvl)) return;

    // ~2 s EMA over 200 ms ticks: reacts to ad-break level shifts, ignores
    // one-off bangs and beats.
    if (!norm_ema_init_) { norm_ema_db_ = lvl; norm_ema_init_ = true; }
    else norm_ema_db_ = 0.9 * norm_ema_db_ + 0.1 * lvl;

    uint64_t now = now_ms();
    if (now - norm_last_nudge_ms_ < (uint64_t)std::max(500, cfg_.norm_interval_ms))
        return;

    double err = norm_ema_db_ - cfg_.norm_target_db;
    if (err > cfg_.norm_tolerance_db && norm_net_steps_ < cfg_.norm_max_range) {
        // Room is louder than the user's chosen level: one step down.
        std::lock_guard<std::mutex> lk(tv_mtx_);
        tv_->send(TvCommand::VolumeDown);
        norm_net_steps_++;
        norm_last_nudge_ms_ = now;
        LOG_INFO("normalize: %.1f dB over target -> volume down (net -%d)",
                 err, norm_net_steps_);
    } else if (err < -cfg_.norm_tolerance_db && norm_net_steps_ > -cfg_.norm_max_range &&
               norm_ema_db_ > cfg_.norm_target_db - 15.0) {
        // Quieter than target: one step up. The -15 dB floor stops the
        // normalizer from cranking the volume during silence (dialogue
        // pauses, TV off) -- that quiet isn't a mixing choice to correct.
        std::lock_guard<std::mutex> lk(tv_mtx_);
        tv_->send(TvCommand::VolumeUp);
        norm_net_steps_--;
        norm_last_nudge_ms_ = now;
        LOG_INFO("normalize: %.1f dB under target -> volume up (net -%d)",
                 -err, norm_net_steps_);
    }
}

void App::restore_norm(const char* why) {
    if (norm_net_steps_ == 0) { norm_ema_init_ = false; return; }
    std::lock_guard<std::mutex> lk(tv_mtx_);
    LOG_INFO("normalize: restoring %+d volume steps (%s)", -norm_net_steps_, why);
    while (norm_net_steps_ > 0) { tv_->send(TvCommand::VolumeUp);   norm_net_steps_--; sleep_ms(150); }
    while (norm_net_steps_ < 0) { tv_->send(TvCommand::VolumeDown); norm_net_steps_++; sleep_ms(150); }
    norm_ema_init_ = false;
}


// ---------------------------------------------------------------------------
// Web settings hub
// ---------------------------------------------------------------------------
namespace {
// Editable settings. "live" fields are read by the run loop at use time, so
// a change applies immediately; everything else is consumed at init and
// needs a restart (the UI shows which).
struct SettingDef { const char* key; bool live; };
const SettingDef kSettings[] = {
    {"tv_brand", false},   {"tv_model", false},   {"method", false},
    {"tv_ip", false},      {"ir_backend", false}, {"ir_device", false},
    {"ir_remote", false},  {"audio_device", false},{"audio_tap", false},
    {"mute_mode", false},  {"drop_steps", true},  {"max_mute_s", true},
    {"norm_tolerance_db", true}, {"norm_interval_ms", true},
    {"norm_max_range", true},    {"web_port", false},
    {"acr_provider", false}, {"acr_host", false},
    {"acr_key", false},      {"acr_secret", false},
};
}  // namespace

std::string App::config_json() {
    std::lock_guard<std::mutex> lk(cal_mtx_);
    json j;
    j["tv_brand"] = cfg_.tv_brand;         j["tv_model"] = cfg_.tv_model;
    j["method"] = Config::method_to_string(cfg_.method);
    j["tv_ip"] = cfg_.tv_ip;               j["ir_backend"] = cfg_.ir_backend;
    j["ir_device"] = cfg_.ir_device;       j["ir_remote"] = cfg_.ir_remote;
    j["audio_device"] = cfg_.audio_device; j["audio_tap"] = cfg_.audio_tap;
    j["mute_mode"] = cfg_.mute_mode;       j["drop_steps"] = cfg_.drop_steps;
    j["max_mute_s"] = cfg_.max_mute_s;
    j["norm_tolerance_db"] = cfg_.norm_tolerance_db;
    j["norm_interval_ms"] = cfg_.norm_interval_ms;
    j["norm_max_range"] = cfg_.norm_max_range;
    j["web_port"] = cfg_.web_port;
    j["acr_provider"] = cfg_.acr_provider; j["acr_host"] = cfg_.acr_host;
    // Both ACR credentials are secrets -- never reflect them back in the clear.
    j["acr_key"] = cfg_.acr_key.empty() ? "" : "********";
    j["acr_secret"] = cfg_.acr_secret.empty() ? "" : "********";
    return j.dump();
}

std::string App::config_set(const std::string& key, const std::string& value) {
    const SettingDef* def = nullptr;
    for (const auto& d : kSettings) if (key == d.key) { def = &d; break; }
    json r;
    if (!def) { r["ok"] = false; r["error"] = "unknown or read-only setting"; return r.dump(); }

    // Validation for enums / numbers.
    auto is_int = [](const std::string& v) {
        return !v.empty() && v.find_first_not_of("0123456789") == std::string::npos; };
    auto is_num = [](const std::string& v) {
        return !v.empty() && v.find_first_not_of("0123456789.") == std::string::npos; };
    std::string v = trim(value);
    std::string lv = to_lower(v);
    auto reject = [&](const char* why) { r["ok"] = false; r["error"] = why; return r.dump(); };

    // Reject control chars and cap length on ALL free-text input (defense in
    // depth; the IR transmitter is already shell-free, but keep values sane).
    if (v.size() > 128) return reject("value too long (max 128)");
    for (unsigned char c : v)
        if (c < 0x20 || c == 0x7F) return reject("value contains control characters");
    // Per-field allowlists for the fields that reach the shell / filesystem /
    // network layer. Conservative on purpose.
    auto charset_ok = [](const std::string& s, const char* allowed) {
        return s.find_first_not_of(allowed) == std::string::npos;
    };
    static const char* HOST = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:";
    static const char* NAME = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._- ";
    static const char* PATH = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/:";
    if ((key == "tv_ip" || key == "acr_host") && !v.empty() && !charset_ok(v, HOST))
        return reject("only letters, digits, . - : allowed");
    if ((key == "ir_device" || key == "audio_device") && !v.empty() && !charset_ok(v, PATH))
        return reject("invalid characters in device path");
    if ((key == "tv_brand" || key == "tv_model" || key == "ir_remote" ||
         key == "acr_provider") && !v.empty() && !charset_ok(v, NAME))
        return reject("only letters, digits, . _ - allowed");

    // Masked secrets: if the client submits the placeholder unchanged (it sees
    // "********" in /config), treat it as "leave as-is" so the mask is never
    // written over the real secret.
    if ((key == "acr_secret" || key == "acr_key") && v == "********") {
        r["ok"] = true; r["key"] = key; r["restart_required"] = !def->live;
        r["persisted"] = true; return r.dump();
    }

    std::lock_guard<std::mutex> lk(cal_mtx_);
    if (key == "method") {
        if (lv != "auto" && lv != "api" && lv != "ir") return reject("auto | api | ir");
        cfg_.method = Config::method_from_string(lv);
    } else if (key == "ir_backend") {
        if (lv != "irsend" && lv != "ir-ctl" && lv != "dryrun") return reject("irsend | ir-ctl | dryrun");
        cfg_.ir_backend = lv;
    } else if (key == "audio_tap") {
        if (lv != "room" && lv != "upstream") return reject("room | upstream");
        cfg_.audio_tap = lv;
    } else if (key == "mute_mode") {
        if (lv != "mute" && lv != "volume_drop" && lv != "normalize") return reject("mute | volume_drop | normalize");
        if (lv == "mute" && to_lower(cfg_.audio_tap) != "upstream")
            return reject("mute needs audio_tap=upstream (a room mic can't hear when to unmute)");
        cfg_.mute_mode = lv;
    } else if (key == "drop_steps" || key == "max_mute_s" ||
               key == "norm_interval_ms" || key == "norm_max_range" ||
               key == "web_port") {
        if (!is_int(v)) return reject("whole number required");
        if (v.size() > 6) return reject("number out of range");   // no overflow
        int n = atoi(v.c_str());
        auto in_range = [&](int lo, int hi) { return n >= lo && n <= hi; };
        if (key == "web_port") {
            if (!in_range(1, 65535)) return reject("port must be 1-65535");
            cfg_.web_port = n;
        } else if (key == "drop_steps") {
            if (!in_range(1, 50)) return reject("volume drop steps must be 1-50");
            cfg_.drop_steps = n;
        } else if (key == "max_mute_s") {
            if (!in_range(0, 3600)) return reject("max mute must be 0-3600 s");
            cfg_.max_mute_s = n;
        } else if (key == "norm_interval_ms") {
            if (!in_range(200, 60000)) return reject("interval must be 200-60000 ms");
            cfg_.norm_interval_ms = n;
        } else {  // norm_max_range
            if (!in_range(1, 50)) return reject("max steps must be 1-50");
            cfg_.norm_max_range = n;
        }
    } else if (key == "norm_tolerance_db") {
        if (!is_num(v)) return reject("number required");
        double d = atof(v.c_str());
        if (d < 0.0 || d > 60.0) return reject("tolerance must be 0-60 dB");
        cfg_.norm_tolerance_db = d;
    } else if (key == "tv_brand") cfg_.tv_brand = lv;
    else if (key == "tv_model") cfg_.tv_model = lv;
    else if (key == "tv_ip") cfg_.tv_ip = v;
    else if (key == "ir_device") cfg_.ir_device = v;
    else if (key == "ir_remote") cfg_.ir_remote = v;
    else if (key == "audio_device") cfg_.audio_device = v;
    else if (key == "acr_provider") cfg_.acr_provider = lv;
    else if (key == "acr_host") cfg_.acr_host = v;
    else if (key == "acr_key") cfg_.acr_key = v;
    else if (key == "acr_secret") cfg_.acr_secret = v;

    bool saved = config_path_.empty() ? false : cfg_.save(config_path_);
    LOG_INFO("settings: %s = %s (%s)%s", key.c_str(),
             (key == "acr_secret" || key == "acr_key") ? "********" : v.c_str(),
             def->live ? "applied live" : "restart required",
             saved ? "" : " [NOT PERSISTED]");
    r["ok"] = true; r["key"] = key; r["restart_required"] = !def->live;
    r["persisted"] = saved;
    return r.dump();
}

void App::rebuild_controller() {
    std::lock_guard<std::mutex> lk(tv_mtx_);
    auto fresh = make_controller(cfg_, db_, credential_saver());
    if (fresh) { tv_ = std::move(fresh); tv_->connect(); }
}

std::string App::record_key_json(const std::string& key) {
    RecordResult rec = record_ir_key(key);
    json r;
    r["ok"] = rec.ok;
    if (rec.ok) {
        r["key"] = rec.key; r["pulses"] = rec.pulses; r["device"] = rec.device;
        // Apply immediately: the recorded code beats the DB from now on.
        rebuild_controller();
    } else {
        r["error"] = rec.error;
    }
    return r.dump();
}

std::string App::recorded_keys_json() {
    json j;
    j["keys"] = list_recorded_keys();
    return j.dump();
}

std::string App::info_json() {
    json j;
    j["version"] = ADMUFFS_VERSION;
    j["os"] = os_pretty_name();
    j["hardware"] = hardware_model();
    j["kernel"] = kernel_version();
    j["uptime"] = uptime_human();
    j["controller"] = tv_ ? tv_->name() : "none";
    j["mode"] = cfg_.mute_mode;
    j["automute"] = automute_.load();
    j["service"] = service_state();

    // I2C sensors (ANAVI pHAT slots). I2C is on GPIO2/3, fully independent
    // of the IR transceiver on GPIO17/18 -- reading them never disturbs IR.
    SensorReadings s = read_sensors();
    j["i2c"] = s.i2c_present;
    if (!s.i2c_present) j["i2c_error"] = s.i2c_error;
    if (s.htu21d_ok) j["htu21d"] = {{"temp_c", (int)(s.temp_c*10)/10.0},
                                    {"humidity_pct", (int)(s.humidity_pct*10)/10.0}};
    if (s.bh1750_ok) j["bh1750"] = {{"lux", (int)(s.lux*10)/10.0}};
    if (s.bmp180_ok) j["bmp180"] = {{"pressure_hpa", (int)(s.pressure_hpa*10)/10.0},
                                    {"temp_c", (int)(s.bmp_temp_c*10)/10.0}};
    return j.dump();
}

void App::set_automute(bool enabled) {
    bool was = automute_.exchange(enabled);
    if (was == enabled) return;
    LOG_INFO("auto-mute %s (via web remote)", enabled ? "ENABLED" : "DISABLED");
    if (!fusion_ || !tv_) return;
    if (to_lower(cfg_.mute_mode) == "normalize") {
        // Normalizer: disabling returns the TV to the user's set volume.
        if (!enabled) restore_norm("auto-mute disabled");
        return;
    }
    // Mute/volume-drop: keep the TV's state consistent when we're mid-commercial.
    if (!enabled && suppressing_) end_suppression("auto-mute disabled");
    else if (enabled && fusion_->in_commercial() && !suppressing_) begin_suppression();
}

AudioSampleResult App::sample_audio(bool commercial) {
    AudioSampleResult r;
    if (!audio_bus_ || !audio_bus_->running()) {
        r.error = "no audio capture (set audio_device / plug in the mic)";
        return r;
    }

    // Measure ~2 s of the most recent audio.
    const size_t want = (size_t)audio_bus_->rate() * 2;
    std::vector<int16_t> buf;
    if (audio_bus_->latest(want, buf) < want / 2) {
        r.error = "audio still warming up; try again in a few seconds";
        return r;
    }
    double sumsq = 0.0;
    for (int16_t s16 : buf) { double s = s16 / 32768.0; sumsq += s * s; }
    r.db = 20.0 * std::log10(std::sqrt(sumsq / (double)buf.size()) + 1e-9);

    std::lock_guard<std::mutex> lk(cal_mtx_);
    double& avg = commercial ? cfg_.cal_commercial_db : cfg_.cal_normal_db;
    int&    n   = commercial ? cfg_.cal_commercial_n  : cfg_.cal_normal_n;
    avg = (avg * n + r.db) / (n + 1);   // running average
    n += 1;
    r.ok = true;
    r.avg_db = avg;
    r.n = n;
    r.have_both = cfg_.cal_normal_n > 0 && cfg_.cal_commercial_n > 0;

    if (r.have_both) {
        // Commercials are compressed/louder; fire at 60% of the measured gap
        // above baseline, bounded to sane values. A negative or tiny gap
        // (unusual audio chain) falls back to the floor rather than disabling.
        double gap = cfg_.cal_commercial_db - cfg_.cal_normal_db;
        double derived = std::clamp(gap * 0.6, 1.5, 12.0);
        cfg_.loudness_delta_db = derived;
        if (loudness_) loudness_->set_delta_db(derived);
        r.derived_delta_db = derived;
        LOG_INFO("calibration: %s sample %.1f dB (avg %.1f x%d); gap %.1f dB "
                 "-> threshold %.1f dB",
                 commercial ? "commercial" : "normal", r.db, avg, n, gap, derived);
    } else {
        r.derived_delta_db = loudness_ ? loudness_->delta_db() : cfg_.loudness_delta_db;
        LOG_INFO("calibration: %s sample %.1f dB (avg %.1f x%d); waiting for "
                 "the other label", commercial ? "commercial" : "normal", r.db, avg, n);
    }

    if (!config_path_.empty() && !cfg_.save(config_path_))
        LOG_WARN("calibration: could not persist to %s", config_path_.c_str());
    return r;
}

int App::run(const std::atomic<bool>& stop) {
    // Web remote: mirrors the physical remote in a browser; every press goes
    // through the same controller (API primary, IR fallback) as the mute loop.
    if (cfg_.web_port > 0) {
        auth_ = std::make_unique<Auth>(cfg_, config_path_);
        WebHooks hooks;
        hooks.send = [this](TvCommand c) {
            std::lock_guard<std::mutex> lk(tv_mtx_);
            return tv_ && tv_->send(c);
        };
        hooks.status = [this]() { return tv_ ? tv_->name() : std::string("no controller"); };
        hooks.automute_get = [this]() { return automute_enabled(); };
        hooks.automute_set = [this](bool v) { set_automute(v); return automute_enabled(); };
        hooks.sample = [this](bool commercial) { return sample_audio(commercial); };
        hooks.norm_target = [this]() { return set_norm_target(); };
        hooks.config_get = [this]() { return config_json(); };
        hooks.config_set = [this](const std::string& k, const std::string& v) {
            return config_set(k, v);
        };
        hooks.record_key = [this](const std::string& k) { return record_key_json(k); };
        hooks.recorded_keys = [this]() { return recorded_keys_json(); };
        hooks.info = [this]() { return info_json(); };
        hooks.restart = [this]() { request_restart(); };
        hooks.auth_login = [this](const std::string& pin, int& retry) {
            return auth_->login(pin, retry);
        };
        hooks.auth_valid = [this](const std::string& tok) { return auth_->valid(tok); };
        hooks.auth_change = [this](const std::string& c, const std::string& n) {
            return auth_->change_pin(c, n);
        };
        hooks.auth_logout = [this](const std::string& tok) { auth_->logout(tok); };
        web_ = std::make_unique<WebServer>(cfg_.web_port, std::move(hooks));
        if (!web_->start()) web_.reset();
    }

    LOG_INFO("admuffs running. Ctrl-C to stop.");
    const bool normalize_mode = to_lower(cfg_.mute_mode) == "normalize";
    const bool muting_blinds_audio =
        room_tap() && !normalize_mode && to_lower(cfg_.mute_mode) != "volume_drop";
    if (muting_blinds_audio && audio_bus_)
        LOG_WARN("audio_tap=room with mute_mode=mute: a room mic can't hear "
                 "when to unmute, so unmuting relies solely on the max_mute_s "
                 "timer (%ds). The wizard no longer offers this combination -- "
                 "consider mute_mode=volume_drop or normalize instead.",
                 cfg_.max_mute_s);
    if (normalize_mode) {
        if (cfg_.norm_target_db < -1.0)
            LOG_INFO("normalize mode: leveling toward %.1f dB (±%.1f dB)",
                     cfg_.norm_target_db, cfg_.norm_tolerance_db);
        else
            LOG_WARN("normalize mode: no target set -- press FIX VOLUME TARGET "
                     "on the web remote while the TV is at the volume you like");
    }

    while (!stop.load() && !restart_requested_.load()) {
        std::vector<SourceVerdict> verdicts;
        for (auto& s : sources_) {
            SourceVerdict v = s->poll();

            // Mute paradox guard: with a ROOM tap in mute mode, a muted TV
            // means the mic hears the silence WE created -- audio sources'
            // "Program" verdicts are self-referential and must be discarded
            // or the loop would unmute into the ad it just muted. Upstream
            // taps and volume-drop mode (with compensation) keep hearing the
            // broadcast, so their verdicts stay valid.
            if (suppressing_ && muting_blinds_audio && s->hears_tv_audio() &&
                v.signal == Signal::Program) {
                v.signal = Signal::Unknown;
                v.confidence = 0.0;
            }

            // Fold the source's trust weight into the confidence the fusion sees.
            v.confidence = std::min(1.5, v.confidence * s->weight() / 0.4);
            verdicts.push_back(v);
        }

        // Fusion always runs (so state stays current), but only drives the TV
        // while auto-mute is enabled; the web-remote toggle gates the sends.
        FusionEvent ev = fusion_->update(verdicts);
        if (automute_.load() && normalize_mode) {
            // Normalize mode: no mute/volume-drop reaction -- the continuous leveler
            // below pulls loud ads down on its own. Events are informational.
            if (ev != FusionEvent::None)
                LOG_INFO("normalize mode: %s (leveling handles it)",
                         ev == FusionEvent::EnterCommercial ? "commercial detected"
                                                            : "program resumed");
            if (audio_bus_ && audio_bus_->running()) normalize_tick();
        } else if (automute_.load()) {
            switch (ev) {
                case FusionEvent::EnterCommercial:
                    if (!suppressing_) begin_suppression();
                    break;
                case FusionEvent::ExitCommercial:
                    if (suppressing_) end_suppression("program resumed");
                    break;
                case FusionEvent::None:
                    break;
            }
        } else if (ev != FusionEvent::None) {
            LOG_INFO("auto-mute disabled; ignoring %s",
                     ev == FusionEvent::EnterCommercial ? "commercial start" : "program resume");
        }

        // Failsafe: never stay suppressed past max_mute_s without a positive
        // program signal. Essential for room-tap mute mode (which has no
        // signal-driven unmute at all); a harmless upper bound otherwise.
        // After restoring, reset fusion so a still-running break can trigger
        // a fresh EnterCommercial (and a fresh window).
        if (suppressing_ && cfg_.max_mute_s > 0 &&
            now_ms() - suppress_since_ms_ >= (uint64_t)cfg_.max_mute_s * 1000) {
            LOG_WARN("suppression exceeded max_mute_s=%d; restoring audio", cfg_.max_mute_s);
            end_suppression("failsafe timer");
            fusion_->reset();
        }

        sleep_ms(200);
    }

    // Never leave the TV muted/dropped/shifted when admuffs exits.
    if (suppressing_) end_suppression("shutting down");
    if (normalize_mode) restore_norm("shutting down");

    if (web_) web_->stop();
    for (auto& s : sources_) s->stop();
    if (audio_bus_) audio_bus_->stop();
    if (restart_requested_.load()) {
        LOG_INFO("admuffs restarting (web request). Exit code 75; the systemd "
                 "unit (Restart=always) starts a fresh instance.");
        return 75;
    }
    LOG_INFO("admuffs stopped.");
    return 0;
}

bool App::test_mute() {
    if (!tv_) { tv_ = make_controller(cfg_, db_, credential_saver()); }
    if (!tv_) return false;
    tv_->connect();
    LOG_INFO("test: sending Mute via %s", tv_->name().c_str());
    bool a = tv_->send(TvCommand::Mute);
    sleep_ms(1500);
    LOG_INFO("test: sending Unmute");
    bool b = tv_->send(TvCommand::Unmute);
    return a && b;
}

}  // namespace admuffs
