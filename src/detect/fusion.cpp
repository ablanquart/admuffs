// SPDX-License-Identifier: MIT
#include "detect/detector.h"
#include "config.h"
#include "common.h"

#include <algorithm>

namespace admuffs {

void FusionEngine::reset() {
    in_commercial_ = false;
    candidate_commercial_ = false;
    candidate_since_ = 0;
    last_score_ = 0.0;
}

FusionEngine::FusionEngine(const Config& cfg)
    : fire_threshold_(cfg.fire_threshold),
      mute_debounce_ms_(cfg.mute_debounce_ms),
      unmute_debounce_ms_(cfg.unmute_debounce_ms) {}

FusionEvent FusionEngine::update(const std::vector<SourceVerdict>& verdicts) {
    // Weighted signed sum: commercial = +conf, program = -conf, unknown = 0.
    double numer = 0.0, denom = 0.0;
    for (const auto& v : verdicts) {
        if (v.signal == Signal::Unknown) continue;
        double sign = (v.signal == Signal::Commercial) ? 1.0 : -1.0;
        // weight is folded in by the caller via SourceVerdict? No: caller sets
        // confidence; weighting happens here using a per-verdict weight baked
        // into confidence upstream is avoided — we treat confidence as trust.
        numer += v.confidence * sign;
        denom += v.confidence;
    }

    double score = (denom > 0.0) ? (numer / denom) : 0.0;  // -1..+1
    last_score_ = score;

    double commercial_conf = std::max(0.0, score);
    double program_conf = std::max(0.0, -score);
    uint64_t now = now_ms();

    if (!in_commercial_) {
        // Looking to enter a commercial.
        bool strong = denom > 0.0 && commercial_conf >= fire_threshold_;
        if (strong) {
            if (!candidate_commercial_) { candidate_commercial_ = true; candidate_since_ = now; }
            if (now - candidate_since_ >= (uint64_t)mute_debounce_ms_) {
                in_commercial_ = true;
                candidate_commercial_ = false;
                LOG_INFO("detector: COMMERCIAL (score=%.2f)", score);
                return FusionEvent::EnterCommercial;
            }
        } else {
            candidate_commercial_ = false;
        }
    } else {
        // Looking to exit back to program.
        bool back = denom > 0.0 && (program_conf >= fire_threshold_ ||
                                    commercial_conf < fire_threshold_ * 0.5);
        if (back) {
            if (candidate_commercial_) { candidate_commercial_ = false; candidate_since_ = now; }
            else if (candidate_since_ == 0) candidate_since_ = now;
            if (now - candidate_since_ >= (uint64_t)unmute_debounce_ms_) {
                in_commercial_ = false;
                candidate_since_ = 0;
                LOG_INFO("detector: PROGRAM resumed (score=%.2f)", score);
                return FusionEvent::ExitCommercial;
            }
        } else {
            candidate_since_ = 0;  // still clearly in a commercial
        }
    }
    return FusionEvent::None;
}

}  // namespace admuffs
