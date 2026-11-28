#include "RingAlertFrames.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
constexpr uint32_t width = 16;
constexpr uint32_t height = 8;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class ScratchDirectory {
public:
    ScratchDirectory()
        : parent_(fs::canonical(fs::temp_directory_path())),
          path_(parent_ / ("connect-ring-frames-test-" +
                           std::to_string(ring::Clock::now().time_since_epoch().count()))) {
        require(fs::create_directory(path_), "Could not reserve a temporary directory");
    }

    ~ScratchDirectory() {
        std::error_code ignored;
        const auto resolved = fs::weakly_canonical(path_, ignored);
        if (!ignored && resolved == path_ && resolved.parent_path() == parent_)
            fs::remove_all(resolved, ignored);
    }

    const fs::path& path() const { return path_; }

private:
    const fs::path parent_;
    const fs::path path_;
};

class ComApartment {
public:
    ComApartment() {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "Could not initialize COM");
    }
    ~ComApartment() { CoUninitialize(); }
};

std::string awaitMarker(const fs::path& directory) {
    const auto marker = directory / "ready";
    const auto deadline = ring::Clock::now() + 5s;
    while (!fs::exists(marker) && ring::Clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    std::ifstream input(marker);
    require(input.is_open(), "No ready marker while the sampler was running");
    require(!fs::exists(directory / "ready.tmp"), "A marker was published before its temporary file was renamed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<int64_t> markerTimes(const std::string& marker) {
    constexpr char field[] = "\"times_ms\":[";
    const auto position = marker.find(field);
    require(position != std::string::npos, "Ready marker is missing timestamps");
    std::istringstream values(marker.substr(position + sizeof(field) - 1));
    std::vector<int64_t> times;
    int64_t time;
    char separator;
    while (values >> time >> separator) {
        times.push_back(time);
        if (separator == ']') return times;
        require(separator == ',', "Ready marker timestamps have an invalid separator");
    }
    throw std::runtime_error("Ready marker timestamps are incomplete");
}

void checkBatch(const std::string& marker, unsigned episode, unsigned batch, bool demo = false) {
    const auto metadata = "\"episode\":" + std::to_string(episode) + ",\"batch\":" + std::to_string(batch) + ',';
    require(marker.find("\"version\":1,") != std::string::npos, "Ready marker contract version changed");
    require(marker.find(metadata) != std::string::npos, "Ready marker refers to a different episode or batch");
    require(marker.find(demo ? "\"demo\":true," : "\"demo\":false,") != std::string::npos,
            "Ready marker has an incorrect demo flag");
}

void checkJpeg(const fs::path& path) {
    ComPtr<IWICImagingFactory> factory;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(factory.GetAddressOf()))), "Could not create a WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    require(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())),
            "A published frame cannot be decoded");
    GUID format{};
    require(SUCCEEDED(decoder->GetContainerFormat(&format)) && IsEqualGUID(format, GUID_ContainerFormatJpeg),
            "A published image was encoded in the wrong format");
    ComPtr<IWICBitmapFrameDecode> frame;
    require(SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())), "The JPEG has no frame");
    UINT decodedWidth = 0;
    UINT decodedHeight = 0;
    require(SUCCEEDED(frame->GetSize(&decodedWidth, &decodedHeight)) &&
                decodedWidth == width && decodedHeight == height, "The JPEG dimensions did not match the sample");

    ComPtr<IWICFormatConverter> converter;
    require(SUCCEEDED(factory->CreateFormatConverter(converter.GetAddressOf())) &&
                SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR,
                                                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)),
            "Could not decode the JPEG as BGR");
    std::vector<uint8_t> pixels(width * height * 3);
    require(SUCCEEDED(converter->CopyPixels(nullptr, width * 3, static_cast<UINT>(pixels.size()), pixels.data())) &&
                std::ranges::any_of(pixels, [](uint8_t value) { return value != 0; }), "The JPEG pixels did not decode");
}

void sampleFrames(RingAlertFrames& sampler, ring::Clock::time_point origin, int first, int last) {
    for (int second = first; second <= last; ++second) {
        std::vector<uint8_t> pixels(width * height * 3, static_cast<uint8_t>(80 + second));
        sampler.sample(origin + std::chrono::seconds(second), pixels, width, height);
    }
}

void activeEpisode(const fs::path& root) {
    RingAlertFrames sampler(root, false);
    ring::HighlightOptions options;
    options.enabled = true;
    options.fixedRange = {75, 75};
    options.hold = 2s;
    ring::HighlightDetector detector(options);
    const auto origin = ring::Clock::now() - 40s;

    const auto samplePulse = [&](int first, int last, int bpm) {
        for (int second = first; second <= last; ++second) {
            const auto stamp = origin + std::chrono::seconds(second);
            const auto update = detector.update(stamp, bpm);
            sampler.pulse(update.onset, stamp, detector.active());
        }
    };

    // The synthetic clock is 40 seconds behind, so both batches are already due.
    sampleFrames(sampler, origin, -7, 10);
    samplePulse(-2, 0, 100);
    const auto firstDirectory = root / "event-1-1";
    const auto first = awaitMarker(firstDirectory);
    require(detector.active(), "First frames must publish before pulse recovery");
    checkBatch(first, 1, 1);
    require(first.find("\"status\":\"ready\"") != std::string::npos, "Valid first batch was marked insufficient");
    const auto times = markerTimes(first);
    require(first.find("\"onset_ms\":-2000,") != std::string::npos,
            "First batch must distinguish heart-rate onset from alert confirmation");
    require(times.size() >= 3 && times.front() <= -2000 && times.back() >= 0 &&
                std::ranges::is_sorted(times) && std::ranges::adjacent_find(times) == times.end(),
            "First batch must include chronological frames around onset and confirmation");
    checkJpeg(firstDirectory / "frame-000.jpg");

    samplePulse(1, 4, 75);
    const auto followup = awaitMarker(root / "event-1-2");
    require(!detector.active(), "Synthetic pulse failed to recover before the follow-up");
    checkBatch(followup, 1, 2);
    require(followup.find("\"status\":\"ready\"") != std::string::npos && markerTimes(followup).back() >= 8000,
            "Follow-up did not contain newer video frames");

    sampleFrames(sampler, origin, 11, 20);
    samplePulse(11, 14, 75);
    require(!detector.active(), "Synthetic pulse failed to recover");
    samplePulse(15, 17, 100);
    const auto secondEpisode = awaitMarker(root / "event-2-1");
    checkBatch(secondEpisode, 2, 1);
    require(secondEpisode.find("\"status\":\"ready\"") != std::string::npos, "New episode was not usable");
}

void tooFewFrames(const fs::path& root) {
    RingAlertFrames sampler(root, true);
    const auto origin = ring::Clock::now() - 10s;
    sampleFrames(sampler, origin, -2, -1);
    sampler.pulse(origin - 2s, origin, true);
    const auto marker = awaitMarker(root / "event-1-1");
    checkBatch(marker, 1, 1, true);
    require(marker.find("\"status\":\"insufficient\"") != std::string::npos,
            "An incomplete batch must be visible as insufficient, never ready for upload");
}

std::vector<char> previewBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void cameraPreview(const fs::path& root) {
    RingAlertFrames sampler(root, true, true);
    const auto origin = ring::Clock::now() - 2s;
    const auto preview = root / "preview.jpg";
    sampleFrames(sampler, origin, 0, 0);
    const auto waitForChange = [&](const std::vector<char>& previous) {
        const auto deadline = ring::Clock::now() + 5s;
        do {
            auto current = previewBytes(preview);
            if (!current.empty() && current != previous) return current;
            std::this_thread::sleep_for(20ms);
        } while (ring::Clock::now() < deadline);
        throw std::runtime_error("Camera preview did not update");
    };
    const auto first = waitForChange({});
    checkJpeg(preview);
    sampleFrames(sampler, origin, 2, 2);
    waitForChange(first);
    checkJpeg(preview);
    require(!fs::exists(root / "event-1-1"), "Preview should not start a fall-assessment batch");
}
} // namespace

int main() try {
    const ScratchDirectory scratch;
    const ComApartment apartment;
    activeEpisode(scratch.path() / "active");
    tooFewFrames(scratch.path() / "incomplete");
    cameraPreview(scratch.path() / "preview");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
