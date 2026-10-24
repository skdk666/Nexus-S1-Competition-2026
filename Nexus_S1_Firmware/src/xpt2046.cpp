#include "xpt2046.h"

namespace xpt2046 {
static int absolute(int n) { return n < 0 ? -n : n; }
static bool validCalibration(const Calibration& c) {
    return c.confirmed && c.x0 >= 0 && c.x0 <= 4095 && c.x1 >= 0 && c.x1 <= 4095 &&
           c.y0 >= 0 && c.y0 <= 4095 && c.y1 >= 0 && c.y1 <= 4095 &&
           absolute(c.x1-c.x0) >= 128 && absolute(c.y1-c.y0) >= 128;
}
bool validConfig(const Config& c) {
    if (c.mode != Mode::RawDiagnostics && c.mode != Mode::Calibrated) return false;
    if (!c.pins_confirmed || c.cs_pin < 0 || c.cs_pin > 48 || c.miso_pin < 0 || c.miso_pin > 48 || c.cs_pin == c.miso_pin ||
        c.cs_pin == 19 || c.cs_pin == 20 || c.miso_pin == 19 || c.miso_pin == 20) return false;
    if (c.spi_hz < 100000 || c.spi_hz > 2000000 || c.rotation > 3 ||
        c.release_threshold >= c.press_threshold || c.press_threshold > 8190 ||
        c.max_spread == 0 || c.max_spread > 4095 || c.stable_polls < 2 ||
        c.stable_polls > 10 || c.bad_polls == 0 || c.bad_polls > 10) return false;
    return c.mode == Mode::RawDiagnostics || validCalibration(c.calibration);
}
static int scale(int raw, int start, int end, int maximum) {
    // Signed arithmetic is required for reversed calibration endpoints.
    int value = (raw-start)*maximum/(end-start);
    return value < 0 ? 0 : (value > maximum ? maximum : value);
}
bool mapPoint(const Config& c, uint16_t x, uint16_t y, Point& p) {
    if (!validCalibration(c.calibration) || c.rotation > 3 || x > 4095 || y > 4095) return false;
    const auto& a = c.calibration;
    const int u = scale(a.swap_xy ? y : x, a.x0, a.x1, 239);
    const int v = scale(a.swap_xy ? x : y, a.y0, a.y1, 319);
    switch (c.rotation) {
        case 0: p = {int16_t(u), int16_t(v)}; break;
        case 1: p = {int16_t(319-v), int16_t(u)}; break;
        case 2: p = {int16_t(239-u), int16_t(319-v)}; break;
        case 3: p = {int16_t(v), int16_t(239-u)}; break;
    }
    return true;
}
bool Driver::begin(const Config& c, ReadFrame reader, void* context) {
    *this = Driver{};
    config_ = c; reader_ = reader; context_ = context;
    ready_ = validConfig(c) && reader;
    return ready_;
}
State Driver::cancel() {
    armed_ = pressed_ = false; presses_ = releases_ = bad_ = 0;
    return State::Cancel;
}
static uint16_t median(const uint16_t* v) {
    uint16_t a=v[0], b=v[1], c=v[2];
    if(a>b) {auto t=a;a=b;b=t;} if(b>c) {auto t=b;b=c;c=t;}
    return a>b ? a : b;
}
static bool stable(const uint16_t* v, uint16_t limit) {
    uint16_t lo=v[0], hi=v[0];
    for (int i=0;i<3;++i) {
        if (v[i]>4095) return false;
        if(v[i]<lo) lo=v[i];
        if(v[i]>hi) hi=v[i];
    }
    return hi-lo <= limit;
}
State Driver::poll(Point& point) {
    if (!ready_) return State::Released; // Invalid/disabled config never calls a bus reader.
    Frame sample;
    last_read_valid_ = reader_(context_, sample);
    if (!last_read_valid_) return cancel();
    frame_ = sample;
    if (config_.mode == Mode::RawDiagnostics) return State::Released; // Never operates the UI.
    if (sample.pressure_before > 8190 || sample.pressure_after > 8190) return cancel();
    if (sample.pressure_before <= config_.release_threshold &&
        sample.pressure_after <= config_.release_threshold) {
        presses_ = bad_ = 0;
        if (releases_ < config_.stable_polls) ++releases_;
        if (releases_ >= config_.stable_polls) { pressed_=false; armed_=true; }
        point=last_;
        return pressed_ ? State::Pressed : State::Released;
    }
    releases_ = 0;
    const uint16_t threshold = pressed_ ? config_.release_threshold : config_.press_threshold;
    const bool clean = sample.pressure_before >= threshold && sample.pressure_after >= threshold &&
                       stable(sample.x,config_.max_spread) && stable(sample.y,config_.max_spread);
    if (!clean) {
        presses_ = 0;
        if (pressed_ && ++bad_ >= config_.bad_polls) return cancel();
        point=last_;
        return pressed_ ? State::Pressed : State::Released;
    }
    bad_ = 0;
    if (!armed_) return State::Released; // Ignore a finger already down at boot/recovery.
    if (!pressed_ && ++presses_ < config_.stable_polls) return State::Released;
    if (!mapPoint(config_,median(sample.x),median(sample.y),last_)) return cancel();
    pressed_ = true; point=last_;
    return State::Pressed;
}
}
