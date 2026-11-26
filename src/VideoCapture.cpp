#include "VideoCapture.h"
#include "CameraCaptureSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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

void waitEncoder(HANDLE child) {
    const DWORD wait = WaitForSingleObject(child, 30000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(child, 1);
        WaitForSingleObject(child, 2000);
        throw std::runtime_error("FFmpeg did not finish within 30 seconds");
    }
    DWORD code = 0;
    if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(child, &code) || code != 0)
        throw std::runtime_error("FFmpeg failed; see video-encoder.log");
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

} // namespace

VideoCapture::VideoCapture(const std::filesystem::path& directory)
    : directory_(std::filesystem::absolute(directory)), ffmpeg_(findFfmpeg()) {}

VideoCapture::~VideoCapture() {
    stop();
    if (worker_.joinable()) worker_.join();
}

void VideoCapture::open(ViaimConnectSdk* sdk) {
    mode_ = openCaptureCamera(sdk);
    std::cout << "CAMERA_MODE=" << mode_.width << 'x' << mode_.height << '@' << mode_.fps << std::endl;
    worker_ = std::thread(&VideoCapture::encode, this);
}

void VideoCapture::start() {
    std::lock_guard lock(mutex_);
    started_ = Clock::now();
    recording_ = true;
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
    if (queue_.size() >= 5) {
        ++dropped_;
        return;
    }
    try {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(captured - started_).count();
        if (ms < 0) return;
        const uint64_t index = (static_cast<uint64_t>(ms) * mode_.fps + 500) / 1000;
        auto pixels = copyBgrCameraFrame(frame, mode_);
        if (pixels.empty()) {
            ++dropped_;
            return;
        }
        queue_.push_back({std::move(pixels), index, snapshotPending_});
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
        auto child = startEncoder(ffmpeg_, L"-f rawvideo -pixel_format bgr24 -video_size " + size +
            L" -framerate " + std::to_wstring(mode_.fps) +
            L" -i pipe:0 -c:v libx264 -preset ultrafast -crf 21 -pix_fmt yuv420p " +
            quoted(directory_ / ".video-track.mp4"), input.get(), directory_ / "video-encoder.log");
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
                    saveCameraBitmap(directory_ / name, frame.pixels, mode_.width, mode_.height);
                    std::cout << "SNAPSHOT_SAVED=" << name << std::endl;
                } catch (const std::exception& error) {
                    std::lock_guard lock(mutex_);
                    snapshotFailed_ = true;
                    std::cerr << "SNAPSHOT_ERROR: " << error.what() << std::endl;
                }
            }
            if (frame.index < written_) continue;
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

bool VideoCapture::finish(const std::filesystem::path& audio) {
    stop();
    if (!worker_.joinable()) return false;
    worker_.join();
    std::cout << "VIDEO_FRAMES=" << written_ << " VIDEO_DROPPED=" << dropped_ << std::endl;
    if (cameraFailed_) std::cerr << "VIDEO_ERROR: Camera reported an error or disconnected" << std::endl;
    if (snapshotFailed_) std::cerr << "SNAPSHOT_ERROR: A requested snapshot could not be saved" << std::endl;
    if (encodeFailed_ || written_ == 0) return false;

    const auto videoTrack = directory_ / ".video-track.mp4";
    const auto finalVideo = directory_ / "video.mp4";
    try {
        if (!std::filesystem::exists(audio))
            throw std::runtime_error("Audio file is missing; keeping the video-only track");
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        auto input = checkedHandle(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr), "Opening FFmpeg's empty input");
        auto child = startEncoder(ffmpeg_, L"-i " + quoted(videoTrack) + L" -i " +
            quoted(std::filesystem::absolute(audio)) +
            L" -map 0:v:0 -map 1:a:0 -c:v copy -c:a aac -movflags +faststart " + quoted(finalVideo),
            input.get(), directory_ / "video-encoder.log");
        waitEncoder(child.get());
        std::error_code unused;
        std::filesystem::remove(videoTrack, unused);
        std::cout << "VIDEO_SAVED=video.mp4" << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "VIDEO_ERROR: " << error.what() << std::endl;
        return false;
    }
    return !cameraFailed_ && !snapshotFailed_;
}
