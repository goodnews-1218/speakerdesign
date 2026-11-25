#include "RingMonitor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <syncstream>

namespace ring {
namespace {
using namespace std::chrono_literals;

struct CloseHttp {
    void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); }
};
using HttpHandle = std::unique_ptr<void, CloseHttp>;

struct Snapshot {
    std::wstring session;
    double sequence = 0;
    unsigned rate = 0;
    std::vector<double> green;
};

class BioScope {
public:
    BioScope()
        : http_(WinHttpOpen(L"Connect Ring/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)),
          connection_(http_ ? WinHttpConnect(http_.get(), L"127.0.0.1", 8000, 0) : nullptr) {
        if (http_) WinHttpSetTimeouts(http_.get(), 1000, 1000, 1000, 1000);
    }

    Snapshot poll() const {
        if (!connection_) return {};
        const HttpHandle request(WinHttpOpenRequest(connection_.get(), L"GET",
            L"/getdata?device_type=ppg", nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
        if (!request) return {};
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
            !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr))
            return {};

        DWORD status = 0, statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) || status != 200)
            return {};

        std::string body;
        char chunk[8192];
        const auto deadline = Clock::now() + 2s;
        for (;;) {
            DWORD read = 0;
            if (Clock::now() > deadline || !WinHttpReadData(request.get(), chunk, sizeof(chunk), &read))
                return {};
            if (!read) break;
            if (body.size() + read > 512 * 1024) return {};
            body.append(chunk, read);
        }

        const auto data = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
        if (data.GetNamedString(L"state", L"") != L"streaming" ||
            data.GetNamedString(L"device_type", L"") != L"ppg") return {};
        Snapshot result;
        result.session = data.GetNamedString(L"session_id", L"").c_str();
        result.sequence = data.GetNamedNumber(L"last_sample_id", 0);
        const auto rate = data.GetNamedNumber(L"sample_rate_hz", 0);
        if (result.session.empty() || result.sequence <= 0 || rate != 100) return {};
        result.rate = static_cast<unsigned>(rate);
        const auto green = data.GetNamedObject(L"channels").GetNamedArray(L"GREEN");
        const auto timestamps = data.GetNamedArray(L"timestamps");
        const auto count = green.Size();
        if (count != timestamps.Size() || count != data.GetNamedNumber(L"sample_count", 0) ||
            count == 0 || count > 20 * result.rate) return {};
        const double newest = timestamps.GetNumberAt(count - 1);
        const double now = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (!std::isfinite(newest) || now - newest > 3 || newest - now > 2) return {};

        auto first = count > 7 * result.rate ? count - 7 * result.rate : 0;
        // Never estimate across a sizable sample-time gap in the recent window.
        for (auto i = count - 1; i > first; --i) {
            const double gap = timestamps.GetNumberAt(i) - timestamps.GetNumberAt(i - 1);
            if (!std::isfinite(gap) || gap > 0.25 || gap < -0.03) {
                first = i;
                break;
            }
        }
        result.green.reserve(count - first);
        for (auto i = first; i < count; ++i) result.green.push_back(green.GetNumberAt(i));
        return result;
    }

private:
    HttpHandle http_, connection_;
};

Snapshot demoSnapshot(double elapsed, double calibratedAt) {
    Snapshot snapshot{L"demo", std::floor(elapsed * 100) + 1, 100, {}};
    const double changedAt = calibratedAt + 8;
    const auto phase = [&](double t) {
        return (t * 75 + std::clamp(t - changedAt, 0.0, 16.0) * 35) / 60;
    };
    for (int i = 0; i < 700; ++i) {
        const double t = elapsed - (699 - i) / 100.0;
        const double wave = 2 * std::numbers::pi * phase(t);
        snapshot.green.push_back(5000 + 240 * std::sin(wave) + 40 * std::sin(2 * wave));
    }
    return snapshot;
}

void log(std::string message) {
    std::osyncstream(std::cout) << message << '\n' << std::flush_emit;
}

void run(std::stop_token stop, bool demo, HighlightOptions options,
         const Monitor::Display& display, const Monitor::Save& save) {
    HighlightDetector detector(options);
    const auto start = Clock::now();
    std::wstring session;
    double previousSequence = 0;
    auto lastAdvance = start;
    PulseReadout readout;
    std::string previousStatus;
    std::mutex waitMutex;
    std::condition_variable_any wake;
    const BioScope api;
    if (options.enabled && options.fixedRange)
        log("RING_BASELINE=" + std::to_string(options.fixedRange->first) + ".." +
            std::to_string(options.fixedRange->second));
    if (options.enabled && !options.fixedRange) log("RING_BASELINE=CALIBRATING");
    auto next = start;
    while (!stop.stop_requested()) {
        Snapshot snapshot;
        try {
            snapshot = demo ? demoSnapshot(std::chrono::duration<double>(Clock::now() - start).count(),
                options.enabled && !options.fixedRange ? double(options.calibration.count()) : 0)
                : api.poll();
        } catch (const winrt::hresult_error&) {
            // A malformed response is handled the same way as interrupted input.
        }
        if (stop.stop_requested()) break;
        const auto now = Clock::now();
        if (!snapshot.session.empty() && (snapshot.session != session ||
            snapshot.sequence < previousSequence)) {
            if (const auto episode = detector.finish()) save(*episode);
            detector.reset();
            session = snapshot.session;
            previousSequence = 0;
            readout = {};
            if (options.enabled && !options.fixedRange && !previousStatus.empty())
                log("RING_BASELINE=CALIBRATING");
        }
        const bool advanced = snapshot.sequence > previousSequence;
        std::optional<PulseEstimate> estimate;
        if (advanced) {
            previousSequence = snapshot.sequence;
            lastAdvance = now;
            estimate = estimateBpm(snapshot.green, snapshot.rate, readout.recentVerified(now));
        }
        if (advanced || snapshot.session.empty() || now - lastAdvance >= 2s) {
            const auto view = readout.update(now, estimate, advanced);
            display({view.bpm, demo, view.state, view.age});
            const std::string status = estimate ? (estimate->recovered ? "RECOVERED" : "TRACKING") :
                view.state == PulseDisplay::held ? "HOLDING" :
                view.state == PulseDisplay::last ? "LAST_KNOWN" :
                snapshot.session.empty() ? "UNAVAILABLE" : "WAITING_FOR_PULSE";
            if (status != previousStatus) log("RING_STATUS=" + status);
            previousStatus = status;
            if (estimate) {
                const auto label = demo ? "RING_DEMO_BPM=" :
                                   estimate->recovered ? "RING_RECOVERED_BPM=" : "RING_BPM=";
                log(label + std::to_string(estimate->bpm));
            } else if (view.state == PulseDisplay::held) {
                log("RING_HELD_BPM=" + std::to_string(view.bpm));
            } else if (view.state == PulseDisplay::last) {
                log("RING_LAST_BPM=" + std::to_string(view.bpm) +
                    " AGE_SECONDS=" + std::to_string(view.age.count()));
            }

            // Display continuity must not advance the baseline or trigger a highlight.
            const auto bpm = estimate && !estimate->recovered ? std::optional{estimate->bpm} : std::nullopt;
            const auto update = detector.update(now, bpm);
            if (update.baselineReady)
                log("RING_BASELINE=" + std::to_string(detector.baseline()->first));
            if (update.triggered) log("RING_HIGHLIGHT_TRIGGERED=" + std::to_string(*bpm));
            if (update.completed) save(*update.completed);
        }
        next = std::max(next + 1s, Clock::now() + 10ms);
        std::unique_lock lock(waitMutex);
        wake.wait_until(lock, stop, next, [] { return false; });
    }
    if (const auto episode = detector.finish()) save(*episode);
}
} // namespace

Monitor::Monitor(bool demo, HighlightOptions options, Display display, Save save)
    : worker_([=](std::stop_token stop) {
          try {
              winrt::init_apartment();
              struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
              run(stop, demo, options, display, save);
          } catch (const winrt::hresult_error& error) {
              display({0, demo});
              log("RING_ERROR=" + winrt::to_string(error.message()));
          } catch (const std::exception& error) {
              display({0, demo});
              log(std::string("RING_ERROR=") + error.what());
          }
      }) {}

Monitor::~Monitor() { stop(); }

void Monitor::stop() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
}
} // namespace ring
