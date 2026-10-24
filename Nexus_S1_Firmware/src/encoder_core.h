#pragma once
#include <stdint.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_attr.h>
#define NEXUS_ENCODER_IRAM IRAM_ATTR
#else
#define NEXUS_ENCODER_IRAM
#endif
namespace nexus_encoder {
class Quadrature {
    uint8_t previous_=3,edges_;
    int8_t partial_=0;
    int16_t pending_=0;
    uint32_t invalid_=0;
public:
    explicit Quadrature(uint8_t edges=4):edges_(edges) {}
    void reset(uint8_t state) { previous_=state&3;partial_=0;pending_=0; }
    // Called under adapter's ISR lock; arithmetic only, no flash lookup table.
    void NEXUS_ENCODER_IRAM edge(uint8_t state) {
        state&=3;const uint8_t changed=state^previous_;
        if(!changed)return;
        if(changed==3) { ++invalid_;partial_=0;previous_=state;return; }
        partial_+=((previous_>>1)^(state&1)) ? 1 : -1;
        previous_=state;
        if(partial_==edges_ || partial_==-int(edges_)) {
            if(partial_>0) { if(pending_<32767)++pending_; }
            else if(pending_>-32767)--pending_;
            partial_=0;
        }
    }
    int16_t take() { const auto n=pending_;pending_=0;return n; }
    uint32_t invalid() const { return invalid_; }
};
enum class Action { None,Click,Back };
class Button {
    bool raw_=false,stable_=false,armed_=false,held_=false,long_sent_=false;
    uint32_t since_=0,down_=0;
    uint32_t presses_=0;
    Action pending_=Action::None;
public:
    static constexpr uint32_t debounce_ms=30;
    void begin(bool pressed,uint32_t now) {
        // Navigation may rebind while held. Require release; keep the cumulative count.
        raw_=stable_=pressed;since_=now;armed_=held_=long_sent_=false;pending_=Action::None;
    }
    void cancel() { armed_=held_=false;pending_=Action::None; }
    void update(bool pressed,uint32_t now) {
        if(raw_!=pressed) {raw_=pressed;since_=now;}
        if(uint32_t(now-since_)<debounce_ms)return;
        if(stable_!=raw_) {
            stable_=raw_;
            if(stable_) {
                if(armed_) {++presses_;held_=true;long_sent_=false;down_=since_;}
            } else {
                if(held_ && !long_sent_)pending_=uint32_t(since_-down_)>=800U?Action::Back:Action::Click;
                held_=false;armed_=true;
            }
        }
        if(!stable_)armed_=true;
        if(stable_ && held_ && !long_sent_ && uint32_t(now-down_)>=800U) {
            pending_=Action::Back;long_sent_=true;armed_=false;
        }
    }
    Action take() { const auto action=pending_;pending_=Action::None;return action; }
    // Read-only diagnostics: observing the button must not consume a gesture.
    bool pressed() const { return stable_; }
    bool armed() const { return armed_; }
    bool held() const { return held_; }
    // A physical click is counted at the debounced press, independently of the
    // short-on-release / long-back classification used by existing navigation.
    uint32_t pressCount() const { return presses_; }
};
}
