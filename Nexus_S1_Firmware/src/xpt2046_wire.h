#pragma once
#include "xpt2046.h"

namespace xpt2046 {
// Optional diagnostic capture of the exact transaction, before decoding/filtering.
struct WireTrace {
    uint8_t tx[63] = {}, rx[63] = {}, count = 0;
    uint16_t adc[10] = {}, first[10] = {};
    bool settled = false;
    bool started = false, payload_zero = true, payload_ones = true;
};
// Bus exposes begin()/transfer(byte)/end(). The ESP32 implementation uses the
// existing TFT_eSPI SPI instance; host tests record the exact wire sequence.
template<class Bus> bool readWireFrame(Bus& bus, Frame& frame, WireTrace* trace=nullptr,bool settled=false) {
    if (trace) { *trace=WireTrace{};trace->settled=settled; }
    if (!bus.begin()) return false;
    if (trace) trace->started=true;
    struct EndTransaction { Bus& bus; ~EndTransaction() { bus.end(); } } guard{bus};
    bool all_ones = true, all_zero = true;
    uint8_t conversion=0;
    auto transfer = [&](uint8_t tx) -> uint8_t {
        const uint8_t rx=bus.transfer(tx);
        if (trace) {
            const uint8_t n=trace->count++;
            trace->tx[n]=tx; trace->rx[n]=rx;
        }
        return rx;
    };
    auto adc = [&](uint8_t command) -> uint16_t {
        transfer(command);
        const uint8_t high = transfer(0), low = transfer(0);
        if (high != 0xFF || low != 0xFF) all_ones = false;
        if (high != 0 || low != 0) all_zero = false;
        const uint16_t value=((uint16_t(high)<<8 | low)>>3)&0xFFF;
        return value;
    };
    auto sample = [&](uint8_t command) -> uint16_t {
        // Independent 24-clock conversions, not a guessed shift of a pipeline.
        // PD=01 retains ADC/driver power; repeat the SAME channel and discard the
        // first result. The final PD=00 conversion below restores power-down.
        const uint8_t control=settled ? uint8_t(command|1) : command;
        if (settled) {
            const uint16_t first=adc(control);
            if (trace) trace->first[conversion]=first;
        }
        const uint16_t value=adc(control);
        if (trace) trace->adc[conversion]=value;
        ++conversion;
        return value;
    };
    auto pressure = [&]() -> uint16_t {
        const int z1=sample(0xB0), z2=sample(0xC0);
        const int z=4095+z1-z2;
        return uint16_t(z==4095 ? 0 : z);
    };
    frame.pressure_before=pressure();
    for (int i=0;i<3;++i) { frame.x[i]=sample(0xD0); frame.y[i]=sample(0x90); }
    frame.pressure_after=pressure();
    if (settled) adc(0x90); // Complete PD=00 before CS rises, including its 16 data clocks.
    // Legacy: 30 bytes. RAW settled experiment: 20+1 conversions, exactly 63 bytes.
    if (trace) { trace->payload_zero=all_zero; trace->payload_ones=all_ones; }
    // Both constant-low and constant-high data are suspicious, not valid releases.
    // SPI has no ACK: accepting other data still does not prove communication.
    return !all_ones && !all_zero;
}
// Conservative plausibility checks, not a calibrated pressure measurement.
// Keep the raw values even when rejecting them; never turn invalid Z into PRESSED.
inline const char* pressureIssue(const WireTrace& t,uint16_t release_threshold) {
    if (!t.started || t.count!=(t.settled?63:30)) return "NO_FRAME";
    for (int i=0;i<=8;i+=8) {
        const unsigned z1=t.adc[i],z2=t.adc[i+1];
        if (!z2) return "Z2_ZERO";
        if (z1>=z2) return "Z_ORDER";
        if (!z1 && 4095U-z2>release_threshold) return "Z1_ZERO";
    }
    return nullptr;
}
inline uint16_t coordinateSpread(const WireTrace& t,int start) {
    uint16_t lo=t.adc[start],hi=lo;
    for (int i=start+2;i<=start+4;i+=2) {
        if(t.adc[i]<lo)lo=t.adc[i];
        if(t.adc[i]>hi)hi=t.adc[i];
    }
    return hi-lo;
}
}
