#include "RingPulse.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace std::chrono_literals;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static std::vector<double> pulse(int bpm) {
    std::vector<double> samples;
    std::mt19937 generator(5);
    std::normal_distribution<double> noise(0, 8);
    for (int i = 0; i < 700; ++i) {
        const double t = i / 100.0;
        const double phase = 2 * std::numbers::pi * t * bpm / 60;
        samples.push_back(28000 + 200 * std::sin(phase) - 40 * std::cos(2 * phase) +
                          80 * std::sin(t) + noise(generator));
    }
    return samples;
}

int main(int argc, char** argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "--samples-100hz") {
        double sample;
        std::vector<double> samples;
        while (std::cin >> sample) samples.push_back(sample);
        ring::PulseReadout readout;
        for (size_t end = 500; end <= samples.size(); end += 100) {
            const size_t second = end / 100;
            const auto now = ring::Clock::time_point{} + std::chrono::seconds(second);
            const auto estimate = ring::estimateBpm(std::span(samples).first(end), 100,
                                                   readout.recentVerified(now));
            const auto view = readout.update(now, estimate, true);
            std::cout << second << "s: "
                      << (view.bpm ? std::to_string(view.bpm) : "--") << " BPM";
            if (view.state == ring::PulseDisplay::last)
                std::cout << " (LAST " << view.age.count() << "s)";
            else if (view.state == ring::PulseDisplay::held) std::cout << " (HOLD)";
            else if (estimate && estimate->recovered) std::cout << " (recovered)";
            std::cout << '\n';
        }
        return 0;
    }
    for (const int expected : {50, 75, 90, 110, 155}) {
        const auto estimate = ring::estimateBpm(pulse(expected), 100);
        require(estimate && !estimate->recovered && std::abs(estimate->bpm - expected) <= 2,
                "Synthetic pulse within 2 BPM with direct peak agreement");
    }
    require(!ring::estimateBpm(std::vector<double>(700, 1000), 100), "Flat signal rejected");
    std::vector<double> extraPeaks;
    for (int i = 0; i < 700; ++i) {
        const double phase = 2 * std::numbers::pi * i / 80;
        const double earlySpike = (i - 60) / 4.0, laterSpike = (i - 140) / 4.0;
        extraPeaks.push_back(28000 + 200 * std::sin(phase) - 40 * std::cos(2 * phase) +
            375 * (std::exp(-earlySpike * earlySpike) + std::exp(-laterSpike * laterSpike)));
    }
    const auto recovered = ring::estimateBpm(extraPeaks, 100);
    require(recovered && recovered->recovered && std::abs(recovered->bpm - 75) <= 2,
            "Recover 75 BPM from repeating waveform despite spurious peaks");
    std::vector<double> changingRate;
    for (int i = 0; i < 700; ++i) {
        const double t = 12 - (699 - i) / 100.0;
        const double phase = 2 * std::numbers::pi * (75 * t + 35 * std::max(0.0, t - 8)) / 60;
        changingRate.push_back(5000 + 240 * std::sin(phase) + 40 * std::sin(2 * phase));
    }
    const auto transition = ring::estimateBpm(changingRate, 100);
    require(!transition || transition->bpm >= 70, "Reject spurious low BPM at a rate transition");
    auto artifact = pulse(75);
    artifact[350] += 100000;
    require(!ring::estimateBpm(artifact, 100), "Large isolated artifact rejected");
    std::mt19937 generator(55);
    std::normal_distribution<double> noise(0, 180);
    for (auto& sample : artifact) sample = 28000 + noise(generator);
    require(!ring::estimateBpm(artifact, 100), "Aperiodic noise rejected");
    require(!ring::estimateBpm(artifact, 100, 75), "Recent reference alone cannot turn noise into BPM");

    const auto start = ring::Clock::now();
    ring::PulseReadout readout;
    require(readout.update(start, {}, true).state == ring::PulseDisplay::waiting,
            "Wait for a first estimate");
    require(readout.update(start, ring::PulseEstimate{65, false}, true).state == ring::PulseDisplay::live,
            "Valid estimate is live");
    require(readout.update(start + 2s, {}, true).state == ring::PulseDisplay::held,
            "Short noise gap keeps the last estimate");
    require(readout.update(start + 2s, {}, false).state == ring::PulseDisplay::last,
            "Stopped samples are historical immediately");
    const auto stale = readout.update(start + 4s, {}, true);
    require(stale.state == ring::PulseDisplay::last && stale.bpm == 65 && stale.age == 4s,
            "Show and age the last known value beyond the hold period");
    readout.update(start + 5s, ring::PulseEstimate{64, true}, true);
    require(!readout.recentVerified(start + 6s), "Recovery does not prolong the verified reference");
    require(readout.update(start + 7s, {}, true).bpm == 64, "Recovery supplies a new displayed estimate");

    ring::HighlightOptions options;
    options.enabled = true;
    options.fixedRange = {60, 60};
    ring::HighlightDetector smallDeviation(options);
    for (int i = 0; i <= 7; ++i)
        require(!smallDeviation.update(start + i * 1s, 66).triggered, "Ten percent does not trigger by default");
    for (int i = 8; i <= 14; ++i)
        require(smallDeviation.update(start + i * 1s, 72).triggered == (i == 14),
                "Twenty percent starts its own six-second hold");

    options.fixedRange = {90, 90};
    ring::HighlightDetector detector(options);
    for (int i = 0; i <= 4; ++i)
        require(!detector.update(start + i * 1s, 110).triggered, "Short spikes do not trigger");
    detector.update(start + 5s, 90);
    for (int i = 6; i <= 12; ++i) {
        const auto update = detector.update(start + i * 1s, 108);
        require(update.triggered == (i == 12),
                "Exact upper twenty percent threshold triggers after six seconds");
        require(update.onset.has_value() == (i == 12) && detector.active() == (i == 12),
                "Visual checking starts at the confirmed trigger");
        if (update.onset) require(*update.onset == start + 6s, "Initial alert carries the original onset");
    }
    for (int i = 13; i <= 24; ++i) {
        const auto update = detector.update(start + i * 1s, i < 21 ? 110 : 90);
        require(!update.triggered, "No repeated triggers");
        require(detector.active() == (i < 24), "Visual checking can continue until recovery is confirmed");
        require(update.completed.has_value() == (i == 24), "Wait for sustained recovery before closing");
        if (update.completed)
            require(update.completed->begin == start + 6s && update.completed->end == start + 24s,
                    "Episode includes onset before hold and the full deviation");
    }
    for (int i = 25; i <= 31; ++i)
        require(detector.update(start + i * 1s, 72).triggered == (i == 31),
                "Exact lower twenty percent threshold triggers after six seconds");
    const auto lost = detector.update(start + 32s, {}).completed;
    require(lost && lost->end == start + 31s && !detector.finish(), "Loss closes at last valid reading exactly once");

    detector.reset();
    for (int i = 0; i <= 4; ++i) detector.update(start + i * 1s, 110);
    for (int i = 10; i < 16; ++i)
        require(!detector.update(start + i * 1s, 110).triggered, "A sample gap breaks hold");
    require(detector.update(start + 16s, 110).triggered, "Hold restarts after a sample gap");
    require(detector.finish().has_value() && !detector.finish(), "Stop exports a confirmed active event once");

    options.fixedRange.reset();
    options.calibration = 10s;
    ring::HighlightDetector learned(options);
    for (int i = 0; i < 12; ++i)
        require(!learned.update(start + i * 1s, i % 2 ? 100 : 75).baselineReady,
                "Unstable input does not set a baseline");
    require(!learned.baseline(), "No learned baseline yet");
    for (int i = 12; i <= 22; ++i) learned.update(start + i * 1s, 89 + i % 3);
    require(learned.baseline() && std::abs(learned.baseline()->first - 90) <= 1, "Learn personal baseline");
    for (int i = 23; i <= 29; ++i)
        require(learned.update(start + i * 1s, 110).triggered == (i == 29), "Learned baseline drives detection");
    learned.reset();
    require(!learned.baseline(), "Device session reset clears learned baseline");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
