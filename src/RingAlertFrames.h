#pragma once

#include "RingPulse.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Samples the existing camera stream and hands completed batches to the Python launcher.
class RingAlertFrames {
public:
    RingAlertFrames(std::filesystem::path root, bool demo, bool preview = false);
    ~RingAlertFrames();
    void sample(ring::Clock::time_point captured, const std::vector<uint8_t>& bgr,
                uint32_t width, uint32_t height) noexcept;
    void pulse(std::optional<ring::Clock::time_point> onset, ring::Clock::time_point now, bool active) noexcept;
    void finish();

private:
    using Time = ring::Clock::time_point;
    struct Sample {
        Time captured;
        uint32_t width, height;
        std::vector<uint8_t> bgr;
    };
    struct Batch {
        unsigned episode, number;
        Time confirmed, begin, end;
        std::optional<Time> onset;
    };

    void run();
    void publish(const Batch& batch, const std::vector<std::shared_ptr<Sample>>& selected);
    void publishPreview(Sample& sample);

    const std::filesystem::path root_;
    const bool demo_;
    const bool preview_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::shared_ptr<Sample>> recent_;
    std::deque<Batch> batches_;
    std::shared_ptr<Sample> nextPreview_;
    std::optional<Time> confirmed_;
    Time lastEnd_{};
    unsigned episode_ = 0, batch_ = 0;
    bool stopped_ = false;
    std::thread worker_;
};
