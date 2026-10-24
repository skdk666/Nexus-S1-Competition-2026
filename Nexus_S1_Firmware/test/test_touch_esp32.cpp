#include "Arduino.h"
#include "TFT_eSPI.h"
#include "touch_esp32.h"
#include "touch_config.h"
#include "ui.h"
#include "tusb.h"
#include <cstdio>
TaskHandle_t hostTask=reinterpret_cast<void*>(1);
bool hostISR=false;
uint32_t hostMillis=1000,hostCdcSpace=CFG_TUD_CDC_TX_BUFSIZE,hostCdcWrites=0,hostCdcFlushes=0;
uint32_t hostCdcMaxWrite=0;
uint8_t hostCdcLineState=1; // DTR=1/RTS=0 must work with the native TinyUSB CDC path.
bool hostCdcConnected=true;
bool hostCdcPartial=false;
std::string hostCdcOutput;
int gpioWrites=0,pinModes=0,pinLevels[49]={};
SPIClass hostSPI; SerialSpy Serial;
static ui_touch_read_cb_t reader=nullptr;
void ui_set_touch_reader(ui_touch_read_cb_t cb) { reader=cb; }
static xpt2046::Config configured() {
    xpt2046::Config c; c.mode=xpt2046::Mode::Calibrated; c.pins_confirmed=true;
    c.cs_pin=7;c.miso_pin=8;c.calibration={true,false,100,3900,100,3900}; // Synthetic calibration.
    return c;
}
static void mainFor(unsigned duration,lv_point_t* poll=nullptr,bool drain=true) {
    for(unsigned i=0;i<duration;i+=5) {
        hostMillis+=5;
        if(drain)hostCdcSpace=CFG_TUD_CDC_TX_BUFSIZE; // Host consumes previous endpoint bytes.
        serviceNexusTouchDiagnostics(); // Same ordering as production loop(), outside LVGL.
        if(poll && i%30==0)assert(reader(poll)==UI_TOUCH_RELEASED);
    }
}
static std::string diagnosticCycle(lv_point_t& p,bool change_after_capture=false) {
    hostCdcOutput.clear();
    unsigned long id=0; bool captured=false;uint32_t captured_at=0;
    for(unsigned i=0;i<4000;i+=5) {
        mainFor(5,i%30==0 ? &p : nullptr);
        const auto cfg=hostCdcOutput.find("TOUCH CFG id=");
        if(!captured && cfg!=std::string::npos && hostCdcOutput.find('\n',cfg)!=std::string::npos) {
            assert(std::sscanf(hostCdcOutput.c_str()+cfg,"TOUCH CFG id=%lu",&id)==1);
            captured=true;
            captured_at=hostMillis;
            if(change_after_capture)hostSPI.zero=true;
        }
        const auto quality=hostCdcOutput.find("TOUCH QUALITY id="+std::to_string(id)+" ");
        if(captured && quality!=std::string::npos && hostCdcOutput.find('\n',quality)!=std::string::npos) {
            assert(hostMillis-captured_at>=950); // Four 250ms phases, allowing chunk-drain skew.
            assert(hostCdcOutput.find("TOUCH ADC id="+std::to_string(id)+" ")!=std::string::npos);
            assert(hostCdcOutput.find("TOUCH TX id="+std::to_string(id)+" ")!=std::string::npos);
            assert(hostCdcOutput.find("TOUCH RX id="+std::to_string(id)+" ")!=std::string::npos);
            return hostCdcOutput;
        }
    }
    assert(false && "Complete 5-line transaction must drain through the actual SDK-sized FIFO");
    return {};
}
int main() {
    TFT_eSPI display; lv_point_t p={0,0};
    assert(!prepareNexusTouchPins(xpt2046::Config{}));
    assert(!initNexusTouch(display,xpt2046::Config{}));
    assert(!reader && gpioWrites==0 && pinModes==0 && hostSPI.begins==0);
    auto bad=configured();bad.pins_confirmed=false;
    assert(!prepareNexusTouchPins(bad));
    assert(!initNexusTouch(display,bad));
    bad=configured();bad.miso_pin=22; assert(!initNexusTouch(display,bad)); // TFT_MISO mismatch.
    bad=configured();bad.cs_pin=12; assert(!initNexusTouch(display,bad)); // Display clock collision.
    bad=configured();bad.miso_pin=4; assert(!initNexusTouch(display,bad)); // SHT30 collision.
    bad=configured();bad.calibration.confirmed=false;assert(!initNexusTouch(display,bad));
    display.rotation=3;assert(!initNexusTouch(display,configured()));display.rotation=1;
    mainFor(1200);
    assert(hostCdcOutput.find("MAIN HEARTBEAT")!=std::string::npos);
    assert(hostCdcOutput.find("init=ROTATION_MISMATCH")!=std::string::npos);
    assert(hostCdcOutput.find("sample=NO_LVGL_POLL")!=std::string::npos);
    assert(!reader && gpioWrites==0 && pinModes==0 && hostSPI.begins==0);
    assert(initNexusTouch(display,configured()) && reader && pinModes==1);
    assert(gpioWrites==1 && pinLevels[7]==HIGH && hostSPI.bytes==0);
    assert(reader(&p)==UI_TOUCH_RELEASED && reader(&p)==UI_TOUCH_RELEASED);
    hostSPI.touched=true;
    assert(reader(&p)==UI_TOUCH_RELEASED && reader(&p)==UI_TOUCH_PRESSED);
    assert(p.x==160 && p.y==56 && !hostSPI.active && pinLevels[7]==HIGH);
    assert(hostSPI.begins==4 && hostSPI.ends==4 && hostSPI.bytes==120);
    const int reads=hostSPI.bytes,writes=gpioWrites,finishes=display.finishes;
    hostTask=reinterpret_cast<void*>(2);assert(reader(&p)==UI_TOUCH_CANCEL);
    hostTask=reinterpret_cast<void*>(1);hostISR=true;assert(reader(&p)==UI_TOUCH_CANCEL);hostISR=false;
    display.busy=true;assert(reader(&p)==UI_TOUCH_CANCEL);display.busy=false;
    assert(hostSPI.bytes==reads && gpioWrites==writes && display.finishes==finishes);
    hostSPI.touched=false;reader(&p);reader(&p);hostSPI.touched=true;reader(&p);
    assert(reader(&p)==UI_TOUCH_PRESSED);
    hostSPI.floating=true;assert(reader(&p)==UI_TOUCH_CANCEL && !hostSPI.active);
    hostSPI.floating=false;
    hostSPI.touchZ1=4;hostSPI.touchZ2=0;
    assert(reader(&p)==UI_TOUCH_CANCEL); // Invalid Z must not reach the calibrated press filter either.
    hostSPI.touchZ1=400;hostSPI.touchZ2=3500;
    const auto raw=nexusTouchConfig();
    assert(raw.mode==xpt2046::Mode::RawDiagnostics && !raw.calibration.confirmed);
    assert(prepareNexusTouchPins());
    assert(initNexusTouch(display)); // Exercise the actual application config, with mocked SDK.
    hostCdcOutput.clear();
    const auto spi_before_heartbeat=hostSPI.bytes;
    mainFor(1500); // No reader/LVGL calls: heartbeat/stage diagnostics must still be observable.
    assert(hostSPI.bytes==spi_before_heartbeat);
    assert(hostCdcOutput.find("MAIN HEARTBEAT")!=std::string::npos);
    assert(hostCdcOutput.find("init=OK mode=RAW polls=0 samples=0 ok=0")!=std::string::npos);
    assert(hostCdcOutput.find("dtr=1 rts=0")!=std::string::npos);
    assert(hostCdcOutput.find("sample=NO_LVGL_POLL")!=std::string::npos);
    // Root-cause regression: previous whole-line guard rejected every line longer
    // than the real 64-byte FIFO. Now observe exact complete lines via <=32-byte writes.
    auto captured=diagnosticCycle(p,true);
    assert(captured.find("spi=FSPI")!=std::string::npos);
    const std::string profile=std::string("n=")+(raw.raw_settled_probe?"63 read=SETTLED24":"30 read=LEGACY30")+" exp="+nexusTouchExperiment(raw);
    assert(captured.find("CLK=12 DIN=11 DO=8 TCS=7 LCDCS=10 hz="+std::to_string(raw.spi_hz)+" mode=0 MSB cs=1/0/1 lcd=1/1 started=1 "+profile)!=std::string::npos);
    assert(hostSPI.lastHz==raw.spi_hz); // Actual SPISettings, not just a log label.
    assert(captured.find("ok=1 ack=NA rx0=0 adc0=0 data=MIXED x=1000,1000,1000 y=2000,2000,2000 z1=400,400 z2=3500,3500 p=995,995 eff=995,995 contact=PRESSED raw=1\n")!=std::string::npos);
    if(raw.raw_settled_probe) {
        assert(captured.find("n=63 hex=B1 00 00 B1 00 00 C1 00 00 C1 00 00 D1 00 00 D1 00 00 91 00 00 91 00 00")!=std::string::npos);
        assert(captured.find("n=63 hex=00 03 D8 00 0C 80 00 03 D8 00 6D 60")!=std::string::npos);
        assert(captured.find("first_z1=123,123 first_z2=123,123")!=std::string::npos);
    } else {
        assert(captured.find("n=30 hex=B0 00 00 C0 00 00 D0 00 00 90 00 00")!=std::string::npos);
        assert(captured.find("n=30 hex=00 0C 80 00 6D 60")!=std::string::npos);
        assert(captured.find("first_z1=0,0 first_z2=0,0")!=std::string::npos);
    }
    assert(captured.find("contact=UNKNOWN")==std::string::npos); // Still one snapshot, not later zero frames.
    assert(hostCdcMaxWrite<=32 && CFG_TUD_CDC_TX_BUFSIZE==64);
    auto log=diagnosticCycle(p);
    assert(log.find("ok=0 ack=NA rx0=1 adc0=1 data=ZERO")!=std::string::npos);
    assert(log.find("z1=0,0 z2=0,0 p=4095,4095 eff=0,0 contact=UNKNOWN raw=1")!=std::string::npos);
    assert(log.find("hex=00 00 00 00 00 00 00 00 00 00")!=std::string::npos);
    hostSPI.zero=false;hostSPI.paddingOnly=true;
    log=diagnosticCycle(p);
    assert(log.find("ok=0 ack=NA rx0=0 adc0=1 data=MIXED")!=std::string::npos); // Decoded zero Z is now rejected.
    hostSPI.paddingOnly=false;hostSPI.touched=false;
    log=diagnosticCycle(p);
    assert(log.find("z1=0,0 z2=4095,4095 p=0,0 eff=0,0 contact=RELEASED")!=std::string::npos);
    hostSPI.floating=true;log=diagnosticCycle(p);
    assert(log.find("ok=0 ack=NA rx0=0 adc0=0 data=FF")!=std::string::npos);
    assert(log.find("contact=UNKNOWN")!=std::string::npos);
    hostSPI.floating=false;hostSPI.touched=true;hostSPI.touchZ1=0;hostSPI.touchZ2=3595;
    log=diagnosticCycle(p);assert(log.find("contact=UNKNOWN")!=std::string::npos && log.find("issue=Z1_ZERO")!=std::string::npos);
    hostSPI.touchZ1=100;hostSPI.touchZ2=3695;
    log=diagnosticCycle(p);assert(log.find("contact=UNSTABLE")!=std::string::npos);
    hostSPI.touchZ1=400;hostSPI.touchZ2=3500;
    hostSPI.touchZ1=0;hostSPI.touchZ2=0;log=diagnosticCycle(p);
    assert(log.find("contact=UNKNOWN")!=std::string::npos && log.find("issue=Z2_ZERO")!=std::string::npos);
    hostSPI.touchZ1=4;hostSPI.touchZ2=0;log=diagnosticCycle(p);
    assert(log.find("p=4099,4099")!=std::string::npos && log.find("contact=PRESSED")==std::string::npos);
    hostSPI.touchZ1=400;hostSPI.touchZ2=3500;hostSPI.noisy=true;log=diagnosticCycle(p);
    assert(log.find("contact=UNSTABLE")!=std::string::npos && log.find("issue=XY_SPREAD")!=std::string::npos);
    hostSPI.noisy=false;
    const uint32_t writes_before_full=hostCdcWrites;const int samples_before_full=hostSPI.begins;
    hostCdcSpace=0;mainFor(1500,&p,false);
    assert(hostCdcWrites==writes_before_full && hostSPI.begins>samples_before_full);
    hostCdcConnected=false;mainFor(1500,&p);
    assert(hostCdcWrites==writes_before_full); // No wait on a disconnected host.
    hostCdcConnected=true;hostCdcPartial=true;mainFor(1000,&p);
    assert(hostCdcWrites>writes_before_full); // Finite partial writes, offset retained.
    hostCdcPartial=false;mainFor(1000,&p);
    assert(diagnosticCycle(p).find("TOUCH RX")!=std::string::npos); // Full-FIFO/disconnect/partial recovery.
    hostCdcOutput.clear();hostMillis=UINT32_MAX-100;mainFor(80,&p);
    hostMillis=0;mainFor(1500,&p);
    assert(hostCdcOutput.find("MAIN HEARTBEAT")!=std::string::npos);
    assert(hostCdcOutput.find("TOUCH ADC")!=std::string::npos); // Timers recover across wrap.
    hostCdcOutput.clear();hostTask=reinterpret_cast<void*>(2);reader(&p);hostTask=reinterpret_cast<void*>(1);
    mainFor(1200);assert(hostCdcOutput.find("sample=POLL_WRONG_TASK")!=std::string::npos);
    hostCdcOutput.clear();display.busy=true;mainFor(1200,&p);display.busy=false;
    assert(hostCdcOutput.find("sample=DMA_BUSY")!=std::string::npos);
    assert(hostSPI.begins==hostSPI.ends);
    hostISR=true;assert(!prepareNexusTouchPins(configured()));assert(reader(&p)==UI_TOUCH_CANCEL);hostISR=false;
    const int before_prepare=hostSPI.bytes,old_writes=gpioWrites;
    assert(prepareNexusTouchPins(configured()));
    assert(pinLevels[7]==HIGH && gpioWrites==old_writes+1 && hostSPI.bytes==before_prepare);
    std::puts("PASS ESP32 adapter WITH MOCK SDK: actual 64-byte CDC FIFO, <=32-byte writes, complete long lines, independent heartbeat/init and guard reasons, DTR1/RTS0, partial/full/disconnected recovery, timer wrap; CS/MISO/owner/ISR/DMA guards and RAW inhibition. NOT hardware.");
}
