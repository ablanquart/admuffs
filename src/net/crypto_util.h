// SPDX-License-Identifier: MIT
// crypto_util.h - tiny crypto abstraction so the rest of the code never talks
// to a crypto library directly.
//
// Backends, chosen at build time:
//   ADMUFFS_HAVE_OPENSSL=1  -> OpenSSL (Linux always; macOS when Homebrew's
//                              openssl@3 is installed)
//   ADMUFFS_HAVE_OPENSSL=0  -> Apple CommonCrypto + arc4random_buf(3)
//                              (macOS without Homebrew; TLS is unavailable in
//                              this configuration -- see ws_client.cpp)
#pragma once

#include <cstddef>
#include <string>

namespace admuffs {

// Fill buf with n cryptographically secure random bytes. Returns false only
// when no CSPRNG is available (never silently falls back to a weak RNG).
bool secure_random(unsigned char* buf, size_t n);

// SHA-256 of data into out[32].
void sha256_digest(const void* data, size_t n, unsigned char out[32]);

// Base64(HMAC-SHA1(key, data)) -- the signature format ACRCloud expects.
std::string hmac_sha1_b64(const std::string& key, const std::string& data);

}  // namespace admuffs
