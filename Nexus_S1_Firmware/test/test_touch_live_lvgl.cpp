// Appended to the actual main.cpp macro sender by test_touch_live.py.
#include "Arduino.h"
#include "TFT_eSPI.h"
#include "touch_esp32.h"
#include "touch_config.h"
#include "touch_live_ui.h"
#include "tusb.h"
#include <cstdlib>
TaskHandle_t hostTask=reinterpret_cast<void*>(1);
bool hostISR=false;
uint32_t hostMillis=0,hostCdcSpace=CFG_TUD_CDC_TX_BUFSIZE,hostCdcWrites=0,hostCdcFlushes=0,hostCdcMaxWrite=0;
uint8_t hostCdcLineState=1;
bool hostCdcConnected=true,hostCdcPartial=false;
std::string hostCdcOutput;
int gpioWrites=0,pinModes=0,pinLevels[49]={};
SPIClass hostSPI;SerialSpy Serial;
static uint16_t framebuffer[320*240];
static void flushLive(lv_disp_drv_t* d,const lv_area_t* a,lv_color_t* pixels) {
    for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x)framebuffer[y*320+x]=(pixels++)->full;
    lv_disp_flush_ready(d);
}
static void runLive(unsigned ms) {
    for(unsigned t=0;t<ms;t+=5) {
        hostMillis+=5;lv_tick_inc(5);hostCdcSpace=CFG_TUD_CDC_TX_BUFSIZE;
        serviceNexusTouchDiagnostics();serviceMacro();lv_timer_handler();
    }
}
static lv_obj_t* labelLive(lv_obj_t* root,const char* needle,bool exact=false) {
    if(!lv_obj_is_visible(root))return nullptr;
    if(lv_obj_check_type(root,&lv_label_class)) {
        auto text=lv_label_get_text(root);
        if(exact?!strcmp(text,needle):strstr(text,needle)!=nullptr)return root;
    }
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i)if(auto found=labelLive(lv_obj_get_child(root,i),needle,exact))return found;
    return nullptr;
}
static unsigned clickLive() {
    auto l=labelLive(lv_layer_top(),"CLICK:");assert(l);
    return unsigned(std::strtoul(strstr(lv_label_get_text(l),"CLICK:")+6,nullptr,10));
}
static void moveLive(int x,int y) {
    hostSPI.touchX=uint16_t((y*4095+119)/239);
    hostSPI.touchY=uint16_t(((319-x)*4095+159)/319);
}
static void tapLive(int x,int y) {
    hostSPI.touched=false;runLive(400);moveLive(x,y);hostSPI.touched=true;
    runLive(140);hostSPI.touched=false;runLive(260);
}
static void tapLabelLive(const char* name) {
    auto l=labelLive(lv_scr_act(),name,true);assert(l);lv_obj_update_layout(l);
    lv_area_t a;lv_obj_get_coords(l,&a);tapLive((a.x1+a.x2)/2,(a.y1+a.y2)/2);
}
static lv_obj_t* sliderLive(lv_obj_t* root) {
    if(!lv_obj_is_visible(root))return nullptr;
    if(lv_obj_check_type(root,&lv_slider_class))return root;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i)if(auto s=sliderLive(lv_obj_get_child(root,i)))return s;
    return nullptr;
}
static void saveLive(const char* name) {
    lv_refr_now(nullptr);auto f=std::fopen(name,"wb");assert(f);
    assert(std::fwrite(framebuffer,sizeof(framebuffer),1,f)==1);std::fclose(f);
    auto label=labelLive(lv_layer_top(),"CLICK:");assert(label);lv_obj_update_layout(label);
    auto parent=lv_obj_get_parent(label);
    if(lv_obj_get_height(label)>lv_obj_get_height(parent)-1)
        std::printf("HUD overflow h=%d parent=%d text=%s\n",int(lv_obj_get_height(label)),int(lv_obj_get_height(parent)),lv_label_get_text(label));
    assert(lv_obj_get_height(label)<=lv_obj_get_height(parent)-1); // All diagnostic rows really fit.
    lv_obj_update_layout(parent);
    lv_area_t area;lv_obj_get_coords(parent,&area);
    assert(area.x1>=0 && area.y1>=20 && area.x2<320 && area.y2<240);
    // Check every rendered row, not just the wrapping label's assigned width.
    const char* row=lv_label_get_text(label);
    while(*row) {
        const char* end=strchr(row,'\n');const auto size=end?size_t(end-row):strlen(row);
        lv_point_t measured;
        std::string single(row,size);
        lv_txt_get_size(&measured,single.c_str(),&lv_font_montserrat_14,-1,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
        assert(measured.x<=lv_obj_get_width(label));
        if(!end)break;
        row=end+1;
    }
}
int main() {
    lv_init();static lv_color_t buffer[320*10];static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw,buffer,nullptr,3200);
    static lv_disp_drv_t display;lv_disp_drv_init(&display);display.hor_res=320;display.ver_res=240;
    display.draw_buf=&draw;display.flush_cb=flushLive;lv_disp_drv_register(&display);
    ui_init();ui_set_macro_callback(onMacroRequested);
    TFT_eSPI tft;
    auto bad=nexusTouchConfig();bad.miso_pin=22;
    assert(!initNexusTouch(tft,bad));runLive(200);
    assert(labelLive(lv_layer_top(),"MISO_MISMATCH"));
    assert(initNexusTouch(tft));runLive(500);
    assert(nexusTouchConfig().spi_hz==1000000 && !nexusTouchConfig().raw_settled_probe);
    assert(clickLive()==0 && ui_current_page()==nexus::Page::Standby);
    auto toggle=labelLive(lv_layer_top(),"DIAG",true);assert(toggle);lv_obj_update_layout(toggle);
    lv_area_t toggle_area;lv_obj_get_coords(toggle,&toggle_area);
    assert(toggle_area.x1>=277 && toggle_area.y1>=0 && toggle_area.y2<20);
    tapLive(298,10);assert(!labelLive(lv_layer_top(),"CLICK:"));
    tapLive(298,10);assert(clickLive()==0 && ui_current_page()==nexus::Page::Standby);
    // This point lies under the transparent-to-input diagnostic panel.
    tapLive(190,65);
    assert(clickLive()==1 && ui_current_page()==nexus::Page::Home);
    tapLabelLive("Control");assert(ui_current_page()==nexus::Page::Control);
    for(auto name:{"Ctrl+C","Ctrl+V","Alt+Tab","Win+D"}) {
        auto before=clickLive();tapLabelLive(name);assert(clickLive()==before+1);
        assert(labelLive(lv_scr_act(),"PREVIEW HID OFF"));
    }
    assert(macroHid.attempts.empty());
    for(int id=0;id<nexus::ActionCount;++id) {
        assert(executeMacro(id));serviceMacro();assert(!macroBusy);
        assert(!sendActionReport(id,false) && !sendActionReport(id,true));
    }
    assert(macroHid.attempts.empty()); // Keyboard and Consumer, both routes, no physical API.
    tapLive(21,19);tapLabelLive("Settings");
    assert(ui_current_page()==nexus::Page::Settings);
    auto slider=sliderLive(lv_scr_act());assert(slider);lv_area_t a;lv_obj_get_coords(slider,&a);
    hostSPI.touched=false;runLive(400);moveLive(a.x1+4,(a.y1+a.y2)/2);hostSPI.touched=true;runLive(140);
    for(int x=a.x1+12;x<a.x2-3;x+=8){moveLive(x,(a.y1+a.y2)/2);runLive(35);}
    hostSPI.touched=false;runLive(200);assert(lv_slider_get_value(slider)>85);
    tapLive(21,19);tapLabelLive("Focus");assert(ui_current_page()==nexus::Page::Focus);
    tapLabelLive("+");tapLabelLive("START");runLive(1000);
    tapLive(21,19);assert(ui_current_page()==nexus::Page::Home);
    tapLabelLive("Monitor");assert(ui_current_page()==nexus::Page::Dashboard);
    tapLabelLive("METRIC >");tapLive(21,19);
    auto before=clickLive();hostSPI.touched=false;runLive(400);moveLive(200,120);hostSPI.touched=true;runLive(140);
    hostSPI.floating=true;runLive(300);hostSPI.floating=false;hostSPI.touched=false;runLive(400);
    assert(clickLive()==before); // Cancelled press is not an application click.
    hostSPI.noisy=true;hostSPI.touched=true;runLive(300);
    assert(clickLive()==before && labelLive(lv_layer_top(),"RAW:1800,2500"));
    assert(labelLive(lv_layer_top(),"dx:800 dy:400") && labelLive(lv_layer_top(),"S:N M:N"));
    assert(labelLive(lv_layer_top(),"HIST:") && labelLive(lv_layer_top(),"XY_SPREAD"));
    saveLive("live_spread.rgb565");
    hostSPI.noisy=false;hostSPI.touched=false;runLive(400);
    hostSPI.floating=true;runLive(300);assert(labelLive(lv_layer_top(),"RX:FF"));saveLive("live_ff.rgb565");
    hostSPI.floating=false;
#if NEXUS_TOUCH_LIVE_XY_ONLY
    // Return to Standby with pressure, then use genuinely differing synthetic ADC Z=0/XY data.
    hostSPI.touched=false;runLive(400);tapLive(21,19);assert(ui_current_page()==nexus::Page::Standby);
    before=clickLive();hostSPI.zero=true;runLive(400);hostSPI.zero=false;
    moveLive(160,120);hostSPI.touchZ1=hostSPI.touchZ2=0;hostSPI.touched=true;
    runLive(150);assert(labelLive(lv_layer_top(),"XY_ONLY_EXPERIMENTAL"));saveLive("live_xy_press.rgb565");
    runLive(300);assert(clickLive()==before+1 && ui_current_page()==nexus::Page::Home);
    runLive(1600);assert(clickLive()==before+1); // No repeated fake clicks while same signal remains.
#else
    hostSPI.touchZ1=hostSPI.touchZ2=0;hostSPI.touched=true;before=clickLive();runLive(1000);
    assert(clickLive()==before && labelLive(lv_layer_top(),"Z2_ZERO"));saveLive("live_bad_z.rgb565");
#endif
    hostSPI.touched=false;runLive(400);hostSPI.touchZ1=100;hostSPI.touchZ2=3745;
    before=clickLive();moveLive(160,120);hostSPI.touched=true;runLive(150);
    assert(labelLive(lv_layer_top(),"P:450,450 MID") && labelLive(lv_layer_top(),"RAW:"));
    assert(labelLive(lv_layer_top(),"Z1:100,100") && labelLive(lv_layer_top(),"Z2:3745,3745"));
#if NEXUS_TOUCH_LIVE_XY_ONLY
    assert(labelLive(lv_layer_top(),"XY_ONLY_EXPERIMENTAL"));
    saveLive("live_mid_xy.rgb565");runLive(300);assert(clickLive()==before+1);
    runLive(1200);assert(clickLive()==before+1);
    hostSPI.touched=false;runLive(400);before=clickLive();
    hostSPI.touched=true;runLive(140); // Inferred press, before its normal tap-end.
    hostSPI.noisy=true;runLive(300);
    assert(clickLive()==before && labelLive(lv_layer_top(),"HIST:"));
    assert(labelLive(lv_layer_top(),"XY_SPREAD")); // History cannot complete the cancelled click.
    hostSPI.noisy=false;
#else
    assert(labelLive(lv_layer_top(),"PRESSURE_MID"));
    saveLive("live_mid_pressure.rgb565");runLive(1200);assert(clickLive()==before);
#endif
    hostSPI.touched=false;runLive(400);
    assert(labelLive(lv_layer_top(),"P:0,0 RELEASE"));saveLive("live_release.rgb565");
    // HUD ages an explicitly historical coordinate even without new touch polls.
    xpt2046::LiveResult history;history.history_valid=true;history.history_pixel={319,239};history.history_at=0xFFFFFF00U;
    touchLiveUiUpdate(history,"NONE",0x100U);
    assert(labelLive(lv_layer_top(),"HIST:319,239 512ms") && labelLive(lv_layer_top(),"RAW:N/A"));
    touchLiveUiUpdate(history,"NONE",0xFFFFFEFFU);saveLive("live_history.rgb565");
    assert(labelLive(lv_layer_top(),"4294967295ms"));
    assert(hostSPI.begins==hostSPI.ends && pinLevels[7]==HIGH && pinLevels[10]==HIGH);
    assert(macroHid.attempts.empty());
    std::puts("PASS LIVE OFFLINE: actual production SPI adapter -> shared UI registered POINTER -> real LVGL clicks/invalid cancellation, five pages/slider/Focus/macros; all 21 keyboard/Consumer actions produce ZERO reports; diagnostic overlay/layout. Synthetic SDK/ADC only.");
}
