#pragma once

#include "Sdk.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

class VideoCapture {
public:
    explicit VideoCapture(const std::filesystem::path& directory);
    ~VideoCapture();

    void open(ViaimConnectSdk* sdk);
    void start();
    bool requestSnapshot();
    void stop();
    bool finish(const std::filesystem::path& audio);

    void receive(const ViaimConnectCameraFrame& frame) noexcept;
    void cameraState(ViaimConnectCameraState state) noexcept;

private:
    using Clock = std::chrono::steady_clock;
    struct Frame {
        std::vector<uint8_t> pixels;
        uint64_t index;
        bool snapshot;
    };

    void encode() noexcept;

    const std::filesystem::path directory_;
    const std::filesystem::path ffmpeg_;
    ViaimConnectCameraOpenOptions mode_ = VIAIM_CONNECT_CAMERA_OPEN_OPTIONS_INIT;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Frame> queue_;
    std::thread worker_;
    Clock::time_point started_;
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
