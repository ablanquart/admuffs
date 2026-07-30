// SPDX-License-Identifier: MIT
// detector.h - detection-source interface + sensor-fusion decision engine.
//
// Each source (loudness, ACR, external metadata) independently emits a verdict
// with a confidence in [0,1]. The FusionEngine combines weighted verdicts and
// applies hysteresis so the TV only mutes/unmutes on a sustained decision.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace admuffs {

struct Config;

enum class Signal { Unknown, Program, Commercial };

struct SourceVerdict {
    Signal signal = Signal::Unknown;
    double confidence = 0.0;   // 0..1
    uint64_t ts_ms = 0;
    std::string source;
};

class DetectionSource {
public:
    virtual ~DetectionSource() = default;
    virtual bool start() = 0;               // returns false if unusable (skipped)
    virtual void stop() = 0;
    virtual SourceVerdict poll() = 0;       // latest verdict (non-blocking)
    virtual std::string name() const = 0;
    virtual double weight() const = 0;      // relative trust in the fusion
    virtual bool enabled() const = 0;

    // True when the source listens to the TV's audio (mic/line-in). With a
    // ROOM tap, muting the TV blinds these sources -- the app discards their
    // Program verdicts while it has the TV muted, else the detector would
    // "hear" the silence it created and immediately unmute (mute paradox).
    virtual bool hears_tv_audio() const { return false; }
};

enum class FusionEvent { None, EnterCommercial, ExitCommercial };

class FusionEngine {
public:
    explicit FusionEngine(const Config& cfg);

    // Feed the latest verdict from every source; returns a transition event.
    FusionEvent update(const std::vector<SourceVerdict>& verdicts);

    bool in_commercial() const { return in_commercial_; }
    double last_score() const { return last_score_; }  // -1 (program) .. +1 (commercial)

    // Forget the current commercial state (used after a failsafe unmute so a
    // still-running break can trigger a fresh EnterCommercial).
    void reset();

private:
    double fire_threshold_;
    int mute_debounce_ms_;
    int unmute_debounce_ms_;

    bool in_commercial_ = false;
    double last_score_ = 0.0;
    uint64_t candidate_since_ = 0;   // when the pending transition began
    bool candidate_commercial_ = false;
};

}  // namespace admuffs
