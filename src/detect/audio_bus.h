// SPDX-License-Identifier: MIT
// audio_bus.h - single shared ALSA capture pipeline.
//
// Exactly one thread owns the microphone; every detection source that needs
// audio (loudness, ACR) reads from this bus instead of opening the device
// itself. The bus keeps a ring buffer of the most recent ~15 s of mono S16
// samples so consumers with different window sizes can coexist.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace admuffs {

struct Config;

class AudioBus {
public:
    AudioBus(const std::string& device, int rate, int history_seconds = 15);
    ~AudioBus();

    AudioBus(const AudioBus&) = delete;
    AudioBus& operator=(const AudioBus&) = delete;

    // Opens the device and starts the capture thread. False if the device
    // cannot be opened (bus stays inert; consumers should disable themselves).
    bool start();
    void stop();
    bool running() const { return running_.load(); }

    int rate() const { return rate_; }

    // Copies the most recent `n` samples into `out` (oldest first). Returns
    // the number actually copied (< n until the ring has filled that far).
    size_t latest(size_t n, std::vector<int16_t>& out) const;

    // Total samples captured since start (monotonic; lets consumers detect
    // whether new audio arrived since they last looked).
    uint64_t total_captured() const { return total_.load(); }

private:
    void capture_loop();

    std::string device_;
    int rate_;
    size_t capacity_;                 // ring capacity in samples

    mutable std::mutex mtx_;
    std::vector<int16_t> ring_;
    size_t write_pos_ = 0;
    size_t filled_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> total_{0};
    std::thread thread_;
};

}  // namespace admuffs
