#include "CameraCaptureSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    std::filesystem::path output;
    unsigned long vid = 0;
    unsigned long pid = 0;
    std::chrono::seconds timeout{12};
    bool jpeg = false;
};

unsigned long parseNumber(const std::wstring& value, unsigned long maximum, int base) {
    if (value.empty() || value.front() < L'0' || value.front() > L'9') {
        throw std::runtime_error("Expected a positive number");
    }
    size_t used = 0;
    const auto number = std::stoul(value, &used, base);
    if (used != value.size() || number == 0 || number > maximum) {
        throw std::runtime_error("Numeric option out of range (allowed 1 to " + std::to_string(maximum) + ")");
    }
    return number;
}

Options parseOptions(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            throw std::runtime_error("Missing option value; see --help");
        }
        const std::wstring key = argv[index];
        const std::wstring value = argv[index + 1];
        if (key == L"--output") {
            options.output = value;
        } else if (key == L"--vid") {
            options.vid = parseNumber(value, 65535, value.starts_with(L"0x") || value.starts_with(L"0X") ? 16 : 10);
        } else if (key == L"--pid") {
            options.pid = parseNumber(value, 65535, value.starts_with(L"0x") || value.starts_with(L"0X") ? 16 : 10);
        } else if (key == L"--timeout") {
            options.timeout = std::chrono::seconds(parseNumber(value, 120, 10));
        } else {
            throw std::runtime_error("Unknown option; see --help");
        }
    }
    if (options.output.empty()) {
        throw std::runtime_error("Set --output to a .jpg, .jpeg, or .bmp file");
    }
    if (bool(options.vid) != bool(options.pid)) {
        throw std::runtime_error("Specify both --vid and --pid together");
    }
    options.output = std::filesystem::absolute(options.output);
    const auto extension = options.output.extension().wstring();
    options.jpeg = _wcsicmp(extension.c_str(), L".jpg") == 0 || _wcsicmp(extension.c_str(), L".jpeg") == 0;
    if (!options.jpeg && _wcsicmp(extension.c_str(), L".bmp") != 0) {
        throw std::runtime_error("Set --output to a .jpg, .jpeg, or .bmp file");
    }
    if (!std::filesystem::is_directory(options.output.parent_path())) {
        throw std::runtime_error("The output directory must already exist");
    }
    if (std::filesystem::exists(options.output)) {
        throw std::runtime_error("The output file already exists");
    }
    return options;
}

struct Capture {
    std::mutex mutex;
    std::condition_variable ready;
    bool connected = false;
    bool connectionLost = false;
    bool cameraFailed = false;
    bool copyFailed = false;
    bool accepting = false;
    ViaimConnectCameraOpenOptions mode = VIAIM_CONNECT_CAMERA_OPEN_OPTIONS_INIT;
    std::vector<uint8_t> pixels;
};

void __cdecl onConnection(ViaimConnectConnectionState state, void* data) {
    auto& capture = *static_cast<Capture*>(data);
    std::lock_guard lock(capture.mutex);
    if (state == VIAIM_CONNECT_CONNECTION_CONNECTED) {
        capture.connected = true;
    } else if (capture.connected) {
        capture.connectionLost = true;
    }
    capture.ready.notify_one();
}

void __cdecl onCameraState(ViaimConnectCameraState state, void* data) {
    if (state != VIAIM_CONNECT_CAMERA_DISCONNECTED && state != VIAIM_CONNECT_CAMERA_ERROR) return;
    auto& capture = *static_cast<Capture*>(data);
    std::lock_guard lock(capture.mutex);
    capture.cameraFailed = true;
    capture.ready.notify_one();
}

void __cdecl onFrame(const ViaimConnectCameraFrame* frame, void* data) {
    if (frame == nullptr) return;
    auto& capture = *static_cast<Capture*>(data);
    std::lock_guard lock(capture.mutex);
    if (!capture.accepting || !capture.pixels.empty() || capture.copyFailed) return;
    try {
        capture.pixels = copyBgrCameraFrame(*frame, capture.mode);
    } catch (const std::exception&) {
        capture.copyFailed = true;
    }
    capture.ready.notify_one();
}

void checkSdk(ViaimConnectResult status, const char* operation) {
    if (status != VIAIM_CONNECT_OK) {
        throw std::runtime_error(std::string(operation) + " failed (SDK code " + std::to_string(status) + ")");
    }
}

void captureFrame(Capture& capture, const Options& options) {
    ViaimConnectSdk* instance = nullptr;
    checkSdk(viaim_connect_create(nullptr, &instance), "Initializing Connect");
    std::unique_ptr<ViaimConnectSdk, decltype(&viaim_connect_destroy)> sdk(instance, viaim_connect_destroy);
    viaim_connect_set_log_level(sdk.get(), VIAIM_CONNECT_LOG_OFF);
    viaim_connect_set_raw_log_flags(sdk.get(), VIAIM_CONNECT_RAW_LOG_NONE);

    ViaimConnectCallbacks callbacks = VIAIM_CONNECT_CALLBACKS_INIT;
    callbacks.connection_state = onConnection;
    callbacks.camera_state = onCameraState;
    callbacks.camera_frame = onFrame;
    callbacks.user_data = &capture;

    ViaimConnectOpenOptions connection = VIAIM_CONNECT_OPEN_OPTIONS_INIT;
    if (options.vid) {
        connection.vendor_id = static_cast<uint16_t>(options.vid);
        connection.product_id = static_cast<uint16_t>(options.pid);
    }
    const auto deadline = Clock::now() + options.timeout;
    checkSdk(viaim_connect_open(sdk.get(), &connection, &callbacks), "Opening the Connect device");
    {
        std::unique_lock lock(capture.mutex);
        if (!capture.ready.wait_until(lock, deadline, [&] { return capture.connected; })) {
            throw std::runtime_error("Connect device not detected. Verify the USB cable and VID/PID");
        }
        if (capture.connectionLost) {
            throw std::runtime_error("Connect device disconnected before opening the camera");
        }
    }

    const auto mode = openCaptureCamera(sdk.get(), [&capture] {
        std::lock_guard lock(capture.mutex);
        capture.cameraFailed = false;
    });
    {
        std::unique_lock lock(capture.mutex);
        capture.mode = mode;
        // Wait for a new frame after the SDK has opened the camera successfully.
        capture.accepting = true;
        const bool received = capture.ready.wait_until(lock, deadline, [&] {
            return !capture.pixels.empty() || capture.connectionLost || capture.cameraFailed || capture.copyFailed;
        });
        capture.accepting = false;
        if (!received) {
            throw std::runtime_error("Timed out waiting for a valid camera frame");
        }
        if (capture.connectionLost || capture.cameraFailed) {
            throw std::runtime_error("The Connect camera disconnected or reported an error");
        }
        if (capture.copyFailed) {
            throw std::runtime_error("Could not copy the camera frame");
        }
    }
    checkSdk(viaim_connect_camera_close(sdk.get()), "Closing the camera");
    checkSdk(viaim_connect_close(sdk.get()), "Closing the Connect device");
    checkSdk(viaim_connect_destroy(sdk.release()), "Releasing the Connect SDK");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        std::cout << "connect_frame --output <photo.jpg|photo.bmp> [--vid 0x2207 --pid 0x0017] [--timeout 12]\n";
        return 0;
    }
    Options options;
    try {
        options = parseOptions(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "SNAPSHOT_ARGUMENT_ERROR: " << error.what() << '\n';
        return 2;
    }

    try {
        Capture capture;
        captureFrame(capture, options);
        if (options.jpeg) {
            saveCameraJpeg(options.output, capture.pixels, capture.mode.width, capture.mode.height);
        } else {
            saveCameraBitmap(options.output, capture.pixels, capture.mode.width, capture.mode.height);
        }
        std::cout << "SNAPSHOT_SAVED=" << capture.mode.width << 'x' << capture.mode.height << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SNAPSHOT_ERROR: " << error.what() << '\n';
        return 1;
    }
}
