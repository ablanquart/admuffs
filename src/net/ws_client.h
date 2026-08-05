// SPDX-License-Identifier: MIT
// ws_client.h - minimal RFC6455 WebSocket client with optional TLS (wss://).
//
// Used for LAN control of Samsung Tizen (ws://ip:8001, or wss://ip:8002 with a
// token on newer models) and LG webOS (ws://ip:3000). TLS is provided by
// OpenSSL; certificate verification is intentionally disabled because TVs
// present self-signed certificates on the local network.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Forward-declare OpenSSL types to keep this header light.
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace admuffs {

class WsClient {
public:
    WsClient() = default;
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // Perform TCP connect (+ TLS handshake when use_tls) + HTTP/1.1 Upgrade.
    bool connect(const std::string& host, int port, const std::string& path,
                 const std::vector<std::string>& extra_headers = {},
                 int timeout_ms = 4000, bool use_tls = false);

    bool send_text(const std::string& payload);

    // Blocks up to timeout_ms for the next application (text/binary) message.
    // Ping frames are answered with pong transparently. Returns false on
    // timeout, close, or error; `out` receives the message on success.
    bool recv_text(std::string& out, int timeout_ms = 4000);

    void close();
    bool connected() const { return fd_ >= 0; }
    bool tls() const { return ssl_ != nullptr; }

private:
    int fd_ = -1;
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;

    // Transport-agnostic I/O (plain socket or TLS).
    bool io_send(const void* buf, size_t n);
    bool io_recv_n(void* buf, size_t n);

    bool tls_handshake(const std::string& host, int timeout_ms);
    bool send_frame(uint8_t opcode, const std::string& payload);
    void set_timeouts(int timeout_ms);
};

}  // namespace admuffs
