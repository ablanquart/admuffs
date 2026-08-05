// SPDX-License-Identifier: MIT
#include "tv/api_controllers.h"
#include "net/http.h"
#include "common.h"

#include "json.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using json = nlohmann::json;

namespace admuffs {

namespace {
// Lightweight "is this TCP port open?" probe used by controllers whose APIs
// have no cheap unauthenticated GET.
bool tcp_can_connect(const std::string& host, int port, int timeout_ms) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return false;
    bool ok = false;
    for (auto* p = res; p; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) { ok = true; ::close(fd); break; }
        ::close(fd);
    }
    freeaddrinfo(res);
    return ok;
}
}  // namespace

const char* tv_command_name(TvCommand c) {
    switch (c) {
        case TvCommand::Mute:        return "Mute";
        case TvCommand::Unmute:      return "Unmute";
        case TvCommand::ToggleMute:  return "ToggleMute";
        case TvCommand::VolumeUp:    return "VolumeUp";
        case TvCommand::VolumeDown:  return "VolumeDown";
        case TvCommand::PowerToggle: return "PowerToggle";
        case TvCommand::Up:          return "Up";
        case TvCommand::Down:        return "Down";
        case TvCommand::Left:        return "Left";
        case TvCommand::Right:       return "Right";
        case TvCommand::Ok:          return "Ok";
        case TvCommand::Back:        return "Back";
        case TvCommand::Settings:    return "Settings";
        case TvCommand::Home:        return "Home";
        case TvCommand::ChannelUp:   return "ChannelUp";
        case TvCommand::ChannelDown: return "ChannelDown";
        case TvCommand::Input:       return "Input";
        case TvCommand::Digit0:      return "Digit0";
        case TvCommand::Digit1:      return "Digit1";
        case TvCommand::Digit2:      return "Digit2";
        case TvCommand::Digit3:      return "Digit3";
        case TvCommand::Digit4:      return "Digit4";
        case TvCommand::Digit5:      return "Digit5";
        case TvCommand::Digit6:      return "Digit6";
        case TvCommand::Digit7:      return "Digit7";
        case TvCommand::Digit8:      return "Digit8";
        case TvCommand::Digit9:      return "Digit9";
    }
    return "?";
}

// ------------------------------- Roku --------------------------------------
bool RokuController::available() {
    auto r = Http::get("http://" + ip_ + ":8060/query/device-info", 2500);
    return r.ok && r.is2xx();
}

bool RokuController::send(TvCommand cmd) {
    std::string key;
    switch (cmd) {
        case TvCommand::Mute:
        case TvCommand::Unmute:
        case TvCommand::ToggleMute:  key = "VolumeMute";  break;  // toggle
        case TvCommand::VolumeUp:    key = "VolumeUp";    break;
        case TvCommand::VolumeDown:  key = "VolumeDown";  break;
        case TvCommand::PowerToggle: key = "Power";       break;
        case TvCommand::Up:          key = "Up";          break;
        case TvCommand::Down:        key = "Down";        break;
        case TvCommand::Left:        key = "Left";        break;
        case TvCommand::Right:       key = "Right";       break;
        case TvCommand::Ok:          key = "Select";      break;
        case TvCommand::Back:        key = "Back";        break;
        case TvCommand::Home:        key = "Home";        break;
        case TvCommand::Settings:    key = "Info";        break;  // * options menu;
                                                                  // closest ECP has
        case TvCommand::ChannelUp:   key = "ChannelUp";   break;  // Roku TVs
        case TvCommand::ChannelDown: key = "ChannelDown"; break;
        case TvCommand::Input:
            // ECP has no input *cycle*, only absolute InputHDMI1 etc.; let the
            // composite fall back to IR for this one.
            return false;
        default:
            if (tv_command_is_digit(cmd)) {
                // Best effort: ECP digits exist only as keyboard literals.
                key = "Lit_" + std::to_string(tv_command_digit(cmd));
                break;
            }
            return false;
    }
    auto r = Http::post("http://" + ip_ + ":8060/keypress/" + key, "", {}, 2500);
    return r.ok && r.is2xx();
}

// ------------------------------ Samsung ------------------------------------
bool SamsungController::available() {
    auto r = Http::get("http://" + ip_ + ":8001/api/v2/", 2500);
    return r.ok && r.is2xx();
}

bool SamsungController::handle_greeting(const std::string& greeting) {
    // The TV's first message is an ms.channel.connect event. On wss:8002 with
    // a fresh pairing, its payload carries the token we must persist and reuse.
    if (greeting.empty()) return true;  // some firmwares stay silent; carry on
    try {
        json j = json::parse(greeting);
        std::string event = j.value("event", "");
        if (event == "ms.channel.unauthorized") {
            LOG_WARN("Samsung: TV denied the connection (pairing prompt declined?)");
            return false;
        }
        if (j.contains("data") && j["data"].contains("token")) {
            std::string tok = j["data"]["token"].is_string()
                ? j["data"]["token"].get<std::string>()
                : j["data"]["token"].dump();
            if (!tok.empty() && tok != token_) {
                LOG_INFO("Samsung: received new pairing token");
                token_ = tok;
                if (on_token_update_) on_token_update_(token_);
            }
        }
    } catch (...) { /* non-JSON greeting: ignore */ }
    return true;
}

bool SamsungController::connect_port(int port, bool tls, int greeting_timeout_ms) {
    std::string name_b64 = base64_encode("admuffs");
    std::string path = "/api/v2/channels/samsung.remote.control?name=" + name_b64;
    if (!token_.empty()) path += "&token=" + token_;
    if (!ws_.connect(ip_, port, path, {}, 4000, tls)) return false;
    std::string greeting;
    ws_.recv_text(greeting, greeting_timeout_ms);
    if (!handle_greeting(greeting)) { ws_.close(); return false; }
    return true;
}

bool SamsungController::connect() {
    if (ws_.connected()) return true;

    // With a stored token, go straight to the secure channel it belongs to.
    if (!token_.empty()) {
        if (connect_port(8002, /*tls=*/true, 3000)) return true;
        LOG_WARN("Samsung: wss:8002 with stored token failed; retrying plaintext");
    }

    // No token: legacy models accept plaintext :8001.
    if (connect_port(8001, /*tls=*/false, 3000)) return true;

    // Newer models refuse :8001 -> pair over wss:8002. The TV shows an "allow"
    // prompt; give the user time to accept it (the greeting arrives after).
    LOG_INFO("Samsung: trying wss:8002 (accept the prompt on the TV if shown)");
    if (connect_port(8002, /*tls=*/true, 30000)) return true;

    LOG_WARN("Samsung: websocket connect failed on both :8001 and wss:8002");
    return false;
}

bool SamsungController::send_key(const std::string& key) {
    if (!connect()) return false;
    json j = {
        {"method", "ms.remote.control"},
        {"params", {
            {"Cmd", "Click"},
            {"DataOfCmd", key},
            {"Option", "false"},
            {"TypeOfRemote", "SendRemoteKey"}
        }}
    };
    return ws_.send_text(j.dump());
}

bool SamsungController::send(TvCommand cmd) {
    switch (cmd) {
        case TvCommand::Mute:
        case TvCommand::Unmute:
        case TvCommand::ToggleMute:  return send_key("KEY_MUTE");   // toggle
        case TvCommand::VolumeUp:    return send_key("KEY_VOLUP");
        case TvCommand::VolumeDown:  return send_key("KEY_VOLDOWN");
        case TvCommand::PowerToggle: return send_key("KEY_POWER");
        case TvCommand::Up:          return send_key("KEY_UP");
        case TvCommand::Down:        return send_key("KEY_DOWN");
        case TvCommand::Left:        return send_key("KEY_LEFT");
        case TvCommand::Right:       return send_key("KEY_RIGHT");
        case TvCommand::Ok:          return send_key("KEY_ENTER");
        case TvCommand::Back:        return send_key("KEY_RETURN");
        case TvCommand::Settings:    return send_key("KEY_MENU");
        case TvCommand::Home:        return send_key("KEY_HOME");
        case TvCommand::ChannelUp:   return send_key("KEY_CHUP");
        case TvCommand::ChannelDown: return send_key("KEY_CHDOWN");
        case TvCommand::Input:       return send_key("KEY_SOURCE");
        default:
            if (tv_command_is_digit(cmd))
                return send_key("KEY_" + std::to_string(tv_command_digit(cmd)));
            return false;
    }
}

// -------------------------------- LG ---------------------------------------
// webOS listens on 3001 (secure) and/or 3000 (plaintext); either means "here".
bool LgController::available() {
    return tcp_can_connect(ip_, 3001, 2000) || tcp_can_connect(ip_, 3000, 2000);
}

// Pairing prompt can be slow to render if the TV was in standby; wait
// generously (deadline-based) rather than a short fixed window.
static const uint64_t kPairDeadlineMs = 300000;   // 5 minutes

bool LgController::register_session() {
    // webOS SSAP registration handshake. If client_key_ is empty the TV shows
    // an on-screen pairing prompt and returns a client-key we should persist.
    json manifest = {
        {"manifestVersion", 1},
        {"permissions", json::array({
            "CONTROL_AUDIO", "CONTROL_POWER", "READ_TV_CURRENT_CHANNEL",
            "CONTROL_INPUT_MEDIA_PLAYBACK", "CONTROL_INPUT_TV"})}
    };
    json payload = {
        {"forcePairing", false},
        {"pairingType", "PROMPT"},
        {"manifest", manifest}
    };
    if (!client_key_.empty()) payload["client-key"] = client_key_;

    json reg = {{"type", "register"}, {"id", "register_0"}, {"payload", payload}};
    uint64_t t0 = now_ms();
    if (!ws_.send_text(reg.dump())) return false;
    LOG_INFO("LG: register request sent; accept the prompt ON THE TV SCREEN. "
             "(If the TV was in standby the prompt can take a while to render "
             "as webOS wakes.) Waiting up to %ds...", (int)(kPairDeadlineMs / 1000));

    // Wait on a DEADLINE, not a fixed read count: the prompt can take minutes
    // when routed to a phone via LG's cloud push. Keep reading intermediate
    // frames (the TV sends a "response" ack first) until "registered" arrives
    // or we time out. Timing is logged so the delay's source is visible.
    bool acked = false;
    uint64_t deadline = t0 + kPairDeadlineMs;
    while (now_ms() < deadline) {
        std::string resp;
        if (!ws_.recv_text(resp, 20000)) continue;   // idle read; keep waiting
        try {
            json j = json::parse(resp);
            std::string type = j.value("type", "");
            if (type == "response" && !acked) {
                acked = true;
                LOG_INFO("LG: TV acknowledged the request after %.1fs -- now "
                         "waiting for you to ACCEPT the prompt",
                         (now_ms() - t0) / 1000.0);
            } else if (type == "error") {
                LOG_WARN("LG: registration error: %s",
                         j.value("error", "unknown").c_str());
                return false;
            } else if (type == "registered") {
                if (j.contains("payload") && j["payload"].contains("client-key"))
                    client_key_ = j["payload"]["client-key"].get<std::string>();
                LOG_INFO("LG: paired and accepted after %.1fs total",
                         (now_ms() - t0) / 1000.0);
                return true;
            }
        } catch (...) { /* ignore non-JSON keepalives */ }
    }
    LOG_WARN("LG: pairing not accepted within %ds. Make sure the TV is fully "
             "ON (not standby) and accept the on-screen prompt, then retry.",
             (int)(kPairDeadlineMs / 1000));
    return false;
}

bool LgController::connect() {
    if (ws_.connected()) return true;
    // Prefer the secure SSAP socket (wss://:3001): on webOS 4.x+ (2018+) the
    // service is primarily bound there, and the plaintext :3000 path is often
    // slow-pathed -- which can delay the on-screen pairing prompt by minutes.
    // Fall back to ws://:3000 for older sets.
    if (ws_.connect(ip_, 3001, "/", {}, 4000, /*tls=*/true)) {
        LOG_INFO("LG: connected over wss:3001 (secure)");
        return register_session();
    }
    LOG_INFO("LG: wss:3001 unavailable; falling back to ws:3000");
    if (!ws_.connect(ip_, 3000, "/")) return false;
    return register_session();
}

std::string LgController::pair() {
    client_key_.clear();
    if (connect()) return client_key_;
    return "";
}

bool LgController::send_ssap(const std::string& uri, const std::string& payload_json) {
    if (!connect()) return false;
    json j = {
        {"id", "req_" + std::to_string(++msg_id_)},
        {"type", "request"},
        {"uri", uri}
    };
    if (!payload_json.empty()) j["payload"] = json::parse(payload_json);
    return ws_.send_text(j.dump());
}

bool LgController::send(TvCommand cmd) {
    switch (cmd) {
        case TvCommand::Mute:        return send_ssap("ssap://audio/setMute", R"({"mute":true})");
        case TvCommand::Unmute:      return send_ssap("ssap://audio/setMute", R"({"mute":false})");
        case TvCommand::ToggleMute:  return send_ssap("ssap://audio/setMute", R"({"mute":true})");
        case TvCommand::VolumeUp:    return send_ssap("ssap://audio/volumeUp", "");
        case TvCommand::VolumeDown:  return send_ssap("ssap://audio/volumeDown", "");
        case TvCommand::PowerToggle: return send_ssap("ssap://system/turnOff", "");
        case TvCommand::ChannelUp:   return send_ssap("ssap://tv/channelUp", "");
        case TvCommand::ChannelDown: return send_ssap("ssap://tv/channelDown", "");
        case TvCommand::Input:
            // No generic input-cycle over SSAP; launching the input picker is
            // the closest equivalent.
            return send_ssap("ssap://system.launcher/launch",
                             R"({"id":"com.webos.app.inputpicker"})");
        case TvCommand::Settings:
            // Open the system settings app (webOS has no bare "menu" key).
            return send_ssap("ssap://system.launcher/launch",
                             R"({"id":"com.palm.app.settings"})");
        case TvCommand::Home:
            return send_ssap("ssap://system.launcher/launch",
                             R"({"id":"com.webos.app.home"})");
        // D-pad, Back, and digits need webOS's separate pointer-input socket,
        // which this client doesn't implement; return false so IR covers them.
        case TvCommand::Up: case TvCommand::Down:
        case TvCommand::Left: case TvCommand::Right:
        case TvCommand::Ok: case TvCommand::Back:
        default:
            return false;
    }
}

// ------------------------------- Sony --------------------------------------
bool SonyController::available() {
    std::string body = R"({"method":"getPowerStatus","id":1,"params":[],"version":"1.0"})";
    std::vector<std::string> h = {"Content-Type: application/json"};
    if (!psk_.empty()) h.push_back("X-Auth-PSK: " + psk_);
    auto r = Http::post("http://" + ip_ + "/sony/system", body, h, 2500);
    return r.ok && r.is2xx();
}

bool SonyController::send_ircc(const std::string& code) {
    std::string soap =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:X_SendIRCC xmlns:u=\"urn:schemas-sony-com:service:IRCC:1\">"
        "<IRCCCode>" + code + "</IRCCCode></u:X_SendIRCC></s:Body></s:Envelope>";
    std::vector<std::string> h = {
        "Content-Type: text/xml; charset=UTF-8",
        "SOAPACTION: \"urn:schemas-sony-com:service:IRCC:1#X_SendIRCC\""
    };
    if (!psk_.empty()) h.push_back("X-Auth-PSK: " + psk_);
    auto r = Http::post("http://" + ip_ + "/sony/IRCC", soap, h, 2500);
    return r.ok && r.is2xx();
}

bool SonyController::set_mute(bool on) {
    json body = {
        {"method", "setAudioMute"}, {"id", 601}, {"version", "1.0"},
        {"params", json::array({ json{{"status", on}} })}
    };
    std::vector<std::string> h = {"Content-Type: application/json"};
    if (!psk_.empty()) h.push_back("X-Auth-PSK: " + psk_);
    auto r = Http::post("http://" + ip_ + "/sony/audio", body.dump(), h, 2500);
    if (r.ok && r.is2xx()) return true;
    // Fall back to the IRCC mute toggle if the audio service refuses.
    return send_ircc("AAAAAQAAAAEAAAAUAw==");
}

bool SonyController::send(TvCommand cmd) {
    // IRCC codes are Sony's published "IRCC-IP" command table (base64).
    static const char* kDigits[10] = {
        "AAAAAQAAAAEAAAAJAw==",  // 0
        "AAAAAQAAAAEAAAAAAw==",  // 1
        "AAAAAQAAAAEAAAABAw==",  // 2
        "AAAAAQAAAAEAAAACAw==",  // 3
        "AAAAAQAAAAEAAAADAw==",  // 4
        "AAAAAQAAAAEAAAAEAw==",  // 5
        "AAAAAQAAAAEAAAAFAw==",  // 6
        "AAAAAQAAAAEAAAAGAw==",  // 7
        "AAAAAQAAAAEAAAAHAw==",  // 8
        "AAAAAQAAAAEAAAAIAw==",  // 9
    };
    switch (cmd) {
        case TvCommand::Mute:        return set_mute(true);
        case TvCommand::Unmute:      return set_mute(false);
        case TvCommand::ToggleMute:  return send_ircc("AAAAAQAAAAEAAAAUAw==");
        case TvCommand::VolumeUp:    return send_ircc("AAAAAQAAAAEAAAASAw==");
        case TvCommand::VolumeDown:  return send_ircc("AAAAAQAAAAEAAAATAw==");
        case TvCommand::PowerToggle: return send_ircc("AAAAAQAAAAEAAAAVAw==");
        case TvCommand::Up:          return send_ircc("AAAAAQAAAAEAAAB0Aw==");
        case TvCommand::Down:        return send_ircc("AAAAAQAAAAEAAAB1Aw==");
        case TvCommand::Left:        return send_ircc("AAAAAQAAAAEAAAA0Aw==");
        case TvCommand::Right:       return send_ircc("AAAAAQAAAAEAAAAzAw==");
        case TvCommand::Ok:          return send_ircc("AAAAAQAAAAEAAABlAw==");
        case TvCommand::Back:        return send_ircc("AAAAAgAAAJcAAAAjAw==");  // Return
        case TvCommand::Settings:    return send_ircc("AAAAAgAAAMQAAABLAw==");  // ActionMenu
        case TvCommand::Home:        return send_ircc("AAAAAQAAAAEAAABgAw==");
        case TvCommand::ChannelUp:   return send_ircc("AAAAAQAAAAEAAAAQAw==");
        case TvCommand::ChannelDown: return send_ircc("AAAAAQAAAAEAAAARAw==");
        case TvCommand::Input:       return send_ircc("AAAAAQAAAAEAAAAlAw==");
        default:
            if (tv_command_is_digit(cmd)) return send_ircc(kDigits[tv_command_digit(cmd)]);
            return false;
    }
}

// ------------------------------- Vizio -------------------------------------
bool VizioController::available() { return tcp_can_connect(ip_, 7345, 2000); }

bool VizioController::key_command(int codeset, int code) {
    json body = {{"KEYLIST", json::array({
        json{{"CODESET", codeset}, {"CODE", code}, {"ACTION", "KEYPRESS"}}})}};
    HttpRequest r;
    r.method = "PUT";
    r.url = "https://" + ip_ + ":7345/key_command/";
    r.body = body.dump();
    r.insecure = true;  // SmartCast uses a self-signed certificate
    r.headers = {"Content-Type: application/json"};
    if (!token_.empty()) r.headers.push_back("AUTH: " + token_);
    auto resp = Http::request(r);
    return resp.ok && resp.is2xx();
}

bool VizioController::send(TvCommand cmd) {
    // Codesets per the community SmartCast API docs: Volume=5 (up 1, down 0,
    // mute on 3 / off 2 / toggle 4), Channel=8 (up 1, down 0), Power=11
    // (toggle 2), Input cycle=7/1, D-pad=3 (community-mapped codes; models
    // vary -- a false return lets IR cover it).
    switch (cmd) {
        case TvCommand::Mute:        return key_command(5, 3);
        case TvCommand::Unmute:      return key_command(5, 2);
        case TvCommand::ToggleMute:  return key_command(5, 4);
        case TvCommand::VolumeUp:    return key_command(5, 1);
        case TvCommand::VolumeDown:  return key_command(5, 0);
        case TvCommand::PowerToggle: return key_command(11, 2);
        case TvCommand::ChannelUp:   return key_command(8, 1);
        case TvCommand::ChannelDown: return key_command(8, 0);
        case TvCommand::Input:       return key_command(7, 1);
        case TvCommand::Up:          return key_command(3, 8);
        case TvCommand::Down:        return key_command(3, 0);
        case TvCommand::Left:        return key_command(3, 1);
        case TvCommand::Right:       return key_command(3, 7);
        case TvCommand::Ok:          return key_command(3, 2);
        // Back/Settings/Home codeset values aren't consistently documented
        // for SmartCast; let the composite fall back to IR for these.
        case TvCommand::Back:
        case TvCommand::Settings:
        case TvCommand::Home:
            return false;
        default:
            if (tv_command_is_digit(cmd))
                return key_command(0, 48 + tv_command_digit(cmd));  // ASCII codeset
            return false;
    }
}

}  // namespace admuffs
