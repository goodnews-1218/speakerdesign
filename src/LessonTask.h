#pragma once
#include <cstdint>
#include <string>

// Student exercise: return "mic_bytes=N; audio_seconds=S" with S to 2 decimals.
// A sample is 2 bytes; 16000 samples occupy 32000 bytes per second.
inline std::string recordingSummary(uint64_t micBytes) {
    (void)micBytes;
    return "Recording finished"; // TODO: implement the lesson exercise.
}
