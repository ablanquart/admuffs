// SPDX-License-Identifier: MIT
#include "detect/audio_bus.h"
#include "common.h"

#if ADMUFFS_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

#include <algorithm>

namespace admuffs {

AudioBus::AudioBus(const std::string& device, int rate, int history_seconds)
    : device_(device), rate_(rate),
      capacity_((size_t)rate * (size_t)history_seconds) {
    ring_.resize(capacity_, 0);
}

AudioBus::~AudioBus() { stop(); }

#if ADMUFFS_HAVE_ALSA

namespace {
// Scan sound cards for one that can actually capture and return its plughw
// name. Needed because on Raspberry Pi the onboard audio is playback-only, so
// ALSA's "default" has no capture slave even when a USB mic is plugged in.
std::string find_capture_device() {
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        std::string dev = "plughw:" + std::to_string(card) + ",0";
        snd_pcm_t* pcm = nullptr;
        if (snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_CAPTURE,
                         SND_PCM_NONBLOCK) == 0) {
            snd_pcm_close(pcm);
            char* name = nullptr;
            if (snd_card_get_name(card, &name) == 0 && name) {
                LOG_INFO("audio bus: found capture device %s (%s)", dev.c_str(), name);
                free(name);
            } else {
                LOG_INFO("audio bus: found capture device %s", dev.c_str());
            }
            return dev;
        }
    }
    return "";
}
}  // namespace

bool AudioBus::start() {
    if (running_) return true;
    if (device_.empty()) return false;

    // "auto" always scans; a concrete device gets a scan fallback if it fails
    // to open (the common case: 'default' on a Pi, where onboard audio can't
    // capture but a USB mic can).
    if (device_ == "auto") {
        std::string found = find_capture_device();
        if (found.empty()) {
            LOG_WARN("audio bus: no capture-capable sound card found "
                     "(is the USB mic plugged in? check 'arecord -l')");
            return false;
        }
        device_ = found;
    }

    // Verify the device opens before committing to the thread.
    snd_pcm_t* test = nullptr;
    int rc = snd_pcm_open(&test, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        LOG_WARN("audio bus: cannot open ALSA device '%s': %s",
                 device_.c_str(), snd_strerror(rc));
        std::string found = find_capture_device();
        if (found.empty() || found == device_) {
            LOG_WARN("audio bus: no usable capture device "
                     "(check 'arecord -l', or set audio_device=plughw:N,0)");
            return false;
        }
        LOG_INFO("audio bus: falling back to %s", found.c_str());
        device_ = found;
        if (snd_pcm_open(&test, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) {
            LOG_WARN("audio bus: fallback device failed too");
            return false;
        }
    }
    snd_pcm_close(test);

    running_ = true;
    thread_ = std::thread(&AudioBus::capture_loop, this);
    LOG_INFO("audio bus: capturing '%s' @ %d Hz (%zu s history)",
             device_.c_str(), rate_, capacity_ / (size_t)rate_);
    return true;
}

void AudioBus::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioBus::capture_loop() {
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) {
        running_ = false;
        return;
    }
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           1, (unsigned)rate_, 1, 200000) < 0) {
        LOG_WARN("audio bus: set_params failed on '%s'", device_.c_str());
        snd_pcm_close(pcm);
        running_ = false;
        return;
    }

    const size_t chunk = (size_t)rate_ / 10;  // 100 ms reads
    std::vector<int16_t> buf(chunk);

    while (running_) {
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, buf.data(), chunk);
        if (got < 0) {
            snd_pcm_recover(pcm, (int)got, 1);
            continue;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        for (snd_pcm_sframes_t i = 0; i < got; ++i) {
            ring_[write_pos_] = buf[i];
            write_pos_ = (write_pos_ + 1) % capacity_;
        }
        filled_ = std::min(capacity_, filled_ + (size_t)got);
        total_ += (uint64_t)got;
    }
    snd_pcm_close(pcm);
}

#else  // !ADMUFFS_HAVE_ALSA (e.g. macOS): no capture backend in this build.

bool AudioBus::start() {
    if (device_.empty()) return false;
    LOG_WARN("audio bus: audio capture is not available in this build "
             "(ALSA is Linux-only). Loudness/ACR detection is disabled; "
             "network/IR TV control and the web remote work normally.");
    return false;
}

void AudioBus::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioBus::capture_loop() {}

#endif  // ADMUFFS_HAVE_ALSA

size_t AudioBus::latest(size_t n, std::vector<int16_t>& out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t take = std::min(n, filled_);
    out.resize(take);
    // Copy the last `take` samples ending at write_pos_ (exclusive).
    size_t start = (write_pos_ + capacity_ - take) % capacity_;
    for (size_t i = 0; i < take; ++i)
        out[i] = ring_[(start + i) % capacity_];
    return take;
}

}  // namespace admuffs
