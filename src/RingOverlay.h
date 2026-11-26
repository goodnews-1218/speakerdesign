#pragma once

#include "RingMonitor.h"

#include <cstdint>
#include <span>

namespace ring {
void drawOverlay(std::span<uint8_t> bgr, unsigned width, unsigned height,
                 double videoSeconds, Reading reading);
}
