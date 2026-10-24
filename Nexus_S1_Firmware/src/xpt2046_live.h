#pragma once
#include "xpt2046_wire.h"

namespace xpt2046 {
enum class LivePressure { Invalid, Release, Press, Mid };
inline const char* livePressureName(LivePressure pressure) {
    switch(pressure) {
        case LivePressure::Release:return "RELEASE";
        case LivePressure::Press:return "PRESS";
        case LivePressure::Mid:return "MID";
        default:return "INVALID";
    }
}
struct LiveResult {
    State state=State::Released;
    Point pixel;
    uint16_t x=0,y=0,dx=0,dy=0;
    uint16_t z1_before=0,z2_before=0,z1_after=0,z2_after=0;
    uint16_t pressure_before=0,pressure_after=0;
    bool raw_available=false,rx_valid=false,xy_stable=false;
    bool mapped=false, xy_only=false, z_valid=false;
    LivePressure pressure=LivePressure::Invalid;
    // Diagnostic-only history. Never read by candidate/press/release decisions.
    bool history_valid=false;
    Point history_pixel;
    uint32_t history_at=0;
    const char* reason="NO_SAMPLE";
};
// Temporary FULL ADC RANGE mapping, not a measured calibration.
inline bool liveMap(uint16_t x,uint16_t y,uint8_t rotation,Point& p) {
    if(x>4095 || y>4095 || rotation>3) return false;
    int u=int(uint32_t(x)*239/4095),v=int(uint32_t(y)*319/4095);
    switch(rotation) {
        case 0:p={int16_t(u),int16_t(v)};break;
        case 1:p={int16_t(319-v),int16_t(u)};break;
        case 2:p={int16_t(239-u),int16_t(319-v)};break;
        case 3:p={int16_t(v),int16_t(239-u)};break;
    }
    return true;
}
class LiveFilter {
    LiveResult result_;
    bool allow_xy_=false, armed_=false, neutral_=false, sampled_=false, active_=false, latched_=false;
    uint8_t rotation_=1, candidates_=0, releases_=0, coherent_=0;
    uint32_t last_=0, neutral_at_=0, candidate_at_=0, pressed_at_=0;
    uint16_t candidate_x_=0,candidate_y_=0;
    static unsigned distance(unsigned a,unsigned b) {return a>b?a-b:b-a;}
    static uint16_t median(const WireTrace& t,int start) {
        uint16_t a=t.adc[start],b=t.adc[start+2],c=t.adc[start+4];
        if(a>b){auto v=a;a=b;b=v;}if(b>c){auto v=b;b=c;c=v;}
        return a>b?a:b;
    }
    LiveResult cancel(const char* reason) {
        active_=armed_=neutral_=false; latched_=true; candidates_=releases_=coherent_=0;
        result_.state=State::Cancel;result_.reason=reason;return result_;
    }
public:
    void begin(bool allow_xy,uint8_t rotation=1) {
        *this=LiveFilter{};allow_xy_=allow_xy;rotation_=rotation;
    }
    const LiveResult& result() const {return result_;}
    LiveResult update(const WireTrace& t,bool wire_ok,uint32_t now) {
        const bool gap=sampled_ && uint32_t(now-last_)>150;
        last_=now;sampled_=true;
        result_.mapped=result_.z_valid=result_.raw_available=result_.rx_valid=result_.xy_stable=false;
        result_.pixel=Point{};result_.x=result_.y=result_.dx=result_.dy=0;
        result_.z1_before=result_.z2_before=result_.z1_after=result_.z2_after=0;
        result_.pressure_before=result_.pressure_after=0;result_.pressure=LivePressure::Invalid;
        bool complete=t.started && t.count==(t.settled?63:30);
        for(unsigned i=0;i<10;++i) if(t.adc[i]>4095) complete=false;
        result_.raw_available=complete; // Decoded data can be visible even when RX is rejected.
        if(complete) {
            result_.x=median(t,2);result_.y=median(t,3);
            result_.dx=coordinateSpread(t,2);result_.dy=coordinateSpread(t,3);
            result_.z1_before=t.adc[0];result_.z2_before=t.adc[1];
            result_.z1_after=t.adc[8];result_.z2_after=t.adc[9];
            result_.pressure_before=uint16_t(4095U+t.adc[0]-t.adc[1]);
            result_.pressure_after=uint16_t(4095U+t.adc[8]-t.adc[9]);
        }
        const bool transport=complete && wire_ok && !t.payload_zero && !t.payload_ones;
        result_.rx_valid=transport;
        const bool xy=transport && result_.dx<=20 && result_.dy<=20;
        result_.xy_stable=xy;
        const char* issue=transport?pressureIssue(t,300):"NO_TRANSPORT";
        result_.z_valid=transport && !issue;
        if(xy) {
            result_.mapped=liveMap(result_.x,result_.y,rotation_,result_.pixel);
        }
        const unsigned p1=result_.pressure_before,p2=result_.pressure_after;
        const bool release=result_.z_valid && p1<=300 && p2<=300;
        const bool pressure=result_.z_valid && p1>=600 && p2>=600;
        const bool holding_pressure=result_.z_valid && p1>=300 && p2>=300;
        result_.pressure=!result_.z_valid?LivePressure::Invalid:release?LivePressure::Release:
            pressure?LivePressure::Press:LivePressure::Mid;
        const bool rail=xy && (result_.x<=16 || result_.x>=4079 || result_.y<=16 || result_.y>=4079);
        if(result_.mapped && !rail) {
            result_.history_valid=true;result_.history_pixel=result_.pixel;result_.history_at=now;
        }
        if(gap) return cancel("STALE_CANCEL");
        // MID is not a proven release. Only the explicitly opted-in XY experiment
        // may infer a tap from it; trusted pressure thresholds/hysteresis stay unchanged.
        const bool fallback=allow_xy_ && result_.mapped && !rail && !release && !pressure;
        const bool candidate=result_.mapped && !rail &&
            (pressure || (active_ && !result_.xy_only && holding_pressure) || fallback);
        // An invalid/unstable current frame must cancel, even if Z says release.
        // In particular, diagnostic history cannot finish a LVGL click.
        if(active_ && (!result_.mapped || rail))
            return cancel(!transport?(t.payload_ones?"RX_FF":t.payload_zero?"RX_ZERO":"NO_FRAME"):
                !xy?"XY_SPREAD":rail?"XY_RAIL":"MAP_INVALID");
        // In XY-only there is no physical lift signal. Rearm needs 300ms of bad/
        // absent XY or a plausible pressure release; a cooldown alone never rearms.
        const bool neutral=release || (allow_xy_ && !candidate);
        if(neutral) {
            candidates_=0;
            if(!neutral_) {neutral_=true;neutral_at_=now;releases_=0;}
            if(releases_<10) ++releases_;
            if(active_) {
                if(!release) return cancel(t.payload_ones?"RX_FF_CANCEL":t.payload_zero?"RX_ZERO_CANCEL":"INVALID_CANCEL");
                if(releases_<2) return result_;
                active_=false;result_.state=State::Released;result_.reason="PRESSURE_RELEASE";
                latched_=true;
            } else {
                result_.state=State::Released;
                result_.reason=release?"RELEASED":!transport?(t.payload_ones?"RX_FF":t.payload_zero?"RX_ZERO":"NO_FRAME"):
                    !xy?"XY_SPREAD":rail?"XY_RAIL":!result_.mapped?"MAP_INVALID":"WAIT_NEUTRAL";
            }
            if(releases_>=2 && uint32_t(now-neutral_at_)>=300) {armed_=true;latched_=false;}
            return result_;
        }
        neutral_=false;releases_=0;
        if(!candidate) {
            const char* why=!transport?(t.payload_ones?"RX_FF":t.payload_zero?"RX_ZERO":"NO_FRAME"):
                !xy?"XY_SPREAD":rail?"XY_RAIL":!result_.mapped?"MAP_INVALID":issue?issue:"PRESSURE_MID";
            if(active_) return cancel(why);
            candidates_=0;result_.state=State::Released;result_.reason=why;return result_;
        }
        if(active_) {
            if(!result_.xy_only && !holding_pressure) return cancel("PRESSURE_LOST");
            if(uint32_t(now-pressed_at_)>(result_.xy_only?600U:1500U)) return cancel("HOLD_TIMEOUT");
            if(distance(candidate_x_,result_.x)>512 || distance(candidate_y_,result_.y)>512) return cancel("JUMP_CANCEL");
            candidate_x_=result_.x;candidate_y_=result_.y;
            if(coherent_<20)++coherent_;
            // Explicit experimental tap inferred from ongoing real, coherent XY.
            // Invalid data/age timeout CANCELS instead, so it cannot produce a click.
            if(result_.xy_only && coherent_>=6 && uint32_t(now-pressed_at_)>=180) {
                active_=false;armed_=false;latched_=true;
                result_.state=State::Released;result_.reason="XY_TAP_END";return result_;
            }
            result_.state=State::Pressed;result_.reason=result_.xy_only?"XY_ONLY_EXPERIMENTAL":"PRESSURE";return result_;
        }
        if(!armed_ || latched_) {result_.state=State::Released;result_.reason="WAIT_NEUTRAL";return result_;}
        if(!candidates_ || distance(candidate_x_,result_.x)>64 || distance(candidate_y_,result_.y)>64) {
            candidates_=1;candidate_at_=now;candidate_x_=result_.x;candidate_y_=result_.y;
        } else if(candidates_<10) ++candidates_;
        result_.state=State::Released;result_.reason="DEBOUNCE";
        if(candidates_>=3 && uint32_t(now-candidate_at_)>=60) {
            active_=true;pressed_at_=now;coherent_=0;result_.xy_only=!pressure;
            result_.state=State::Pressed;result_.reason=result_.xy_only?"XY_ONLY_EXPERIMENTAL":"PRESSURE";
        }
        return result_;
    }
};
}
