#include "CameraCaptureSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

ViaimConnectCameraOpenOptions openCaptureCamera(ViaimConnectSdk* sdk) {
    ViaimConnectCameraEnumeration* result = nullptr;
    const auto status = viaim_connect_camera_enumerate(sdk, &result);
    if (status != VIAIM_CONNECT_OK) {
        throw std::runtime_error("Camera enumeration failed: " + std::to_string(status));
    }
    std::unique_ptr<ViaimConnectCameraEnumeration, decltype(&viaim_connect_camera_enumeration_destroy)>
        enumeration(result, viaim_connect_camera_enumeration_destroy);
    ViaimConnectCameraDeviceInfo info = VIAIM_CONNECT_CAMERA_DEVICE_INFO_INIT;
    if (viaim_connect_camera_enumeration_get_device_info(enumeration.get(), &info) != VIAIM_CONNECT_OK) {
        throw std::runtime_error("Could not read camera capabilities");
    }

    auto bestScore = std::numeric_limits<int>::max();
    ViaimConnectCameraOpenOptions mode = VIAIM_CONNECT_CAMERA_OPEN_OPTIONS_INIT;
    for (uint32_t i = 0; i < info.capability_count; ++i) {
        ViaimConnectCameraCapability cap = VIAIM_CONNECT_CAMERA_CAPABILITY_INIT;
        if (viaim_connect_camera_enumeration_get_capability(enumeration.get(), i, &cap) != VIAIM_CONNECT_OK ||
            cap.pixel_format != VIAIM_CONNECT_CAMERA_PIXEL_FORMAT_BGR24 ||
            cap.min_width == 0 || cap.min_width > 1920 || cap.min_width % 2 != 0 ||
            cap.min_height == 0 || cap.min_height > 1080 || cap.min_height % 2 != 0 ||
            cap.min_fps == 0 || cap.min_fps > 60 || cap.min_fps > cap.max_fps) {
            continue;
        }
        const auto fps = std::clamp(30u, cap.min_fps, cap.max_fps);
        const int score = std::abs(static_cast<int>(cap.min_width) - 1280) +
                          std::abs(static_cast<int>(cap.min_height) - 720) +
                          10 * std::abs(static_cast<int>(fps) - 30);
        if (score < bestScore) {
            bestScore = score;
            mode.width = cap.min_width;
            mode.height = cap.min_height;
            mode.fps = fps;
        }
    }
    if (mode.fps == 0) {
        throw std::runtime_error("No supported BGR24 camera mode found");
    }
    const auto opened = viaim_connect_camera_open_with_options(sdk, enumeration.get(), &mode);
    if (opened != VIAIM_CONNECT_OK) {
        throw std::runtime_error("Camera open failed: " + std::to_string(opened));
    }
    return mode;
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
