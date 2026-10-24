#include "ui_m4.h"
#include <stdio.h>
#include <stdint.h>
#include "hid_actions.h"

namespace {
lv_obj_t *standby, *browser, *coding, *media, *quick, *clock_label, *date_label, *source_label;
lv_obj_t *weather_label, *environment_label, *connection_label, *quick_scene;
struct Scene { lv_obj_t *status, *metrics, *feedback; } scenes[3];
void (*navigate_page)(nexus::Page);
ui_macro_cb_t macro_callback;
char macro_message[48] = "Ready - tap a shortcut";
uint32_t macro_at = 0;
bool macro_active = false;

lv_obj_t* label(lv_obj_t* parent, const char* text, int x, int y, uint32_t color = 0xEAEAEA) {
    auto obj = lv_label_create(parent); lv_label_set_text(obj, text); lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}
lv_obj_t* root() {
    auto obj = lv_obj_create(lv_scr_act()); lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, 320, 240); lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x050508), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    return obj;
}
lv_obj_t* button(lv_obj_t* parent, lv_group_t* group, const char* text,
                 int x, int y, int w, int h, lv_event_cb_t cb, intptr_t value) {
    auto obj = lv_btn_create(parent); lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x12121A), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x1E1E28), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x00E5FF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUSED);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(value));
    if (text && *text) { auto text_obj = label(obj, text, 0, 0, 0x00E5FF); lv_obj_center(text_obj); }
    lv_group_add_obj(group, obj); return obj;
}
void onNavigate(lv_event_t* event) {
    navigate_page(static_cast<nexus::Page>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event))));
}
void onMacro(lv_event_t* event) {
    if (macro_callback) macro_callback(int(reinterpret_cast<intptr_t>(lv_event_get_user_data(event))));
}
lv_obj_t* scene(lv_group_t* group, nexus::Scene kind, Scene& widgets) {
    using namespace nexus;
    auto page = root();
    button(page,group,"<",6,4,30,30,onNavigate,intptr_t(Page::Home));
    label(page,sceneName(kind),48,10,0x00E5FF);
    widgets.status=label(page,"PC Offline",10,40,0x999999);
    widgets.feedback=widgets.status; // Bounded transient action feedback replaces this line.
    label(page,kind==nexus::Scene::Coding ? "VS Code / compatible keymaps" :
               kind==nexus::Scene::Browser ? "Browser keyboard shortcuts" : "Track/session: unavailable",10,59,0x999999);
    if(kind==nexus::Scene::Coding) {
        const char* text[]={"Command\nCtrl+Shift+P","Open file\nCtrl+P","Terminal\nCtrl+`","Comment\nCtrl+/"};
        const int ids[]={CommandPalette,QuickOpen,Terminal,CommentLine};
#if defined(NEXUS_ENCODER_PREVIEW) && NEXUS_ENCODER_PREVIEW
        // Reuse the existing compact scene grid; Ctrl+D is distinct from Win+D.
        for(int i=0;i<4;++i) button(page,group,text[i],10+(i%2)*155,81+(i/2)*39,145,33,onMacro,ids[i]);
        button(page,group,"Ctrl+D",10,159,145,33,onMacro,CtrlD);
        button(page,group,"Focus",165,159,145,33,onNavigate,intptr_t(Page::Focus));
        button(page,group,"Quick Actions",10,203,300,30,onNavigate,intptr_t(Page::QuickActions));
#else
        for(int i=0;i<4;++i) button(page,group,text[i],10+(i%2)*155,81+(i/2)*47,145,40,onMacro,ids[i]);
        button(page,group,"Focus",10,181,145,32,onNavigate,intptr_t(Page::Focus));
        button(page,group,"Quick Actions",165,181,145,32,onNavigate,intptr_t(Page::QuickActions));
#endif
    } else {
        const char* browser_text[]={"New tab","Close tab","Reopen tab","Address bar","Refresh","Back"};
        const char* media_text[]={"Play/Pause","Previous","Next","Volume +","Volume -","Mute"};
        const int browser_ids[]={NewTab,CloseTab,ReopenTab,AddressBar,Refresh,BrowserBack};
        const int media_ids[]={PlayPause,PreviousTrack,NextTrack,VolumeUp,VolumeDown,Mute};
        for(int i=0;i<6;++i) button(page,group,kind==nexus::Scene::Browser ? browser_text[i] : media_text[i],
                10+(i%2)*155,81+(i/2)*39,145,33,onMacro,kind==nexus::Scene::Browser ? browser_ids[i] : media_ids[i]);
        button(page,group,"Quick Actions",10,203,300,30,onNavigate,intptr_t(Page::QuickActions));
    }
    return page;
}
}

namespace nexus {
void createM4Pages(lv_group_t* group, void (*navigate)(Page), ui_macro_cb_t macro) {
    navigate_page = navigate; macro_callback = macro;
    standby = root();
    // Full-screen real LVGL button: touch and group activation share one event.
    auto surface = button(standby, group, "", 0, 0, 320, 240, onNavigate, intptr_t(Page::Home));
    lv_obj_set_style_bg_color(surface, lv_color_hex(0x050508), 0);
    lv_obj_set_style_border_width(surface, 0, 0);
    lv_obj_set_style_border_width(surface, 1, LV_STATE_FOCUSED);
    label(surface, "NEXUS S1", 12, 12, 0x00E5FF);
    clock_label = label(surface, "--:--", 12, 44, 0x00E5FF);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_48, 0);
    date_label = label(surface, "Date unavailable", 14, 104);
    source_label = label(surface, "Time not synchronized", 14, 126, 0x999999);
    weather_label = label(surface, "Weather: location not configured", 14, 153, 0x999999);
    lv_obj_set_width(weather_label, 292);
    lv_label_set_long_mode(weather_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(weather_label, 17);
    environment_label = label(surface, "INDOOR N/A", 14, 175);
    connection_label = label(surface, "PC Offline  WiFi --", 14, 197, 0x999999);
    label(surface, "Tap to enter", 14, 220, 0x00E5FF);
    browser = scene(group, nexus::Scene::Browser, scenes[0]);
    coding = scene(group, nexus::Scene::Coding, scenes[1]);
    media = scene(group, nexus::Scene::Media, scenes[2]);
    quick=root();
    button(quick,group,"<",6,4,30,30,onNavigate,intptr_t(Page::Home));
    label(quick,"QUICK ACTIONS",48,10,0x00E5FF);
    quick_scene=label(quick,"NORMAL",234,10,0x999999);
    const char* tools[]={"Monitor","Focus","Control","Settings"};
    const Page pages[]={Page::Dashboard,Page::Focus,Page::Control,Page::Settings};
    for(int i=0;i<4;++i) button(quick,group,tools[i],10+(i%2)*155,65+(i/2)*55,145,43,onNavigate,intptr_t(pages[i]));
    button(quick,group,"Explorer  Win+E",10,181,300,38,onMacro,Explorer);
}
lv_obj_t* m4Page(Page page, nexus::Scene active) {
    if(page==Page::Standby) return standby;
    if(page==Page::QuickActions) return quick;
    if(page!=Page::Home) return nullptr;
    return active==nexus::Scene::Browser ? browser : active==nexus::Scene::Coding ? coding : active==nexus::Scene::Media ? media : nullptr;
}
void m4MacroStatus(const char* message) {
    if (!message) return;
    snprintf(macro_message, sizeof(macro_message), "%s", message);
    macro_at = lv_tick_get(); macro_active = true;
    for (auto& scene : scenes) if (scene.feedback) lv_label_set_text(scene.feedback, macro_message);
}
void clearM4Feedback() { macro_active=false; }
void refreshM4(bool online, nexus::Scene active, bool wifi, bool env_valid, float temp, float humidity,
               bool cpu_valid, float cpu, bool ram_valid, float ram, bool temp_valid, float cpu_temp) {
    char text[112], c[16] = "N/A", r[16] = "N/A", t[32] = "N/A";
    if (env_valid) snprintf(text, sizeof(text), "INDOOR %.1fC   RH %.0f%%", temp, humidity);
    else snprintf(text, sizeof(text), "INDOOR N/A");
    lv_label_set_text(environment_label, text);
    snprintf(text, sizeof(text), "PC %s  WiFi %s", online ? "Online" : "Offline", wifi ? "OK" : "--");
    lv_label_set_text(connection_label, text);
    if (online && cpu_valid) snprintf(c, sizeof(c), "%d%%", int(cpu));
    if (online && ram_valid) snprintf(r, sizeof(r), "%d%%", int(ram));
    if (online && temp_valid) snprintf(t, sizeof(t), "%.1fC", cpu_temp);
    if (macro_active && lv_tick_elaps(macro_at) >= 2000) macro_active = false;
    for (auto& scene : scenes) {
        snprintf(text,sizeof(text),"PC %s | CPU %s RAM %s",online ? "Online" : "Offline",c,r);
        lv_label_set_text(scene.status,macro_active ? macro_message : text);
    }
    lv_label_set_text(quick_scene,sceneName(active));
    (void)t; // Scene titles never use raw detected context.

}
}

void ui_set_ambient_clock(bool valid, const char* time, const char* date, const char* source) {
    if (!clock_label) return;
    lv_label_set_text(clock_label, valid && time ? time : "--:--");
    lv_label_set_text(date_label, valid && date ? date : "Date unavailable");
    lv_label_set_text(source_label, valid && source ? source : "Time not synchronized");
}

void ui_set_ambient_weather(const char* summary, bool valid, bool cached) {
    if (!weather_label) return;
    char text[96];
    if (valid && summary && *summary)
        snprintf(text, sizeof(text), "Weather%s: %s", cached ? " (cached)" : "", summary);
    else snprintf(text, sizeof(text), "Weather: unavailable / not configured");
    lv_label_set_text(weather_label, text);
}
