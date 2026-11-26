#pragma once

#include "Sdk.h"

#include <filesystem>
#include <functional>
#include <vector>

ViaimConnectCameraOpenOptions openCaptureCamera(ViaimConnectSdk* sdk, const std::function<void()>& beforeTry);

// Returns an empty vector for an invalid frame. The SDK lends the source pixels
// only for the duration of the frame callback.
std::vector<uint8_t> copyBgrCameraFrame(const ViaimConnectCameraFrame& frame,
                                        const ViaimConnectCameraOpenOptions& mode);

void saveCameraBitmap(const std::filesystem::path& path, const std::vector<uint8_t>& pixels,
                      uint32_t width, uint32_t height);

void saveCameraJpeg(const std::filesystem::path& path, std::vector<uint8_t>& pixels,
                    uint32_t width, uint32_t height);
