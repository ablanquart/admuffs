// SPDX-License-Identifier: MIT
// web_server.h - embedded web remote + settings hub (default port 8995).
//
// Serves a self-contained HTML app and a small REST surface:
//   GET  /             remote UI (Power, D-pad+OK, Back, Home, Settings key,
//                      digits, Vol/Ch, Mute, Input) + ADMUFFS SETTINGS hub
//   POST /key/<name>   press a button via the active TvController (API
//                      primary, IR fallback). {"ok":bool}
//   GET  /status       controller name + auto-mute state
//   POST /automute     toggle automatic muting (manual keys keep working)
//   POST /sample/normal      loudness calibration: label current audio
//   POST /sample/commercial  as program / commercial
//   POST /sample/level       set the volume-normalization target
//   GET  /config       editable settings as JSON (TUI-equivalent set)
//   POST /config/set?k=<key>&v=<value>   change one setting; response says
//                      whether it applied live or needs a restart
//   POST /ir/record/<KEY_NAME>  wait ~9 s for a button press on the pHAT's
//                      IR receiver, save it as a user override, hot-apply
//   GET  /ir/recorded  KEY_* names that already have a recorded override
//   GET  /info         version, OS, hardware, uptime, service state, and
//                      live I2C sensor readings (HTU21D/BH1750/BMP180)
//   POST /restart      exit the process with code 75 (the systemd unit's
//                      Restart=always brings it back with fresh config)
//
// Single-threaded blocking HTTP/1.1, one request per connection, no TLS,
// binds 0.0.0.0. PIN-gated: all endpoints except GET / and POST /auth require a
// valid session cookie. LAN-trust model (plaintext HTTP) -- do not expose off
// the local network without adding TLS + auth in front.
#pragma once

#include "tv/tv_controller.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace admuffs {

// Result of one calibration sample (Sample buttons / volume target).
struct AudioSampleResult {
    bool ok = false;
    std::string error;
    double db = 0.0;
    double avg_db = 0.0;
    int n = 0;
    bool have_both = false;
    double derived_delta_db = 0.0;
};

// Everything the server needs from the app, in one bundle. JSON-returning
// hooks hand back a complete response body (the app owns the schema).
struct WebHooks {
    std::function<bool(TvCommand)> send;
    std::function<std::string()> status;
    std::function<bool()> automute_get;
    std::function<bool(bool)> automute_set;                 // returns new state
    std::function<AudioSampleResult(bool commercial)> sample;
    std::function<AudioSampleResult()> norm_target;
    std::function<std::string()> config_get;                // JSON
    std::function<std::string(const std::string& key,
                              const std::string& value)> config_set;  // JSON
    std::function<std::string(const std::string& key)> record_key;    // JSON
    std::function<std::string()> recorded_keys;             // JSON {"keys":[...]}
    std::function<std::string()> info;                      // JSON
    std::function<void()> restart;
    // Auth: login returns a session token ("" on failure; retry_after_s>0 when
    // locked out). valid checks a token. change returns "" on success else an
    // error. logout drops a token.
    std::function<std::string(const std::string& pin, int& retry_after_s)> auth_login;
    std::function<bool(const std::string& token)> auth_valid;
    std::function<std::string(const std::string& cur, const std::string& next)> auth_change;
    std::function<void(const std::string& token)> auth_logout;
};

class WebServer {
public:
    WebServer(int port, WebHooks hooks);
    ~WebServer();

    bool start();
    void stop();
    bool running() const { return running_.load(); }
    int port() const { return port_; }

    // "Power", "Up", "5", ... -> TvCommand. Exposed for tests.
    static bool key_from_name(const std::string& name, TvCommand& out);

private:
    void loop();
    void handle_client(int fd);

    int port_;
    WebHooks hooks_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace admuffs
