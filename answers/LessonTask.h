#pragma once
#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>
inline std::string recordingSummary(uint64_t micBytes) {
    std::ostringstream out;
    out << "mic_bytes=" << micBytes << "; audio_seconds="
        << std::fixed << std::setprecision(2) << double(micBytes)/32000.0;
    return out.str();
}
