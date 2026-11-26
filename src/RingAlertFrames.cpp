#include "RingAlertFrames.h"
#include "CameraCaptureSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <syncstream>

namespace {
using namespace std::chrono_literals;
constexpr auto historyLength = 72s; // The configurable heart-rate hold can be as long as 60 seconds.
constexpr auto samplePeriod = 1s;
constexpr auto previewPeriod = 80ms;
constexpr auto followupInterval = 7s;

void reportError(const std::exception& error) {
    std::osyncstream(std::cerr) << "FALL_ALERT_ERROR=" << error.what() << '\n' << std::flush_emit;
}
} // namespace

RingAlertFrames::RingAlertFrames(std::filesystem::path root, bool demo, bool preview)
    : root_(std::move(root)), demo_(demo), preview_(preview) {
    std::filesystem::create_directory(root_);
    worker_ = std::thread(&RingAlertFrames::run, this);
}

RingAlertFrames::~RingAlertFrames() { finish(); }

void RingAlertFrames::sample(Time captured, const std::vector<uint8_t>& bgr,
                             uint32_t width, uint32_t height) noexcept {
    if (!width || !height || bgr.size() != size_t(width) * height * 3) return;
    try {
        std::lock_guard lock(mutex_);
        if (stopped_) return;
        const bool takeEvidence = recent_.empty() || captured - recent_.back()->captured >= samplePeriod;
        const bool takePreview = preview_ && (!lastPreview_ || captured - *lastPreview_ >= previewPeriod);
        if (!takeEvidence && !takePreview) return;
        auto sample = std::make_shared<Sample>();
        sample->captured = captured;
        const auto step = (std::max(width, height) + 639) / 640;
        sample->width = std::max(1u, width / step);
        sample->height = std::max(1u, height / step);
        sample->bgr.resize(size_t(sample->width) * sample->height * 3);
        for (uint32_t y = 0; y < sample->height; ++y) {
            for (uint32_t x = 0; x < sample->width; ++x) {
                const size_t source = (size_t(y) * step * width + x * step) * 3;
                const size_t destination = (size_t(y) * sample->width + x) * 3;
                std::memcpy(sample->bgr.data() + destination, bgr.data() + source, 3);
            }
        }
        if (takeEvidence) {
            recent_.push_back(sample);
            while (captured - recent_.front()->captured > historyLength) recent_.pop_front();
        }
        if (takePreview) {
            lastPreview_ = captured;
            nextPreview_ = std::move(sample);
            ready_.notify_one();
        }
    } catch (const std::exception& error) {
        reportError(error);
    }
}

void RingAlertFrames::pulse(std::optional<Time> onset, Time now, bool active) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (stopped_) return;
        if (onset) {
            confirmed_ = now;
            lastEnd_ = now + 3s;
            ++episode_;
            batch_ = 1;
            batches_.push_back({episode_, batch_, now, now - 3s, lastEnd_, onset});
        } else if (confirmed_ && ((active && now >= lastEnd_ + followupInterval) ||
                                  (!active && now > lastEnd_ + 1s))) {
            batches_.push_back({episode_, ++batch_, *confirmed_, lastEnd_ - 1s, now, {}});
            lastEnd_ = now;
        }
        if (!active) confirmed_.reset();
        ready_.notify_one();
    } catch (const std::exception& error) {
        reportError(error);
    }
}

void RingAlertFrames::finish() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    ready_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void RingAlertFrames::run() {
    for (;;) {
        try {
            std::vector<std::shared_ptr<Sample>> selected;
            Batch batch{};
            std::shared_ptr<Sample> preview;
            {
                std::unique_lock lock(mutex_);
                for (;;) {
                    if (!batches_.empty() && (stopped_ || ring::Clock::now() >= batches_.front().end + 450ms)) {
                        batch = batches_.front();
                        batches_.pop_front();
                        for (const auto& sample : recent_) {
                            const auto time = sample->captured;
                            const bool nearOnset = batch.onset && time >= *batch.onset - 2s && time <= *batch.onset + 2s;
                            if (nearOnset || (time >= batch.begin && time <= batch.end)) selected.push_back(sample);
                        }
                        // Keep the most recent samples if a pulse-reading delay extended the window.
                        if (selected.size() > 12) selected.erase(selected.begin(), selected.end() - 12);
                        break;
                    }
                    if (nextPreview_) {
                        preview = std::move(nextPreview_);
                        break;
                    }
                    if (stopped_) return;
                    if (batches_.empty()) ready_.wait(lock);
                    else ready_.wait_until(lock, batches_.front().end + 450ms);
                }
            }
            if (preview) publishPreview(*preview);
            else publish(batch, selected);
        } catch (const std::exception& error) {
            reportError(error);
        }
    }
}

void RingAlertFrames::publishPreview(Sample& sample) {
    const auto temporary = root_ / "preview.tmp";
    const auto target = root_ / "preview.jpg";
    saveCameraJpeg(temporary, sample.bgr, sample.width, sample.height);
    if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
    const auto error = GetLastError();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    // A browser may briefly have the previous image open. Try again on the next sample.
    if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
        throw std::system_error(static_cast<int>(error), std::system_category(), "Could not update camera preview");
}

void RingAlertFrames::publish(const Batch& batch, const std::vector<std::shared_ptr<Sample>>& selected) {
    const auto name = "event-" + std::to_string(batch.episode) + "-" + std::to_string(batch.number);
    const auto directory = root_ / name;
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Alert batch already exists");

    const auto requiredTime = batch.number == 1 ? batch.confirmed : batch.end - 2s;
    const char* status = selected.size() >= 3 && selected.back()->captured >= requiredTime
        ? "ready" : "insufficient";
    std::vector<int64_t> times;
    try {
        for (const auto& sample : selected) {
            std::ostringstream filename;
            filename << "frame-" << std::setfill('0') << std::setw(3) << times.size() << ".jpg";
            saveCameraJpeg(directory / filename.str(), sample->bgr, sample->width, sample->height);
            times.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                sample->captured - batch.confirmed).count());
        }
    } catch (const std::exception& error) {
        status = "failed";
        reportError(error);
    }
    const auto temporary = directory / "ready.tmp";
    std::ofstream out(temporary);
    out << "{\"version\":1,\"demo\":" << (demo_ ? "true" : "false")
        << ",\"episode\":" << batch.episode << ",\"batch\":" << batch.number
        << ",\"status\":\"" << status << "\",\"times_ms\":[";
    for (size_t index = 0; index < times.size(); ++index) {
        if (index) out << ',';
        out << times[index];
    }
    out << "]}\n";
    out.close();
    if (!out) throw std::runtime_error("Could not write alert marker");
    std::filesystem::rename(temporary, directory / "ready");
    std::osyncstream(std::cout) << "FALL_ALERT_FRAMES=" << name << " STATUS=" << status << '\n' << std::flush_emit;
}
