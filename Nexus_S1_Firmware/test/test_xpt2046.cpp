#include "xpt2046.h"
#include "xpt2046_wire.h"
#include "touch_config.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace xpt2046;

static Config configured() {
    Config c; c.mode=Mode::Calibrated; c.pins_confirmed=true;
    c.cs_pin=21; c.miso_pin=18; // Synthetic test configuration, NOT a PCB assignment.
    c.calibration={true,false,100,3900,100,3900}; return c;
}
struct Source {
    Frame frame; int reads=0; bool ok=true;
    static bool read(void* context,Frame& out) {
        auto& self=*static_cast<Source*>(context); ++self.reads; out=self.frame; return self.ok;
    }
    void sample(int x,int y,int z) {
        frame={}; for(int i=0;i<3;++i) {frame.x[i]=uint16_t(x);frame.y[i]=uint16_t(y);}
        frame.pressure_before=frame.pressure_after=uint16_t(z);
    }
};
static void arm(Driver& d,Source& s) {
    Point p; s.sample(0,0,0);
    assert(d.poll(p)==State::Released); assert(d.poll(p)==State::Released);
}
static void configuration() {
    Source s; Driver d; Point p;
    assert(!d.begin(Config{},Source::read,&s)); // Disabled configuration still performs no IO.
    assert(d.poll(p)==State::Released && s.reads==0);
    std::vector<Config> bad;
    auto c=configured(); c.pins_confirmed=false; bad.push_back(c);
    c=configured(); c.cs_pin=-1; bad.push_back(c);
    c=configured(); c.miso_pin=-1; bad.push_back(c);
    c=configured(); c.cs_pin=c.miso_pin; bad.push_back(c);
    c=configured(); c.cs_pin=19; bad.push_back(c);
    c=configured(); c.miso_pin=20; bad.push_back(c);
    c=configured(); c.cs_pin=99; bad.push_back(c);
    c=configured(); c.calibration.confirmed=false; bad.push_back(c);
    c=configured(); c.calibration.x1=c.calibration.x0; bad.push_back(c);
    c=configured(); c.calibration.y0=-1; bad.push_back(c);
    c=configured(); c.calibration.y1=4096; bad.push_back(c);
    c=configured(); c.spi_hz=40000000; bad.push_back(c);
    c=configured(); c.rotation=4; bad.push_back(c);
    c=configured(); c.release_threshold=c.press_threshold; bad.push_back(c);
    c=configured(); c.stable_polls=0; bad.push_back(c);
    c=configured(); c.bad_polls=0; bad.push_back(c);
    for(auto invalid:bad) {
        assert(!d.begin(invalid,Source::read,&s)); assert(d.poll(p)==State::Released);
    }
    assert(s.reads==0 && !d.begin(configured(),nullptr,nullptr));
    c=nexusTouchConfig();
    static const bool expected_settled[]={false,true,true};
    static const uint32_t expected_hz[]={1000000,1000000,250000};
    static const char* expected_name[]={"A","B","C"};
    assert(c.raw_settled_probe==expected_settled[NEXUS_TOUCH_RAW_EXPERIMENT]);
    assert(c.spi_hz==expected_hz[NEXUS_TOUCH_RAW_EXPERIMENT]);
    assert(std::strcmp(nexusTouchExperiment(c),expected_name[NEXUS_TOUCH_RAW_EXPERIMENT])==0);
    assert(c.mode==Mode::RawDiagnostics && c.pins_confirmed && c.cs_pin==7 && c.miso_pin==8);
    assert(!c.calibration.confirmed); // No invented calibration or LVGL press.
    assert(d.begin(c,Source::read,&s)); s.sample(2000,2000,900);
    for(int i=0;i<12;++i) assert(d.poll(p)==State::Released);
    assert(s.reads==12 && d.lastReadValid() && d.lastFrame().x[0]==2000);
    std::puts("PASS config: disabled/16 invalid configs cause zero reads; temporary board raw config never presses");
}
static void mapping() {
    Config c=configured(); Point p;
    for(int swap=0;swap<2;++swap) for(int ix=0;ix<2;++ix) for(int iy=0;iy<2;++iy) {
        c.calibration={true,bool(swap),int16_t(ix?3900:100),int16_t(ix?100:3900),
                       int16_t(iy?3900:100),int16_t(iy?100:3900)};
        for(int rotation=0;rotation<4;++rotation) {
            c.rotation=uint8_t(rotation);
            for(int u: {0,239}) for(int v: {0,319}) {
                int a=u?c.calibration.x1:c.calibration.x0, b=v?c.calibration.y1:c.calibration.y0;
                assert(mapPoint(c,uint16_t(swap?b:a),uint16_t(swap?a:b),p));
                const Point expected[]={ {int16_t(u),int16_t(v)}, {int16_t(319-v),int16_t(u)},
                    {int16_t(239-u),int16_t(319-v)}, {int16_t(v),int16_t(239-u)} };
                assert(p.x==expected[rotation].x && p.y==expected[rotation].y);
            }
        }
    }
    c=configured();
    for(int rotation=0;rotation<4;++rotation) {
        c.rotation=uint8_t(rotation);
        const int w=rotation%2?320:240,h=rotation%2?240:320;
        for(int x=0;x<=4095;x+=31) for(int y=0;y<=4095;y+=49) {
            assert(mapPoint(c,uint16_t(x),uint16_t(y),p));
            assert(p.x>=0 && p.x<w && p.y>=0 && p.y<h);
        }
    }
    p={12,34}; assert(!mapPoint(c,4096,0,p) && p.x==12 && p.y==34);
    assert(mapPoint(configured(),2000,2000,p)); assert(p.x==160 && p.y==119);
    std::puts("PASS mapping: all 4 rotations x swaps/inversions, corners, center, dense ADC grid, clamped edges");
}
static void filtering() {
    Config c=configured(); Source s; Driver d; Point p;
    assert(d.begin(c,Source::read,&s)); s.sample(1000,2000,900);
    for(int i=0;i<4;++i) assert(d.poll(p)==State::Released); // Finger down at boot is ignored.
    arm(d,s); s.sample(1000,2000,500); assert(d.poll(p)==State::Released);
    s.sample(1000,2000,900); assert(d.poll(p)==State::Released);
    s.frame.x[0]=998;s.frame.x[1]=1002;s.frame.x[2]=1000;
    assert(d.poll(p)==State::Pressed); Point expected; assert(mapPoint(c,1000,2000,expected));
    assert(p.x==expected.x && p.y==expected.y);
    s.sample(1000,2000,450); assert(d.poll(p)==State::Pressed); // Pressure hysteresis.
    s.sample(0,0,0); assert(d.poll(p)==State::Pressed); // One release bounce cannot click.
    s.sample(1200,2400,900); assert(d.poll(p)==State::Pressed);
    assert(mapPoint(c,1200,2400,expected)); assert(p.x==expected.x && p.y==expected.y);
    s.sample(0,0,0); assert(d.poll(p)==State::Pressed); assert(d.poll(p)==State::Released);
    arm(d,s); s.sample(1200,2400,900); d.poll(p); assert(d.poll(p)==State::Pressed);
    const Point last=p; s.frame.x[1]=3500;
    assert(d.poll(p)==State::Pressed && p.x==last.x && p.y==last.y);
    assert(d.poll(p)==State::Pressed); assert(d.poll(p)==State::Cancel);
    s.sample(1000,2000,900); assert(d.poll(p)==State::Released); // No replay after persistent noise.
    arm(d,s); d.poll(p); s.sample(1000,2000,900); d.poll(p); assert(d.poll(p)==State::Pressed);
    s.ok=false; assert(d.poll(p)==State::Cancel && !d.lastReadValid()); s.ok=true;
    assert(d.poll(p)==State::Released); arm(d,s);
    s.sample(1000,2000,900); s.frame.x[0]=4096;
    for(int i=0;i<5;++i) assert(d.poll(p)==State::Released);
    s.sample(1000,2000,900); s.frame.pressure_after=0;
    for(int i=0;i<5;++i) assert(d.poll(p)==State::Released);
    s.frame.pressure_after=9000; assert(d.poll(p)==State::Cancel);
    std::puts("PASS filter: startup gating, pressure/coordinate debounce, median/drag, release bounce, noise cancel/recovery, errors");
}
struct WireBus {
    bool allowed=true, selected=false, floating=false, zero=false, paddingOnly=false, commandOnly=false,settled=false;
    int begins=0, ends=0, bytes=0;
    const std::vector<uint8_t> commands={0xB0,0xC0,0xD0,0x90,0xD0,0x90,0xD0,0x90,0xB0,0xC0};
    const std::vector<uint16_t> adc={400,3995,1000,2000,1001,2001,999,1999,450,3945};
    bool begin() { ++begins; selected=allowed; return allowed; }
    uint8_t transfer(uint8_t value) {
        assert(selected); const int conversion=bytes/3, phase=bytes++%3;
        const int sample=settled?conversion/2:conversion;
        assert(conversion<(settled?21:10));
        const uint8_t command=settled?(conversion==20?0x90:uint8_t(commands[sample]|1)):commands[sample];
        assert(value==(phase==0?command:0));
        if(floating) return 0xFF;
        if(zero) return 0;
        if(commandOnly)return phase==0?0xA5:0;
        if(paddingOnly)return phase==2?7:0;
        const uint16_t raw=conversion==20?0:settled && conversion%2==0?uint16_t(42+sample):adc[sample];
        return phase==0?0:uint8_t(phase==1?(raw<<3)>>8:(raw<<3)&255);
    }
    void end() {assert(selected);selected=false;++ends;}
};
static void wire() {
    WireBus bus; Frame frame; WireTrace trace;
    assert(readWireFrame(bus,frame,&trace));
    assert(bus.begins==1 && bus.ends==1 && bus.bytes==30 && !bus.selected);
    assert(frame.x[0]==1000 && frame.x[1]==1001 && frame.x[2]==999);
    assert(frame.y[0]==2000 && frame.y[1]==2001 && frame.y[2]==1999);
    assert(frame.pressure_before==500 && frame.pressure_after==600);
    assert(trace.started && trace.count==30 && trace.adc[0]==400 && trace.adc[1]==3995);
    for(int i=0;i<30;++i) assert(trace.tx[i]==(i%3==0?bus.commands[i/3]:0));
    assert(trace.rx[7]==uint8_t((1000<<3)>>8) && trace.rx[8]==uint8_t((1000<<3)&255));
    assert(!trace.payload_zero && !trace.payload_ones);
    WireBus failed; failed.allowed=false;
    assert(!readWireFrame(failed,frame,&trace) && failed.bytes==0 && failed.ends==0);
    assert(!trace.started && trace.count==0); // No stale trace from previous transaction.
    WireBus disconnected; disconnected.floating=true;
    assert(!readWireFrame(disconnected,frame,&trace) && disconnected.bytes==30 && disconnected.ends==1);
    assert(trace.payload_ones && trace.adc[0]==4095 && trace.rx[0]==0xFF);
    WireBus low;low.zero=true;
    assert(!readWireFrame(low,frame,&trace) && trace.payload_zero && trace.count==30);
    for(int i=0;i<30;++i)assert(trace.rx[i]==0);
    for(int i=0;i<10;++i)assert(trace.adc[i]==0);
    // Preserve raw ADC evidence even though the legacy pressure formula maps 4095 to 0.
    assert(frame.x[0]==0 && frame.pressure_before==0 && 4095+trace.adc[0]-trace.adc[1]==4095);
    WireBus padding;padding.paddingOnly=true;
    assert(readWireFrame(padding,frame,&trace) && !trace.payload_zero && trace.rx[2]==7);
    for(int i=0;i<10;++i)assert(trace.adc[i]==0); // Different from physical RX all-zero.
    WireBus command;command.commandOnly=true;
    assert(!readWireFrame(command,frame,&trace) && trace.payload_zero && trace.rx[0]==0xA5);
    WireBus probe;probe.settled=true;
    assert(readWireFrame(probe,frame,&trace,true));
    assert(probe.begins==1 && probe.ends==1 && probe.bytes==63 && trace.count==63 && trace.settled);
    for(int i=0;i<10;++i) {
        assert(trace.first[i]==42+i && trace.adc[i]==probe.adc[i]);
        assert(trace.tx[6*i]==uint8_t(probe.commands[i]|1) && trace.tx[6*i+3]==trace.tx[6*i]);
    }
    assert(trace.tx[60]==0x90 && trace.tx[61]==0 && trace.tx[62]==0); // Completed power-down conversion.
    assert(frame.x[0]==1000 && frame.x[1]==1001 && frame.y[0]==2000 && frame.pressure_before==500);
    assert(!pressureIssue(trace,300));
    trace.adc[0]=0;trace.adc[1]=0;assert(pressureIssue(trace,300));
    trace.adc[0]=4;trace.adc[1]=0;assert(pressureIssue(trace,300));
    trace.adc[0]=400;trace.adc[1]=300;assert(pressureIssue(trace,300));
    trace.adc[0]=0;trace.adc[1]=3000;assert(pressureIssue(trace,300));
    trace.adc[0]=0;trace.adc[1]=4095;assert(!pressureIssue(trace,300));
    trace.adc[8]=22;trace.adc[9]=0;assert(pressureIssue(trace,300)); // User's second pressure pair must also reject.
    trace.adc[8]=22;trace.adc[9]=0;assert(pressureIssue(trace,300)); // User's second pressure pair must also reject.
    assert(coordinateSpread(trace,2)==2 && coordinateSpread(trace,3)==2);
    trace.adc[2]=1773;trace.adc[4]=3023;trace.adc[6]=3851;
    assert(coordinateSpread(trace,2)==2078); // User's spurious PRESSED sample is nowhere near stable.
    std::puts("PASS SPI wire: exact 30 TX/RX bytes and Z1/Z2, ADC padding/mask, zero/FF/no-transfer evidence, paired CS; SPI has no ACK");
    std::puts("PASS RAW experiment: 63 bytes, PD01 same-channel dummy/kept results, final PD00; invalid zero/reversed Z rejection and coordinate spreads");
}
int main() {
    configuration(); mapping(); filtering(); wire();
    std::puts("PASS XPT2046 OFFLINE ONLY: synthetic ADC/transport; no physical hardware or real HID.");
}
