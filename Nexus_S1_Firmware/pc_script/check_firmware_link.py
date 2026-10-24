"""Offline production parser/UI/M1 checks; fake transport, no hardware or HTTP.

Requires installed PlatformIO libraries and a MinGW GCC toolchain. Set
NEXUS_HOST_CXX_DIR to override the local compiler directory. Test products go
under .pio/offline_link_test; real LVGL C sources are compiled afresh.
"""
from pathlib import Path
import concurrent.futures
import os
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
work = project / '.pio/offline_link_test'
work.mkdir(parents=True, exist_ok=True)
lvgl = project / '.pio/libdeps/esp32-s3-devkitc-1/lvgl'
jsonlib = project / '.pio/libdeps/esp32-s3-devkitc-1/ArduinoJson/src'
import shutil
host_bin = os.environ.get('NEXUS_HOST_CXX_DIR')
if not host_bin:
    host_cxx = shutil.which('g++')
    if not host_cxx:
        raise SystemExit('Set NEXUS_HOST_CXX_DIR or put MinGW g++ on PATH')
    host_bin = str(Path(host_cxx).resolve().parent)
compiler = Path(host_bin)
env = os.environ.copy()
env['PATH'] = str(compiler) + os.pathsep + env.get('PATH', '')

prefix = r'''
#include "ui.h"
#include "hid_actions.h"
#include "xpt2046.h"
#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
struct KeyReport { uint8_t modifiers, reserved, keys[6]; };
static const int KEYBOARD_MODIFIER_LEFTCTRL=1, KEYBOARD_MODIFIER_LEFTALT=4,
    KEYBOARD_MODIFIER_LEFTGUI=8, HID_KEY_C=6, HID_KEY_V=25, HID_KEY_TAB=43,
    HID_KEY_D=7, HID_REPORT_ID_KEYBOARD=1, HID_REPORT_ID_CONSUMER_CONTROL=4;
uint32_t millis() { return lv_tick_get(); }
struct portMUX_TYPE { std::mutex lock; };
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) ((p)->lock.lock())
#define portEXIT_CRITICAL(p) ((p)->lock.unlock())
struct FakeUSB { bool mounted=true; operator bool() const { return mounted; } } USB;
struct USBHID {
    struct Attempt { KeyReport report; uint32_t at; bool success; uint8_t id; uint16_t usage; };
    bool available=true, next_success=true;
    std::vector<Attempt> attempts;
    bool ready() { return available; }
    bool SendReport(uint8_t id, const void* data, size_t len, uint32_t timeout) {
        assert(timeout==10 && ((id==1 && len==8) || (id==4 && len==2)));
        bool result=next_success; next_success=true;
        KeyReport report={}; uint16_t usage=0;
        if(id==1) report=*static_cast<const KeyReport*>(data);
        else {auto bytes=static_cast<const uint8_t*>(data);usage=bytes[0]|(uint16_t(bytes[1])<<8);}
        attempts.push_back({report,millis(),result,id,usage});
        return result;
    }
};
'''
source = (project/'src/main.cpp').read_text(encoding='utf-8')
enum = source[source.index('typedef enum {'):source.index('} SystemMode_t;')+len('} SystemMode_t;')]
runtime = source[source.index('// Protected by dataMutex.'):source.index('// Macro requests may come')]
macro = source[source.index('// Macro requests may come'):source.index('// void broadcastData')]
semaphores = r'''
#include <ArduinoJson.h>
#include <math.h>
static std::mutex dataLock;
static std::mutex* dataMutex=&dataLock;
static bool lockAllowed=true;
static const int pdTRUE=1;
#define pdMS_TO_TICKS(x) (x)
int xSemaphoreTake(std::mutex* lock, int timeout) { assert(timeout==10); return lockAllowed && lock->try_lock(); }
void xSemaphoreGive(std::mutex* lock) { lock->unlock(); }
'''
tests = r'''
static uint16_t framebuffer[320*240];
static void flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* pixels) {
    for(int y=area->y1;y<=area->y2;y++)
        for(int x=area->x1;x<=area->x2;x++) framebuffer[y*320+x]=(pixels++)->full;
    lv_disp_flush_ready(drv);
}
static void feed(const std::string& frame) { for(char c:frame) receivePcByte(c,millis()); }
static bool apply(const std::string& frame, uint32_t now) { return applyPcFrame(frame.c_str(),frame.size(),now); }
static void resumeTool(nexus::Page page);
static void display() {
    auto previous=ui_current_page();
    auto snapshot=runtimeData;
    ui_set_pc_state(pcOnline(snapshot,millis()),snapshot.cpu_valid,snapshot.mem_valid,snapshot.cpu_temp_valid);
    ui_update_sensor(24.5f,42.0f,snapshot.cpu_temp,snapshot.cpu,snapshot.mem);
    ui_set_environment_valid(true);
    ui_set_mode(snapshot.mode);
    lv_tick_inc(1); lv_timer_handler();
    lv_tick_inc(800); lv_timer_handler();
    // M4.1 intentionally interrupts a tool on a new scene. Assert that transition,
    // then explicitly reopen the tool for the pre-existing numeric/validity tests.
    if(previous!=nexus::Page::Home && previous!=nexus::Page::Standby && ui_current_page()==nexus::Page::Home)
        resumeTool(previous);
    lv_tick_inc(200); lv_timer_handler();
}
static bool hasLabel(lv_obj_t* obj,const char* text) {
    if(lv_obj_check_type(obj,&lv_label_class) && !std::strcmp(lv_label_get_text(obj),text)) return true;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(obj);i++) if(hasLabel(lv_obj_get_child(obj,i),text)) return true;
    return false;
}
static lv_obj_t* findLabel(lv_obj_t* obj,const char* text) {
    if(lv_obj_check_type(obj,&lv_label_class) && !std::strcmp(lv_label_get_text(obj),text)) return obj;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(obj);++i)
        if(auto found=findLabel(lv_obj_get_child(obj,i),text)) return found;
    return nullptr;
}
static lv_obj_t* findType(lv_obj_t* obj,const lv_obj_class_t* type) {
    if(lv_obj_check_type(obj,type)) return obj;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(obj);++i)
        if(auto found=findType(lv_obj_get_child(obj,i),type)) return found;
    return nullptr;
}
static lv_obj_t* button(lv_obj_t* page,const char* text) {
    auto obj=findLabel(page,text);
    while(obj && !lv_obj_check_type(obj,&lv_btn_class)) obj=lv_obj_get_parent(obj);
    assert(obj); return obj;
}
static void click(lv_obj_t* page,const char* text) { lv_event_send(button(page,text),LV_EVENT_CLICKED,nullptr); }
static lv_obj_t* currentRoot() {
    const auto& n=ui_navigation_state();
    unsigned index=n.current_page==nexus::Page::Home ?
        (n.active_scene==nexus::Scene::Normal ? 0 : n.active_scene==nexus::Scene::Browser ? 6 : n.active_scene==nexus::Scene::Coding ? 7 : 8) :
        n.current_page==nexus::Page::Dashboard ? 1 : n.current_page==nexus::Page::Focus ? 2 :
        n.current_page==nexus::Page::Control ? 3 : n.current_page==nexus::Page::Settings ? 4 :
        n.current_page==nexus::Page::Standby ? 5 : 9;
    auto root=lv_obj_get_child(lv_scr_act(),index); assert(!lv_obj_has_flag(root,LV_OBJ_FLAG_HIDDEN)); return root;
}
static void resumeTool(nexus::Page page) {
    assert(ui_current_page()==nexus::Page::Home);
    assert(hasLabel(currentRoot(),nexus::sceneName(ui_navigation_state().active_scene)));
    if(ui_navigation_state().active_scene!=nexus::Scene::Normal) click(currentRoot(),"Quick Actions");
    if(page!=nexus::Page::QuickActions) {
        const char* name=page==nexus::Page::Dashboard ? "Monitor" : page==nexus::Page::Focus ? "Focus" :
                         page==nexus::Page::Control ? "Control" : "Settings";
        click(currentRoot(),name);
    }
    assert(ui_current_page()==page);
}
static void checkAllHidActions() {
    const int modifiers[]={1,1,4,8,8,3,1,1,1,1,1,3,1,1,4};
    const int keys[]={6,25,43,7,8,19,19,53,56,23,26,23,15,21,80};
    const int usages[]={0xCD,0xB6,0xB5,0xE9,0xEA,0xE2};
    auto count=macroHid.attempts.size(); serviceMacro(); assert(macroHid.attempts.size()==count);
    assert(!executeMacro(nexus::ActionCount) && !executeMacro(255));
    for(int id=0;id<nexus::ActionCount;++id) {
        auto start=macroHid.attempts.size(); assert(executeMacro(id)); serviceMacro();
        auto down=macroHid.attempts.back(); assert(!executeMacro(id));
        if(id<15) assert(down.id==1 && down.report.modifiers==modifiers[id] && down.report.keys[0]==keys[id]);
        else assert(down.id==4 && down.usage==usages[id-15]);
        lv_tick_inc(29);serviceMacro();assert(macroHid.attempts.size()==start+1);
        lv_tick_inc(1);serviceMacro();auto up=macroHid.attempts.back();
        KeyReport zero={}; assert(up.id==down.id && up.usage==0 && !std::memcmp(&up.report,&zero,8));
        assert(macroHid.attempts.size()==start+2 && up.at-down.at==30);
    }
    for(int id:{0,15}) {
        macroHid.next_success=false;assert(executeMacro(id));serviceMacro(); // Failed down must still release.
        assert(!executeMacro(id));lv_tick_inc(20);serviceMacro();assert(macroHid.attempts.back().success);
        assert(executeMacro(id));serviceMacro();lv_tick_inc(30);macroHid.next_success=false;serviceMacro();
        assert(!executeMacro(id));lv_tick_inc(19);auto n=macroHid.attempts.size();serviceMacro();assert(n==macroHid.attempts.size());
        lv_tick_inc(1);serviceMacro();assert(macroHid.attempts.back().success);
        USB.mounted=false;assert(executeMacro(id));auto before=macroHid.attempts.size();serviceMacro();
        USB.mounted=true;serviceMacro();assert(before==macroHid.attempts.size()); // No stale replay.
        assert(executeMacro(id));serviceMacro();macroHid.available=false;lv_tick_inc(30);serviceMacro();assert(!executeMacro(id));
        macroHid.available=true;lv_tick_inc(20);serviceMacro();
        assert(executeMacro(id));serviceMacro();USB.mounted=false;serviceMacro();USB.mounted=true;
        before=macroHid.attempts.size();serviceMacro();assert(before==macroHid.attempts.size());
    }
    puts("PASS actual HID sender: 21 independent mappings, report ID/size, 30ms release, busy, down/up failures/retry, not-ready, disconnect/reconnect/no replay; synthetic USB only");
}
static void reject(const std::string& frame) {
    auto before=runtimeData;
    assert(!apply(frame,before.pc_updated_at+999));
    assert(runtimeData.pc_updated_at==before.pc_updated_at && runtimeData.cpu==before.cpu &&
           runtimeData.mem==before.mem && runtimeData.cpu_temp==before.cpu_temp &&
           runtimeData.cpu_temp_valid==before.cpu_temp_valid && runtimeData.mode==before.mode);
}
// M3-3: a fake backend tests the production registered POINTER, not a hardware driver.
static ui_touch_state_t touchState=UI_TOUCH_RELEASED;
static lv_point_t touchPoint={0,0};
static int touchReads=0;
static std::vector<int> touchMacros;
static std::vector<std::string> touchActions;
static ui_touch_state_t fakeTouch(lv_point_t* point) {
    ++touchReads; *point=touchPoint; return touchState;
}
static void touchSample(ui_touch_state_t state,int x,int y,uint32_t elapsed=31) {
    touchState=state; touchPoint={lv_coord_t(x),lv_coord_t(y)};
    lv_tick_inc(elapsed); lv_timer_handler();
}
static lv_point_t center(lv_obj_t* obj) {
    lv_obj_update_layout(lv_scr_act()); lv_area_t a; lv_obj_get_coords(obj,&a);
    return {lv_coord_t((a.x1+a.x2)/2),lv_coord_t((a.y1+a.y2)/2)};
}
static void touchTap(lv_obj_t* page,const char* text) {
    auto p=center(button(page,text));
    touchSample(UI_TOUCH_PRESSED,p.x,p.y);
    touchSample(UI_TOUCH_RELEASED,-1,-1); // Release coordinates are intentionally unusable.
}
static void checkTouchAdapter() {
    auto pointer=lv_indev_get_next(nullptr);
    while(pointer && lv_indev_get_type(pointer)!=LV_INDEV_TYPE_POINTER) pointer=lv_indev_get_next(pointer);
    assert(pointer);
    lv_indev_data_t data={}; data.continue_reading=true;
    pointer->driver->read_cb(pointer->driver,&data);
    assert(data.state==LV_INDEV_STATE_REL && !data.continue_reading && touchReads==0);
    ui_set_touch_reader(fakeTouch); touchSample(UI_TOUCH_RELEASED,0,0);
    // Inclusive display edges, held coordinates, and release preserving the last valid point.
    for(auto p : {lv_point_t{0,0},lv_point_t{319,239},lv_point_t{0,239},lv_point_t{319,0}}) {
        touchState=UI_TOUCH_PRESSED; touchPoint=p;
        const int before=touchReads; pointer->driver->read_cb(pointer->driver,&data);
        assert(touchReads==before+1 && data.state==LV_INDEV_STATE_PR && !data.continue_reading);
        assert(data.point.x==p.x && data.point.y==p.y);
        touchState=UI_TOUCH_RELEASED; touchPoint={-1,-1}; pointer->driver->read_cb(pointer->driver,&data);
        assert(data.state==LV_INDEV_STATE_REL && data.point.x==p.x && data.point.y==p.y);
    }
    auto launcher=lv_obj_get_child(lv_scr_act(),0), dashboard=lv_obj_get_child(lv_scr_act(),1);
    auto focus=lv_obj_get_child(lv_scr_act(),2), macros=lv_obj_get_child(lv_scr_act(),3);
    auto settings=lv_obj_get_child(lv_scr_act(),4);
    touchTap(launcher,"Monitor"); assert(!lv_obj_has_flag(dashboard,LV_OBJ_FLAG_HIDDEN));
    touchTap(dashboard,"METRIC >"); lv_tick_inc(1001); lv_timer_handler(); assert(hasLabel(dashboard,"RAM"));
    touchTap(dashboard,"<"); assert(!lv_obj_has_flag(launcher,LV_OBJ_FLAG_HIDDEN));
    touchTap(launcher,"Control");
    ui_set_macro_callback([](int id){touchMacros.push_back(id);});
    const char* names[]={"Ctrl+C","Ctrl+V","Alt+Tab","Win+D"};
    for(int id=0;id<4;++id) {
        const auto before=touchMacros.size(); auto p=center(button(macros,names[id]));
        touchSample(UI_TOUCH_PRESSED,p.x,p.y); touchSample(UI_TOUCH_PRESSED,p.x,p.y,1500);
        assert(touchMacros.size()==before); // Holding/long-press repeats must not execute CLICKED.
        touchSample(UI_TOUCH_RELEASED,-1,-1);
        assert(touchMacros.size()==before+1 && touchMacros.back()==id);
    }
    auto p=center(button(macros,names[0])); const auto before=touchMacros.size();
    touchSample(UI_TOUCH_PRESSED,p.x,p.y); touchSample(UI_TOUCH_CANCEL,0,0);
    assert(!lv_obj_has_state(button(macros,names[0]),LV_STATE_PRESSED));
    touchSample(UI_TOUCH_PRESSED,p.x,p.y); touchSample(UI_TOUCH_RELEASED,-1,-1);
    assert(touchMacros.size()==before); // Read failure cancels; no stale-coordinate click/replay.
    touchTap(macros,names[0]); assert(touchMacros.size()==before+1 && touchMacros.back()==0);
    for(auto bad : {lv_point_t{-1,0},lv_point_t{320,0},lv_point_t{0,-1},lv_point_t{0,240}}) {
        const auto count=touchMacros.size();
        touchSample(UI_TOUCH_PRESSED,p.x,p.y); touchSample(UI_TOUCH_PRESSED,bad.x,bad.y);
        touchSample(UI_TOUCH_PRESSED,p.x,p.y); touchSample(UI_TOUCH_RELEASED,-1,-1);
        assert(touchMacros.size()==count && !lv_obj_has_state(button(macros,names[0]),LV_STATE_PRESSED));
    }
    const auto count=touchMacros.size(); touchSample(UI_TOUCH_PRESSED,p.x,p.y);
    ui_set_touch_reader(nullptr); lv_tick_inc(31); lv_timer_handler();
    assert(touchMacros.size()==count && !lv_obj_has_state(button(macros,names[0]),LV_STATE_PRESSED));
    const int reads=touchReads; lv_tick_inc(31); lv_timer_handler(); assert(touchReads==reads);
    ui_set_touch_reader(fakeTouch); touchSample(UI_TOUCH_PRESSED,p.x,p.y);
    touchSample(UI_TOUCH_RELEASED,-1,-1); assert(touchMacros.size()==count);
    touchTap(macros,names[2]); assert(touchMacros.size()==count+1 && touchMacros.back()==2);
    touchTap(macros,"<"); touchTap(launcher,"Focus"); assert(hasLabel(focus,"05:00"));
    touchTap(focus,"+"); assert(hasLabel(focus,"10 min")); touchTap(focus,"-");
    touchTap(focus,"START"); touchTap(focus,"+"); assert(lv_obj_has_flag(button(focus,"START"),LV_OBJ_FLAG_HIDDEN));
    touchTap(focus,"<"); touchTap(launcher,"Focus"); assert(lv_obj_has_flag(button(focus,"START"),LV_OBJ_FLAG_HIDDEN));
    touchTap(focus,"EXIT"); touchTap(launcher,"Focus"); assert(hasLabel(focus,"05:00")); touchTap(focus,"<");
    touchTap(launcher,"Settings"); assert(!lv_obj_has_flag(settings,LV_OBJ_FLAG_HIDDEN));
    auto slider=findType(settings,&lv_slider_class); lv_area_t a; lv_obj_get_coords(slider,&a);
    touchSample(UI_TOUCH_PRESSED,a.x1+2,(a.y1+a.y2)/2);
    touchSample(UI_TOUCH_PRESSED,a.x2-2,(a.y1+a.y2)/2);
    touchSample(UI_TOUCH_RELEASED,-1,-1); assert(lv_slider_get_value(slider)>80);
    ui_set_action_callback([](const char* action){touchActions.emplace_back(action);});
    touchTap(settings,"Restart"); touchTap(settings,"Reset WiFi");
    assert(touchActions==std::vector<std::string>({"restart","reset_wifi"}));
    touchTap(settings,"<"); assert(!lv_obj_has_flag(launcher,LV_OBJ_FLAG_HIDDEN));
    ui_set_touch_reader(nullptr); ui_set_macro_callback(onMacroRequested); ui_set_action_callback(nullptr);
    std::puts("PASS M3-3 adapter ONLY: disabled/no I/O, one read/poll, edges, hold/release, cancel/out-of-range/rebind without ghost clicks, actual POINTER hit testing across five pages; NO real touch hardware.");
}
static xpt2046::Driver filteredTouch;
static bool rawForUI(void*,xpt2046::Frame& frame) {
    if(touchState==UI_TOUCH_CANCEL) return false;
    frame={};
    if(touchState==UI_TOUCH_PRESSED) {
        // Inverse of the production rotation=1 transform; synthetic ADC source only.
        const int x=(touchPoint.y*4095+119)/239, y=((319-touchPoint.x)*4095+159)/319;
        for(int i=0;i<3;++i){frame.x[i]=uint16_t(x);frame.y[i]=uint16_t(y);}
        frame.pressure_before=frame.pressure_after=900;
    }
    return true;
}
static ui_touch_state_t readFilteredTouch(lv_point_t* point) {
    xpt2046::Point p; const auto state=filteredTouch.poll(p);
    if(state==xpt2046::State::Pressed){point->x=p.x;point->y=p.y;return UI_TOUCH_PRESSED;}
    return state==xpt2046::State::Cancel?UI_TOUCH_CANCEL:UI_TOUCH_RELEASED;
}
static void filteredTap(lv_obj_t* page,const char* text) {
    const auto p=center(button(page,text));
    touchSample(UI_TOUCH_PRESSED,p.x,p.y); touchSample(UI_TOUCH_PRESSED,p.x,p.y);
    touchSample(UI_TOUCH_RELEASED,-1,-1); touchSample(UI_TOUCH_RELEASED,-1,-1);
}
static void checkFilteredTouchUI() {
    xpt2046::Config c; c.mode=xpt2046::Mode::Calibrated;c.pins_confirmed=true;
    c.cs_pin=21;c.miso_pin=18;c.calibration={true,false,0,4095,0,4095}; // Test-only.
    assert(filteredTouch.begin(c,rawForUI,nullptr));ui_set_touch_reader(readFilteredTouch);
    touchSample(UI_TOUCH_RELEASED,0,0);touchSample(UI_TOUCH_RELEASED,0,0);
    auto launcher=lv_obj_get_child(lv_scr_act(),0),dashboard=lv_obj_get_child(lv_scr_act(),1);
    auto focus=lv_obj_get_child(lv_scr_act(),2),macros=lv_obj_get_child(lv_scr_act(),3),settings=lv_obj_get_child(lv_scr_act(),4);
    filteredTap(launcher,"Monitor");assert(!lv_obj_has_flag(dashboard,LV_OBJ_FLAG_HIDDEN));
    filteredTap(dashboard,"METRIC >");lv_tick_inc(1001);lv_timer_handler();assert(hasLabel(dashboard,"CPU LOAD"));
    filteredTap(dashboard,"<");filteredTap(launcher,"Control");touchMacros.clear();
    ui_set_macro_callback([](int id){touchMacros.push_back(id);});
    for(const char* name:{"Ctrl+C","Ctrl+V","Alt+Tab","Win+D"}) filteredTap(macros,name);
    assert(touchMacros==std::vector<int>({0,1,2,3}));
    auto p=center(button(macros,"Ctrl+C"));touchSample(UI_TOUCH_PRESSED,p.x,p.y);touchSample(UI_TOUCH_PRESSED,p.x,p.y);
    touchSample(UI_TOUCH_CANCEL,0,0);touchSample(UI_TOUCH_RELEASED,0,0);touchSample(UI_TOUCH_RELEASED,0,0);
    assert(touchMacros.size()==4 && !lv_obj_has_state(button(macros,"Ctrl+C"),LV_STATE_PRESSED));
    filteredTap(macros,"<");filteredTap(launcher,"Focus");filteredTap(focus,"+");
    assert(hasLabel(focus,"10 min"));filteredTap(focus,"-");filteredTap(focus,"START");
    filteredTap(focus,"<");filteredTap(launcher,"Focus");assert(lv_obj_has_flag(button(focus,"START"),LV_OBJ_FLAG_HIDDEN));
    filteredTap(focus,"EXIT");filteredTap(launcher,"Settings");
    auto slider=findType(settings,&lv_slider_class);lv_area_t a;lv_obj_get_coords(slider,&a);
    for(int x:{a.x1+2,a.x2-2}){touchSample(UI_TOUCH_PRESSED,x,(a.y1+a.y2)/2);touchSample(UI_TOUCH_PRESSED,x,(a.y1+a.y2)/2);}
    touchSample(UI_TOUCH_RELEASED,-1,-1);touchSample(UI_TOUCH_RELEASED,-1,-1);
    assert(lv_slider_get_value(slider)>80);filteredTap(settings,"<");
    ui_set_touch_reader(nullptr);ui_set_macro_callback(onMacroRequested);
    std::puts("PASS XPT production filter -> registered LVGL POINTER -> five pages/metric/macros/Pomodoro/slider/error cancellation; synthetic raw input, NOT hardware.");
}
int main() {
    lv_init(); static lv_color_t pixels[320*10]; static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw,pixels,nullptr,320*10);
    static lv_disp_drv_t drv; lv_disp_drv_init(&drv); drv.hor_res=320; drv.ver_res=240;
    drv.draw_buf=&draw; drv.flush_cb=flush; lv_disp_drv_register(&drv);
    ui_init(); ui_set_macro_callback(onMacroRequested);
    assert(ui_current_page()==nexus::Page::Standby);
    click(lv_obj_get_child(lv_scr_act(),5),"Tap to enter");
    assert(ui_current_page()==nexus::Page::Home);
    auto launcher=lv_obj_get_child(lv_scr_act(),0);
    auto dashboard=lv_obj_get_child(lv_scr_act(),1);
    auto macros=lv_obj_get_child(lv_scr_act(),3);
    auto launchGrid=lv_obj_get_child(launcher,1);
    lv_event_send(lv_obj_get_child(launchGrid,0),LV_EVENT_CLICKED,nullptr);
    assert(!lv_obj_has_flag(dashboard,LV_OBJ_FLAG_HIDDEN));
    display(); assert(hasLabel(dashboard,"N/A"));
    assert(hasLabel(dashboard,"PC Offline  OFFLINE"));
    assert(hasLabel(dashboard,"CPU: N/A\nRAM: N/A\nTEMP: N/A"));
    assert(!runtimeData.pc_seen);
    const std::string frame=R"({"cpu":12.5,"mem":42,"cpu_temp":64.5,"status":"coding"})";
    feed(frame.substr(0,20)); assert(!runtimeData.pc_seen);
    feed(frame.substr(20)+"\r\n");
    assert(runtimeData.cpu==12.5f && runtimeData.mem==42 && runtimeData.cpu_temp==64.5f);
    assert(runtimeData.mode==1 && runtimeData.cpu_valid && runtimeData.mem_valid && runtimeData.cpu_temp_valid);
    display(); assert(hasLabel(dashboard,"64.5"));
    assert(hasLabel(dashboard,"PC Online  CODING"));
    // Only recognized, valid fields are patched; missing fields retain value/validity/mode.
    assert(apply(R"({"cpu":70})",millis()));
    assert(runtimeData.cpu==70 && runtimeData.mem==42 && runtimeData.cpu_temp==64.5f && runtimeData.mode==1);
    assert(apply(R"({"cpu_temp":null,"status":"browser"})",millis()));
    assert(!runtimeData.cpu_temp_valid && runtimeData.cpu_temp==64.5f && runtimeData.mode==2);
    display(); assert(hasLabel(dashboard,"N/A")); assert(hasLabel(dashboard,"LOAD: 70%"));
    assert(hasLabel(dashboard,"PC Online  BROWSER"));
    assert(hasLabel(dashboard,"CPU: 70%\nRAM: 42%\nTEMP: N/A"));
    std::FILE* image=std::fopen("dashboard_null.rgb565","wb");
    assert(image); std::fwrite(framebuffer,sizeof(framebuffer),1,image); std::fclose(image);
    auto arc=findType(dashboard,&lv_arc_class); assert(arc);
    lv_event_send(arc,LV_EVENT_CLICKED,nullptr); display(); assert(hasLabel(dashboard,"42"));
    lv_event_send(arc,LV_EVENT_CLICKED,nullptr); display(); assert(hasLabel(dashboard,"70"));
    reject("not-json"); reject("[]"); reject("{}"); reject(R"({"other":1})");
    reject(R"({"cpu":true})"); reject(R"({"cpu":"12"})"); reject(R"({"cpu":null})");
    reject(R"({"cpu":101})"); reject(R"({"mem":-1})"); reject(R"({"cpu":1e400})");
    reject(R"({"cpu_temp":"60"})"); reject(R"({"status":null})"); reject(R"({"status":"gaming"})");
    reject(R"({"cpu":1}x)"); reject(R"({"cpu":1}{"cpu":2})");
    assert(apply(R"({"cpu_temp":0,"status":"media"})",millis()));
    assert(runtimeData.cpu_temp_valid && runtimeData.cpu_temp==0 && runtimeData.mode==3);
    lv_event_send(arc,LV_EVENT_CLICKED,nullptr); display(); assert(hasLabel(dashboard,"0.0"));
    assert(hasLabel(dashboard,"PC Online  MEDIA"));
    assert(apply(R"({"status":"normal"})",millis())); assert(runtimeData.mode==0);
    display(); assert(hasLabel(dashboard,"PC Online  NORMAL"));
    auto stamp=runtimeData.pc_updated_at;
    feed(std::string(513,' ')+frame+"\n"); assert(runtimeData.pc_updated_at==stamp);
    std::string binary="{\"cpu\":2}"; binary.push_back('\0'); feed(binary+"\n");
    assert(runtimeData.cpu==70);
    std::string exact=frame+std::string(512-frame.size(),' ');
    feed(exact+"\n"); assert(runtimeData.cpu==12.5f && runtimeData.mode==1);
    feed(R"({"cpu":10})" "\n" R"({"mem":20,"status":"media"})" "\n");
    assert(runtimeData.cpu==10 && runtimeData.mem==20 && runtimeData.mode==3);
    stamp=runtimeData.pc_updated_at; lockAllowed=false;
    reject(R"({"cpu":80})"); lockAllowed=true;
    assert(pcOnline(runtimeData,stamp+4999)); assert(!pcOnline(runtimeData,stamp+5000));
    auto wrapped=runtimeData; wrapped.pc_updated_at=0xFFFFFFF0U;
    assert(pcOnline(wrapped,10)); assert(!pcOnline(wrapped,5000));
    lv_tick_inc(5000); display(); assert(hasLabel(dashboard,"N/A")); assert(hasLabel(dashboard,"LOAD: N/A"));
    assert(hasLabel(dashboard,"PC Offline  OFFLINE"));
    assert(hasLabel(dashboard,"CPU: N/A\nRAM: N/A\nTEMP: N/A"));
    feed(frame+"\n"); display(); assert(hasLabel(dashboard,"LOAD: 12%"));
    assert(hasLabel(dashboard,"PC Online  CODING"));
    // Inject actual Agent-produced JSON captured separately by the runner (no CDC hardware).
    std::FILE* real=std::fopen("agent_frames.jsonl","rb"); assert(real);
    char buffer[1024]; unsigned seenModes=0; int captured=0;
    while(std::fgets(buffer,sizeof(buffer),real)) {
        feed(buffer);
        assert(runtimeData.pc_updated_at==millis());
        assert(runtimeData.cpu_valid && runtimeData.mem_valid && !runtimeData.cpu_temp_valid);
        assert((runtimeData.cpu==20 || runtimeData.cpu==80) && runtimeData.mem==50);
        seenModes |= 1U << runtimeData.mode; ++captured;
        display();
        const char* modes[]={"NORMAL","CODING","BROWSER","MEDIA"};
        char expected[128];
        std::snprintf(expected,sizeof(expected),"PC Online  %s",modes[runtimeData.mode]);
        assert(hasLabel(dashboard,expected));
        assert(hasLabel(dashboard,runtimeData.cpu==20 ? "LOAD: 20%" : "LOAD: 80%"));
        assert(hasLabel(dashboard,"N/A"));
        assert(hasLabel(dashboard,runtimeData.cpu==20 ? "CPU: 20%\nRAM: 50%\nTEMP: N/A" : "CPU: 80%\nRAM: 50%\nTEMP: N/A"));
    }
    std::fclose(real); assert(captured>=4 && seenModes==15);
    assert(runtimeData.cpu==80);
    // Bad lines must not poison the following valid line through the actual framer.
    for(const std::string bad : {std::string("not-json"),std::string("{}"),
                                std::string(513,'x'),std::string(R"({"cpu":null})")}) {
        auto before=runtimeData;
        feed(bad+"\n"); assert(runtimeData.pc_updated_at==before.pc_updated_at);
        feed(R"({"cpu":37,"mem":61,"cpu_temp":null,"status":"coding"})" "\n");
        assert(runtimeData.cpu==37 && runtimeData.mem==61 && runtimeData.mode==1);
        assert(runtimeData.cpu_valid && runtimeData.mem_valid && !runtimeData.cpu_temp_valid);
        assert(runtimeData.pc_updated_at==millis()); display();
        assert(hasLabel(dashboard,"LOAD: 37%")); assert(hasLabel(dashboard,"N/A"));
    }
    // Existing M1 mapping/press-release remains correct after Context changes.
    lv_event_send(lv_obj_get_child(launchGrid,2),LV_EVENT_CLICKED,nullptr);
    auto grid=lv_obj_get_child(macros,2);
    const char* labels[]={"Ctrl+C","Ctrl+V","Alt+Tab","Win+D"};
    const int modifiers[]={1,1,4,8}, keys[]={6,25,43,7};
    for(int id=0;id<4;id++) {
        auto button=lv_obj_get_child(grid,id);
        assert(hasLabel(button,labels[id]));
        auto size=macroHid.attempts.size(); lv_event_send(button,LV_EVENT_CLICKED,nullptr); serviceMacro();
        assert(macroHid.attempts.back().report.modifiers==modifiers[id] && macroHid.attempts.back().report.keys[0]==keys[id]);
        assert(!executeMacro(0)); lv_tick_inc(30); serviceMacro();
        const KeyReport zero={}; assert(!std::memcmp(&macroHid.attempts.back().report,&zero,8));
        assert(macroHid.attempts.size()==size+2);
        assert(macroHid.attempts[size+1].at-macroHid.attempts[size].at==30);
    }
    ui_set_macro_status("Macro 1 sent"); assert(hasLabel(macros,"Macro 1 sent"));
    assert(apply(R"({"status":"browser"})",millis()));
    display(); assert(!hasLabel(macros,"Macro 1 sent")); // New scene clears obsolete feedback.
    ui_set_macro_status("Macro 1 sent"); display(); assert(hasLabel(macros,"Macro 1 sent"));
    display(); display(); assert(hasLabel(macros,"DECK: BROWSER"));
    lv_obj_update_layout(lv_scr_act());
    // New controls change child positions: preserve bounds assertions by semantic lookup.
    click(macros,"<"); lv_event_send(lv_obj_get_child(launchGrid,0),LV_EVENT_CLICKED,nullptr); display();
    auto label=findLabel(dashboard,"ENV 24.5C\nRH  42%"); assert(label);
    lv_obj_update_layout(lv_scr_act());
    lv_area_t area; lv_obj_get_coords(label,&area);
    assert(area.x1>=145 && area.y1>=100 && area.y2<240);
    assert(lv_obj_get_child_cnt(dashboard)==13); // status + three-metric label + metric button added.
    label=findLabel(dashboard,"LOAD: 37%"); assert(label); lv_obj_get_coords(label,&area);
    assert(area.x1>=145 && area.x2<320 && area.y1>=0);
    assert(hasLabel(dashboard,"CPU: 37%\nRAM: 61%\nTEMP: N/A"));
    click(dashboard,"<");
    assert(hasLabel(launcher,"NORMAL") && !hasLabel(launcher,"12:00") && ui_navigation_state().active_scene==nexus::Scene::Browser);
    lv_event_send(lv_obj_get_child(launchGrid,1),LV_EVENT_CLICKED,nullptr);
    auto focus=lv_obj_get_child(lv_scr_act(),2);
    for(int i=0;i<4;++i) click(focus,"-");
    assert(hasLabel(focus,"05:00") && hasLabel(focus,"5 min"));
    click(focus,"START");
    assert(lv_obj_has_state(button(focus,"+"),LV_STATE_DISABLED));
    assert(lv_obj_has_state(button(focus,"-"),LV_STATE_DISABLED));
    click(focus,"+"); click(focus,"-"); // Even direct injected events cannot edit a running duration.
    lv_tick_inc(6500); lv_timer_handler(); assert(hasLabel(focus,"04:54"));
    lv_tick_inc(1000); lv_timer_handler(); assert(hasLabel(focus,"04:53"));
    for(int i=0;i<10;++i) { lv_tick_inc(1100); lv_timer_handler(); }
    assert(hasLabel(focus,"04:42")); // 18.5 elapsed seconds, including fractional remainders.
    lv_tick_inc(225000); lv_timer_handler(); assert(hasLabel(focus,"00:57"));
    assert(lv_obj_get_style_arc_color(findType(focus,&lv_arc_class),LV_PART_INDICATOR).full == lv_color_hex(0xFF1744).full);
    click(focus,"<"); lv_event_send(lv_obj_get_child(launchGrid,0),LV_EVENT_CLICKED,nullptr);
    lv_tick_inc(60000); lv_timer_handler();
    assert(!lv_obj_has_flag(dashboard,LV_OBJ_FLAG_HIDDEN));
    assert(hasLabel(focus,"00:00") && hasLabel(focus,"Done"));
    click(dashboard,"<"); lv_event_send(lv_obj_get_child(launchGrid,1),LV_EVENT_CLICKED,nullptr);
    assert(!lv_obj_has_state(button(focus,"+"),LV_STATE_DISABLED));
    click(focus,"EXIT");
    // Start close to the 32-bit wrap and verify actual production countdown across it.
    lv_tick_inc(0xFFFFFF00U-lv_tick_get()); lv_timer_handler();
    lv_event_send(lv_obj_get_child(launchGrid,1),LV_EVENT_CLICKED,nullptr); click(focus,"START");
    lv_tick_inc(1500); lv_timer_handler(); assert(hasLabel(focus,"04:59"));
    lv_tick_inc(1500); lv_timer_handler(); assert(hasLabel(focus,"04:57"));
    click(focus,"EXIT");
    checkAllHidActions();
    ui_set_mode(0);ui_set_pc_state(true,true,true,true);
    lv_tick_inc(1);lv_timer_handler();lv_tick_inc(800);lv_timer_handler();
    while(ui_current_page()!=nexus::Page::Home) click(currentRoot(),"<");
    assert(ui_navigation_state().active_scene==nexus::Scene::Normal);
    checkTouchAdapter();
    checkFilteredTouchUI();
    std::puts("PASS M3-2: simultaneous validity, Launcher Context, elapsed/fractional/stalled/wrapping Pomodoro, disabled edit, last-minute color, background completion and EXIT.");
    std::puts("PASS: production bounded framer/ArduinoJson parser + real LVGL; split/coalesced/CRLF/512-byte frames, overflow/NUL/tail/type/range rejection, missing-field preservation, null/zero validity, all modes, lock contention, 5s offline/recovery/wrap, Agent JSON, M1 regression and Dashboard bounds.");
}
'''
harness = work/'harness.cpp'
harness.write_text(prefix+semaphores+enum+runtime+macro+tests,encoding='utf-8')
includes=['-DLV_CONF_INCLUDE_SIMPLE','-I'+str(project/'include'),'-I'+str(project/'src'),
          '-I'+str(lvgl),'-I'+str(jsonlib)]
def compile_c(src):
    output=work/(str(src.relative_to(lvgl)).replace('\\','_').replace('/','_')+'.o')
    subprocess.run([str(compiler/'gcc.exe'),'-std=c99','-O0',*includes,
                    '-c',str(src),'-o',str(output)],env=env,check=True)
    return output
sources=sorted((lvgl/'src').rglob('*.c'))
assert sources, 'Install the current PlatformIO dependencies first'
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    objects=list(pool.map(compile_c,sources))
print(f'Compiled {len(objects)} real LVGL C sources',flush=True)
exe=work/'context_test.exe'
cmd=[str(compiler/'g++.exe'),'-std=c++17','-O0','-pthread',*includes,str(harness),
     str(project/'src/ui.cpp'),str(project/'src/ui_m4.cpp'),str(project/'src/xpt2046.cpp'),*[str(obj) for obj in objects],'-o',str(exe)]
r=subprocess.run(cmd,env=env,capture_output=True,text=True)
if r.returncode: print(r.stdout+r.stderr); sys.exit(r.returncode)
# Capture actual writes while mocked LHM requests time out; no real port is opened.
from test_agent import capture_timeout_frames
frames=capture_timeout_frames()
(work/'agent_frames.jsonl').write_bytes(b''.join(frames))
print(f'Captured {len(frames)} actual SerialSender writes under simulated LHM timeouts',flush=True)
r=subprocess.run([str(exe)],cwd=work,env=env,capture_output=True,text=True)
print(r.stdout+r.stderr)
if r.returncode: sys.exit(r.returncode)
try:
    from PIL import Image
    import struct
    values=struct.unpack('<76800H',(work/'dashboard_null.rgb565').read_bytes())
    rgb=bytes(channel for value in values for channel in
              (((value>>11)&31)*255//31,((value>>5)&63)*255//63,(value&31)*255//31))
    Image.frombytes('RGB',(320,240),rgb).save(work/'dashboard_null.png')
    print('Rendered actual headless LVGL Dashboard: dashboard_null.png')
except ImportError:
    pass
