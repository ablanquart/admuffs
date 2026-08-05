// SPDX-License-Identifier: MIT
#include "net/crypto_util.h"
#include "common.h"

#if ADMUFFS_HAVE_OPENSSL
// ---------------------------------------------------------------- OpenSSL --
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace admuffs {

bool secure_random(unsigned char* buf, size_t n) {
    return RAND_bytes(buf, (int)n) == 1;
}

void sha256_digest(const void* data, size_t n, unsigned char out[32]) {
    SHA256(static_cast<const unsigned char*>(data), n, out);
}

std::string hmac_sha1_b64(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha1(), key.data(), (int)key.size(),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &len);
    return base64_encode(std::string(reinterpret_cast<char*>(digest), len));
}

}  // namespace admuffs

#elif defined(__APPLE__)
// ----------------------------------------- CommonCrypto (no-Homebrew macOS) --
// CommonCrypto ships with every macOS SDK; arc4random_buf(3) is the system
// CSPRNG (kernel-seeded, never fails). No extra frameworks to link.
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>

#include <cstdlib>

namespace admuffs {

bool secure_random(unsigned char* buf, size_t n) {
    arc4random_buf(buf, n);
    return true;
}

void sha256_digest(const void* data, size_t n, unsigned char out[32]) {
    CC_SHA256(data, (CC_LONG)n, out);
}

std::string hmac_sha1_b64(const std::string& key, const std::string& data) {
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA1, key.data(), key.size(), data.data(), data.size(),
           digest);
    return base64_encode(std::string(reinterpret_cast<char*>(digest),
                                     sizeof(digest)));
}

}  // namespace admuffs

#else
#error "admuffs requires OpenSSL on this platform (only macOS has a CommonCrypto fallback)"
#endif
