#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class VideoCapture;
struct SherpaOnnxOfflineRecognizer;

class VoiceSnapshot {
public:
    VoiceSnapshot(VideoCapture& video, void (*log)(const std::string&));
    explicit VoiceSnapshot(void (*log)(const std::string&));
    ~VoiceSnapshot();

    void start();
    void submit(const std::vector<uint8_t>& pcm) noexcept;
    void resume();
    void stop();

private:
    void listen() noexcept;
    void recognize(const std::vector<float>& samples);

    VideoCapture* video_;
    void (*log_)(const std::string&);
    const SherpaOnnxOfflineRecognizer* recognizer_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::vector<uint8_t>> queue_;
    std::thread worker_;
    size_t queuedBytes_ = 0;
    bool stopping_ = false;
    bool gap_ = false;
    bool paused_ = false;
};
