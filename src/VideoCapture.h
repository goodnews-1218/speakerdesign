#pragma once

#include "Sdk.h"
#include "RingMonitor.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RingAlertFrames;

class VideoCapture {
public:
    explicit VideoCapture(const std::filesystem::path& directory, bool microphone = true,
                          unsigned segmentSeconds = 120, bool ringOverlay = false,
                          bool ringDemo = false, bool fallAlert = false, bool fallAlertPreview = false);
    ~VideoCapture();

    void open(ViaimConnectSdk* sdk);
    void start();
    bool requestSnapshot();
    bool requestHighlight(std::chrono::steady_clock::time_point marked = std::chrono::steady_clock::now());
    bool requestRingHighlight(ring::Episode episode);
    void ringReading(ring::Reading reading);
    void ringAlert(std::optional<ring::Clock::time_point> onset, ring::Clock::time_point now, bool active);
    void audioWritten(bool microphone, uint64_t bytes) noexcept;
    bool failed();
    void stop();
    bool finish();

    void receive(const ViaimConnectCameraFrame& frame) noexcept;
    void cameraState(ViaimConnectCameraState state) noexcept;

private:
    using Clock = std::chrono::steady_clock;
    struct Frame {
        std::vector<uint8_t> pixels;
        uint64_t index;
        bool snapshot;
        ring::Reading pulse;
        Clock::time_point captured;
    };

    void encode() noexcept;
    void exportClips() noexcept;
    void exportRange(const std::string& name, uint64_t begin, uint64_t end, bool transcode);
    uint64_t frameAt(Clock::time_point time) const;
    bool queueHighlight(Clock::time_point begin, Clock::time_point end, bool fromRing);

    const std::filesystem::path directory_;
    const std::filesystem::path ffmpeg_;
    const std::filesystem::path audio_;
    const bool microphone_;
    const unsigned segmentSeconds_;
    const unsigned fragmentSeconds_;
    const bool ringOverlay_;
    std::unique_ptr<RingAlertFrames> fallAlert_;
    ViaimConnectCameraOpenOptions mode_ = VIAIM_CONNECT_CAMERA_OPEN_OPTIONS_INIT;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Frame> queue_;
    std::thread worker_;
    std::thread exporter_;
    Clock::time_point started_;
    struct Highlight { unsigned id; uint64_t begin; uint64_t end; bool fromRing; };
    std::deque<Highlight> highlights_;
    unsigned highlightCount_ = 0;
    ring::Reading ringReading_;
    std::atomic<uint64_t> audioBytes_{0};
    bool finalized_ = false;
    bool exportFailed_ = false;
    bool recording_ = false;
    bool stopped_ = false;
    bool snapshotPending_ = false;
    bool cameraFailed_ = false;
    bool encodeFailed_ = false;
    bool snapshotFailed_ = false;
    uint64_t dropped_ = 0;
    uint64_t written_ = 0;
    unsigned snapshots_ = 0;
};
