#pragma once

#include "RingPulse.h"

#include <functional>
#include <thread>

namespace ring {
struct Reading {
    int bpm = 0;
    bool demo = false;
    PulseDisplay state = PulseDisplay::waiting;
    std::chrono::seconds age{};
};

class Monitor {
public:
    using Display = std::function<void(Reading)>;
    using Save = std::function<void(Episode)>;

    Monitor(bool demo, HighlightOptions options, Display display, Save save);
    ~Monitor();
    void stop();

private:
    std::jthread worker_;
};
} // namespace ring
