#include <Arduino.h>
#include <TFT_eSPI.h>
#include <driver/gpio.h>
#include <tusb.h>
#include <string.h>
#include "board_config.h"
#include "touch_config.h"
#include "touch_esp32.h"
#include "ui.h"
#include "xpt2046_wire.h"
#include "touch_live_config.h"
#include "touch_spi_diag.h"
#if NEXUS_TOUCH_LIVE_PREVIEW
#include "touch_live_ui.h"
#endif

namespace {
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
static_assert(NEXUS_TOUCH_LIVE_PREVIEW && !NEXUS_TOUCH_LIVE_XY_ONLY &&
              NEXUS_TOUCH_RAW_EXPERIMENT==0,"D0/D1 require unchanged LIVE_PRESSURE LEGACY30");
TouchSpiDisplayDiagnostic diagnostic_display;
const char* const diagnostic_experiment=NEXUS_TOUCH_SPI_FREEZE_TFT ? "D1_LIVE_PRESSURE" : "D0_LIVE_PRESSURE";
#endif
TFT_eSPI* screen = nullptr;
TaskHandle_t ui_task = nullptr;
xpt2046::Config config;
xpt2046::Driver driver;
uint32_t last_diagnostic = 0;
xpt2046::WireTrace wire_trace, diagnostic_trace;
struct CsLevels { int idle=-1, active=-1, released=-1, lcd_active=-1, lcd_released=-1; } cs_levels, diagnostic_cs;
uint8_t diagnostic_phase = 0;
uint32_t diagnostic_id = 0;
bool diagnostic_ok = false;
char debug_pending[256];
uint16_t debug_length=0, debug_offset=0;
uint32_t debug_dropped=0, debug_full=0, debug_offline=0;
uint32_t loop_entries=0, poll_entries=0, sample_entries=0, samples_ok=0;
uint32_t last_heartbeat=0, last_stage_report=500;
uint32_t last_sample_marker=0;
const char* init_reason="NOT_CALLED";
const char* sample_reason="NOT_POLLED";
const char* stage="BOOT";
const char* tx_reason="IDLE";
#if NEXUS_TOUCH_LIVE_PREVIEW
xpt2046::LiveFilter live_filter;
xpt2046::LiveResult live_result;
bool live_wire_ok=false;
uint32_t live_sample_at=0,live_display_at=0;
#endif

void pumpDebugWrite() {
    if (!debug_length) return;
    if (!tud_cdc_n_connected(0)) {
        ++debug_offline; ++debug_dropped; debug_length=debug_offset=0;
        tx_reason="CDC_NOT_CONNECTED"; return;
    }
    const uint32_t available=tud_cdc_n_write_available(0);
    if (!available) {
        ++debug_full; tx_reason="CDC_FULL";
        tud_cdc_n_write_flush(0); // Request progress, never wait for completion.
        return;
    }
    uint32_t count=debug_length-debug_offset;
    if (count>32) count=32;
    if (count>available) count=available;
    const uint32_t sent=tud_cdc_n_write(0,debug_pending+debug_offset,count);
    debug_offset+=sent;
    tx_reason=sent<count ? "PARTIAL_WRITE" : "SENDING";
    if (debug_offset==debug_length) { debug_length=debug_offset=0;tx_reason="IDLE"; }
    tud_cdc_n_write_flush(0);
}
bool tryDebugWrite(const char* text,uint32_t length) {
    // Serial is CDC0 in this build. Its real TX FIFO is only 64 bytes. Keep one
    // bounded line and drain <=32 bytes per call; never use Serial.write's loop.
    pumpDebugWrite();
    if (!tud_cdc_n_connected(0)) {
        ++debug_offline; ++debug_dropped; tx_reason="CDC_NOT_CONNECTED"; return false;
    }
    if (debug_length) { ++debug_dropped;tx_reason="LINE_PENDING";return false; }
    if (length>=sizeof(debug_pending)) { ++debug_dropped;tx_reason="LINE_TOO_LONG";return false; }
    memcpy(debug_pending,text,length);debug_length=uint16_t(length);debug_offset=0;
    pumpDebugWrite();
    return true;
}

bool reservedPin(int pin) {
    const int pins[] = {TFT_CS,TFT_DC,TFT_RST,TFT_BL,TFT_SCLK,TFT_MOSI,
        I2C_SDA_PIN,I2C_SCL_PIN,USB_DN_PIN,USB_DP_PIN,
        MIC_I2S_BCLK,MIC_I2S_WS,MIC_I2S_DATA,AMP_I2S_BCLK,AMP_I2S_LRC,AMP_I2S_DIN};
    for (int used:pins) if (pin==used) return true;
    // N8's module flash connections are not candidates for expansion wiring.
    return pin>=26 && pin<=32;
}
bool validBoardConfig(const xpt2046::Config& c) {
    return xpt2046::validConfig(c) && c.miso_pin==TFT_MISO &&
           GPIO_IS_VALID_OUTPUT_GPIO(c.cs_pin) && GPIO_IS_VALID_GPIO(c.miso_pin) &&
           !reservedPin(c.cs_pin) && !reservedPin(c.miso_pin);
}
const char* configurationReason(const xpt2046::Config& c,TFT_eSPI& display) {
    if (c.mode==xpt2046::Mode::Disabled) return "MODE_DISABLED";
    if (!c.pins_confirmed) return "PINS_UNCONFIRMED";
    if (!xpt2046::validConfig(c)) return "INVALID_CONFIG";
    if (c.miso_pin!=TFT_MISO) return "MISO_MISMATCH";
    if (!GPIO_IS_VALID_OUTPUT_GPIO(c.cs_pin) || !GPIO_IS_VALID_GPIO(c.miso_pin)) return "INVALID_GPIO";
    if (reservedPin(c.cs_pin) || reservedPin(c.miso_pin)) return "PIN_CONFLICT";
    if (c.rotation!=display.getRotation()) return "ROTATION_MISMATCH";
    if (display.width()!=320 || display.height()!=240) return "RESOLUTION_MISMATCH";
    return nullptr;
}
struct SharedBus {
    SPIClass* spi = nullptr;
    bool begin() {
        if (!screen) {sample_reason="NO_DISPLAY";return false;}
        if (xPortInIsrContext()) {sample_reason="SAMPLE_ISR";return false;}
        if (xTaskGetCurrentTaskHandle()!=ui_task) {sample_reason="SAMPLE_WRONG_TASK";return false;}
        if (screen->dmaBusy()) {sample_reason="DMA_BUSY";return false;}
        // Both flush and this callback run sequentially on the UI task. Finish any
        // TFT transaction, leaving its internal flags consistent before raw SPI.
        stage="TFT_END_WRITE";screen->endWrite();
        digitalWrite(TFT_CS,HIGH);
        spi=&TFT_eSPI::getSPIinstance(); // Same FSPI object, never SPI.begin()/SPI.end().
        stage="SPI_LOCK_WAIT";
        spi->beginTransaction(SPISettings(config.spi_hz,MSBFIRST,SPI_MODE0));
        stage="SPI_TRANSFER";
        cs_levels.idle=digitalRead(config.cs_pin);
        digitalWrite(config.cs_pin,LOW);
        cs_levels.active=digitalRead(config.cs_pin);
        cs_levels.lcd_active=digitalRead(TFT_CS);
        return true;
    }
    uint8_t transfer(uint8_t value) { return spi->transfer(value); }
    void end() {
        digitalWrite(config.cs_pin,HIGH);
        cs_levels.released=digitalRead(config.cs_pin);
        cs_levels.lcd_released=digitalRead(TFT_CS);
        spi->endTransaction();
    }
};
bool readFrame(void*,xpt2046::Frame& frame) {
    ++sample_entries;stage="SAMPLE_ENTER";
    const uint32_t now=millis();
    if (now-last_sample_marker>=1000U) {
        last_sample_marker=now;
        const char* marker="TOUCH ENTER SAMPLE_ENTER\n";
        tryDebugWrite(marker,uint32_t(strlen(marker)));
    }
    SharedBus bus;
    cs_levels=CsLevels{};
    const bool wire_ok=xpt2046::readWireFrame(bus,frame,&wire_trace,
        config.mode==xpt2046::Mode::RawDiagnostics && config.raw_settled_probe);
#if NEXUS_TOUCH_LIVE_PREVIEW
    live_wire_ok=wire_ok; // Keep raw transport result; bad Z must remain visible to XY experiment.
#endif
    const char* pressure_issue=wire_ok ? xpt2046::pressureIssue(wire_trace,config.release_threshold) : nullptr;
    const bool ok=wire_ok && !pressure_issue;
    if (ok) {++samples_ok;sample_reason="READ_OK";}
    else if (wire_trace.started)
        sample_reason=pressure_issue ? pressure_issue : wire_trace.payload_zero ? "DATA_ZERO" :
            wire_trace.payload_ones ? "DATA_FF" : "READ_FAILED";
    stage="SAMPLE_RETURN";
    return ok;
}
// <=4 lines/sec. Capture ONE transaction; retain its ID and RX even on failure.
void rawDiagnostic() {
    const uint32_t now=millis();
    if (now-last_diagnostic<250U) return;
    last_diagnostic=now;
    char line[256]; int length=0;
    if (diagnostic_phase==0) {
        diagnostic_trace=wire_trace; diagnostic_cs=cs_levels;
        diagnostic_ok=driver.lastReadValid(); ++diagnostic_id;
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
        diagnostic_display=nexusTouchDisplayDiagnostic();
#endif
        auto& spi=TFT_eSPI::getSPIinstance();
        length=snprintf(line,sizeof(line),
            "TOUCH CFG id=%lu spi=FSPI obj=%p bus=%p reg_port=%u CLK=%d DIN=%d DO=%d TCS=%d LCDCS=%d hz=%lu mode=0 MSB cs=%d/%d/%d lcd=%d/%d started=%d n=%u read=%s exp=%s\n",
            (unsigned long)diagnostic_id,static_cast<void*>(&spi),static_cast<void*>(spi.bus()),
            unsigned(SPI_PORT),TFT_SCLK,TFT_MOSI,TFT_MISO,config.cs_pin,TFT_CS,(unsigned long)config.spi_hz,
            diagnostic_cs.idle,diagnostic_cs.active,diagnostic_cs.released,
            diagnostic_cs.lcd_active,diagnostic_cs.lcd_released,diagnostic_trace.started,diagnostic_trace.count,
            diagnostic_trace.settled?"SETTLED24":"LEGACY30",
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
            diagnostic_experiment);
#else
            nexusTouchExperiment(config));
#endif
    } else if (diagnostic_phase==1) {
        const auto& t=diagnostic_trace;
        bool rx_zero=t.count==(t.settled?63:30), adc_zero=rx_zero;
        for (unsigned i=0;i<t.count;++i) if(t.rx[i]) rx_zero=false;
        for (unsigned i=0;i<10;++i) if(t.adc[i]) adc_zero=false;
        const unsigned before=4095U+t.adc[0]-t.adc[1],after=4095U+t.adc[8]-t.adc[9];
        const unsigned effective_before=before==4095 ? 0 : before,effective_after=after==4095 ? 0 : after;
        const bool xy_stable=xpt2046::coordinateSpread(t,2)<=config.max_spread &&
                             xpt2046::coordinateSpread(t,3)<=config.max_spread;
        const char* contact=!diagnostic_ok ? "UNKNOWN" :
            effective_before>=config.press_threshold && effective_after>=config.press_threshold && xy_stable ? "PRESSED" :
            effective_before<=config.release_threshold && effective_after<=config.release_threshold ? "RELEASED" : "UNSTABLE";
        length=snprintf(line,sizeof(line),
            "TOUCH ADC id=%lu ok=%d ack=NA rx0=%d adc0=%d data=%s x=%u,%u,%u y=%u,%u,%u z1=%u,%u z2=%u,%u p=%u,%u eff=%u,%u contact=%s raw=1\n",
            (unsigned long)diagnostic_id,diagnostic_ok,rx_zero,adc_zero,
            !t.started ? "NOT_STARTED" : t.payload_zero ? "ZERO" : t.payload_ones ? "FF" : "MIXED",
            t.adc[2],t.adc[4],t.adc[6],t.adc[3],t.adc[5],t.adc[7],t.adc[0],t.adc[8],t.adc[1],t.adc[9],
            before,after,effective_before,effective_after,contact);
    } else if (diagnostic_phase<4) {
        const uint8_t* bytes=diagnostic_phase==2 ? diagnostic_trace.tx : diagnostic_trace.rx;
        length=snprintf(line,sizeof(line),"TOUCH %s id=%lu n=%u hex=",
            diagnostic_phase==2 ? "TX" : "RX",(unsigned long)diagnostic_id,diagnostic_trace.count);
        for (unsigned i=0;i<diagnostic_trace.count && length>0 && length+4<int(sizeof(line));++i)
            length+=snprintf(line+length,sizeof(line)-length,"%02X%s",bytes[i],i+1==diagnostic_trace.count?"":" ");
        if (length>0 && length+1<int(sizeof(line))) { line[length++]='\n';line[length]='\0'; }
    } else if (diagnostic_phase==4) {
        const auto& t=diagnostic_trace;
        const char* issue=!t.started ? "NO_FRAME" : t.payload_zero ? "DATA_ZERO" : t.payload_ones ? "DATA_FF" :
            xpt2046::pressureIssue(t,config.release_threshold);
        const unsigned dx=xpt2046::coordinateSpread(t,2),dy=xpt2046::coordinateSpread(t,3);
        length=snprintf(line,sizeof(line),
            "TOUCH QUALITY id=%lu issue=%s dx=%u dy=%u limit=%u first_z1=%u,%u first_z2=%u,%u\n",
            (unsigned long)diagnostic_id,issue?issue:dx>config.max_spread || dy>config.max_spread?"XY_SPREAD":"NONE",
            dx,dy,config.max_spread,t.first[0],t.first[8],t.first[1],t.first[9]);
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
    } else {
        const auto& d=diagnostic_display;
        length=snprintf(line,sizeof(line),
            "TOUCH DISPLAY id=%lu exp=%s first=%d pending=%d frozen=%d handlers=%lu flushes=%lu writes=%lu skipped=%lu pixels=%lu\n",
            (unsigned long)diagnostic_id,diagnostic_experiment,d.first_complete,d.pending,d.frozen,
            (unsigned long)d.handlers,(unsigned long)d.flushes,(unsigned long)d.writes,
            (unsigned long)d.skipped,(unsigned long)d.pixels);
#endif
    }
    if (length>0 && length<int(sizeof(line)) && tryDebugWrite(line,uint32_t(length)))
#if NEXUS_TOUCH_SPI_DIAGNOSTIC
        diagnostic_phase=(diagnostic_phase+1)%6;
#else
        diagnostic_phase=(diagnostic_phase+1)%5;
#endif
}
ui_touch_state_t readTouch(lv_point_t* point) {
    ++poll_entries;
    if (xPortInIsrContext()) {sample_reason="POLL_ISR";return UI_TOUCH_CANCEL;}
    if (xTaskGetCurrentTaskHandle()!=ui_task) {sample_reason="POLL_WRONG_TASK";return UI_TOUCH_CANCEL;}
    stage="POLL_ENTER";
    xpt2046::Point p;
    const auto state=driver.poll(p);
    stage="POLL_RETURN";
    if (config.mode==xpt2046::Mode::RawDiagnostics) rawDiagnostic();
#if NEXUS_TOUCH_LIVE_PREVIEW
    live_sample_at=millis();
    live_result=live_filter.update(wire_trace,live_wire_ok,live_sample_at);
    if(live_result.state==xpt2046::State::Pressed) {
        point->x=live_result.pixel.x;point->y=live_result.pixel.y;return UI_TOUCH_PRESSED;
    }
    return live_result.state==xpt2046::State::Cancel?UI_TOUCH_CANCEL:UI_TOUCH_RELEASED;
#endif
    if (config.mode==xpt2046::Mode::RawDiagnostics) return UI_TOUCH_RELEASED;
    if (state==xpt2046::State::Pressed) { point->x=p.x;point->y=p.y;return UI_TOUCH_PRESSED; }
    return state==xpt2046::State::Cancel ? UI_TOUCH_CANCEL : UI_TOUCH_RELEASED;
}
}

#if defined(NEXUS_ENCODER_PREVIEW) && NEXUS_ENCODER_PREVIEW
bool tryNexusEncoderDiagnosticLine(const char* text,uint32_t length) {
    if (xPortInIsrContext()) return false;
    return tryDebugWrite(text,length);
}
#endif

void serviceNexusTouchDiagnostics() {
    if (xPortInIsrContext()) return;
    ++loop_entries;
    pumpDebugWrite();
    const uint32_t now=millis();
#if NEXUS_TOUCH_LIVE_PREVIEW
    if(uint32_t(now-live_display_at)>=150U) {
        live_display_at=now;
        auto shown=live_result;
        if(strcmp(init_reason,"OK") || uint32_t(now-live_sample_at)>150U) {
            shown.raw_available=shown.rx_valid=shown.xy_stable=shown.mapped=shown.z_valid=false;
            shown.pressure=xpt2046::LivePressure::Invalid;shown.state=xpt2046::State::Cancel;
            shown.reason=strcmp(init_reason,"OK")?init_reason:"NO_LVGL_POLL";
        }
        touchLiveUiUpdate(shown,!wire_trace.started?"NONE":wire_trace.payload_ones?"FF":wire_trace.payload_zero?"ZERO":"MIXED",now);
    }
#endif
    char line[256]; int length=0;
#if NEXUS_TOUCH_LIVE_PREVIEW
    const char* mode=NEXUS_TOUCH_LIVE_XY_ONLY?"LIVE_XY":"LIVE_PRESSURE";
#else
    const char* mode=config.mode==xpt2046::Mode::RawDiagnostics ? "RAW" :
        config.mode==xpt2046::Mode::Calibrated ? "CALIBRATED" : "DISABLED";
#endif
    if (now-last_heartbeat>=1000U) {
        const uint8_t lines=tud_cdc_n_get_line_state(0);
        length=snprintf(line,sizeof(line),
            "MAIN HEARTBEAT ms=%lu loops=%lu init=%s mode=%s polls=%lu samples=%lu ok=%lu fifo=%u dtr=%u rts=%u\n",
            (unsigned long)now,(unsigned long)loop_entries,init_reason,mode,(unsigned long)poll_entries,
            (unsigned long)sample_entries,(unsigned long)samples_ok,unsigned(CFG_TUD_CDC_TX_BUFSIZE),
            unsigned(lines&1),unsigned((lines>>1)&1));
        if (length>0 && length<int(sizeof(line)) && tryDebugWrite(line,uint32_t(length))) last_heartbeat=now;
    } else if (now-last_stage_report>=1000U) {
        length=snprintf(line,sizeof(line),
            "TOUCH STAGE last=%s sample=%s tx=%s drop=%lu full=%lu offline=%lu\n",
            stage,poll_entries ? sample_reason : "NO_LVGL_POLL",tx_reason,
            (unsigned long)debug_dropped,(unsigned long)debug_full,(unsigned long)debug_offline);
        if (length>0 && length<int(sizeof(line)) && tryDebugWrite(line,uint32_t(length))) last_stage_report=now;
    }
}

bool prepareNexusTouchPins() { return prepareNexusTouchPins(nexusTouchConfig()); }
bool prepareNexusTouchPins(const xpt2046::Config& requested) {
    if (xPortInIsrContext() || !validBoardConfig(requested)) return false;
    digitalWrite(requested.cs_pin,HIGH); // Inactive latch before switching to output mode.
    pinMode(requested.cs_pin,OUTPUT);
    return true;
}
bool initNexusTouch(TFT_eSPI& display) {
    return initNexusTouch(display,nexusTouchConfig());
}
bool initNexusTouch(TFT_eSPI& display,const xpt2046::Config& requested) {
    stage="INIT_ENTER";
    if (xPortInIsrContext()) {init_reason="INIT_ISR";return false;}
    ui_set_touch_reader(nullptr);
    config=requested;
#if NEXUS_TOUCH_LIVE_PREVIEW
    touchLiveUiBegin();live_filter.begin(NEXUS_TOUCH_LIVE_XY_ONLY,config.rotation);
    live_result=xpt2046::LiveResult{};live_wire_ok=false;live_sample_at=millis();
#endif
    poll_entries=sample_entries=samples_ok=0;
    const char* reason=configurationReason(config,display);
    if (reason) {
        init_reason=reason;stage="INIT_FAILED";
        char message[80];const int length=snprintf(message,sizeof(message),"TOUCH INIT reason=%s\n",reason);
        if (length>0 && length<int(sizeof(message))) tryDebugWrite(message,uint32_t(length));
        return false; // Disabled/invalid configuration: no GPIO/SPI access or fabricated coordinate.
    }
    screen=&display; ui_task=xTaskGetCurrentTaskHandle();
    prepareNexusTouchPins(config);
    driver.begin(config,readFrame,nullptr);
    diagnostic_phase=0; last_diagnostic=millis();
    ui_set_touch_reader(readTouch);
    init_reason="OK";stage="INIT_READY";sample_reason="NOT_POLLED";
#if NEXUS_TOUCH_LIVE_PREVIEW
    const char* message="Touch LIVE experimental input: UNCAL, HID OFF\n";
#else
    const char* message=config.mode==xpt2046::Mode::RawDiagnostics ?
        "Touch RAW diagnostics: UI input disabled\n" : "Touch configured: hardware validation pending\n";
#endif
    tryDebugWrite(message,uint32_t(strlen(message)));
    return true;
}
