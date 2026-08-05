// SPDX-License-Identifier: MIT
// app.h - the run-time engine: detection loop -> TV control.
#pragma once

#include "config.h"
#include "ir/ir_database.h"
#include "tv/tv_controller.h"
#include "detect/audio_bus.h"
#include "detect/detector.h"
#include "detect/sources.h"
#include "web/web_server.h"
#include "web/auth.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace admuffs {

class App {
public:
    // config_path (optional) lets the app persist credentials the TV hands out
    // at runtime (e.g. the Samsung wss pairing token).
    explicit App(Config cfg, std::string config_path = "");

    // Build controller + detection sources. Returns false on fatal setup error.
    bool init();

    // Run the detection/mute loop until *stop becomes true.
    int run(const std::atomic<bool>& stop);

    // Automatic muting on/off (web-remote toggle). Detection keeps running
    // either way; this only gates whether fusion events drive the TV.
    // Toggling mid-commercial keeps the TV consistent: disable -> unmute,
    // enable -> re-mute.
    bool automute_enabled() const { return automute_.load(); }
    void set_automute(bool enabled);

    // Record a labeled loudness calibration sample from the live audio bus
    // (web remote "Sample - Normal" / "Sample - Commercial" buttons).
    // Updates the running average for the label, persists it to the config,
    // and -- once both labels exist -- derives the loudness trigger threshold
    // from the measured normal-vs-commercial dB gap and applies it to the
    // running detector immediately.
    AudioSampleResult sample_audio(bool commercial);

    // Sample the current room level and make it the volume-normalization
    // target (web remote FIX VOLUME TARGET button). Persisted to the config.
    AudioSampleResult set_norm_target();

    // --- web settings hub hooks (all return JSON strings for the UI) ---
    std::string config_json();                                   // GET /config
    std::string config_set(const std::string& key,
                           const std::string& value);            // POST /config/set
    std::string record_key_json(const std::string& key);         // POST /ir/record
    std::string recorded_keys_json();                            // GET /ir/recorded
    std::string info_json();                                     // GET /info
    void request_restart() { restart_requested_.store(true); }   // POST /restart
    bool restart_requested() const { return restart_requested_.load(); }

private:
    // --- volume normalization (mute_mode=normalize) ---
    // One control tick: nudge the volume one step toward norm_target_db when
    // the smoothed room level has drifted past the tolerance band.
    void normalize_tick();
    // Undo any net normalization steps (back to the user's set volume).
    void restore_norm(const char* why);
    // --- ad suppression (mute or volume drop) ---
    // begin_suppression/end_suppression wrap the configured mute_mode:
    //   mute: TvCommand::Mute / Unmute
    //   volume drop: VolumeDown x drop_steps / VolumeUp x drop_steps, and with a
    //         room tap the measured attenuation is fed to the loudness
    //         source as compensation so detection stays on-scale.
    void begin_suppression();
    void end_suppression(const char* why);
    bool room_tap() const;
    double measure_room_db(int seconds) const;   // NAN when unavailable

public:

    // One-shot helpers used by the CLI.
    bool test_mute();      // mute, wait, unmute — verifies TV control end-to-end.

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    std::string config_path_;
    IrDatabase db_;
    std::shared_ptr<AudioBus> audio_bus_;
    std::unique_ptr<WebServer> web_;
    std::unique_ptr<Auth> auth_;
    // Serializes controller access between the detection loop and the web
    // remote's request thread.
    std::mutex tv_mtx_;
    std::atomic<bool> automute_{true};
    std::atomic<bool> restart_requested_{false};
    // Rebuild the TV controller (after recording IR codes / credential change).
    void rebuild_controller();
    LoudnessSource* loudness_ = nullptr;  // borrowed from sources_ (calibration)
    std::mutex cal_mtx_;                  // guards cfg_ calibration fields + save

    // Suppression state (only the run loop + set_automute touch these).
    bool suppressing_ = false;            // we have the TV muted/dropped
    uint64_t suppress_since_ms_ = 0;

    // Normalizer state (run loop + set_automute + set_norm_target).
    double norm_ema_db_ = 0.0;            // smoothed room level
    bool norm_ema_init_ = false;
    uint64_t norm_last_nudge_ms_ = 0;
    int norm_net_steps_ = 0;              // >0 = currently below user volume

    CredentialSaveFn credential_saver();
    std::unique_ptr<TvController> tv_;
    std::vector<std::unique_ptr<DetectionSource>> sources_;
    std::unique_ptr<FusionEngine> fusion_;
};

}  // namespace admuffs
