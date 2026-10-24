#pragma once
#include <algorithm>
#include <cstdint>

namespace sim {
constexpr int width = 320, height = 240, banner = 28;
struct TickClock {
    bool initialized = false;
    uint32_t previous = 0;
    uint32_t advance(uint32_t now) {
        if (!initialized) { initialized = true; previous = now; return 0; }
        const uint32_t elapsed = now - previous; // Modulo subtraction handles 32-bit wrap.
        previous = now;
        return elapsed;
    }
};
struct Viewport { int x = 0, y = banner, w = 0, h = 0; };
inline Viewport viewport(int output_w, int output_h) {
    if (output_w <= 0 || output_h <= banner) return {};
    const double scale = std::min(output_w / double(width), (output_h - banner) / double(height));
    const int w = std::max(1, int(width * scale)), h = std::max(1, int(height * scale));
    return {(output_w - w) / 2, banner + (output_h - banner - h) / 2, w, h};
}
struct Point { int x = 0, y = 0; bool inside = false; };
inline Point map_mouse(int x, int y, int window_w, int window_h,
                       int output_w, int output_h, Viewport view) {
    if (window_w <= 0 || window_h <= 0 || view.w <= 0 || view.h <= 0) return {};
    // Events use window coordinates; output can have a different DPI scale.
    const double px = double(x) * output_w / window_w;
    const double py = double(y) * output_h / window_h;
    const bool inside = px >= view.x && px < view.x + view.w && py >= view.y && py < view.y + view.h;
    const int lx = int(std::clamp((px - view.x) * width / view.w, 0.0, double(width - 1)));
    const int ly = int(std::clamp((py - view.y) * height / view.h, 0.0, double(height - 1)));
    return {lx, ly, inside};
}
} // namespace sim
