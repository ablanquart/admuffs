// SPDX-License-Identifier: MIT
// sources.h - concrete detection sources. Loudness and ACR share one AudioBus.
#pragma once

#include "detect/detector.h"
#include "detect/audio_bus.h"
#include "config.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace admuffs {

// Flags sustained loudness excursions above a rolling baseline (a classic
// ad-break proxy). Consumes samples from the shared AudioBus; poll() does the
// analysis inline (cheap: one RMS over ~200 ms of samples).
class LoudnessSource : public DetectionSource {
public:
    LoudnessSource(const Config& cfg, std::shared_ptr<AudioBus> bus);

    bool start() override;
    void stop() override {}
    SourceVerdict poll() override;
    std::string name() const override { return "loudness(alsa)"; }
    double weight() const override { return 0.4; }   // weak signal on its own
    bool enabled() const override { return enabled_; }

    // Live-update the "louder than baseline" trigger threshold (dB). Called
    // when the web remote's Sample buttons recalibrate detection.
    void set_delta_db(double db) { delta_db_.store(db); }
    double delta_db() const { return delta_db_.load(); }

    // Compensation added to measured levels (dB). Used in duck mode with a
    // room mic: the app measures how much the duck attenuated the room and
    // sets it here, so ducked audio is judged on the original scale.
    void set_comp_db(double db) { comp_db_.store(db); }

    bool hears_tv_audio() const override { return true; }

private:
    Config cfg_;
    std::shared_ptr<AudioBus> bus_;
    bool enabled_ = false;
    std::atomic<double> delta_db_{4.0};
    std::atomic<double> comp_db_{0.0};

    double baseline_db_ = -30.0;
    bool baseline_init_ = false;
    uint64_t last_seen_total_ = 0;
    SourceVerdict latest_;
};

// Audio content recognition via ACRCloud: every acr_interval_s, grab the last
// acr_sample_seconds of audio from the bus and ask the service whether it
// matches the user's ads bucket. A match is near-ground-truth "commercial".
class AcrSource : public DetectionSource {
public:
    AcrSource(const Config& cfg, std::shared_ptr<AudioBus> bus);
    ~AcrSource() override { stop(); }

    bool start() override;
    void stop() override;
    SourceVerdict poll() override;
    std::string name() const override { return "acr(" + cfg_.acr_provider + ")"; }
    double weight() const override { return 1.0; }
    bool enabled() const override { return enabled_; }
    bool hears_tv_audio() const override { return true; }

private:
    void worker_loop();

    Config cfg_;
    std::shared_ptr<AudioBus> bus_;
    bool enabled_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex mtx_;
    SourceVerdict latest_;
};

// Stub: external metadata feed reporting ad breaks for the current program.
class MetadataSource : public DetectionSource {
public:
    explicit MetadataSource(const Config& cfg) : cfg_(cfg) {}
    bool start() override;
    void stop() override {}
    SourceVerdict poll() override;
    std::string name() const override { return "metadata(stub)"; }
    double weight() const override { return 1.2; }   // ground truth when present
    bool enabled() const override { return enabled_; }
private:
    Config cfg_;
    bool enabled_ = false;
    bool warned_ = false;
};

}  // namespace admuffs
