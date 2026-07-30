// SPDX-License-Identifier: MIT
// config.h - persistent application configuration (key=value file).
#pragma once

#include <string>
#include <map>

namespace admuffs {

enum class ControlMethod { Auto, Api, Ir };

struct Config {
    // --- TV identity ---
    std::string tv_brand;      // e.g. "samsung"
    std::string tv_model;      // e.g. "generic" or a specific model id from the IR DB

    // --- Control ---
    ControlMethod method = ControlMethod::Auto;  // auto = try API, fall back to IR
    std::string tv_ip;                            // LAN IP for API control

    // Per-brand API credentials (filled during pairing in the setup wizard).
    std::string samsung_token;    // Tizen websocket token (newer models)
    std::string lg_client_key;    // webOS SSAP client-key from pairing
    std::string sony_psk;         // Bravia pre-shared key
    std::string vizio_auth_token; // SmartCast auth token from PIN pairing

    // --- Infrared ---
    // Backend: "irsend" (LIRC), "ir-ctl" (kernel rc-core), or "dryrun".
    std::string ir_backend = "irsend";
    std::string ir_device  = "/dev/lirc0";  // used by ir-ctl
    std::string ir_remote;                   // LIRC remote name (overrides DB default)

    // --- Audio capture (loudness detection) ---
    // ALSA capture device. "auto" scans for a capture-capable card (USB mic),
    // or a concrete name like "plughw:1,0". "" disables audio detection.
    // Note: on Raspberry Pi, plain "default" usually can't capture (onboard
    // audio is playback-only); if it fails we fall back to a scan anyway.
    std::string audio_device = "auto";

    // Where the capture point listens:
    //   "room"     - a microphone hearing the TV's speakers. Muting the TV
    //                also silences detection (the mute paradox), so Program
    //                verdicts from audio sources are ignored while we mute,
    //                and unmute falls back to the max_mute_s timer.
    //   "upstream" - a tap BEFORE the TV (source line-out, HDMI audio
    //                extractor, often TV optical-out): detection keeps
    //                hearing the broadcast while the TV is silent, so unmute
    //                timing is signal-driven. Recommended (see HARDWARE.md).
    std::string audio_tap = "room";

    // What to do about commercials:
    //   "mute"      - mute the TV (silent ads). Best with audio_tap=upstream.
    //   "duck"      - lower the volume duck_steps steps instead; with a room
    //                 mic detection keeps hearing the (quieter) audio, and
    //                 the level shift is measured and compensated.
    //   "normalize" - don't react to detections at all; instead continuously
    //                 nudge the TV volume (VolumeUp/Down over IR or API) so
    //                 the room's smoothed level tracks norm_target_db. Loud
    //                 ads get pulled down automatically; quiet program comes
    //                 back up. Set the target with the web remote's
    //                 SET VOLUME TARGET button while the TV is at the volume
    //                 you like.
    // Default is "duck" because the default tap is a room mic, and duck is
    // the only muting mode a mic-only setup can time correctly.
    std::string mute_mode = "duck";
    int duck_steps = 6;

    // --- Volume normalization (mute_mode=normalize) ---
    double norm_target_db = 0.0;     // target room level (dBFS); 0 = unset --
                                     // real levels are negative, so the
                                     // normalizer idles until calibrated
    double norm_tolerance_db = 3.0;  // deadband before any adjustment
    int    norm_interval_ms = 2000;  // min time between volume nudges
    int    norm_max_range = 12;      // never drift more than this many steps
                                     // from the user's set volume

    // Failsafe: never stay muted/ducked longer than this without a positive
    // "program resumed" signal (guards a room-mic setup, wrong calibration,
    // or a dead feed). 0 disables.
    int max_mute_s = 240;
    int    audio_rate        = 16000;        // Hz
    double loudness_delta_db = 4.0;          // dB above rolling baseline => "louder"

    // --- Detection tuning ---
    int    mute_debounce_ms   = 1500;        // sustained commercial before muting
    int    unmute_debounce_ms = 2500;        // sustained program before unmuting
    double fire_threshold      = 0.6;        // fused confidence needed to act

    // --- Loudness calibration (filled by the web remote's Sample buttons) ---
    // Running averages of measured dBFS while normal programming / commercials
    // were airing. Once both exist, loudness_delta_db is derived from their
    // gap instead of the hand-tuned default (see App::sample_audio).
    double cal_normal_db = 0.0;
    int    cal_normal_n = 0;
    double cal_commercial_db = 0.0;
    int    cal_commercial_n = 0;

    // --- Optional external services ---
    // ACR (audio content recognition). Implemented provider: "acrcloud".
    // Create a project + an ads bucket at acrcloud.com, then fill these in.
    std::string acr_provider;    // "acrcloud" enables the source
    std::string acr_host;        // e.g. identify-us-west-2.acrcloud.com
    std::string acr_key;
    std::string acr_secret;
    int acr_interval_s = 20;     // how often to sample+identify
    int acr_sample_seconds = 8;  // audio window sent per identify call
    std::string metadata_provider;
    std::string metadata_url;
    std::string metadata_key;

    // --- Web remote ---
    // Port for the embedded browser remote served during --run.
    // 0 disables it. No auth: LAN-trust model, same as any IR remote.
    int web_port = 8990;

    // --- Misc ---
    // Blank = auto-search: ./data/ir_codes, /usr/local/share/admuffs/ir_codes,
    // /usr/share/admuffs/ir_codes (see resolve_ir_db_dir).
    std::string ir_db_dir;
    std::string log_file;
    int log_level = 1;  // 0=debug 1=info 2=warn 3=error

    // --- Web auth (never store the PIN itself; see web/auth.*) ---
    std::string pin_salt;   // hex, generated on first run
    std::string pin_hash;   // hex(sha256(salt|pin)); default PIN is "0000"

    // Load from file; returns false if the file does not exist (defaults kept).
    bool load(const std::string& path);
    // Persist to file (creates parent dir if needed). Returns success.
    bool save(const std::string& path) const;

    static std::string method_to_string(ControlMethod m);
    static ControlMethod method_from_string(const std::string& s);
};

// Default config path: $XDG_CONFIG_HOME/admuffs/admuffs.conf or ~/.config/admuffs/...
std::string default_config_path();

}  // namespace admuffs
