#include "VideoCapture.h"
#include "RingOverlay.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <syncstream>

namespace {
struct CloseHandleDeleter {
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

Handle checkedHandle(HANDLE handle, const char* operation) {
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        throw std::runtime_error(std::string(operation) + " failed, Windows error " +
                                 std::to_string(GetLastError()));
    return Handle(handle);
}

std::filesystem::path findFfmpeg() {
    std::wstring searchPath(32768, L'\0'), executable(32768, L'\0');
    const DWORD pathLength = GetEnvironmentVariableW(L"PATH", searchPath.data(),
                                                      static_cast<DWORD>(searchPath.size()));
    if (pathLength == 0 || pathLength >= searchPath.size())
        throw std::runtime_error("Cannot read PATH to find ffmpeg.exe");
    const DWORD size = SearchPathW(searchPath.c_str(), L"ffmpeg.exe", nullptr,
                                    static_cast<DWORD>(executable.size()), executable.data(), nullptr);
    if (size == 0 || size >= executable.size())
        throw std::runtime_error("--video requires ffmpeg.exe on PATH");
    executable.resize(size);
    return executable;
}

std::wstring quoted(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

Handle startEncoder(const std::filesystem::path& executable, const std::wstring& arguments,
                    HANDLE input, const std::filesystem::path& log) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    auto output = checkedHandle(CreateFileW(log.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr),
        "Opening the FFmpeg log");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output.get();
    startup.hStdError = output.get();
    PROCESS_INFORMATION child{};
    auto command = quoted(executable) +
        L" -hide_banner -loglevel error -nostats -nostdin -n " + arguments;
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
        throw std::runtime_error("Cannot launch FFmpeg, Windows error " +
                                 std::to_string(GetLastError()));
    CloseHandle(child.hThread);
    return Handle(child.hProcess);
}

void waitEncoder(HANDLE child, DWORD timeout = 30000) {
    const DWORD wait = WaitForSingleObject(child, timeout);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(child, 1);
        WaitForSingleObject(child, 2000);
        throw std::runtime_error("FFmpeg timed out");
    }
    DWORD code = 0;
    if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(child, &code) || code != 0)
        throw std::runtime_error("FFmpeg failed; see video-encoder.log");
}

std::string fragmentName(uint64_t index) {
    return "fragment-" + std::to_string(index) + ".mp4";
}

std::wstring secondsText(uint64_t frames, unsigned fps) {
    std::wostringstream value;
    value.imbue(std::locale::classic());
    value << std::fixed << std::setprecision(9) << double(frames) / fps;
    return value.str();
}

void writeFrame(HANDLE pipe, const std::vector<uint8_t>& pixels) {
    size_t offset = 0;
    while (offset < pixels.size()) {
        DWORD sent = 0;
        const DWORD size = static_cast<DWORD>(pixels.size() - offset);
        if (!WriteFile(pipe, pixels.data() + offset, size, &sent, nullptr) || sent == 0)
            throw std::runtime_error("Could not send a frame to FFmpeg; see video-encoder.log");
        offset += sent;
    }
}

void saveSnapshot(const std::filesystem::path& path, const std::vector<uint8_t>& pixels,
                  uint32_t width, uint32_t height) {
    const uint32_t stride = width * 3;
    const uint32_t paddedStride = (stride + 3) & ~3u;
    BITMAPINFOHEADER image{};
    image.biSize = sizeof(image);
    image.biWidth = static_cast<LONG>(width);
    image.biHeight = -static_cast<LONG>(height);
    image.biPlanes = 1;
    image.biBitCount = 24;
    image.biSizeImage = paddedStride * height;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(image);
    file.bfSize = file.bfOffBits + image.biSizeImage;

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&image), sizeof(image));
    constexpr char padding[3]{};
    for (uint32_t row = 0; row < height; ++row) {
        out.write(reinterpret_cast<const char*>(pixels.data() + size_t(row) * stride), stride);
        out.write(padding, paddedStride - stride);
    }
    out.close();
    if (!out) throw std::runtime_error("Could not save snapshot");
}
} // namespace

VideoCapture::VideoCapture(const std::filesystem::path& directory, bool microphone,
                           unsigned segmentSeconds, bool ringOverlay, bool ringDemo)
    : directory_(std::filesystem::absolute(directory)), ffmpeg_(findFfmpeg()),
      audio_(directory_ / (microphone ? "mic.wav" : "spk.wav")), microphone_(microphone),
      segmentSeconds_(segmentSeconds), fragmentSeconds_(segmentSeconds % 2 == 0 ? 2 : 1),
      ringOverlay_(ringOverlay), ringReading_{0, ringDemo} {
    std::filesystem::create_directory(directory_ / ".video-chunks");
    std::filesystem::create_directory(directory_ / "segments");
}

VideoCapture::~VideoCapture() {
    stop();
    if (worker_.joinable()) worker_.join();
    if (exporter_.joinable()) {
        {
            std::lock_guard lock(mutex_);
            finalized_ = true;
        }
        exporter_.join();
    }
}

void VideoCapture::open(ViaimConnectSdk* sdk) {
    ViaimConnectCameraEnumeration* result = nullptr;
    const auto status = viaim_connect_camera_enumerate(sdk, &result);
    if (status != VIAIM_CONNECT_OK)
        throw std::runtime_error("Camera enumeration failed: " + std::to_string(status));
    std::unique_ptr<ViaimConnectCameraEnumeration, decltype(&viaim_connect_camera_enumeration_destroy)>
        enumeration(result, viaim_connect_camera_enumeration_destroy);
    ViaimConnectCameraDeviceInfo info = VIAIM_CONNECT_CAMERA_DEVICE_INFO_INIT;
    if (viaim_connect_camera_enumeration_get_device_info(enumeration.get(), &info) != VIAIM_CONNECT_OK)
        throw std::runtime_error("Could not read camera capabilities");

    struct Candidate {
        ViaimConnectCameraOpenOptions options;
        int score;
    };
    std::vector<Candidate> candidates;
    for (uint32_t i = 0; i < info.capability_count; ++i) {
        ViaimConnectCameraCapability cap = VIAIM_CONNECT_CAMERA_CAPABILITY_INIT;
        if (viaim_connect_camera_enumeration_get_capability(enumeration.get(), i, &cap) != VIAIM_CONNECT_OK ||
            cap.pixel_format != VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_BGR24 ||
            cap.min_width == 0 || cap.min_width > 1920 || cap.min_width % 2 != 0 ||
            cap.min_height == 0 || cap.min_height > 1080 || cap.min_height % 2 != 0 ||
            cap.min_fps == 0 || cap.min_fps > 60 || cap.min_fps > cap.max_fps)
            continue;
        const auto highestFps = std::min(cap.max_fps, 60u);
        for (const auto fps : {std::clamp(30u, cap.min_fps, highestFps),
                               std::clamp(15u, cap.min_fps, highestFps), cap.min_fps}) {
            const auto duplicate = std::ranges::any_of(candidates, [&](const Candidate& candidate) {
                const auto& options = candidate.options;
                return options.width == cap.min_width && options.height == cap.min_height &&
                       options.fps == fps;
            });
            if (duplicate) continue;
            ViaimConnectCameraOpenOptions options = VIAIM_CONNECT_CAMERA_OPEN_OPTIONS_INIT;
            options.width = cap.min_width;
            options.height = cap.min_height;
            options.fps = fps;
            const int score = std::abs(static_cast<int>(options.width) - 1280) +
                              std::abs(static_cast<int>(options.height) - 720) +
                              10 * std::abs(static_cast<int>(fps) - 30);
            candidates.push_back({options, score});
        }
    }
    if (candidates.empty()) throw std::runtime_error("No supported BGR24 camera mode for recording");
    std::ranges::stable_sort(candidates, {}, &Candidate::score);

    ViaimConnectResult opened = VIAIM_CONNECT_OK;
    for (const auto& candidate : candidates) {
        const auto& options = candidate.options;
        const auto description = std::to_string(options.width) + "x" + std::to_string(options.height)
                                 + "@" + std::to_string(options.fps);
        std::cout << "CAMERA_TRY=" << description << std::endl;
        {
            std::lock_guard lock(mutex_);
            cameraFailed_ = false;
        }
        opened = viaim_connect_camera_open_with_options(sdk, enumeration.get(), &options);
        if (opened == VIAIM_CONNECT_OK) {
            mode_ = options;
            std::cout << "CAMERA_MODE=" << description << std::endl;
            worker_ = std::thread(&VideoCapture::encode, this);
            return;
        }
        std::cerr << "CAMERA_MODE_FAILED=" << description << " CODE=" << opened << std::endl;
        if (opened != VIAIM_CONNECT_ERROR_IO && opened != VIAIM_CONNECT_ERROR_INVALID_ARGUMENT) break;
        const auto closed = viaim_connect_camera_close(sdk);
        if (closed != VIAIM_CONNECT_OK)
            throw std::runtime_error("Could not reset failed camera stream: " + std::to_string(closed));
    }
    throw std::runtime_error("Camera open failed: " + std::to_string(opened) +
        "; close camera previews in browsers and meeting apps, then check Windows camera access");
}

void VideoCapture::start() {
    std::lock_guard lock(mutex_);
    started_ = Clock::now();
    recording_ = true;
    exporter_ = std::thread(&VideoCapture::exportClips, this);
}

bool VideoCapture::requestHighlight(Clock::time_point marked) {
    return queueHighlight(marked, marked, false);
}

bool VideoCapture::requestRingHighlight(ring::Episode episode) {
    return queueHighlight(episode.begin, episode.end, true);
}

void VideoCapture::ringReading(ring::Reading reading) {
    std::lock_guard lock(mutex_);
    ringReading_ = reading;
}

uint64_t VideoCapture::frameAt(Clock::time_point time) const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(time - started_);
    return static_cast<uint64_t>(std::max<int64_t>(0, elapsed.count())) * mode_.fps / 1000;
}

bool VideoCapture::queueHighlight(Clock::time_point begin, Clock::time_point end, bool fromRing) {
    unsigned id;
    {
        std::lock_guard lock(mutex_);
        if (!recording_ || cameraFailed_ || encodeFailed_ || highlights_.size() >= 32) return false;
        constexpr auto padding = std::chrono::seconds(20);
        id = highlightCount_ + 1;
        highlights_.push_back({id, frameAt(begin - padding), frameAt(end + padding), fromRing});
        highlightCount_ = id;
    }
    std::osyncstream(std::cout) << (fromRing ? "RING_HIGHLIGHT_MARKED=" : "HIGHLIGHT_MARKED=")
                                << id << '\n' << std::flush_emit;
    return true;
}

void VideoCapture::audioWritten(bool microphone, uint64_t bytes) noexcept {
    if (microphone == microphone_) audioBytes_ = bytes;
}

bool VideoCapture::failed() {
    std::lock_guard lock(mutex_);
    return cameraFailed_ || encodeFailed_;
}

bool VideoCapture::requestSnapshot() {
    std::lock_guard lock(mutex_);
    if (!recording_ || cameraFailed_ || encodeFailed_ || snapshotPending_) return false;
    snapshotPending_ = true;
    return true;
}

void VideoCapture::stop() {
    {
        std::lock_guard lock(mutex_);
        recording_ = false;
        stopped_ = true;
        if (snapshotPending_) snapshotFailed_ = true;
    }
    ready_.notify_one();
}

void VideoCapture::cameraState(ViaimConnectCameraState state) noexcept {
    if (state != VIAIM_CONNECT_CAMERA_DISCONNECTED && state != VIAIM_CONNECT_CAMERA_ERROR) return;
    std::lock_guard lock(mutex_);
    cameraFailed_ = true;
}

void VideoCapture::receive(const ViaimConnectCameraFrame& frame) noexcept {
    const auto captured = Clock::now();
    std::lock_guard lock(mutex_);
    if (!recording_ || encodeFailed_) return;
    const size_t packedStride = size_t(mode_.width) * 3;
    const size_t stride = frame.strides[0];
    if (frame.pixel_format != VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_BGR24 || frame.plane_count != 1 ||
        frame.width != mode_.width || frame.height != mode_.height || stride < packedStride ||
        !frame.planes[0].data || frame.planes[0].size != uint64_t(stride) * frame.height) {
        ++dropped_;
        return;
    }
    if (queue_.size() >= 5) {
        ++dropped_;
        return;
    }
    try {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(captured - started_).count();
        if (ms < 0) return;
        const uint64_t index = (static_cast<uint64_t>(ms) * mode_.fps + 500) / 1000;
        Frame copy{std::vector<uint8_t>(packedStride * frame.height), index, snapshotPending_, ringReading_};
        const bool flipped = (frame.flags & VIAIM_CONNECT_CAMERA_FRAME_FLAG_VERTICAL_FLIPPED) != 0;
        for (uint32_t row = 0; row < frame.height; ++row) {
            const auto sourceRow = flipped ? frame.height - 1 - row : row;
            std::memcpy(copy.pixels.data() + row * packedStride,
                        frame.planes[0].data + sourceRow * stride, packedStride);
        }
        queue_.push_back(std::move(copy));
        snapshotPending_ = false;
        ready_.notify_one();
    } catch (const std::exception&) {
        ++dropped_;
    }
}

void VideoCapture::encode() noexcept {
    try {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &inherit, 0))
            throw std::runtime_error("Cannot open FFmpeg input pipe");
        Handle input(readPipe), writer(writePipe);
        if (!SetHandleInformation(writer.get(), HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error("Cannot configure FFmpeg input pipe");

        const auto size = std::to_wstring(mode_.width) + L"x" + std::to_wstring(mode_.height);
        const auto keyInterval = std::to_wstring(mode_.fps * fragmentSeconds_);
        auto child = startEncoder(ffmpeg_, L"-f rawvideo -pixel_format bgr24 -video_size " + size +
            L" -framerate " + std::to_wstring(mode_.fps) +
            L" -i pipe:0 -c:v libx264 -threads 2 -preset ultrafast -tune zerolatency -crf 21 -pix_fmt yuv420p"
            L" -g " + keyInterval + L" -keyint_min " + keyInterval + L" -sc_threshold 0 -bf 0"
            L" -f segment -segment_time " + std::to_wstring(fragmentSeconds_) + L" -reset_timestamps 1 " +
            quoted(directory_ / ".video-chunks" / "fragment-%d.mp4"),
            input.get(), directory_ / "video-encoder.log");
        input.reset();

        std::vector<uint8_t> last;
        for (;;) {
            Frame frame{};
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
                if (queue_.empty()) break;
                frame = std::move(queue_.front());
                queue_.pop_front();
            }
            if (frame.snapshot) {
                const auto name = "snapshot-" + std::to_string(++snapshots_) + ".bmp";
                try {
                    saveSnapshot(directory_ / name, frame.pixels, mode_.width, mode_.height);
                    std::cout << "SNAPSHOT_SAVED=" << name << std::endl;
                } catch (const std::exception& error) {
                    std::lock_guard lock(mutex_);
                    snapshotFailed_ = true;
                    std::cerr << "SNAPSHOT_ERROR: " << error.what() << std::endl;
                }
            }
            if (frame.index < written_) continue;
            if (ringOverlay_)
                ring::drawOverlay(frame.pixels, mode_.width, mode_.height,
                                  double(frame.index) / mode_.fps, frame.pulse);
            // Keep camera time in sync with the session even if some frames arrive late or are dropped.
            while (written_ < frame.index) {
                writeFrame(writer.get(), last.empty() ? frame.pixels : last);
                ++written_;
            }
            writeFrame(writer.get(), frame.pixels);
            ++written_;
            last = std::move(frame.pixels);
        }
        writer.reset();
        waitEncoder(child.get());
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_);
        encodeFailed_ = true;
        std::cerr << "VIDEO_ERROR: " << error.what() << std::endl;
    }
}

bool VideoCapture::finish() {
    stop();
    if (!worker_.joinable()) return false;
    worker_.join();
    {
        std::lock_guard lock(mutex_);
        finalized_ = true;
    }
    if (exporter_.joinable()) exporter_.join();
    std::cout << "VIDEO_FRAMES=" << written_ << " VIDEO_DROPPED=" << dropped_ << std::endl;
    if (cameraFailed_) std::cerr << "VIDEO_ERROR: Camera reported an error or disconnected" << std::endl;
    if (snapshotFailed_) std::cerr << "SNAPSHOT_ERROR: A requested snapshot could not be saved" << std::endl;
    return !encodeFailed_ && !cameraFailed_ && !snapshotFailed_ && !exportFailed_ && written_ > 0;
}

void VideoCapture::exportRange(const std::string& name, uint64_t begin, uint64_t end, bool transcode) {
    const auto chunks = directory_ / ".video-chunks";
    const auto framesPerFragment = uint64_t(mode_.fps) * fragmentSeconds_;
    const auto first = begin / framesPerFragment;
    const auto last = (end - 1) / framesPerFragment;
    const auto index = chunks / "export.ffconcat";
    {
        std::ofstream list(index);
        list << "ffconcat version 1.0\n";
        for (auto i = first; i <= last; ++i)
            list << "file '" << fragmentName(i) << "'\nduration " << fragmentSeconds_ << "\n";
        list.close();
        if (!list) throw std::runtime_error("Could not write fragment index");
    }

    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    auto input = checkedHandle(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr), "Opening FFmpeg's empty input");
    const auto temporary = chunks / "export.mp4";
    // The concat demuxer can skip to the next keyframe on a non-aligned seek.
    // Decode from the first fragment and trim by frame for highlights instead.
    const auto videoOptions = transcode
        ? L" -vf trim=start_frame=" + std::to_wstring(begin % framesPerFragment) +
          L",setpts=PTS-STARTPTS -c:v libx264 -threads 2 -preset ultrafast -crf 21"
        : L" -c:v copy";
    auto child = startEncoder(ffmpeg_,
        L"-f concat -i " + quoted(index) +
        // The WAV header is finalized at stop; read the flushed PCM directly during recording.
        L" -f s16le -ar 16000 -ac 1 -skip_initial_bytes 44 -ss " + secondsText(begin, mode_.fps) +
        L" -i " + quoted(audio_) + L" -map 0:v:0 -map 1:a:0 -t " + secondsText(end - begin, mode_.fps) +
        videoOptions +
        L" -af pan=stereo|c0=c0|c1=c0,loudnorm=I=-18:TP=-1.5:LRA=11,apad"
        L" -ar 48000 -ac 2 -c:a aac -b:a 128k -movflags +faststart " + quoted(temporary),
        input.get(), directory_ / "video-encoder.log");
    try {
        waitEncoder(child.get(), 300000);
        std::filesystem::rename(temporary, directory_ / name);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void VideoCapture::exportClips() noexcept {
    try {
        constexpr uint64_t pcmBytesPerSecond = 32000;
        const uint64_t fragmentFrames = uint64_t(fragmentSeconds_) * mode_.fps;
        const uint64_t segmentFrames = uint64_t(segmentSeconds_) * mode_.fps;
        uint64_t closedFragments = 0, nextSegment = 0;
        const auto chunks = directory_ / ".video-chunks";
        const auto exportClip = [&](const std::string& name, uint64_t begin, uint64_t end, bool trim) {
            try {
                if (end <= begin) throw std::runtime_error("No audio and video overlap at this marker");
                exportRange(name, begin, end, trim);
                std::osyncstream(std::cout) << "CLIP_SAVED=" << name << '\n' << std::flush_emit;
            } catch (const std::exception& error) {
                exportFailed_ = true;
                std::osyncstream(std::cerr) << "VIDEO_EXPORT_ERROR=" << name << ": "
                                            << error.what() << '\n';
            }
        };
        for (;;) {
            bool final, encoderFailed;
            uint64_t finalFrames = 0;
            {
                std::lock_guard lock(mutex_);
                final = finalized_;
                encoderFailed = encodeFailed_;
                if (final) finalFrames = written_;
            }
            // The segment muxer closes a fragment before opening its successor.
            while (!final && std::filesystem::exists(chunks / fragmentName(closedFragments + 1)))
                ++closedFragments;
            const uint64_t videoFrames = final ? finalFrames : closedFragments * fragmentFrames;
            const uint64_t audioFrames = audioBytes_.load() * mode_.fps / pcmBytesPerSecond;
            const uint64_t available = std::min(videoFrames, audioFrames);
            Highlight highlight{};
            {
                std::lock_guard lock(mutex_);
                const auto ready = std::ranges::find_if(highlights_, [&](const Highlight& item) {
                    return final || item.end <= available;
                });
                if (ready != highlights_.end()) {
                    highlight = *ready;
                    highlights_.erase(ready);
                }
            }
            if (highlight.id) {
                const auto prefix = highlight.fromRing ? "highlight-ring-" : "highlight-";
                exportClip(prefix + std::to_string(highlight.id) + ".mp4", highlight.begin,
                           std::min(highlight.end, available), true);
                continue;
            }
            if (nextSegment + segmentFrames <= available || (final && nextSegment < available)) {
                const auto end = std::min(nextSegment + segmentFrames, available);
                exportClip("segments/segment-" + std::to_string(nextSegment / segmentFrames + 1) + ".mp4",
                           nextSegment, end, false);
                nextSegment = end;
                continue;
            }
            if (final) {
                if (available && !encoderFailed) exportClip("video.mp4", 0, videoFrames, false);
                else exportFailed_ = true;
                if (!exportFailed_) {
                    std::error_code ignored;
                    for (uint64_t frame = 0; frame < videoFrames; frame += fragmentFrames)
                        std::filesystem::remove(chunks / fragmentName(frame / fragmentFrames), ignored);
                    std::filesystem::remove(chunks / "export.ffconcat", ignored);
                    std::filesystem::remove(chunks, ignored);
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const std::exception& error) {
        exportFailed_ = true;
        std::cerr << "VIDEO_EXPORT_ERROR: " << error.what() << std::endl;
    }
}
