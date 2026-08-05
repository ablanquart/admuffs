// SPDX-License-Identifier: MIT
#include "detect/sources.h"
#include "detect/acr_client.h"
#include "common.h"

#include <cmath>
#include <ctime>
#include <vector>

namespace admuffs {

// ------------------------------ LoudnessSource -----------------------------
LoudnessSource::LoudnessSource(const Config& cfg, std::shared_ptr<AudioBus> bus)
    : cfg_(cfg), bus_(std::move(bus)) {
    delta_db_.store(cfg.loudness_delta_db);
}

bool LoudnessSource::start() {
    enabled_ = bus_ && bus_->running();
    if (!enabled_) LOG_INFO("loudness: disabled (no audio bus)");
    return enabled_;
}

SourceVerdict LoudnessSource::poll() {
    SourceVerdict v;
    v.source = name();
    v.ts_ms = now_ms();
    if (!enabled_) return v;

    // Only recompute when new audio has arrived since the last poll.
    uint64_t total = bus_->total_captured();
    if (total == last_seen_total_) return latest_;
    last_seen_total_ = total;

    const size_t window = (size_t)bus_->rate() / 5;  // ~200 ms
    std::vector<int16_t> buf;
    if (bus_->latest(window, buf) < window / 2) return latest_;  // still warming up

    double sumsq = 0.0;
    for (int16_t s16 : buf) {
        double s = s16 / 32768.0;
        sumsq += s * s;
    }
    double rms = std::sqrt(sumsq / (double)buf.size());
    double cur_db = 20.0 * std::log10(rms + 1e-9) + comp_db_.load();

    if (!baseline_init_) { baseline_db_ = cur_db; baseline_init_ = true; }
    double delta = cur_db - baseline_db_;

    const double thresh = delta_db_.load();   // live-tunable via calibration
    if (delta >= thresh) {
        v.signal = Signal::Commercial;
        v.confidence = std::min(1.0, 0.4 + (delta - thresh) / 10.0);
    } else if (delta <= thresh * 0.5) {
        v.signal = Signal::Program;
        v.confidence = 0.4;
    }  // else: Unknown

    // EMA smoothing for the long-term baseline (~30 s at 200 ms polls). Only
    // track when we believe it's program content, so a long commercial break
    // doesn't drag the baseline up.
    const double alpha = 0.0066;
    if (v.signal != Signal::Commercial)
        baseline_db_ = baseline_db_ * (1.0 - alpha) + cur_db * alpha;

    latest_ = v;
    return v;
}

// -------------------------------- AcrSource --------------------------------
AcrSource::AcrSource(const Config& cfg, std::shared_ptr<AudioBus> bus)
    : cfg_(cfg), bus_(std::move(bus)) {}

bool AcrSource::start() {
    if (to_lower(cfg_.acr_provider) != "acrcloud") {
        if (!cfg_.acr_provider.empty())
            LOG_WARN("acr: unknown provider '%s' (only 'acrcloud' is implemented)",
                     cfg_.acr_provider.c_str());
        else
            LOG_INFO("acr: disabled (set acr_provider=acrcloud to enable)");
        return false;
    }
    if (cfg_.acr_host.empty() || cfg_.acr_key.empty() || cfg_.acr_secret.empty()) {
        LOG_WARN("acr: acrcloud selected but host/key/secret incomplete; disabled");
        return false;
    }
    if (!bus_ || !bus_->running()) {
        LOG_WARN("acr: no audio bus (set audio_device); disabled");
        return false;
    }

    enabled_ = true;
    running_ = true;
    thread_ = std::thread(&AcrSource::worker_loop, this);
    LOG_INFO("acr: acrcloud enabled (host=%s, every %ds, %ds windows)",
             cfg_.acr_host.c_str(), cfg_.acr_interval_s, cfg_.acr_sample_seconds);
    return true;
}

void AcrSource::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    enabled_ = false;
}

void AcrSource::worker_loop() {
    const int interval_ms = std::max(5, cfg_.acr_interval_s) * 1000;
    const size_t want = (size_t)bus_->rate() * (size_t)std::max(3, cfg_.acr_sample_seconds);

    // Let the ring fill before the first identify.
    uint64_t next_at = now_ms() + 3000;

    while (running_) {
        if (now_ms() < next_at) { sleep_ms(100); continue; }
        next_at = now_ms() + (uint64_t)interval_ms;

        std::vector<int16_t> samples;
        if (bus_->latest(want, samples) < want) continue;  // not enough audio yet

        AcrOutcome o = acr_identify(cfg_, samples, bus_->rate(),
                                    (uint64_t)time(nullptr));
        SourceVerdict v;
        v.source = name();
        v.ts_ms = now_ms();
        if (o.transport_ok) {
            v.signal = o.signal;
            v.confidence = o.confidence;
            if (o.signal == Signal::Commercial)
                LOG_INFO("acr: matched ad '%s' (conf %.2f)",
                         o.matched_title.c_str(), o.confidence);
        } else {
            LOG_WARN("acr: identify failed: %s", o.error.c_str());
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            latest_ = v;
        }
    }
}

SourceVerdict AcrSource::poll() {
    std::lock_guard<std::mutex> lk(mtx_);
    // A verdict is only trustworthy while reasonably fresh; after two missed
    // cycles fall back to Unknown so a stale "commercial" can't pin the mute.
    if (latest_.ts_ms != 0 &&
        now_ms() - latest_.ts_ms > (uint64_t)cfg_.acr_interval_s * 2500) {
        SourceVerdict v;
        v.source = latest_.source;
        v.ts_ms = now_ms();
        return v;  // Unknown
    }
    return latest_;
}

// ------------------------------ MetadataSource -----------------------------
bool MetadataSource::start() {
    enabled_ = !cfg_.metadata_url.empty();
    if (enabled_)
        LOG_INFO("metadata: feed '%s' configured (poller is a stub)", cfg_.metadata_url.c_str());
    else
        LOG_INFO("metadata: disabled (set metadata_url to enable)");
    return enabled_;
}

SourceVerdict MetadataSource::poll() {
    // TODO: poll the feed for the current channel/stream and report whether an
    // ad break is currently airing.
    if (enabled_ && !warned_) {
        LOG_DEBUG("metadata: poll() is a stub; returning Unknown");
        warned_ = true;
    }
    SourceVerdict v; v.source = name(); v.ts_ms = now_ms();
    return v;  // Unknown
}

}  // namespace admuffs
