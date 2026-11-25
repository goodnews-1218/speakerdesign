#include "RingPulse.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ring {
namespace {
double median(std::vector<double> values) {
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}
double correlation(std::span<const double> signal, size_t lag) {
    double dot = 0, firstPower = 0, secondPower = 0;
    for (size_t i = lag; i < signal.size(); ++i) {
        const auto a = signal[i - lag], b = signal[i];
        dot += a * b;
        firstPower += a * a;
        secondPower += b * b;
    }
    const double scale = std::sqrt(firstPower * secondPower);
    return scale > 0 ? dot / scale : 0;
}

std::optional<double> peakPeriod(std::span<const double> signal, double threshold,
                                 size_t minLag, size_t maxLag) {
    std::vector<size_t> peaks;
    for (size_t i = 1; i + 1 < signal.size(); ++i) {
        if (signal[i] < threshold || signal[i] <= signal[i - 1] || signal[i] < signal[i + 1])
            continue;
        if (peaks.empty() || i - peaks.back() >= minLag) peaks.push_back(i);
        else if (signal[i] > signal[peaks.back()]) peaks.back() = i;
    }
    if (peaks.size() < 4) return {};
    std::vector<double> intervals;
    for (size_t i = 1; i < peaks.size(); ++i)
        intervals.push_back(static_cast<double>(peaks[i] - peaks[i - 1]));
    const double period = median(intervals);
    if (period < minLag || period > maxLag) return {};

    const auto stable = std::ranges::count_if(intervals, [period](double interval) {
        return std::abs(interval - period) <= 0.14 * period;
    });
    if (stable * 4 < static_cast<ptrdiff_t>(intervals.size()) * 3 ||
        std::abs(intervals.back() - period) > 0.20 * period)
        return {};

    if (correlation(signal, static_cast<size_t>(std::lround(period))) < 0.60) return {};
    return period;
}

std::optional<size_t> repeatingPeriod(std::span<const double> signal, unsigned rate,
                                      size_t minLag, size_t maxLag) {
    // Require periodic evidence both across the window and in its most recent part.
    const auto recent = signal.last(std::min(signal.size(), size_t(rate * 37 / 10)));
    std::vector<double> full, tailScores;
    full.reserve(maxLag + 2);
    tailScores.reserve(maxLag + 2);
    for (size_t lag = 0; lag <= maxLag + 1; ++lag) {
        full.push_back(correlation(signal, lag));
        tailScores.push_back(correlation(recent, lag));
    }

    std::vector<std::pair<size_t, double>> candidates;
    double best = 0;
    for (auto lag = minLag; lag <= maxLag; ++lag) {
        if (full[lag] < 0.62 || full[lag] < full[lag - 1] || full[lag] <= full[lag + 1] ||
            recent.size() < 2 * lag + 1) continue;
        const double tail = tailScores[lag];
        if (tail < 0.73) continue;
        // The tail may have settled at a faster rate while the full window still
        // favors a shared multiple of the old and new periods.
        bool multiple = false;
        for (size_t shorter = minLag; shorter <= lag * 0.55; ++shorter) {
            const double ratio = double(lag) / shorter;
            if (tailScores[shorter] >= std::max(0.73, tail - 0.12) &&
                tailScores[shorter] >= tailScores[shorter - 1] &&
                tailScores[shorter] > tailScores[shorter + 1] &&
                std::abs(ratio - std::round(ratio)) <= 0.12) {
                multiple = true;
                break;
            }
        }
        if (multiple) continue;
        const double score = 0.65 * full[lag] + 0.35 * tail;
        candidates.emplace_back(lag, score);
        best = std::max(best, score);
    }
    // A whole-number multiple of the beat period can score nearly as well.
    const auto strongest = std::ranges::find_if(candidates, [best](const auto& choice) {
        return choice.second >= best - 0.035;
    });
    if (strongest == candidates.end()) return {};
    return strongest->first;
}

std::optional<size_t> supportedRecentPeriod(std::span<const double> signal, unsigned rate,
                                           size_t minLag, size_t maxLag, int previousBpm) {
    const double expected = 60.0 * rate / previousBpm;
    const auto first = std::max(minLag, static_cast<size_t>(std::ceil(expected * 0.92)));
    const auto last = std::min(maxLag, static_cast<size_t>(std::floor(expected * 1.08)));
    const auto recent = signal.last(std::min(signal.size(), size_t(rate * 37 / 10)));
    std::optional<size_t> period;
    double score = 0;
    for (auto lag = first; lag <= last && recent.size() > 2 * lag; ++lag) {
        const auto full = correlation(signal, lag);
        const auto tail = correlation(recent, lag);
        if (full < 0.62 || tail < 0.62 ||
            full < correlation(signal, lag - 1) || full <= correlation(signal, lag + 1)) continue;
        if (const auto agreement = (full + tail) / 2; agreement > score) {
            period = lag;
            score = agreement;
        }
    }
    return period;
}
} // namespace

std::optional<PulseEstimate> estimateBpm(std::span<const double> green, unsigned rate,
                                        std::optional<int> recentVerified) {
    if (rate < 25 || rate > 500 || green.size() < 5 * rate) return {};
    green = green.last(std::min(green.size(), size_t(7 * rate)));
    if (!std::ranges::all_of(green, [](double v) { return std::isfinite(v); })) return {};

    std::vector<double> sum{0};
    sum.reserve(green.size() + 1);
    for (const auto value : green) sum.push_back(sum.back() + value);
    const size_t slow = rate * 2 / 5;
    const size_t fast = rate / 30;
    const auto mean = [&](size_t i, size_t half) {
        return (sum[i + half + 1] - sum[i - half]) / (2 * half + 1);
    };
    std::vector<double> signal;
    for (size_t i = slow; i + slow < green.size(); ++i)
        signal.push_back(mean(i, fast) - mean(i, slow));

    auto sorted = signal;
    std::ranges::sort(sorted);
    const double low = sorted[sorted.size() / 20];
    const double high = sorted[sorted.size() * 19 / 20];
    const double range = high - low;
    if (range < 1.0 || sorted.back() - sorted.front() > 4 * range) return {};

    const size_t minLag = (rate * 60 + 219) / 220;
    const size_t maxLag = rate * 60 / 35;
    if (const auto period = peakPeriod(signal, low + 0.65 * range, minLag, maxLag))
        return PulseEstimate{static_cast<int>(std::lround(60 * rate / *period)), false};
    if (const auto period = repeatingPeriod(signal, rate, minLag, maxLag))
        return PulseEstimate{static_cast<int>(std::lround(60.0 * rate / *period)), true};
    if (recentVerified && *recentVerified > 0) {
        // Reduce the influence of strong motion spikes before looking for evidence
        // of the recent beat period. A fallback result never renews that reference.
        const double lower = sorted[sorted.size() / 10];
        const double upper = sorted[sorted.size() * 9 / 10];
        for (auto& value : signal) value = std::clamp(value, lower, upper);
        if (upper - lower >= 1.0)
            if (const auto period = supportedRecentPeriod(signal, rate, minLag, maxLag, *recentVerified))
                return PulseEstimate{static_cast<int>(std::lround(60.0 * rate / *period)), true};
    }
    return {};
}

std::optional<int> PulseReadout::recentVerified(Clock::time_point now) const {
    using namespace std::chrono_literals;
    if (verifiedBpm_ && now - verifiedTime_ <= 5s) return verifiedBpm_;
    return {};
}

PulseView PulseReadout::update(Clock::time_point now, std::optional<PulseEstimate> estimate,
                               bool advancing) {
    using namespace std::chrono_literals;
    if (estimate && advancing) {
        lastBpm_ = estimate->bpm;
        lastTime_ = now;
        if (!estimate->recovered) {
            verifiedBpm_ = lastBpm_;
            verifiedTime_ = now;
        }
        return {lastBpm_, PulseDisplay::live};
    }
    if (!lastBpm_) return {};
    const auto age = now - lastTime_;
    const auto state = advancing && age <= 3250ms ? PulseDisplay::held : PulseDisplay::last;
    return {lastBpm_, state, std::chrono::duration_cast<std::chrono::seconds>(age)};
}

HighlightDetector::HighlightDetector(HighlightOptions options) : options_(options) { reset(); }

void HighlightDetector::reset() {
    finish();
    calibration_.clear();
    baseline_ = options_.fixedRange;
}

std::optional<Episode> HighlightDetector::finish() {
    std::optional<Episode> result;
    if (active_) result = Episode{*candidate_, *lastGood_};
    active_ = false;
    candidate_.reset();
    recovery_.reset();
    lastGood_.reset();
    direction_ = 0;
    return result;
}

HighlightUpdate HighlightDetector::update(Clock::time_point time, std::optional<int> bpm) {
    using namespace std::chrono_literals;
    HighlightUpdate result;
    if (!options_.enabled) return result;
    if (!bpm || (lastGood_ && time - *lastGood_ > 2500ms)) {
        result.completed = finish();
        calibration_.clear();
        if (!bpm) return result;
    }
    lastGood_ = time;
    if (!baseline_) {
        calibration_.emplace_back(time, *bpm);
        while (calibration_.size() > 1 && time - calibration_[1].first >= options_.calibration)
            calibration_.erase(calibration_.begin());
        if (time - calibration_.front().first < options_.calibration) return result;
        std::vector<double> values;
        for (const auto& [stamp, value] : calibration_) values.push_back(value);
        const double center = median(values);
        const auto [low, high] = std::ranges::minmax_element(values);
        if (*high - *low <= std::max(6.0, 0.08 * center)) {
            baseline_ = {center, center};
            calibration_.clear();
            result.baselineReady = true;
        }
        return result;
    }
    const auto [low, high] = *baseline_;
    const auto fraction = options_.changeFraction * (active_ ? 0.8 : 1);
    const int direction = *bpm - high >= high * fraction ? 1 :
                          low - *bpm >= low * fraction ? -1 : 0;
    if (active_) {
        if (direction == direction_) {
            recovery_.reset();
        } else {
            if (!recovery_) recovery_ = time;
            if (time - *recovery_ >= 3s) result.completed = finish();
        }
        // Bound any one export while continuing to track a sustained event.
        if (active_ && time - *candidate_ >= 60s) {
            result.completed = Episode{*candidate_, time};
            candidate_ = time;
        }
    } else if (!direction) {
        candidate_.reset();
        direction_ = 0;
    } else {
        if (!candidate_ || direction_ != direction) candidate_ = time;
        direction_ = direction;
        if (time - *candidate_ >= options_.hold) {
            active_ = true;
            result.triggered = true;
        }
    }
    return result;
}
} // namespace ring
