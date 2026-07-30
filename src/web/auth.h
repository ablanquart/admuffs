// SPDX-License-Identifier: MIT
// auth.h - PIN authentication + session management for the web remote.
//
// Security model (OWASP-aligned for a LAN appliance):
//   * PIN is never stored in plaintext -- only a salted SHA-256 hash lives in
//     the config (pin_salt + pin_hash). Default PIN is "0000".
//   * Verification uses a constant-time comparison.
//   * A correct PIN issues a 256-bit CSPRNG session token (from /dev/urandom),
//     returned as an HttpOnly, SameSite=Strict cookie with an expiry.
//   * Failed attempts are rate-limited with a lockout window (brute-force
//     mitigation) since a 4-digit PIN has only 10k combinations.
//   * All state is in-memory; tokens die on restart.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace admuffs {

struct Config;

class Auth {
public:
    // Loads the PIN hash from cfg (seeding the default "0000" if unset, which
    // also persists it). config_path may be empty (then changes aren't saved).
    Auth(Config& cfg, std::string config_path);

    // Verify a PIN; on success returns a fresh session token (non-empty) and
    // resets the failure counter. On failure returns "" and, past the
    // threshold, refuses even correct PINs until the lockout expires
    // (locked_out() reports remaining seconds via the out-param).
    std::string login(const std::string& pin, int& retry_after_s);

    // Is this session token valid (and not expired)? Refreshes idle expiry.
    bool valid(const std::string& token);

    void logout(const std::string& token);

    // Change the PIN. Requires the correct current PIN. new_pin must be 4-8
    // digits. Persists the new hash. Returns "" on success, else an error.
    std::string change_pin(const std::string& current, const std::string& new_pin);

private:
    std::string hash_pin(const std::string& pin) const;   // hex(sha256(salt|pin))
    void persist();

    Config& cfg_;
    std::string config_path_;
    std::mutex mtx_;

    std::map<std::string, uint64_t> sessions_;   // token -> expiry ms
    int fails_ = 0;
    uint64_t lock_until_ms_ = 0;
};

}  // namespace admuffs
