// SPDX-License-Identifier: MIT
// api_controllers.h - LAN network-API TV controllers.
#pragma once

#include "tv/tv_controller.h"
#include "net/ws_client.h"

#include <functional>
#include <string>

namespace admuffs {

// Roku: External Control Protocol. Unauthenticated HTTP on :8060.
// VolumeMute is a toggle, so Mute/Unmute/Toggle all issue the same keypress.
class RokuController : public TvController {
public:
    explicit RokuController(std::string ip) : ip_(std::move(ip)) {}
    bool available() override;
    bool connect() override { return available(); }
    bool send(TvCommand cmd) override;
    std::string name() const override { return "Roku ECP (" + ip_ + ")"; }
    bool is_network() const override { return true; }
private:
    std::string ip_;
};

// Samsung Tizen: WebSocket remote. Older models accept plaintext ws://ip:8001;
// newer models require wss://ip:8002 and issue a pairing token on first
// connect (the TV shows an "allow" prompt). We try 8001 first when no token is
// stored, then fall back to wss:8002. A token received from the TV is surfaced
// through on_token_update so the caller can persist it.
class SamsungController : public TvController {
public:
    using TokenCallback = std::function<void(const std::string& token)>;

    SamsungController(std::string ip, std::string token,
                      TokenCallback on_token_update = nullptr)
        : ip_(std::move(ip)), token_(std::move(token)),
          on_token_update_(std::move(on_token_update)) {}
    bool available() override;
    bool connect() override;
    bool send(TvCommand cmd) override;
    std::string name() const override { return "Samsung Tizen (" + ip_ + ")"; }
    bool is_network() const override { return true; }
private:
    std::string ip_, token_;
    TokenCallback on_token_update_;
    WsClient ws_;
    bool connect_port(int port, bool tls, int greeting_timeout_ms);
    bool handle_greeting(const std::string& greeting);
    bool send_key(const std::string& key);
};

// LG webOS: SSAP over WebSocket on ws://ip:3000. Supports explicit mute state.
// Requires a client-key from a one-time pairing handshake.
class LgController : public TvController {
public:
    LgController(std::string ip, std::string client_key)
        : ip_(std::move(ip)), client_key_(std::move(client_key)) {}
    bool available() override;
    bool connect() override;
    bool send(TvCommand cmd) override;
    std::string name() const override { return "LG webOS (" + ip_ + ")"; }
    bool is_network() const override { return true; }
    // Runs the interactive pairing (TV shows a prompt); returns new client-key.
    std::string pair();
    // Current client-key (updated after a successful register/pairing).
    const std::string& client_key() const { return client_key_; }
private:
    std::string ip_, client_key_;
    WsClient ws_;
    int msg_id_ = 0;
    bool register_session();
    bool send_ssap(const std::string& uri, const std::string& payload_json);
};

// Sony Bravia: IRCC-IP (SOAP) + audio REST, authenticated by a pre-shared key.
class SonyController : public TvController {
public:
    SonyController(std::string ip, std::string psk)
        : ip_(std::move(ip)), psk_(std::move(psk)) {}
    bool available() override;
    bool connect() override { return available(); }
    bool send(TvCommand cmd) override;
    std::string name() const override { return "Sony Bravia (" + ip_ + ")"; }
    bool is_network() const override { return true; }
private:
    std::string ip_, psk_;
    bool send_ircc(const std::string& code);
    bool set_mute(bool on);
};

// Vizio SmartCast: HTTPS REST on :7345 (self-signed). Needs an auth token from
// PIN pairing (performed in the setup wizard).
class VizioController : public TvController {
public:
    VizioController(std::string ip, std::string token)
        : ip_(std::move(ip)), token_(std::move(token)) {}
    bool available() override;
    bool connect() override { return available(); }
    bool send(TvCommand cmd) override;
    std::string name() const override { return "Vizio SmartCast (" + ip_ + ")"; }
    bool is_network() const override { return true; }
private:
    std::string ip_, token_;
    bool key_command(int codeset, int code);
};

}  // namespace admuffs
