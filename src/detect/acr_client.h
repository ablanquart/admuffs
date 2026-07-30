// SPDX-License-Identifier: MIT
// acr_client.h - ACRCloud identification pipeline.
//
// Flow: take a mono S16 sample window -> wrap it in a WAV container -> sign
// the request (HMAC-SHA1, ACRCloud signature v1) -> multipart POST to
// /v1/identify -> map the JSON verdict onto our Signal domain.
//
// The intended setup is an ACRCloud project with a *custom bucket* of ad
// audio ("Audio & Video Recognition" with custom files): a custom_files match
// means "this is a known commercial". See docs/ARCHITECTURE.md.
//
// Free functions so the pipeline is unit-testable without ALSA or a real
// subscription (tests point acr_host at a local mock server).
#pragma once

#include "detect/detector.h"

#include <cstdint>
#include <string>
#include <vector>

namespace admuffs {

struct Config;

struct AcrOutcome {
    bool transport_ok = false;   // request reached a server and parsed
    Signal signal = Signal::Unknown;
    double confidence = 0.0;
    int status_code = -1;        // ACRCloud status.code (0 match, 1001 no result)
    std::string matched_title;   // best custom_files match, if any
    std::string error;
};

// Mono S16LE samples -> complete WAV file bytes.
std::string acr_build_wav(const std::vector<int16_t>& samples, int rate);

// base64(HMAC-SHA1(string_to_sign, secret)) per ACRCloud signature v1.
std::string acr_sign(const std::string& string_to_sign, const std::string& secret);

// Full identify round-trip. `epoch_s` is the Unix timestamp used in signing
// (parameterized for testability). Decimates to 8 kHz when rate allows.
AcrOutcome acr_identify(const Config& cfg, const std::vector<int16_t>& samples,
                        int rate, uint64_t epoch_s);

// Response-mapping alone (exposed for tests).
AcrOutcome acr_parse_response(const std::string& json_body);

}  // namespace admuffs
