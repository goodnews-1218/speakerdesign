#include "RingOverlay.h"

#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace ring {
namespace {
using Glyph = std::array<unsigned, 7>;
constexpr Glyph digits[] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}
};

Glyph glyph(char c) {
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
        case '-': return {0,0,0,14,0,0,0};
        case 'B': return {30,17,17,30,17,17,30};
        case 'P': return {30,17,17,30,16,16,16};
        case 'M': return {17,27,21,21,17,17,17};
        case 'D': return {30,17,17,17,17,17,30};
        case 'E': return {31,16,16,30,16,16,31};
        case 'O': return {14,17,17,17,17,17,14};
        case 'H': return {17,17,17,31,17,17,17};
        case 'L': return {16,16,16,16,16,16,31};
        case 'A': return {14,17,17,31,17,17,17};
        case 'S': return {15,16,16,14,1,1,30};
        case 'T': return {31,4,4,4,4,4,4};
        case 'W': return {17,17,17,21,21,27,17};
        case 'I': return {14,4,4,4,4,4,14};
        case '+': return {0,4,4,31,4,4,0};
        default: return {};
    }
}
}

void drawOverlay(std::span<uint8_t> bgr, unsigned width, unsigned height,
                 double videoSeconds, Reading reading) {
    const auto scale = int(height / 240 > 0 ? height / 240 : 1);
    const int margin = 7 * scale, boxHeight = 29 * scale;
    const int boxWidth = (reading.demo ? 110 : 87) * scale;
    if (width < unsigned(boxWidth + 2 * margin) || height < unsigned(boxHeight + 2 * margin) ||
        bgr.size() != size_t(width) * height * 3) return;
    const int top = int(height) - margin - boxHeight;
    const auto put = [&](int x, int y, std::array<uint8_t, 3> color, unsigned alpha = 255) {
        const auto offset = (size_t(top + y) * width + margin + x) * 3;
        for (size_t i = 0; i < color.size(); ++i)
            bgr[offset + i] = static_cast<uint8_t>((alpha * color[i] + (255 - alpha) * bgr[offset + i]) / 255);
    };
    for (int y = 0; y < boxHeight; ++y) {
        for (int x = 0; x < boxWidth; ++x) {
            const int radius = 4 * scale;
            const int dx = x < radius ? radius - x : x >= boxWidth - radius ? x - (boxWidth - radius - 1) : 0;
            const int dy = y < radius ? radius - y : y >= boxHeight - radius ? y - (boxHeight - radius - 1) : 0;
            if (dx * dx + dy * dy <= radius * radius) put(x, y, {23, 21, 19}, 210);
        }
    }
    // Animate at the estimated rate; the icon illustrates rhythm, rather than individual detected peaks.
    const double beats = videoSeconds * reading.bpm / 60;
    const double phase = beats - std::floor(beats);
    const bool measuring = reading.state == PulseDisplay::live;
    const double pulse = measuring ? std::exp(-12 * phase) : 0;
    const double heartSize = (7.0 + 2.4 * pulse) * scale;
    for (int y = 2 * scale; y < 27 * scale; ++y) {
        for (int x = 2 * scale; x < 26 * scale; ++x) {
            const double nx = (x - 14 * scale) / heartSize;
            const double ny = (14 * scale - y) / heartSize;
            const double base = nx * nx + ny * ny - 1;
            if (base * base * base <= nx * nx * ny * ny * ny)
                put(x, y, measuring ? std::array<uint8_t, 3>{111, 92, 251}
                                      : std::array<uint8_t, 3>{139, 133, 128});
        }
    }
    const auto text = [&](std::string_view value, int x, int y, int pixel,
                          std::array<uint8_t, 3> color) {
        for (char c : value) {
            const auto shape = glyph(c);
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (!(shape[row] & (16 >> col))) continue;
                    for (int dy = 0; dy < pixel; ++dy)
                        for (int dx = 0; dx < pixel; ++dx)
                            put(x + col * pixel + dx, y + row * pixel + dy, color);
                }
            }
            x += 6 * pixel;
        }
    };
    const auto bpm = reading.bpm ? std::to_string(reading.bpm) : "--";
    text(bpm, 27 * scale, 7 * scale, 2 * scale, {250, 249, 246});
    text("BPM", 64 * scale, 11 * scale, scale, {186, 181, 178});
    constexpr std::array<uint8_t, 3> amber{97, 204, 255};
    if (reading.state == PulseDisplay::held) {
        text("HOLD", 64 * scale, 2 * scale, scale, amber);
    } else if (reading.state == PulseDisplay::last) {
        text("LAST", 64 * scale, 2 * scale, scale, amber);
        const auto age = reading.age.count();
        text(age >= 999 ? "999+" : std::to_string(age) + "S", 64 * scale, 20 * scale, scale, amber);
    } else if (reading.state == PulseDisplay::waiting) {
        text("WAIT", 64 * scale, 2 * scale, scale, amber);
    }
    if (reading.demo) text("DEMO", 84 * scale, 11 * scale, scale, {97, 204, 255});
}
} // namespace ring
