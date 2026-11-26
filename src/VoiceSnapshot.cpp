#include "VoiceSnapshot.h"
#include "VideoCapture.h"

#include <sherpa-onnx/c-api/c-api.h>
#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <regex>
#include <stdexcept>

namespace {
constexpr int sampleRate = 16000;
constexpr size_t frameSize = sampleRate / 50;
constexpr size_t maxQueuedBytes = sampleRate * 2;

const SherpaOnnxOfflineRecognizer* createRecognizer() {
    wchar_t* overridePath = nullptr;
    _wdupenv_s(&overridePath, nullptr, L"CONNECT_VOICE_MODEL_DIR");
    const std::unique_ptr<wchar_t, decltype(&std::free)> overrideOwner(overridePath, std::free);
    const std::filesystem::path directory = overridePath && *overridePath
        ? std::filesystem::path(overridePath) : std::filesystem::path(CONNECT_VOICE_MODEL_DIR);
    const auto model = directory / "model.int8.onnx";
    const auto tokens = directory / "tokens.txt";
    if (directory.empty() || !std::filesystem::is_regular_file(model) ||
        !std::filesystem::is_regular_file(tokens))
        throw std::runtime_error("Set CONNECT_VOICE_MODEL_DIR to the local SenseVoice model directory");

    const auto modelPath = model.u8string();
    const auto tokensPath = tokens.u8string();
    SherpaOnnxOfflineRecognizerConfig config{};
    config.feat_config.sample_rate = sampleRate;
    config.feat_config.feature_dim = 80;
    config.model_config.sense_voice.model = reinterpret_cast<const char*>(modelPath.c_str());
    config.model_config.sense_voice.language = "zh";
    config.model_config.sense_voice.use_itn = 1;
    config.model_config.tokens = reinterpret_cast<const char*>(tokensPath.c_str());
    config.model_config.num_threads = 2;
    config.model_config.provider = "cpu";
    config.decoding_method = "greedy_search";
    const auto recognizer = SherpaOnnxCreateOfflineRecognizer(&config);
    if (!recognizer) throw std::runtime_error("Could not load the SenseVoice model");
    return recognizer;
}

std::string stripCommandPunctuation(std::string text) {
    for (const std::string punctuation : {" ", "\t", "\r", "\n", "，", "。", "！", "？", "、", ",", ".", "!", "?"}) {
        for (auto position = text.find(punctuation); position != std::string::npos;
             position = text.find(punctuation, position))
            text.erase(position, punctuation.size());
    }
    return text;
}

bool isPhotoCommand(const std::string& text) {
    static const std::regex command(
        "(请|請|麻烦|麻煩)?(帮我|幫我|给我|給我)?"
        "(拍照|照相|拍(一)?(张|張)(照片|相片|照)|照(一)?(张|張)(照片|相片|照)?|"
        "拍(个|個)(照|照片)|照(个|個)相|拍一下|截((一)?(张|張)|个|個)?(图|圖))"
        "(一下|吧|谢谢|謝謝)?");
    return std::regex_match(text, command);
}

bool isHighlightCommand(const std::string& text) {
    static const std::regex command(
        "(请|請|麻烦|麻煩)?(帮我|幫我|给我|給我)?"
        "(保存(一下)?(精彩片段|高光片段|这段视频|這段影片)|标记(精彩片段|高光片段))"
        "(吧|谢谢|謝謝)?");
    return std::regex_match(text, command);
}
} // namespace

VoiceSnapshot::VoiceSnapshot(VideoCapture& video, void (*log)(const std::string&))
    : video_(&video), log_(log), recognizer_(createRecognizer()) {}

VoiceSnapshot::VoiceSnapshot(void (*log)(const std::string&))
    : video_(nullptr), log_(log), recognizer_(createRecognizer()) {}

VoiceSnapshot::~VoiceSnapshot() {
    stop();
    SherpaOnnxDestroyOfflineRecognizer(recognizer_);
}

void VoiceSnapshot::start() {
    worker_ = std::thread(&VoiceSnapshot::listen, this);
}

void VoiceSnapshot::submit(const std::vector<uint8_t>& pcm) noexcept {
    std::lock_guard lock(mutex_);
    if (stopping_ || paused_) return;
    if (pcm.size() > maxQueuedBytes - queuedBytes_) {
        queue_.clear();
        queuedBytes_ = 0;
        gap_ = true;
        return;
    }
    try {
        queue_.push_back(pcm);
        queuedBytes_ += pcm.size();
        ready_.notify_one();
    } catch (const std::exception&) {
        gap_ = true;
    }
}

void VoiceSnapshot::resume() {
    std::lock_guard lock(mutex_);
    paused_ = false;
}

void VoiceSnapshot::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void VoiceSnapshot::listen() noexcept {
    try {
        std::array<float, frameSize> frame{};
        std::deque<float> preRoll;
        std::vector<float> utterance;
        using Clock = std::chrono::steady_clock;
        Clock::time_point onset;
        size_t filled = 0, voiced = 0, silence = 0;
        bool discard = false;
        const size_t silenceLimit = video_ ? 16 : 28;
        const size_t utteranceLimit = (video_ ? 3 : 20) * sampleRate;

        for (;;) {
            std::vector<uint8_t> pcm;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                if (gap_) {
                    filled = voiced = silence = 0;
                    preRoll.clear();
                    utterance.clear();
                    gap_ = false;
                    discard = true;
                }
                pcm = std::move(queue_.front());
                queue_.pop_front();
                queuedBytes_ -= pcm.size();
            }
            const auto blockEnd = Clock::now();
            for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
                const auto bits = static_cast<uint16_t>(pcm[i] | (pcm[i + 1] << 8));
                frame[filled++] = std::bit_cast<int16_t>(bits) / 32768.0f;
                if (filled < frame.size()) continue;
                filled = 0;
                float energy = 0;
                for (const float value : frame) energy += value * value;
                const bool speech = energy / frameSize >= 0.002f * 0.002f;
                silence = speech ? 0 : (silence < silenceLimit ? silence + 1 : silenceLimit);

                if (discard) {
                    if (silence >= silenceLimit) discard = false;
                    continue;
                }
                if (utterance.empty() && !speech) {
                    preRoll.insert(preRoll.end(), frame.begin(), frame.end());
                    if (preRoll.size() > 6 * frameSize)
                        preRoll.erase(preRoll.begin(), preRoll.begin() + frameSize);
                    continue;
                }
                if (utterance.empty()) {
                    onset = blockEnd - std::chrono::microseconds(
                        1000000LL * (pcm.size() - i) / (sampleRate * 2))
                        - std::chrono::milliseconds(20);
                    utterance.assign(preRoll.begin(), preRoll.end());
                    preRoll.clear();
                }
                utterance.insert(utterance.end(), frame.begin(), frame.end());
                if (speech) ++voiced;
                if (silence >= silenceLimit || utterance.size() >= utteranceLimit) {
                    if ((silence >= silenceLimit || !video_) && voiced >= 6) recognize(utterance, onset);
                    discard = video_ && silence < silenceLimit;
                    utterance.clear();
                    voiced = 0;
                }
            }
        }
    } catch (const std::exception& error) {
        log_(std::string("VOICE_SNAPSHOT_ERROR: ") + error.what());
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
}

void VoiceSnapshot::recognize(const std::vector<float>& samples,
                              std::chrono::steady_clock::time_point onset) {
    std::unique_ptr<const SherpaOnnxOfflineStream, decltype(&SherpaOnnxDestroyOfflineStream)>
        stream(SherpaOnnxCreateOfflineStream(recognizer_), SherpaOnnxDestroyOfflineStream);
    if (!stream) throw std::runtime_error("Could not create a recognition stream");
    SherpaOnnxAcceptWaveformOffline(stream.get(), sampleRate, samples.data(),
                                  static_cast<int32_t>(samples.size()));
    SherpaOnnxDecodeOfflineStream(recognizer_, stream.get());
    std::unique_ptr<const SherpaOnnxOfflineRecognizerResult,
                    decltype(&SherpaOnnxDestroyOfflineRecognizerResult)>
        result(SherpaOnnxGetOfflineStreamResult(stream.get()), SherpaOnnxDestroyOfflineRecognizerResult);
    if (!result || !result->text || !*result->text) return;
    std::string text(result->text);

    std::lock_guard lock(mutex_);
    if (stopping_ || gap_ || paused_) return;
    if (video_) {
        const auto command = stripCommandPunctuation(text);
        if (isHighlightCommand(command)) {
            if (!video_->requestHighlight(onset)) log_("HIGHLIGHT_UNAVAILABLE");
        } else if (isPhotoCommand(command) && video_->requestSnapshot()) {
            log_("VOICE_SNAPSHOT_REQUESTED=" + text);
        }
        return;
    }
    // Do not transcribe the response played by the speaker.
    paused_ = true;
    queue_.clear();
    queuedBytes_ = 0;
    for (char& character : text) {
        if (character == '\r' || character == '\n') character = ' ';
    }
    log_("VOICE_CHAT_TEXT=" + text);
}
