// Host-only acceptance checks in the same adapter; every UI action travels via
// SDL events -> production mouse read_cb -> LVGL hit testing -> shared UI events.
#pragma once

static void pump(unsigned milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do { step(); SDL_Delay(5); } while (std::chrono::steady_clock::now() < until);
}
static lv_obj_t* find_text(lv_obj_t* obj, const char* text) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(obj, &lv_label_class) && !std::strcmp(lv_label_get_text(obj), text)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); ++i)
        if (auto result = find_text(lv_obj_get_child(obj, i), text)) return result;
    return nullptr;
}
static bool has_text(const char* text) { return find_text(lv_scr_act(), text) != nullptr; }
static lv_obj_t* find_type(lv_obj_t* obj, const lv_obj_class_t* type) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(obj, type)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); ++i)
        if (auto result = find_type(lv_obj_get_child(obj, i), type)) return result;
    return nullptr;
}
static void push_mouse(Uint32 type, int x, int y) {
    SDL_Event event{};
    event.type = type;
    if (type == SDL_MOUSEMOTION) {
        event.motion.windowID = SDL_GetWindowID(g_window);
        event.motion.x = x; event.motion.y = y;
    } else {
        event.button.windowID = SDL_GetWindowID(g_window);
        event.button.button = SDL_BUTTON_LEFT;
        event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
        event.button.x = x; event.button.y = y;
    }
    require(SDL_PushEvent(&event) == 1, "Cannot enqueue SDL mouse event");
}
static SDL_Point window_point(int logical_x, int logical_y) {
    geometry();
    return {int((g_view.x + (logical_x + 0.5) * g_view.w / sim::width) * g_window_w / g_output_w),
            int((g_view.y + (logical_y + 0.5) * g_view.h / sim::height) * g_window_h / g_output_h)};
}
static void click_at(int x, int y) {
    push_mouse(SDL_MOUSEBUTTONDOWN, x, y); pump(65);
    require(g_mouse_down, "Pointer press was not captured inside viewport");
    push_mouse(SDL_MOUSEBUTTONUP, x, y); pump(65);
    require(!g_mouse_down, "Pointer release was not delivered");
}
static void click_object(lv_obj_t* obj, double rx = 0.5, double ry = 0.5) {
    require(obj != nullptr, "Expected visible UI object not found");
    lv_obj_update_layout(lv_scr_act());
    lv_area_t area; lv_obj_get_coords(obj, &area);
    const auto point = window_point(area.x1 + int((area.x2-area.x1) * rx),
                                    area.y1 + int((area.y2-area.y1) * ry));
    click_at(point.x, point.y);
}
static void click(const char* label) {
    lv_obj_t* obj = find_text(lv_scr_act(), label);
    require(obj != nullptr, (std::string("Visible label not found: ") + label).c_str());
    while (obj && !lv_obj_check_type(obj, &lv_btn_class)) obj = lv_obj_get_parent(obj);
    require(obj != nullptr, "Label has no button ancestor");
    click_object(obj);
}
static void key(SDL_Keycode value, bool pressed = true) {
    SDL_Event event{};
    event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = SDL_GetWindowID(g_window);
    event.key.keysym.sym = value;
    require(SDL_PushEvent(&event) == 1, "Cannot enqueue SDL keyboard event");
    pump(80); // Hold each transition across at least two default 30ms indev polls.
}
static void select_scene(int id) {
    const auto action_count=g_macro_ids.size();
    key(SDL_Keycode(SDLK_1 + id)); key(SDL_Keycode(SDLK_1 + id), false);
    pump(1050); // Allow the unchanged product UI's 1Hz refresh timer to run.
    require(g_scene == id, "Scene shortcut failed");
    require(g_macro_ids.size()==action_count,"PC scene update unexpectedly emitted a HID action");
}
static lv_obj_t* button_for(const char* label) {
    auto obj = find_text(lv_scr_act(), label);
    while (obj && !lv_obj_check_type(obj, &lv_btn_class)) obj = lv_obj_get_parent(obj);
    require(obj != nullptr, (std::string("Visible button missing: ")+label).c_str());
    return obj;
}
static void focus_label(const char* label) {
    auto group = ui_get_encoder_group();
    auto target = button_for(label);
    for (unsigned i=0; i<=lv_group_get_obj_count(group) && lv_group_get_focused(group)!=target; ++i) {
        key(SDLK_UP); key(SDLK_UP,false);
        auto focused = lv_group_get_focused(group);
        require(focused && !lv_obj_has_state(focused,LV_STATE_DISABLED), "Encoder focused a disabled control");
        for(auto parent=focused;parent;parent=lv_obj_get_parent(parent))
            require(!lv_obj_has_flag(parent,LV_OBJ_FLAG_HIDDEN), "Encoder focused a hidden page");
    }
    require(lv_group_get_focused(group)==target, "Encoder cannot reach requested control");
}
static void encoder_click(const char* label) {
    focus_label(label); key(SDLK_SPACE); key(SDLK_SPACE,false); pump(40);
}
static void layout_labels(lv_obj_t* root, std::vector<lv_area_t>& areas) {
    if (lv_obj_has_flag(root,LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(root,&lv_label_class)) {
        lv_area_t area; lv_obj_get_coords(root,&area);
        require(area.x1>=0 && area.y1>=0 && area.x2<320 && area.y2<240, lv_label_get_text(root));
        for (const auto& other:areas)
            require(area.x2<other.x1 || other.x2<area.x1 || area.y2<other.y1 || other.y2<area.y1,
                    (std::string("Overlapping labels: ")+lv_label_get_text(root)).c_str());
        areas.push_back(area);
    }
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i) layout_labels(lv_obj_get_child(root,i),areas);
}
static void check_layout() {
    lv_obj_update_layout(lv_scr_act()); std::vector<lv_area_t> areas; layout_labels(lv_scr_act(),areas);
}
static void check_support() {
    sim::TickClock clock;
    require(clock.advance(123456) == 0, "First clock sample must not advance uptime");
    require(clock.advance(123472) == 16 && clock.advance(123472) == 0, "Clock elapsed/zero delta failed");
    sim::TickClock wrap;
    require(wrap.advance(0xFFFFFFF0U) == 0 && wrap.advance(0x10U) == 32, "Clock wrap failed");
    require(wrap.advance(0x80000010U) == 0x80000000U, "Clock unsigned large delta failed");
    const auto view = sim::viewport(640,508);
    require(view.x == 0 && view.y == 28 && view.w == 640 && view.h == 480, "Default viewport failed");
    auto p = sim::map_mouse(0,28,640,508,640,508,view);
    require(p.inside && p.x == 0 && p.y == 0, "Top-left mapping failed");
    p = sim::map_mouse(639,507,640,508,640,508,view);
    require(p.inside && p.x == 319 && p.y == 239, "Bottom-right mapping failed");
    require(!sim::map_mouse(0,27,640,508,640,508,view).inside, "Banner accepted a click");
    require(!sim::map_mouse(640,508,640,508,640,508,view).inside, "Outside edge accepted");
    p = sim::map_mouse(639,507,640,508,1280,1016,{0,56,1280,960});
    require(p.inside && p.x == 319 && p.y == 239, "High-DPI mapping failed");
    require(!sim::map_mouse(0,0,0,0,0,0,{}).inside, "Zero-sized mapping failed");
    const auto wide = sim::viewport(960,580);
    require(wide.x > 0 && wide.w <= 960 && wide.y >= 28 && wide.y+wide.h <= 580, "Letterbox geometry failed");
    std::puts("PASS support: first sample, unsigned wrap/large delta, scale, DPI, boundaries, letterbox");
}

static void expect(nexus::Scene scene,nexus::Page page) {
    const auto& state=ui_navigation_state();
    require(state.active_scene==scene && state.current_page==page,"Actual navigation state mismatch");
    unsigned visible=0;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(lv_scr_act());++i)
        if(!lv_obj_has_flag(lv_obj_get_child(lv_scr_act(),i),LV_OBJ_FLAG_HIDDEN)) ++visible;
    require(visible==1,"More than one page is actually visible");
    if(page==nexus::Page::Home) require(has_text(nexus::sceneName(scene)),"Visible title does not match active scene");
}
static unsigned object_count(lv_obj_t* root) {
    unsigned count=1;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i) count+=object_count(lv_obj_get_child(root,i));
    return count;
}
static int remaining_seconds(lv_obj_t* root) {
    if(lv_obj_has_flag(root,LV_OBJ_FLAG_HIDDEN)) return -1;
    if(lv_obj_check_type(root,&lv_label_class)) {
        const char* text=lv_label_get_text(root);
        int minutes,seconds;char extra;
        if(std::strlen(text)==5 && std::sscanf(text,"%d:%d%c",&minutes,&seconds,&extra)==2)
            return minutes*60+seconds;
    }
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i) {
        int value=remaining_seconds(lv_obj_get_child(root,i));if(value>=0) return value;
    }
    return -1;
}
static void verify_action(const char* label,int id) {
    auto count=g_macro_ids.size(); click(label);
    require(g_macro_ids.size()==count+1 && g_macro_ids.back()==id,"Visible button dispatched wrong HID action");
}
static void run_self_test() {
    using namespace nexus;
    check_support(); pump(1100);
    require(ui_navigation_state().standby && has_text("Tap to enter") && has_text("--:--"),"Boot/unsynced Standby failed");
    select_scene(1); require(ui_navigation_state().standby,"PC update unlocked Standby");
    check_layout(); save_screenshot("m41_standby_unsynced_simulation");
    ui_set_ambient_clock(true,"12:34","Wed  2026-10-21","SIMULATED UTC+8");
    ui_set_ambient_weather("SIM clear",true,true);
    require(has_text("Weather (cached): SIM clear"),"Cached weather label missing");
    check_layout(); save_screenshot("m41_standby_clock_simulation");
    ui_set_ambient_weather(nullptr,false,false);
    // Path A: unlock into already stable Coding, then return from its Focus.
    click("Tap to enter"); expect(Scene::Coding,Page::Home);
    click("Focus"); expect(Scene::Coding,Page::Focus); click("<"); expect(Scene::Coding,Page::Home);
    select_scene(2); expect(Scene::Browser,Page::Home); click("<");
    require(ui_navigation_state().standby,"Scene home Back must enter Standby");
    // Path B: all four peer scenes, driven only by simulated Agent status inputs.
    select_scene(0); click("Tap to enter"); expect(Scene::Normal,Page::Home);
    pump(1050); check_layout(); save_screenshot("m41_normal_simulation");
    verify_action("Win+E",4); verify_action("Alt+Tab",2); verify_action("Win+D",3);
    select_scene(2); expect(Scene::Browser,Page::Home); require(!has_text("SIM 3 Win+D"),"Previous scene feedback leaked"); check_layout(); save_screenshot("m41_browser_simulation");
    const char* browser[]={"New tab","Close tab","Reopen tab","Address bar","Refresh","Back"};
    for(int i=0;i<6;++i) verify_action(browser[i],9+i);
    select_scene(1); expect(Scene::Coding,Page::Home); pump(2100); check_layout(); save_screenshot("m41_coding_simulation");
    const char* coding[]={"Command\nCtrl+Shift+P","Open file\nCtrl+P","Terminal\nCtrl+`","Comment\nCtrl+/"};
    for(int i=0;i<4;++i) verify_action(coding[i],5+i);
    select_scene(3); expect(Scene::Media,Page::Home); pump(2100); check_layout(); save_screenshot("m41_media_simulation");
    require(has_text("Track/session: unavailable"),"Media invented track metadata");
    const char* media[]={"Play/Pause","Previous","Next","Volume +","Volume -","Mute"};
    for(int i=0;i<6;++i) verify_action(media[i],15+i);
    select_scene(0); expect(Scene::Normal,Page::Home);
    // Path C: nested tools preserve scene and actual return parent.
    for(int id:{1,2,3}) {
        select_scene(id); auto active=static_cast<Scene>(id);
        click("Quick Actions"); expect(active,Page::QuickActions);
        for(const char* tool:{"Monitor","Focus","Control","Settings"}) {
            click(tool); require(ui_navigation_state().return_parent.page==Page::QuickActions,"Nested return target lost");
            click("<"); expect(active,Page::QuickActions);
        }
        verify_action("Explorer  Win+E",4); click("<"); expect(active,Page::Home);
    }
    // Path D: new scene interrupts Focus but never clears its running timer.
    select_scene(1); click("Focus");
    lv_mem_monitor_t focus_memory{};lv_mem_monitor(&focus_memory);
    std::printf("CHECK Focus edit: free=%u largest=%u\n",unsigned(focus_memory.free_size),unsigned(focus_memory.free_biggest_size));std::fflush(stdout);
    click("+");
    require(has_text("30:00") && has_text("30 min"),"Pomodoro duration label did not follow +");
    click("-"); require(has_text("25:00") && has_text("25 min"),"Pomodoro - failed");
    click("START"); require(has_text("Running") && lv_obj_has_state(button_for("+"),LV_STATE_DISABLED),"Running editor still enabled");
    select_scene(2); expect(Scene::Browser,Page::Home);
    click("Quick Actions"); click("Focus"); expect(Scene::Browser,Page::Focus);
    require(!has_text("25:00") && !has_text("START"),"Scene change cleared Focus timer");
    click("<"); expect(Scene::Browser,Page::QuickActions); click("<"); expect(Scene::Browser,Page::Home);
    select_scene(1); click("Focus"); require(!has_text("START"),"Coding lost background timer");
    click("EXIT"); expect(Scene::Coding,Page::Home);
    click("Focus"); require(has_text("25:00") && has_text("START"),"EXIT did not stop/reset timer"); click("<");
    // Path E: Standby protects against any background transition; unlock uses last stable mode.
    click("<"); for(int id:{0,2,1,3}) {select_scene(id);require(ui_navigation_state().standby,"Background scene woke Standby");}
    click("Tap to enter"); expect(Scene::Media,Page::Home);
    const auto changes=g_scene_changes; select_scene(3); select_scene(3);
    require(g_scene_changes==changes,"Repeated frames caused navigation/redraw");
    scene(1); pump(200); scene(3); pump(1050); expect(Scene::Media,Page::Home);
    click("Quick Actions"); click("Settings"); select_scene(4);
    expect(Scene::Media,Page::Settings); require(!ui_navigation_state().pc_online,"Offline was ignored");
    select_scene(3); expect(Scene::Media,Page::Settings); // Same-scene reconnect does not disturb tools.
    select_scene(2); expect(Scene::Browser,Page::Home); // New stable scene must leave Settings.
    select_scene(4); expect(Scene::Browser,Page::Home); pump(2100);
    require(has_text("PC Offline | CPU N/A RAM N/A"),"Offline scene telemetry not invalidated");
    check_layout(); save_screenshot("m41_browser_offline_simulation");
    click("<"); ui_set_environment_valid(false); ui_set_ambient_clock(false,nullptr,nullptr,nullptr); pump(1050);
    require(has_text("INDOOR N/A") && has_text("--:--") && has_text("PC Offline  WiFi --"),"Offline Standby fallback failed");
    encoder_click("Tap to enter"); expect(Scene::Normal,Page::Home);
    select_scene(0); expect(Scene::Normal,Page::Home);
    // Production Dashboard metrics still work in each scene via the common tools.
    static const char* metrics[]={"CPU: 20%\nRAM: 40%\nTEMP: 45.0C","CPU: 75%\nRAM: 62%\nTEMP: 62.0C",
        "CPU: 35%\nRAM: 55%\nTEMP: 51.0C","CPU: 12%\nRAM: 48%\nTEMP: 44.0C"};
    static const char* temp[]={"45.0","62.0","51.0","44.0"};
    for(int id=0;id<4;++id) {
        select_scene(id); if(id) click("Quick Actions"); click("Monitor"); pump(1050);
        require(has_text(metrics[id]) && has_text(temp[id]) && has_text("CPU TEMP"),"Dashboard numeric/context regression");
        check_layout(); click("<"); if(id) click("<"); expect(static_cast<Scene>(id),Page::Home);
    }
    select_scene(5); click("Quick Actions"); click("Monitor"); pump(1050);
    require(has_text("CPU: 30%\nRAM: 55%\nTEMP: N/A") && has_text("N/A"),"Null temperature became zero");
    save_screenshot("m41_monitor_na_simulation");
    auto arc=find_type(lv_scr_act(),&lv_arc_class);
    click_object(arc,0.05,0.5); pump(1050); require(has_text("RAM") && has_text("55"),"Metric RAM mouse hit failed");
    encoder_click("METRIC >"); pump(1050); require(has_text("CPU LOAD") && has_text("30"),"Encoder CPU selection failed");
    encoder_click("METRIC >"); pump(1050); require(has_text("CPU TEMP"),"Encoder temperature selection failed");
    encoder_click("<"); encoder_click("Control");
    const char* common[]={"Ctrl+C","Ctrl+V","Alt+Tab","Win+D"};
    for(int i=0;i<4;++i) {
        auto count=g_macro_ids.size(); encoder_click(common[i]);
        require(g_macro_ids.size()==count+1 && g_macro_ids.back()==i,"Original four macro IDs changed");
    }
    check_layout(); encoder_click("<"); encoder_click("Settings");
    require(has_text("SETTINGS") && has_text("WiFi  --") && has_text("MQTT --"),"Settings content changed");
    auto slider=find_type(lv_scr_act(),&lv_slider_class); require(slider!=nullptr,"Brightness slider missing");
    lv_area_t area; lv_obj_get_coords(slider,&area);
    auto start=window_point(area.x1+(area.x2-area.x1)*8/10,(area.y1+area.y2)/2);
    auto end=window_point(area.x1+(area.x2-area.x1)/5,(area.y1+area.y2)/2);
    push_mouse(SDL_MOUSEBUTTONDOWN,start.x,start.y); pump(65); push_mouse(SDL_MOUSEMOTION,end.x,end.y);pump(65);
    push_mouse(SDL_MOUSEBUTTONUP,end.x,end.y);pump(65);
    require(!g_mouse_down && g_brightness>=10 && g_brightness<80,"Slider press/drag/release failed");
    click("Restart");click("Reset WiFi");
    require(g_actions==std::vector<std::string>({"restart","reset_wifi"}),"Hardware mocks stopped recording intents");
    check_layout(); save_screenshot("m41_settings_simulation"); click("<"); click("Focus");
    for(int i=0;i<4;++i) encoder_click("-");
    require(has_text("05:00") && has_text("5 min"),"Encoder duration selection failed");
    encoder_click("START");
    require(lv_group_get_focused(ui_get_encoder_group())==button_for("EXIT"),"Running timer lost EXIT focus");
    int hits=0; auto timer=lv_timer_create([](lv_timer_t* t){++*static_cast<int*>(t->user_data);},1000,&hits);
    auto wall_start=std::chrono::steady_clock::now(); auto tick_start=lv_tick_get();pump(2200);
    auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-wall_start).count();
    auto ticks=uint32_t(lv_tick_get()-tick_start);
    require(std::abs(double(ticks)-elapsed)<40 && hits>=2 && hits<=3,"LVGL tick drift/acceleration");lv_timer_del(timer);
    require(has_text("04:58") || has_text("04:57"),"Pomodoro not following wall time");
    std::printf("PASS real time: wall=%lldms LVGL=%ums 1s_timer_hits=%d\n",static_cast<long long>(elapsed),ticks,hits);
    const int before_stall=remaining_seconds(lv_scr_act());
    lv_tick_inc(6500);pump(40);
    const int after_stall=remaining_seconds(lv_scr_act());
    require(before_stall>=0 && before_stall-after_stall>=6 && before_stall-after_stall<=7,"Stalled callback dropped time");
    click("<");click("Monitor");lv_tick_inc(300000);pump(40);
    require(has_text("METRIC >"),"Timer completion stole the page");click("<");click("Focus");
    require(has_text("00:00") && has_text("Done") && has_text("START"),"Background completion state missing");
    check_layout();save_screenshot("m41_focus_done_simulation");click("EXIT"); click("<");
    expect(Scene::Coding,Page::Home);
    // Scaling and keyboard navigation use real adapter events and real object bounds.
    select_scene(0); SDL_SetWindowSize(g_window,960,580);pump(160);geometry();require(g_view.x>0,"No resized letterbox");
    push_mouse(SDL_MOUSEBUTTONDOWN,1,100);pump(65);require(!g_mouse_down,"Letterbox click accepted");
    push_mouse(SDL_MOUSEBUTTONUP,1,100);pump(65);click("Monitor");require(has_text("METRIC >"),"Scaled pointer missed Monitor");click("<");
    click("Control"); verify_action("Ctrl+C",0);click("<");
    key(SDLK_SPACE);require(g_encoder_down,"Encoder press missing");key(SDLK_SPACE,false);require(!g_encoder_down,"Encoder release missing");
    if(ui_current_page()==Page::Standby) click("Tap to enter");
    if(ui_current_page()!=Page::Home) click("<");
    // No new roots or objects may be created by repeated automatic switching.
    select_scene(0);pump(2100);unsigned count=object_count(lv_scr_act());lv_mem_monitor_t before{};lv_mem_monitor(&before);
    for(int i=0;i<64;++i) {scene(i%4);pump(60);lv_tick_inc(850);pump(60);expect(static_cast<Scene>(i%4),Page::Home);}
    select_scene(0);pump(2100);lv_mem_monitor_t memory{};lv_mem_monitor(&memory);
    require(object_count(lv_scr_act())==count && memory.free_size+4096>=before.free_size,"Scene changes leak UI objects/memory");
    require(memory.free_biggest_size>=4096,"Insufficient LVGL allocation headroom");
    SDL_SetWindowSize(g_window,320,268);pump(160);check_layout();save_screenshot("m41_native_normal_simulation");
    std::printf("PASS LVGL pool: total=%u free=%u largest=%u used=%u%%; 64 switches, object_count=%u\n",unsigned(memory.total_size),unsigned(memory.free_size),unsigned(memory.free_biggest_size),unsigned(memory.used_pct),count);
    std::puts("PASS M4.1 paths A/B/C/D/E, actual NAV/visible title, all scene actions, original tools, offline/reconnect/flicker, elapsed Focus and scale/encoder");
    std::puts("PASS SIMULATION ONLY: synthetic telemetry/media state; no physical USB, real Windows HID or touch hardware");
}
