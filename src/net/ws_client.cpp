// SPDX-License-Identifier: MIT
#include "net/ws_client.h"
#include "common.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <cstring>
#include <cstdint>

namespace admuffs {

WsClient::~WsClient() { close(); }

void WsClient::set_timeouts(int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool WsClient::tls_handshake(const std::string& host, int timeout_ms) {
    (void)timeout_ms;  // socket-level timeouts already apply to the handshake
    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) { LOG_DEBUG("ws: SSL_CTX_new failed"); return false; }

    // TVs use self-signed certs on the LAN; do not verify the chain.
    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

    ssl_ = SSL_new(ssl_ctx_);
    if (!ssl_) { LOG_DEBUG("ws: SSL_new failed"); return false; }

    // SNI: some stacks (including Tizen's) expect a server_name extension.
    SSL_set_tlsext_host_name(ssl_, host.c_str());
    SSL_set_fd(ssl_, fd_);

    if (SSL_connect(ssl_) != 1) {
        unsigned long e = ERR_get_error();
        char buf[256] = {0};
        ERR_error_string_n(e, buf, sizeof(buf));
        LOG_DEBUG("ws: TLS handshake with %s failed: %s", host.c_str(), buf);
        return false;
    }
    LOG_DEBUG("ws: TLS established with %s (%s)", host.c_str(), SSL_get_version(ssl_));
    return true;
}

bool WsClient::io_send(const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < n) {
        ssize_t w;
        if (ssl_) w = SSL_write(ssl_, p + sent, (int)(n - sent));
        else      w = ::send(fd_, p + sent, n - sent, 0);
        if (w <= 0) return false;
        sent += (size_t)w;
    }
    return true;
}

bool WsClient::io_recv_n(void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r;
        if (ssl_) r = SSL_read(ssl_, p + got, (int)(n - got));
        else      r = ::recv(fd_, p + got, n - got, 0);
        if (r <= 0) return false;  // timeout, close, or error
        got += (size_t)r;
    }
    return true;
}

bool WsClient::connect(const std::string& host, int port, const std::string& path,
                       const std::vector<std::string>& extra_headers, int timeout_ms,
                       bool use_tls) {
    close();

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_s = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
        LOG_DEBUG("ws: getaddrinfo failed for %s", host.c_str());
        return false;
    }

    for (auto* p = res; p; p = p->ai_next) {
        fd_ = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd_ < 0) continue;
        set_timeouts(timeout_ms);
        if (::connect(fd_, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd_); fd_ = -1;
    }
    freeaddrinfo(res);
    if (fd_ < 0) { LOG_DEBUG("ws: connect failed to %s:%d", host.c_str(), port); return false; }

    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (use_tls && !tls_handshake(host, timeout_ms)) { close(); return false; }

    // Sec-WebSocket-Key must be a base64-encoded 16-byte nonce (RFC 6455).
    // A fresh random nonce per connection; strict servers/proxies reject a key
    // that doesn't decode to exactly 16 bytes. We don't validate the server's
    // Sec-WebSocket-Accept in return (harmless on a trusted LAN).
    unsigned char nonce[16];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1)
        for (size_t i = 0; i < sizeof(nonce); ++i) nonce[i] = (unsigned char)(0x5A ^ i);
    const std::string key = base64_encode(std::string((char*)nonce, sizeof(nonce)));
    std::string req;
    req += "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + port_s + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    for (const auto& h : extra_headers) req += h + "\r\n";
    req += "\r\n";

    if (!io_send(req.data(), req.size())) { close(); return false; }

    // Read response headers up to the blank line.
    std::string hdr;
    char c;
    while (hdr.find("\r\n\r\n") == std::string::npos) {
        if (!io_recv_n(&c, 1)) { LOG_DEBUG("ws: handshake read failed"); close(); return false; }
        hdr.push_back(c);
        if (hdr.size() > 8192) break;
    }
    if (hdr.find(" 101 ") == std::string::npos) {
        LOG_DEBUG("ws: server did not switch protocols: %.40s", hdr.c_str());
        close();
        return false;
    }
    return true;
}

bool WsClient::send_frame(uint8_t opcode, const std::string& payload) {
    if (fd_ < 0) return false;
    std::string frame;
    frame.push_back((char)(0x80 | opcode));  // FIN + opcode

    size_t len = payload.size();
    if (len < 126) {
        frame.push_back((char)(0x80 | len));  // MASK bit set
    } else if (len <= 0xFFFF) {
        frame.push_back((char)(0x80 | 126));
        frame.push_back((char)((len >> 8) & 0xFF));
        frame.push_back((char)(len & 0xFF));
    } else {
        frame.push_back((char)(0x80 | 127));
        for (int i = 7; i >= 0; --i) frame.push_back((char)((len >> (i * 8)) & 0xFF));
    }

    uint8_t mask[4] = {0x21, 0x84, 0x37, 0xA5};  // fixed mask is spec-legal
    for (int i = 0; i < 4; ++i) frame.push_back((char)mask[i]);
    for (size_t i = 0; i < len; ++i) frame.push_back((char)(payload[i] ^ mask[i & 3]));

    return io_send(frame.data(), frame.size());
}

bool WsClient::send_text(const std::string& payload) { return send_frame(0x1, payload); }

bool WsClient::recv_text(std::string& out, int timeout_ms) {
    if (fd_ < 0) return false;
    set_timeouts(timeout_ms);
    out.clear();

    for (;;) {
        uint8_t h[2];
        if (!io_recv_n(h, 2)) return false;
        bool fin = h[0] & 0x80;
        uint8_t opcode = h[0] & 0x0F;
        bool masked = h[1] & 0x80;
        uint64_t len = h[1] & 0x7F;

        if (len == 126) {
            uint8_t e[2]; if (!io_recv_n(e, 2)) return false;
            len = ((uint64_t)e[0] << 8) | e[1];
        } else if (len == 127) {
            uint8_t e[8]; if (!io_recv_n(e, 8)) return false;
            len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
        }

        uint8_t mkey[4] = {0, 0, 0, 0};
        if (masked && !io_recv_n(mkey, 4)) return false;

        std::string payload;
        payload.resize((size_t)len);
        if (len && !io_recv_n(&payload[0], (size_t)len)) return false;
        if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mkey[i & 3];

        switch (opcode) {
            case 0x9:  // ping -> pong
                send_frame(0xA, payload);
                continue;
            case 0xA:  // pong
                continue;
            case 0x8:  // close
                close();
                return false;
            case 0x0:  // continuation
            case 0x1:  // text
            case 0x2:  // binary
                out += payload;
                if (fin) return true;
                continue;
            default:
                continue;
        }
    }
}

void WsClient::close() {
    if (fd_ >= 0) send_frame(0x8, "");  // polite close frame; best-effort
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

}  // namespace admuffs
