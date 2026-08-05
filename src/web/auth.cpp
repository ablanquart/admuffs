// SPDX-License-Identifier: MIT
#include "web/auth.h"
#include "config.h"
#include "common.h"
#include "net/crypto_util.h"

#include <cstring>

namespace admuffs {

namespace {
constexpr uint64_t kSessionTtlMs = 12ULL * 60 * 60 * 1000;  // 12 h idle
constexpr int kMaxFails = 5;
constexpr uint64_t kLockoutMs = 60 * 1000;                  // 60 s after 5 fails

std::string to_hex(const unsigned char* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += h[p[i] >> 4]; s += h[p[i] & 0xF]; }
    return s;
}

// CSPRNG hex string of `bytes` bytes; empty on failure (never falls back to a
// weak RNG).
std::string random_hex(size_t bytes) {
    std::string buf(bytes, '\0');
    if (!secure_random(reinterpret_cast<unsigned char*>(&buf[0]), bytes))
        return "";
    return to_hex(reinterpret_cast<const unsigned char*>(buf.data()), bytes);
}

// Constant-time string compare (no early return on first mismatch).
bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}
}  // namespace

Auth::Auth(Config& cfg, std::string config_path)
    : cfg_(cfg), config_path_(std::move(config_path)) {
    if (cfg_.pin_salt.empty()) {
        cfg_.pin_salt = random_hex(16);
        if (cfg_.pin_salt.empty()) cfg_.pin_salt = "admuffs-fallback-salt";  // RAND fail
    }
    if (cfg_.pin_hash.empty()) {
        cfg_.pin_hash = hash_pin("0000");   // default PIN
        persist();
        LOG_INFO("web auth: PIN initialized to default 0000 -- change it under "
                 "ADMUFFS SETTINGS before exposing this on your network");
    }
}

std::string Auth::hash_pin(const std::string& pin) const {
    std::string salted = cfg_.pin_salt + ":" + pin;
    unsigned char md[32];
    sha256_digest(salted.data(), salted.size(), md);
    return to_hex(md, sizeof(md));
}

void Auth::persist() {
    if (!config_path_.empty() && !cfg_.save(config_path_))
        LOG_WARN("web auth: could not persist PIN to %s", config_path_.c_str());
}

std::string Auth::login(const std::string& pin, int& retry_after_s) {
    std::lock_guard<std::mutex> lk(mtx_);
    retry_after_s = 0;
    uint64_t now = now_ms();

    if (lock_until_ms_ > now) {
        retry_after_s = (int)((lock_until_ms_ - now + 999) / 1000);
        LOG_WARN("web auth: login blocked (locked out %ds)", retry_after_s);
        return "";
    }

    if (ct_equal(hash_pin(pin), cfg_.pin_hash)) {
        fails_ = 0;
        // Opportunistically reap expired sessions so tokens that are never
        // revisited (closed tab, phone slept) can't accumulate unbounded.
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            if (it->second < now) it = sessions_.erase(it); else ++it;
        }
        std::string token = random_hex(32);   // 256-bit
        if (token.empty()) { LOG_ERROR("web auth: CSPRNG failed"); return ""; }
        sessions_[token] = now + kSessionTtlMs;
        LOG_INFO("web auth: successful login (%zu active session(s))", sessions_.size());
        return token;
    }

    if (++fails_ >= kMaxFails) {
        lock_until_ms_ = now + kLockoutMs;
        fails_ = 0;
        retry_after_s = (int)(kLockoutMs / 1000);
        LOG_WARN("web auth: too many failed PINs -- locked out %ds", retry_after_s);
    } else {
        LOG_WARN("web auth: incorrect PIN (%d/%d before lockout)", fails_, kMaxFails);
    }
    return "";
}

bool Auth::valid(const std::string& token) {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return false;
    uint64_t now = now_ms();
    if (it->second < now) { sessions_.erase(it); return false; }
    it->second = now + kSessionTtlMs;   // sliding idle expiry
    return true;
}

void Auth::logout(const std::string& token) {
    std::lock_guard<std::mutex> lk(mtx_);
    sessions_.erase(token);
}

std::string Auth::change_pin(const std::string& current, const std::string& new_pin) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!ct_equal(hash_pin(current), cfg_.pin_hash)) return "current PIN is incorrect";
    if (new_pin.size() < 4 || new_pin.size() > 8 ||
        new_pin.find_first_not_of("0123456789") != std::string::npos)
        return "new PIN must be 4-8 digits";
    cfg_.pin_hash = hash_pin(new_pin);
    persist();
    sessions_.clear();   // force re-login everywhere after a PIN change
    LOG_INFO("web auth: PIN changed; all sessions invalidated");
    return "";
}

}  // namespace admuffs
