#include "CameraCaptureSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

ViaimConnectCameraOpenOptions openCaptureCamera(ViaimConnectSdk* sdk, const std::function<void()>& beforeTry) {
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
        beforeTry();
        opened = viaim_connect_camera_open_with_options(sdk, enumeration.get(), &options);
        if (opened == VIAIM_CONNECT_OK) return options;
        std::cerr << "CAMERA_MODE_FAILED=" << description << " CODE=" << opened << std::endl;
        if (opened != VIAIM_CONNECT_ERROR_IO && opened != VIAIM_CONNECT_ERROR_INVALID_ARGUMENT) break;
        const auto closed = viaim_connect_camera_close(sdk);
        if (closed != VIAIM_CONNECT_OK)
            throw std::runtime_error("Could not reset failed camera stream: " + std::to_string(closed));
    }
    throw std::runtime_error("Camera open failed: " + std::to_string(opened) +
        "; close camera previews in browsers and meeting apps, then check Windows camera access");
}

std::vector<uint8_t> copyBgrCameraFrame(const ViaimConnectCameraFrame& frame,
                                        const ViaimConnectCameraOpenOptions& mode) {
    const size_t packedStride = size_t(mode.width) * 3;
    const size_t stride = frame.strides[0];
    if (frame.pixel_format != VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_BGR24 || frame.plane_count != 1 ||
        frame.width != mode.width || frame.height != mode.height || stride < packedStride ||
        !frame.planes[0].data || frame.planes[0].size != uint64_t(stride) * frame.height) {
        return {};
    }

    std::vector<uint8_t> pixels(packedStride * frame.height);
    const bool flipped = (frame.flags & VIAIM_CONNECT_CAMERA_FRAME_FLAG_VERTICAL_FLIPPED) != 0;
    for (uint32_t row = 0; row < frame.height; ++row) {
        const auto sourceRow = flipped ? frame.height - 1 - row : row;
        std::memcpy(pixels.data() + row * packedStride,
                    frame.planes[0].data + sourceRow * stride, packedStride);
    }
    return pixels;
}

void saveCameraBitmap(const std::filesystem::path& path, const std::vector<uint8_t>& pixels,
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
    if (!out) {
        throw std::runtime_error("Could not save snapshot");
    }
}
