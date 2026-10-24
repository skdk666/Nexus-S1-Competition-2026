// Host-only: actual LVGL, actual flush and actual ESP32 adapter, synthetic SPI/CDC.
#include "tusb.h"
#include "touch_esp32.h"
#include "touch_config.h"
#include <string>
TaskHandle_t hostTask=reinterpret_cast<void*>(1);
bool hostISR=false;
uint32_t hostMillis=0,hostCdcSpace=CFG_TUD_CDC_TX_BUFSIZE,hostCdcWrites=0,hostCdcFlushes=0,hostCdcMaxWrite=0;
uint8_t hostCdcLineState=1;
bool hostCdcConnected=true,hostCdcPartial=false;
std::string hostCdcOutput;
int gpioWrites=0,pinModes=0,pinLevels[49]={};
SPIClass hostSPI;SerialSpy Serial;

static void handler() {
    lv_timer_handler();
    // Exact production ordering: the gate is committed AFTER the handler.
    nexusTouchDisplayDiagnostic().afterHandler();
}
static void run(unsigned ms,bool drain=true) {
    for(unsigned t=0;t<ms;t+=5) {
        hostMillis+=5;lv_tick_inc(5);
        if(drain)hostCdcSpace=CFG_TUD_CDC_TX_BUFSIZE;
        serviceNexusTouchDiagnostics();
        lv_obj_invalidate(lv_scr_act()); // Ensure both variants receive ongoing flush requests.
        handler();
    }
}
static void gateTests() {
    TouchSpiDisplayDiagnostic d;
    d.afterHandler();assert(!d.frozen && !d.first_complete);
    d.flushed(0,0,319,9,true,true);d.afterHandler();
    assert(!d.frozen && !d.first_complete); // Last flag alone cannot prime a partial first screen.
    for(int y=0;y<240;y+=10) {
        d.flushed(0,y,319,y+9,y==230,true);
        assert(!d.frozen && !d.first_complete);
    }
    assert(d.pending);d.afterHandler();
    assert(d.first_complete && d.frozen==bool(NEXUS_TOUCH_SPI_FREEZE_TFT));
}
int main(int argc,char** argv) {
    assert(argc==2);gateTests();lv_init();
    static lv_color_t buffer[320*10];static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw,buffer,nullptr,3200);
    static lv_disp_drv_t display;lv_disp_drv_init(&display);
    display.hor_res=320;display.ver_res=240;display.draw_buf=&draw;display.flush_cb=my_disp_flush;
    auto disp=lv_disp_drv_register(&display);
    ui_init();assert(prepareNexusTouchPins());assert(initNexusTouch(tft));
    auto& d=nexusTouchDisplayDiagnostic();
    lv_timer_pause(disp->refr_timer);handler();
    assert(!d.flushes && !d.first_complete && !d.frozen);
    lv_timer_resume(disp->refr_timer);lv_timer_ready(disp->refr_timer);
    // Inspect state before and after the true handler boundary.
    lv_timer_handler();
    assert(d.pending && !d.first_complete && !d.frozen);
    assert(d.writes==24 && d.pixels==320*240 && tft.pushes==24);
    assert(draw.flushing==0 && draw.flushing_last==0);
    d.afterHandler();assert(d.first_complete && d.frozen==bool(NEXUS_TOUCH_SPI_FREEZE_TFT));
    auto writes=d.writes;auto pixels=d.pixels;
    hostSPI.zero=true;run(7000);
    hostSPI.zero=false;hostSPI.floating=true;run(7000);
    hostSPI.floating=false;hostSPI.touched=true;hostSPI.touchZ2=0;run(7000);
    hostSPI.touchZ2=3500;run(7000);
    assert(d.handlers>1000 && hostSPI.begins>500 && hostSPI.begins==hostSPI.ends);
    assert(hostSPI.lastHz==1000000 && !hostSPI.settled);
    assert(hostCdcOutput.find("issue=Z2_ZERO")!=std::string::npos);
    assert(hostCdcOutput.find("data=ZERO")!=std::string::npos);
    assert(hostCdcOutput.find("data=FF")!=std::string::npos);
    assert(hostCdcOutput.find("data=MIXED")!=std::string::npos);
    if(NEXUS_TOUCH_SPI_FREEZE_TFT) {
        assert(d.writes==writes && d.pixels==pixels && d.skipped>100);
    } else assert(d.writes>writes && d.pixels>pixels && !d.skipped);
    assert(tft.pushes==d.writes && tft.windows==d.writes && tft.pushedPixels==d.pixels);
    auto polls=hostSPI.begins;auto cdc=hostCdcWrites;
    hostCdcSpace=0;run(1600,false);assert(hostSPI.begins>polls && hostCdcWrites==cdc);
    hostCdcConnected=false;run(1600);assert(hostCdcWrites==cdc);
    hostCdcConnected=true;hostCdcPartial=true;run(1600);assert(hostCdcWrites>cdc);
    hostCdcPartial=false;run(4000);
    assert(hostCdcMaxWrite<=32 && !hostSPI.active && draw.flushing==0);
    auto f=std::fopen(argv[1],"wb");assert(f);
    assert(std::fwrite(hostCdcOutput.data(),hostCdcOutput.size(),1,f)==1);std::fclose(f);
    std::printf("PASS D%d: real LVGL 24-part first refresh, ready clears last, deferred freeze, ongoing flush/poll/CDC, 30-byte MODE0 shared SPI, failure RX retained and bounded/backpressured CDC. MOCK hardware.\n",NEXUS_TOUCH_SPI_FREEZE_TFT);
}
