#pragma once
#include <stdint.h>

// Controller/filter logic has no Arduino or LVGL dependency; tests use this exact code.
namespace xpt2046 {
enum class Mode { Disabled, RawDiagnostics, Calibrated };
enum class State { Released, Pressed, Cancel };
struct Point {
    int16_t x, y;
    constexpr Point(int16_t px=0,int16_t py=0):x(px),y(py) {}
};
struct Calibration {
    bool confirmed, swap_xy;
    // Oriented endpoints in native portrait space (240x320), after optional raw-axis swap.
    // Reversed endpoints encode inversion. Never substitute library example values.
    int16_t x0, x1, y0, y1;
    constexpr Calibration(bool valid=false,bool swap=false,int16_t xa=0,int16_t xb=0,int16_t ya=0,int16_t yb=0):
        confirmed(valid),swap_xy(swap),x0(xa),x1(xb),y0(ya),y1(yb) {}
};
struct Config {
    Mode mode = Mode::Disabled;
    bool pins_confirmed = false;
    int cs_pin = -1, miso_pin = -1;
    uint32_t spi_hz = 1000000;
    bool raw_settled_probe = false; // RAW-only experiment; does not enable LVGL input.
    uint16_t press_threshold = 600, release_threshold = 300, max_spread = 20;
    uint8_t stable_polls = 2, bad_polls = 3, rotation = 1;
    Calibration calibration;
};
struct Frame {
    uint16_t x[3] = {}, y[3] = {};
    uint16_t pressure_before = 0, pressure_after = 0;
};
using ReadFrame = bool (*)(void*, Frame&);
bool validConfig(const Config& c);
bool mapPoint(const Config& c, uint16_t x, uint16_t y, Point& point);

class Driver {
public:
    bool begin(const Config& c, ReadFrame reader, void* context);
    State poll(Point& point);
    const Frame& lastFrame() const { return frame_; }
    bool lastReadValid() const { return last_read_valid_; }
private:
    State cancel();
    Config config_;
    ReadFrame reader_ = nullptr;
    void* context_ = nullptr;
    Frame frame_;
    Point last_;
    bool ready_ = false, armed_ = false, pressed_ = false, last_read_valid_ = false;
    uint8_t presses_ = 0, releases_ = 0, bad_ = 0;
};
}
