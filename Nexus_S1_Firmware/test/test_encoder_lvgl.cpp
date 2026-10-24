// Appended to the actual firmware HID sender; all GPIO/SPI/USB are synthetic.
#include "Arduino.h"
#include "TFT_eSPI.h"
#include "tusb.h"
#include "encoder_esp32.h"
#include "touch_esp32.h"
#include "touch_config.h"
#include <soc/gpio_struct.h>
#include <string>
#include <cstdlib>
#include <fstream>
TaskHandle_t hostTask=reinterpret_cast<void*>(1);
bool hostISR=false;
uint32_t hostMillis=0,hostCdcSpace=CFG_TUD_CDC_TX_BUFSIZE,hostCdcWrites=0,hostCdcFlushes=0,hostCdcMaxWrite=0;
uint8_t hostCdcLineState=1;
bool hostCdcConnected=true,hostCdcPartial=false;
std::string hostCdcOutput;
int gpioWrites=0,pinModes=0,pinLevels[49]={},encoderPinMode[49]={};
void (*encoderIrq[49])()={};
EncoderGpioRegisters GPIO;
SPIClass hostSPI;SerialSpy Serial;
static uint32_t flushes=0;
static std::vector<int> actions;
static bool syncFromCdc=false;
static bool exercisePreviewFallback=false;
static void intent(int id) { actions.push_back(id);onMacroRequested(id); }
static void flushEncoder(lv_disp_drv_t* d,const lv_area_t*,lv_color_t*) { ++flushes;lv_disp_flush_ready(d); }
static void run(unsigned ms,bool drainCdc=true) {
    for(unsigned i=0;i<ms;i+=5) {
        hostMillis+=5;lv_tick_inc(5);if(drainCdc)hostCdcSpace=64;
        if(syncFromCdc) {
            // Same RuntimeData snapshot/setter order as the actual main loop;
            // receivePcByte/applyPcFrame/pcOnline above are extracted production code.
            const auto snapshot=runtimeData;
            ui_set_pc_state(pcOnline(snapshot,hostMillis),snapshot.cpu_valid,snapshot.mem_valid,snapshot.cpu_temp_valid);
            ui_update_sensor(snapshot.temp,snapshot.humi,snapshot.cpu_temp,snapshot.cpu,snapshot.mem);
            ui_set_mode(snapshot.mode);
            if(exercisePreviewFallback)servicePreviewScene(snapshot,hostMillis);
        }
        serviceNexusTouchDiagnostics();serviceNexusEncoder();serviceMacro();lv_timer_handler();
    }
}
static void ab(uint8_t state) {
    auto old=GPIO.in;GPIO.in=(old&~6U)|(((state>>1)&1)<<1)|((state&1)<<2);
    const int writes=gpioWrites,spi=hostSPI.bytes;const auto cdc=hostCdcWrites;
    for(int pin:{1,2})if(((old^GPIO.in)>>pin)&1) {
        assert(encoderIrq[pin]);hostISR=true;encoderIrq[pin]();hostISR=false;
    }
    assert(gpioWrites==writes && hostCdcWrites==cdc && hostSPI.bytes==spi);
}
static void rotate(int direction=1) {
    if(direction>0)for(auto p:{2,0,1,3})ab(p);
    else for(auto p:{1,0,2,3})ab(p);
    run(40);
}
static std::string latestEncoderRaw() {
    size_t start=0;std::string result;
    while(start<hostCdcOutput.size()) {
        auto end=hostCdcOutput.find('\n',start);if(end==std::string::npos)break;
        auto line=hostCdcOutput.substr(start,end-start);
        if(line.compare(0,8,"ENC RAW ")==0)result=line;
        start=end+1;
    }
    assert(!result.empty());return result;
}
static uint32_t stat(const std::string& line,const char* key,bool second=false) {
    auto offset=line.find(key);assert(offset!=std::string::npos);offset+=strlen(key);
    if(second) { offset=line.find('/',offset);assert(offset!=std::string::npos);++offset; }
    return uint32_t(std::strtoul(line.c_str()+offset,nullptr,10));
}
static lv_obj_t* label(lv_obj_t* root,const char* text) {
    if(!lv_obj_is_visible(root))return nullptr;
    if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text))return root;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i)if(auto o=label(lv_obj_get_child(root,i),text))return o;
    return nullptr;
}
static lv_obj_t* button(const char* text) {
    auto o=label(lv_scr_act(),text);
    while(o && !lv_obj_check_type(o,&lv_btn_class))o=lv_obj_get_parent(o);
    assert(o);return o;
}
static void focus(lv_obj_t* target) {
    for(unsigned i=0;i<150 && lv_group_get_focused(ui_get_encoder_group())!=target;++i) {
        rotate();auto o=lv_group_get_focused(ui_get_encoder_group());
        assert(o && lv_obj_is_visible(o) && !lv_obj_has_state(o,LV_STATE_DISABLED));
    }
    assert(lv_group_get_focused(ui_get_encoder_group())==target);
}
static void click() { pinLevels[21]=LOW;run(60);pinLevels[21]=HIGH;run(140); }
static void open(const char* text) { focus(button(text));click(); }
static void expect(nexus::Scene scene,nexus::Page page) {
    const auto& s=ui_navigation_state();assert(s.active_scene==scene && s.current_page==page);
    auto focused=lv_group_get_focused(ui_get_encoder_group());
    assert(focused && lv_obj_is_visible(focused) && !lv_obj_has_state(focused,LV_STATE_DISABLED));
}
static void back() {
    const auto n=actions.size();pinLevels[21]=LOW;run(780);
    const auto before=ui_current_page();run(35);const auto after=ui_current_page();
    assert(before!=after || before==nexus::Page::Standby);
    run(1000);assert(ui_current_page()==after && actions.size()==n);
    pinLevels[21]=HIGH;run(120);assert(ui_current_page()==after && actions.size()==n);
}
static int seconds(lv_obj_t* root) {
    if(!lv_obj_is_visible(root))return -1;
    if(lv_obj_check_type(root,&lv_label_class)) {
        const auto t=lv_label_get_text(root);
        if(strlen(t)==5 && t[2]==':' && t[0]>='0' && t[0]<='9')return std::atoi(t)*60+std::atoi(t+3);
    }
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i) {int n=seconds(lv_obj_get_child(root,i));if(n>=0)return n;}
    return -1;
}
static lv_obj_t* slider(lv_obj_t* root) {
    if(!lv_obj_is_visible(root))return nullptr;
    if(lv_obj_check_type(root,&lv_slider_class))return root;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i)if(auto o=slider(lv_obj_get_child(root,i)))return o;
    return nullptr;
}
static void checkPreviewFallback(int first) {
    using nexus::Scene;using nexus::Page;
    assert(first>=0 && first<4);
    std::ifstream input("scene_frames.jsonl");assert(input);
    std::string frames[4];for(auto& frame:frames)assert(bool(std::getline(input,frame)));
    auto feed=[](const std::string& frame) { for(char c:frame+"\n")receivePcByte(c,hostMillis); };
    static unsigned unlocks=0,events=0;
    ui_set_navigation_callback([](const nexus::NavState&,const char* reason) {
        ++events;if(!strcmp(reason,"PREVIEW_FIRST_SCENE"))++unlocks;
    });
    syncFromCdc=exercisePreviewFallback=true;
    run(1000);assert(ui_current_page()==Page::Standby && !runtimeData.pc_seen);
    feed(R"({"cpu":20,"mem":50,"cpu_temp":null})");run(1000);
    assert(ui_current_page()==Page::Standby && !runtimeData.pc_scene_seen); // Online is not a recognized status.
    for(const char* bad:{"{", "{}", "[]", R"({"status":""})", R"({"status":null})",
                        R"({"status":1})", R"({"status":"gaming"})", R"({"status":"coding","cpu":101})",
                        R"({"status":"coding"}garbage)"})feed(bad);
    feed(std::string("{\"status\":\"coding\",\"padding\":\"")+std::string(600,'x')+"\"}");
    run(1000);assert(ui_current_page()==Page::Standby && !runtimeData.pc_scene_seen && !unlocks);
    // An expired status cannot unlock just because telemetry later refreshes PC Online.
    feed(frames[first]);hostMillis+=5100;lv_tick_inc(5100);run(100);
    assert(!ui_navigation_state().pc_online && ui_current_page()==Page::Standby);
    feed(R"({"cpu":30})");run(1000);assert(ui_current_page()==Page::Standby && !unlocks);
    // The real framer must receive LF; repeated valid status must not restart debounce.
    const auto midpoint=frames[first].size()/2;
    for(char c:frames[first].substr(0,midpoint))receivePcByte(c,hostMillis);
    run(1000);assert(ui_current_page()==Page::Standby);
    for(char c:frames[first].substr(midpoint)+"\n")receivePcByte(c,hostMillis);
    run(400);assert(ui_current_page()==Page::Standby);
    feed(frames[first]);run(350);assert(ui_current_page()==Page::Standby && !unlocks);
    run(150);expect(Scene(first),Page::Home);assert(unlocks==1);
    const char* visible[]={"Monitor","Command\nCtrl+Shift+P","New tab","Play/Pause"};
    assert(label(lv_scr_act(),visible[first]));
    const auto settledEvents=events;
    for(int i=0;i<3;++i) { feed(frames[first]);run(900);expect(Scene(first),Page::Home); }
    assert(events==settledEvents && unlocks==1); // No repeated unlock/redraw/nav callback.
    for(int offset=1;offset<=4;++offset) {
        const int scene=(first+offset)%4;feed(frames[scene]);run(900);expect(Scene(scene),Page::Home);
        assert(label(lv_scr_act(),visible[scene]));
        for(int other=0;other<4;++other)if(other!=scene)assert(!label(lv_scr_act(),visible[other]));
    }
    feed(frames[1]);run(900);lv_event_send(button("Focus"),LV_EVENT_CLICKED,nullptr);expect(Scene::Coding,Page::Focus);
    feed(frames[1]);run(900);expect(Scene::Coding,Page::Focus); // Repeats do not eject a child page.
    feed(frames[2]);run(200);expect(Scene::Coding,Page::Focus);
    feed(frames[1]);run(900);expect(Scene::Coding,Page::Focus); // Window flicker cancelled.
    feed(frames[2]);run(900);expect(Scene::Browser,Page::Home);
    run(6200);assert(!ui_navigation_state().pc_online);expect(Scene::Browser,Page::Home);
    feed(frames[3]);run(900);assert(ui_navigation_state().pc_online);expect(Scene::Media,Page::Home);
    // This is deliberately a one-shot boot fallback, not a forced wake after user standby.
    lv_event_send(button("<"),LV_EVENT_CLICKED,nullptr);assert(ui_current_page()==Page::Standby);
    feed(frames[2]);run(900);assert(ui_current_page()==Page::Standby && unlocks==1);
    assert(pinLevels[21]==LOW && stat(latestEncoderRaw(),"click=")==0 && actions.empty() && macroHid.attempts.empty());
    std::printf("PASS preview first=%s: actual Agent writes -> CDC parser -> trusted status/800ms -> visible scene; invalid/missing/expired/split/repeat rejection, all four homes, flicker/child/offline/reconnect, one-shot; SW held LOW throughout, zero clicks/HID. HOST SIMULATION only.\n",nexus::sceneName(Scene(first)));
}
static void checkEncoderHid() {
    using nexus::Scene;using nexus::Page;
    assert(NEXUS_ENCODER_HID_ENABLED && !NEXUS_HID_OUTPUT_DISABLED && nexus::ActionCount==22);
    std::ifstream input("scene_frames.jsonl");assert(input);
    std::string frames[4];for(auto& frame:frames)assert(bool(std::getline(input,frame)));
    auto feed=[&](int scene) { for(char c:frames[scene]+"\n")receivePcByte(c,hostMillis); };
    struct Binding { const char* label;int id,mod,key,usage; };
    // Independent expected USB usages, rather than accepting the production table as its own oracle.
    const Binding normal[]={{"Win+E",4,8,8,0},{"Alt+Tab",2,4,43,0},{"Win+D",3,8,7,0}};
    const Binding common[]={{"Ctrl+C",0,1,6,0},{"Ctrl+V",1,1,25,0},{"Alt+Tab",2,4,43,0},{"Win+D",3,8,7,0}};
    const Binding coding[]={{"Command\nCtrl+Shift+P",5,3,19,0},{"Open file\nCtrl+P",6,1,19,0},
        {"Terminal\nCtrl+`",7,1,53,0},{"Comment\nCtrl+/",8,1,56,0},{"Ctrl+D",21,1,7,0}};
    const Binding browser[]={{"New tab",9,1,23,0},{"Close tab",10,1,26,0},{"Reopen tab",11,3,23,0},
        {"Address bar",12,1,15,0},{"Refresh",13,1,21,0},{"Back",14,4,80,0}};
    const Binding media[]={{"Play/Pause",15,0,0,0xCD},{"Previous",16,0,0,0xB6},{"Next",17,0,0,0xB5},
        {"Volume +",18,0,0,0xE9},{"Volume -",19,0,0,0xEA},{"Mute",20,0,0,0xE2}};
    bool covered[22]={};
    auto pair=[&](const Binding& binding,size_t before) {
        assert(macroHid.attempts.size()==before+2);
        const auto& down=macroHid.attempts[before];const auto& up=macroHid.attempts[before+1];
        assert(down.success && up.success && down.id==(binding.usage?4:1) && up.id==down.id);
        assert(down.usage==binding.usage && down.report.modifiers==binding.mod && down.report.keys[0]==binding.key);
        for(int i=1;i<6;++i)assert(down.report.keys[i]==0);
        const KeyReport zero={};assert(up.usage==0 && !std::memcmp(&up.report,&zero,sizeof(zero)));
        assert(up.at-down.at==30);covered[binding.id]=true;
    };
    auto invoke=[&](const Binding& binding) {
        focus(button(binding.label));const auto before=macroHid.attempts.size(),intents=actions.size();
        click();assert(actions.size()==intents+1 && actions.back()==binding.id);pair(binding,before);
        run(100);assert(macroHid.attempts.size()==before+2); // No repeat after release.
    };
    pinLevels[21]=HIGH;syncFromCdc=exercisePreviewFallback=true;run(100);feed(0);run(900);expect(Scene::Normal,Page::Home);
    assert(macroHid.attempts.empty() && actions.empty()); // Auto-unlock alone never types.
    assert(hostCdcOutput.find("HID_OFF=0")!=std::string::npos);
    for(const auto& binding:normal)invoke(binding);
    open("Control");for(const auto& binding:common)invoke(binding);
    for(int i=0;i<3;++i)invoke(common[i%2]); // Consecutive clicks also clear modifiers.
    // Coalesced input: a queued detent and short press must activate the NEW focus.
    focus(button("Ctrl+C"));const auto beforeRace=macroHid.attempts.size(),beforeIntents=actions.size();
    for(auto phase:{2,0,1,3})ab(phase);
    auto swWithoutHandler=[](bool pressed,unsigned advance) {
        hostMillis+=advance;lv_tick_inc(advance);pinLevels[21]=pressed?LOW:HIGH;serviceNexusEncoder();
    };
    swWithoutHandler(true,1);swWithoutHandler(true,35);swWithoutHandler(false,60);swWithoutHandler(false,35);
    run(200);assert(actions.size()==beforeIntents+1 && actions.back()==1);pair(common[1],beforeRace);
    // A long hold on a macro navigates back and must never send ENTER or repeat.
    focus(button("Ctrl+C"));const auto beforeLong=macroHid.attempts.size();back();
    assert(macroHid.attempts.size()==beforeLong);expect(Scene::Normal,Page::Home);
    feed(1);run(900);expect(Scene::Coding,Page::Home);
    std::vector<lv_area_t> codingAreas;
    for(const char* text:{"Command\nCtrl+Shift+P","Open file\nCtrl+P","Terminal\nCtrl+`","Comment\nCtrl+/","Ctrl+D","Focus","Quick Actions"}) {
        auto target=button(text);lv_area_t area,caption;
        lv_obj_get_coords(target,&area);lv_obj_get_coords(label(target,text),&caption);
        assert(area.x1>=0 && area.y1>=0 && area.x2<320 && area.y2<240);
        assert(caption.x1>=area.x1 && caption.x2<=area.x2 && caption.y1>=area.y1 && caption.y2<=area.y2);
        for(const auto& other:codingAreas)assert(area.x2<other.x1 || other.x2<area.x1 || area.y2<other.y1 || other.y2<area.y1);
        codingAreas.push_back(area);
    }
    for(const auto& binding:coding)invoke(binding);
    feed(2);run(900);expect(Scene::Browser,Page::Home);for(const auto& binding:browser)invoke(binding);
    feed(3);run(900);expect(Scene::Media,Page::Home);for(const auto& binding:media)invoke(binding);
    open("Quick Actions");invoke({"Explorer  Win+E",4,8,8,0});
    open("Control");for(const auto& binding:common)invoke(binding); // Same actions keep Media parent context.
    for(bool seen:covered)assert(seen);
    // CDC updates and real UI navigation continue during the HID hold/release state machine.
    feed(2);run(900);expect(Scene::Browser,Page::Home);
    const auto parallel=macroHid.attempts.size();const auto pixels=flushes,cdc=hostCdcWrites;
    lv_event_send(button("New tab"),LV_EVENT_CLICKED,nullptr);serviceMacro();
    assert(macroHid.attempts.size()==parallel+1);feed(1);run(5);
    assert(runtimeData.mode==1 && pcOnline(runtimeData,hostMillis) && macroHid.attempts.size()==parallel+1);
    run(25);pair(browser[0],parallel);run(1000);expect(Scene::Coding,Page::Home);
    assert(flushes>pixels && hostCdcWrites>cdc);
    // Failed down must still release; a failed up keeps the sender busy until the zero report succeeds.
    auto beforeFailure=macroHid.attempts.size();macroHid.next_success=false;assert(executeMacro(nexus::CtrlD));serviceMacro();
    assert(!macroHid.attempts.back().success && !executeMacro(nexus::CtrlD));run(20);
    assert(macroHid.attempts.size()==beforeFailure+2 && macroHid.attempts.back().success && !macroHid.attempts.back().report.modifiers);
    assert(executeMacro(nexus::CtrlD));serviceMacro();run(25);macroHid.next_success=false;run(5);
    assert(!macroHid.attempts.back().success && !executeMacro(nexus::CtrlD));run(20);
    const KeyReport zero={};assert(macroHid.attempts.back().success && !std::memcmp(&macroHid.attempts.back().report,&zero,8));
    const auto notReady=macroHid.attempts.size();macroHid.available=false;onMacroRequested(nexus::CtrlD);serviceMacro();
    assert(macroHid.attempts.size()==notReady);macroHid.available=true;run(200);assert(macroHid.attempts.size()==notReady);
    assert(!executeMacro(nexus::ActionCount) && !executeMacro(255));
    std::puts("PASS encoder HID: all 22 independent Keyboard/Consumer mappings via actual GPIO -> LVGL focus/short press -> UI action -> production sender; current-focus race, consecutive clicks, long-hold exclusion, exact 30ms all-key release, failures/retry/not-ready, CDC scene changes + TFT + diagnostics concurrently. SYNTHETIC USB only; no Windows keys sent.");
}
int main(int argc,char** argv) {
    using nexus::Scene;using nexus::Page;
    lv_init();static lv_color_t buffer[320*10];static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw,buffer,nullptr,3200);
    static lv_disp_drv_t d;lv_disp_drv_init(&d);d.hor_res=320;d.ver_res=240;d.draw_buf=&draw;d.flush_cb=flushEncoder;
    lv_disp_drv_register(&d);ui_init();ui_set_macro_callback(intent);
    pinLevels[21]=LOW;GPIO.in=6;
    TFT_eSPI display;assert(prepareNexusTouchPins() && initNexusTouch(display));
    hostISR=true;assert(!initNexusEncoder());hostISR=false;
    assert(initNexusEncoder() && !initNexusEncoder());
    assert(encoderPinMode[1]==INPUT_PULLUP && encoderPinMode[2]==INPUT_PULLUP && encoderPinMode[21]==INPUT_PULLUP);
    int encoders=0;for(auto i=lv_indev_get_next(nullptr);i;i=lv_indev_get_next(i))if(lv_indev_get_type(i)==LV_INDEV_TYPE_ENCODER)++encoders;
    assert(encoders==1);
    run(2200);
    assert(ui_current_page()==Page::Standby && !actions.size());
    auto boot=latestEncoderRaw();
    assert(stat(boot,"SW=")==0 && stat(boot,"click=")==0 && stat(boot,"back=")==0);
    assert(boot.find("sw=PRESS arm=0 held=0")!=std::string::npos);
    if(argc==2) {
        if(!strcmp(argv[1],"--hid"))checkEncoderHid();
        else checkPreviewFallback(std::atoi(argv[1]));
        return 0;
    }
    pinLevels[21]=HIGH;run(10);assert(ui_current_page()==Page::Standby);
    run(100); // Only a real stable release re-arms the boot-held input.
    // Exercise the actual registered callback: rotation during a short gesture's
    // PR/REL pair must not move focus before release or lose the buffered step.
    run(100);
    auto sampleSw=[](bool pressed,uint32_t advance) {
        hostMillis+=advance;lv_tick_inc(advance);pinLevels[21]=pressed?LOW:HIGH;serviceNexusEncoder();
    };
    sampleSw(true,5);sampleSw(true,30);sampleSw(false,40);sampleSw(false,30);
    auto encoder=lv_indev_get_next(nullptr);
    while(lv_indev_get_type(encoder)!=LV_INDEV_TYPE_ENCODER)encoder=lv_indev_get_next(encoder);
    lv_indev_data_t sample={};encoder->driver->read_cb(encoder->driver,&sample);
    assert(sample.state==LV_INDEV_STATE_PR && !sample.enc_diff);
    for(auto phase:{2,0,1,3})ab(phase);
    sample={};encoder->driver->read_cb(encoder->driver,&sample);
    assert(sample.state==LV_INDEV_STATE_REL && !sample.enc_diff);
    sample={};encoder->driver->read_cb(encoder->driver,&sample);
    assert(sample.state==LV_INDEV_STATE_REL && sample.enc_diff==1);
    hostSPI.touched=true;run(1600);assert(ui_current_page()==Page::Standby && !actions.size()); // RAW never clicks.
    ui_set_pc_state(true,true,true,false);ui_update_sensor(25,45,0,20,50);ui_set_mode(1);run(900);
    assert(ui_current_page()==Page::Standby);click();expect(Scene::Coding,Page::Home);
    open("Focus");expect(Scene::Coding,Page::Focus);open("+");open("-");open("START");
    const int before=seconds(lv_scr_act());assert(before>0);run(1200);back();expect(Scene::Coding,Page::Home);
    open("Focus");assert(seconds(lv_scr_act())<before);back();expect(Scene::Coding,Page::Home);
    ui_set_mode(2);run(900);expect(Scene::Browser,Page::Home);
    open("Quick Actions");open("Focus");assert(seconds(lv_scr_act())<before);
    back();expect(Scene::Browser,Page::QuickActions);back();expect(Scene::Browser,Page::Home);
    back();assert(ui_current_page()==Page::Standby);ui_set_pc_state(false,false,false,false);run(200);click();expect(Scene::Normal,Page::Home);
    auto initial=lv_group_get_focused(ui_get_encoder_group());rotate(1);assert(lv_group_get_focused(ui_get_encoder_group())!=initial);
    rotate(-1);assert(lv_group_get_focused(ui_get_encoder_group())==initial);
    open("Monitor");open("METRIC >");back();expect(Scene::Normal,Page::Home);
    open("Control");for(const char* name:{"Ctrl+C","Ctrl+V","Alt+Tab","Win+D"}) {
        auto n=actions.size();open(name);assert(actions.size()==n+1);
    }
    focus(button("Ctrl+C"));back();expect(Scene::Normal,Page::Home); // Long hold on macro must not invoke it.
    open("Settings");auto s=slider(lv_scr_act());assert(s);focus(s);click();assert(lv_group_get_editing(ui_get_encoder_group()));
    auto value=lv_slider_get_value(s);rotate();assert(lv_slider_get_value(s)!=value);
    click();assert(!lv_group_get_editing(ui_get_encoder_group()));
    click();assert(lv_group_get_editing(ui_get_encoder_group()));back();expect(Scene::Normal,Page::Home);
    ui_set_pc_state(true,true,true,false);ui_set_mode(1);run(700);
    pinLevels[21]=LOW;run(170);expect(Scene::Coding,Page::Home);
    pinLevels[21]=HIGH;run(160);expect(Scene::Coding,Page::Home); // Auto scene change cancels held SW.
    ui_set_mode(3);run(900);expect(Scene::Media,Page::Home);
    for(const char* name:{"Play/Pause","Previous","Next","Volume +","Volume -","Mute"})open(name);
    open("Quick Actions");open("Focus");open("EXIT");
    expect(Scene::Media,Page::QuickActions); // Existing EXIT also navigates to its parent.
    open("Focus");assert(label(lv_scr_act(),"START"));
    const auto stopped=seconds(lv_scr_act());assert(stopped>0);run(1500);
    assert(seconds(lv_scr_act())==stopped); // Only the explicit EXIT stops the retained timer.
    back();expect(Scene::Media,Page::QuickActions);back();expect(Scene::Media,Page::Home);
    for(int id=0;id<nexus::ActionCount;++id) {
        assert(!sendActionReport(uint8_t(id),false) && !sendActionReport(uint8_t(id),true));
        assert(executeMacro(id));serviceMacro();run(40);
    }
    assert(macroHid.attempts.empty());
    const auto cycles=hostSPI.begins;const auto frames=flushes;hostSPI.floating=true;run(3000);hostSPI.floating=false;
    assert(hostSPI.begins>cycles && flushes>frames && hostSPI.lastHz==1000000 && !hostSPI.settled);
    assert(hostCdcOutput.find("read=LEGACY30 exp=A")!=std::string::npos);
    assert(hostCdcOutput.find("TOUCH RX")!=std::string::npos && hostCdcOutput.find("data=FF")!=std::string::npos);
    assert(hostCdcMaxWrite<=32 && hostSPI.begins==hostSPI.ends);
    const auto page=ui_current_page();hostTask=reinterpret_cast<void*>(2);serviceNexusEncoder();hostTask=reinterpret_cast<void*>(1);
    hostISR=true;serviceNexusEncoder();hostISR=false;assert(ui_current_page()==page);
    // Real adapter diagnostics, same bounded CDC FIFO as RAW; no fake serial writer.
    assert(hostCdcOutput.find("ENC CFG env=encoder_preview init=1 A=1 B=2 SW=21 input=PULLUP SW_active=LOW debounce_ms=30 common=GND edges_per_step=4 direction=1 HID_OFF=1\n")!=std::string::npos);
    back();assert(ui_current_page()==Page::Standby);run(1200);
    auto baseline=latestEncoderRaw();rotate();rotate();rotate(-1);run(1200);
    auto moved=latestEncoderRaw();
    assert(stat(moved,"irq=")-stat(baseline,"irq=")==12);
    assert(stat(moved,"dec=")-stat(baseline,"dec=")==2 && stat(moved,"dec=",true)-stat(baseline,"dec=",true)==1);
    assert(stat(moved,"lv=")-stat(baseline,"lv=")==2 && stat(moved,"lv=",true)-stat(baseline,"lv=",true)==1);
    assert(stat(moved,"bad=")==stat(baseline,"bad=") && stat(moved,"rd=")>stat(baseline,"rd="));
    assert(stat(moved,"A=")==1 && stat(moved,"B=")==1 && stat(moved,"SW=")==1);
    assert(moved.find("sw=REL arm=1 held=0")!=std::string::npos);
    click();run(1200);auto pressed=latestEncoderRaw();
    assert(stat(pressed,"click=")==stat(moved,"click=")+1 && stat(pressed,"short=")==stat(moved,"short=")+1);
    assert(stat(pressed,"edges=")>=stat(moved,"edges=")+2);
    assert(ui_current_page()==Page::Home); // Logging did not consume the short press.
    // Stable raw levels other than idle are reported even without a full step.
    ab(2);pinLevels[21]=LOW;run(2300);auto held=latestEncoderRaw();
    assert(stat(held,"A=")==1 && stat(held,"B=")==0 && stat(held,"SW=")==0);
    assert(stat(held,"click=")==stat(pressed,"click=")+1 && stat(held,"short=")==stat(pressed,"short=")); // Counted before release, even for a long hold.
    assert(held.find("sw=PRESS arm=0 held=0")!=std::string::npos);
    assert(stat(held,"back=")==stat(pressed,"back=")+1 && ui_current_page()==Page::Standby);
    pinLevels[21]=HIGH;ab(3);run(1200);
    assert(stat(latestEncoderRaw(),"click=")==stat(held,"click=")); // Release never counts again, including navigation rebind.
    // Full/disconnected host cannot stall input, SPI, or UI. No busy-wait.
    hostCdcSpace=0;auto writes=hostCdcWrites;auto spiBefore=hostSPI.begins;auto flushBefore=flushes;
    for(auto phase:{2,0,1,3})ab(phase);run(1600,false);
    assert(hostCdcWrites==writes && hostSPI.begins>spiBefore && flushes>flushBefore);
    hostCdcConnected=false;run(1400);assert(hostCdcWrites==writes);
    hostCdcConnected=true;run(1600);assert(hostCdcWrites>writes);
    // Disconnection may truncate a queued line. Complete diagnostic lines still
    // have to retain their own fields, with no interleaved RAW touch message.
    uint32_t previousMs=0;bool haveMs=false;
    size_t start=0;
    while(start<hostCdcOutput.size()) {
        auto end=hostCdcOutput.find('\n',start);if(end==std::string::npos)break;
        auto line=hostCdcOutput.substr(start,end-start);
        if(line.compare(0,8,"ENC RAW ")==0) {
            assert(line.size()<256 && line.find("TOUCH")==std::string::npos);
            auto ms=stat(line,"ms=");if(haveMs)assert(uint32_t(ms-previousMs)>=1000);
            previousMs=ms;haveMs=true;
        }
        start=end+1;
    }
    assert(haveMs && hostCdcMaxWrite<=32 && macroHid.attempts.empty());
    // End-to-end software path: actual Agent writes -> production CDC parser ->
    // RuntimeData -> real LVGL pages -> the same physical encoder adapter.
    std::ifstream input("scene_frames.jsonl");assert(input);
    std::string sceneFrames[4];for(auto& frame:sceneFrames)assert(bool(std::getline(input,frame)));
    auto sendFrame=[](const std::string& frame) { for(char c:frame+"\n")receivePcByte(c,hostMillis); };
    auto sendScene=[&](int mode) { sendFrame(sceneFrames[mode]); };
    const char* specific[]={"Monitor","Command\nCtrl+Shift+P","New tab","Play/Pause"};
    runtimeData=RuntimeData{};syncFromCdc=true;run(100);
    assert(ui_current_page()==Page::Standby && !ui_navigation_state().pc_online);
    for(int mode=0;mode<4;++mode) {
        sendScene(mode);run(1000);
        assert(ui_current_page()==Page::Standby && ui_navigation_state().stable_scene==Scene(mode));
    }
    click();expect(Scene::Media,Page::Home);assert(label(lv_scr_act(),specific[3]));
    for(int mode=0;mode<4;++mode) {
        sendScene(mode);run(1000);expect(Scene(mode),Page::Home);
        assert(label(lv_scr_act(),specific[mode])); // Visible controls, not merely a title.
        for(int other=0;other<4;++other)if(other!=mode)assert(!label(lv_scr_act(),specific[other]));
    }
    sendScene(1);run(200);expect(Scene::Media,Page::Home);
    sendScene(3);run(1000);expect(Scene::Media,Page::Home); // Window flicker is cancelled.
    run(5100);expect(Scene::Media,Page::Home);
    assert(!ui_navigation_state().pc_online && label(lv_scr_act(),"PC Offline | CPU N/A RAM N/A"));
    sendScene(3);run(1000);expect(Scene::Media,Page::Home);assert(ui_navigation_state().pc_online);
    sendScene(1);run(1000);expect(Scene::Coding,Page::Home);
    open("Focus");open("START");const int running=seconds(lv_scr_act());assert(running>0);
    back();expect(Scene::Coding,Page::Home);open("Focus");assert(seconds(lv_scr_act())<running);
    sendScene(2);run(1000);expect(Scene::Browser,Page::Home);assert(label(lv_scr_act(),specific[2]));
    back();assert(ui_current_page()==Page::Standby);
    for(int mode:{0,1,2}) {sendScene(mode);run(1000);assert(ui_current_page()==Page::Standby);}
    click();expect(Scene::Browser,Page::Home); // Latest stable scene, not the boot value.
    back();run(5100);assert(!ui_navigation_state().pc_online);click();expect(Scene::Normal,Page::Home);
    sendScene(1);run(1000);expect(Scene::Coding,Page::Home); // Online recovery after offline unlock.
    back();pinLevels[21]=LOW;run(2500);assert(ui_current_page()==Page::Standby);
    auto fault=latestEncoderRaw();assert(fault.find("sw=PRESS arm=0 held=0")!=std::string::npos);
    run(2000);assert(ui_current_page()==Page::Standby && stat(latestEncoderRaw(),"click=")==stat(fault,"click="));
    pinLevels[21]=HIGH;run(100);assert(ui_current_page()==Page::Standby); // Long release is not a click.
    sendScene(2);run(1000);click();expect(Scene::Browser,Page::Home);assert(macroHid.attempts.empty());
    std::puts("PASS Agent foreground/collector/actual writes -> production CDC framer+JSON+RuntimeData -> 800ms navigation and distinct visible four-scene controls; Standby latest-scene/encoder unlock, boot-held SW, no false unlock, offline/reconnect, flicker, long/short/rearm and retained Focus; no real hardware or HID.");
    std::puts("PASS encoder diagnostic: actual GPIO levels, ISR/decoded/LVGL counters, one count per debounced SW press before release, separate short/long/rearm state, <=1 line/sec shared CDC <=32-byte chunks, full/offline host nonblocking; logging never consumes input or sends HID. SYNTHETIC SDK only.");
    std::puts("PASS actual encoder GPIO ISR/30ms SW -> existing single LVGL encoder: focus +/-/confirm, Standby/Coding/Browser/Media/Normal, Focus parent+background countdown, metrics/four macros, slider edit/back, held-scene cancellation and long/short exclusion; all Keyboard/Consumer actions ZERO reports with HID disabled; RAW CDC and TFT continue. SYNTHETIC SDK only.");
}
