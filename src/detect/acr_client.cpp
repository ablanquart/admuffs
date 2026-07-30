// SPDX-License-Identifier: MIT
#include "detect/acr_client.h"
#include "config.h"
#include "common.h"
#include "net/http.h"

#include "json.hpp"

#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cstring>

using json = nlohmann::json;

namespace admuffs {

namespace {
void put_le32(std::string& s, uint32_t v) {
    s.push_back((char)(v & 0xFF)); s.push_back((char)((v >> 8) & 0xFF));
    s.push_back((char)((v >> 16) & 0xFF)); s.push_back((char)((v >> 24) & 0xFF));
}
void put_le16(std::string& s, uint16_t v) {
    s.push_back((char)(v & 0xFF)); s.push_back((char)((v >> 8) & 0xFF));
}
}  // namespace

std::string acr_build_wav(const std::vector<int16_t>& samples, int rate) {
    const uint32_t data_bytes = (uint32_t)(samples.size() * 2);
    std::string w;
    w.reserve(44 + data_bytes);
    w += "RIFF"; put_le32(w, 36 + data_bytes); w += "WAVE";
    w += "fmt "; put_le32(w, 16);
    put_le16(w, 1);                    // PCM
    put_le16(w, 1);                    // mono
    put_le32(w, (uint32_t)rate);
    put_le32(w, (uint32_t)rate * 2);   // byte rate
    put_le16(w, 2);                    // block align
    put_le16(w, 16);                   // bits per sample
    w += "data"; put_le32(w, data_bytes);
    w.append(reinterpret_cast<const char*>(samples.data()), data_bytes);
    return w;
}

std::string acr_sign(const std::string& string_to_sign, const std::string& secret) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha1(),
         secret.data(), (int)secret.size(),
         reinterpret_cast<const unsigned char*>(string_to_sign.data()),
         string_to_sign.size(), digest, &len);
    return base64_encode(std::string(reinterpret_cast<char*>(digest), len));
}

AcrOutcome acr_parse_response(const std::string& body) {
    AcrOutcome out;
    json j;
    try { j = json::parse(body); }
    catch (const std::exception& e) {
        out.error = std::string("bad JSON: ") + e.what();
        return out;
    }
    out.transport_ok = true;
    out.status_code = j.value("status", json::object()).value("code", -1);

    if (out.status_code == 1001) {
        // "No result": the sample matched nothing in the bucket. With an
        // ads-only bucket, that is weak evidence we're in program content.
        out.signal = Signal::Program;
        out.confidence = 0.25;
        return out;
    }
    if (out.status_code != 0) {
        out.error = j.value("status", json::object()).value("msg", "acr error");
        return out;
    }

    // status 0: something matched. A custom_files hit (the ads bucket) is a
    // positive commercial identification.
    const auto& meta = j.value("metadata", json::object());
    if (meta.contains("custom_files") && meta["custom_files"].is_array() &&
        !meta["custom_files"].empty()) {
        const auto& best = meta["custom_files"][0];
        double score = 0.0;
        if (best.contains("score")) {
            score = best["score"].is_number() ? best["score"].get<double>()
                                              : atof(best["score"].dump().c_str());
        }
        out.signal = Signal::Commercial;
        // Map ACRCloud's 0..100 score to a confident 0.5..1.0 band.
        out.confidence = std::clamp(0.5 + score / 200.0, 0.5, 1.0);
        out.matched_title = best.value("title", "");
        return out;
    }

    // A plain music-catalog match doesn't discriminate ad vs. program
    // (both use licensed music); stay neutral.
    out.signal = Signal::Unknown;
    out.confidence = 0.0;
    return out;
}

AcrOutcome acr_identify(const Config& cfg, const std::vector<int16_t>& samples,
                        int rate, uint64_t epoch_s) {
    AcrOutcome out;
    if (cfg.acr_host.empty() || cfg.acr_key.empty() || cfg.acr_secret.empty()) {
        out.error = "acr_host/acr_key/acr_secret not configured";
        return out;
    }

    // Decimate to 8 kHz when the capture rate divides evenly — smaller upload,
    // and ACRCloud's recommended rate. Otherwise ship the native rate.
    std::vector<int16_t> tx;
    int tx_rate = rate;
    if (rate > 8000 && rate % 8000 == 0) {
        int step = rate / 8000;
        tx.reserve(samples.size() / step + 1);
        for (size_t i = 0; i < samples.size(); i += (size_t)step) tx.push_back(samples[i]);
        tx_rate = 8000;
    } else {
        tx = samples;
    }
    std::string wav = acr_build_wav(tx, tx_rate);

    const std::string uri = "/v1/identify";
    const std::string data_type = "audio";
    const std::string sig_version = "1";
    const std::string ts = std::to_string(epoch_s);

    std::string string_to_sign = "POST\n" + uri + "\n" + cfg.acr_key + "\n" +
                                 data_type + "\n" + sig_version + "\n" + ts;
    std::string signature = acr_sign(string_to_sign, cfg.acr_secret);

    std::string base = cfg.acr_host;
    if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0)
        base = "https://" + base;

    auto resp = Http::post_multipart(
        base + uri,
        {{"access_key", cfg.acr_key},
         {"sample_bytes", std::to_string(wav.size())},
         {"timestamp", ts},
         {"signature", signature},
         {"data_type", data_type},
         {"signature_version", sig_version}},
        "sample", "sample.wav", wav, 10000);

    if (!resp.ok) { out.error = "http: " + resp.error; return out; }
    if (!resp.is2xx()) {
        out.error = "http status " + std::to_string(resp.status);
        return out;
    }
    return acr_parse_response(resp.body);
}

}  // namespace admuffs
