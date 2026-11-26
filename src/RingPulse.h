#pragma once

#include <chrono>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ring {
using Clock = std::chrono::steady_clock;

struct PulseEstimate {
    int bpm;
    bool recovered;
};

// Returns an estimate from the latest usable PPG samples, or no estimate.
std::optional<PulseEstimate> estimateBpm(std::span<const double> green, unsigned sampleRate,
                                        std::optional<int> recentVerified = {});

enum class PulseDisplay { waiting, live, held, last };

struct PulseView {
    int bpm = 0;
    PulseDisplay state = PulseDisplay::waiting;
    std::chrono::seconds age{};
};

class PulseReadout {
public:
    std::optional<int> recentVerified(Clock::time_point now) const;
    PulseView update(Clock::time_point now, std::optional<PulseEstimate> estimate, bool advancing);

private:
    int lastBpm_ = 0;
    Clock::time_point lastTime_{};
    int verifiedBpm_ = 0;
    Clock::time_point verifiedTime_{};
};

struct HighlightOptions {
    bool enabled = false;
    std::optional<std::pair<double, double>> fixedRange;
    double changeFraction = 0.20;
    std::chrono::seconds hold{6};
    std::chrono::seconds calibration{30};
};

struct Episode {
    Clock::time_point begin;
    Clock::time_point end;
};

struct HighlightUpdate {
    bool baselineReady = false;
    bool triggered = false;
    std::optional<Clock::time_point> onset;
    std::optional<Episode> completed;
};

class HighlightDetector {
public:
    explicit HighlightDetector(HighlightOptions options);
    HighlightUpdate update(Clock::time_point time, std::optional<int> bpm);
    std::optional<Episode> finish();
    void reset();
    auto baseline() const { return baseline_; }
    bool active() const { return active_; }

private:
    HighlightOptions options_;
    std::optional<std::pair<double, double>> baseline_;
    std::vector<std::pair<Clock::time_point, int>> calibration_;
    std::optional<Clock::time_point> candidate_, recovery_, lastGood_;
    bool active_ = false;
    int direction_ = 0;
};
} // namespace ring
