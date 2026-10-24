/**
 * @file   ui.cpp
 * @brief  Nexus_S1 物理屏 UI — Apple Watch 风格 OS 主界面
 *         屏幕: 320×240  LVGL v8.3.11
 *
 *         架构:
 *           page_launcher  — 主界面 (状态栏 + 2x2 App 网格)
 *           page_dashboard — 系统监控 (CPU弧 + 折线图 + 环境)
 *           page_pomodoro  — 沉浸番茄钟
 *           page_macros    — PC 控制中枢
 *           page_settings  — 硬件设置
 *
 *         导航: 点 App 进入 → 左上角 [<] 返回主界面
 */

#include "ui.h"
#include "ui_m4.h"
#include "hid_actions.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

/* ===================================================================
 *  1. 色板
 * =================================================================== */
#define C_BG       lv_color_hex(0x050508)
#define C_CARD     lv_color_hex(0x12121A)
#define C_CYAN     lv_color_hex(0x00E5FF)
#define C_RED      lv_color_hex(0xFF1744)
#define C_AMBER    lv_color_hex(0xFFAB00)
#define C_GREEN    lv_color_hex(0x00E676)
#define C_WHITE    lv_color_hex(0xEAEAEA)
#define C_GREY     lv_color_hex(0x6A6A6A)
#define C_DIM      lv_color_hex(0x1E1E28)

/* ===================================================================
 *  2. 样式池
 * =================================================================== */
static lv_style_t sty_bg, sty_text, sty_text_cyan, sty_text_big, sty_text_huge, sty_text_title;
static lv_style_t sty_card, sty_card_focus;
static lv_style_t sty_arc_bg, sty_arc_cyan, sty_arc_red;
static lv_style_t sty_btn, sty_btn_focus;
static lv_style_t sty_bar_bg;
static lv_style_t sty_back_btn;

/* ===================================================================
 *  3. 页面容器
 * =================================================================== */
static lv_obj_t *page_launcher  = NULL;
static lv_obj_t *page_dashboard = NULL;
static lv_obj_t *page_pomodoro  = NULL;
static lv_obj_t *page_macros    = NULL;
static lv_obj_t *page_settings  = NULL;

/* Central navigation: only stable context edges on Launcher may auto-navigate. */
static nexus::Navigation navigation;
static lv_obj_t *g_active_page = NULL;
static lv_obj_t *launcher_cards[4];
static int launcher_focus = 0;

/* ===================================================================
 *  4. Launcher 控件
 * =================================================================== */
static lv_obj_t *label_context;
static lv_obj_t *label_normal_metrics;
static ui_navigation_cb_t cb_navigation=nullptr;
static lv_obj_t *label_status_icons;

/* ===================================================================
 *  5. Dashboard 控件
 * =================================================================== */
static lv_obj_t *arc_dash, *label_dash_val, *label_dash_title;
static lv_obj_t *chart_cpu, *label_env, *label_chart_val;
static lv_obj_t *label_dash_status, *label_pc_metrics;
static lv_chart_series_t *ser_cpu;
static int       dash_state = 0;   /* 0=CPU温度 1=RAM 2=CPU负载 */

/* ===================================================================
 *  6. Macros 控件
 * =================================================================== */
static lv_obj_t *label_macro_mode;
static lv_obj_t *btns_m[4], *labels_m[4];
static const char *macro_labels[4] = {"Ctrl+C", "Ctrl+V", "Alt+Tab", "Win+D"};
static bool macro_status_active = false;
static uint32_t macro_status_at = 0;

/* ===================================================================
 *  7. Pomodoro 控件
 * =================================================================== */
static lv_obj_t *arc_pomo, *label_pomo, *label_pomo_min;
static lv_timer_t *timer_pomo = NULL;
static lv_obj_t *btn_pomo_start, *label_pomo_start;
static lv_obj_t *btn_pomo_minus, *btn_pomo_plus, *btn_pomo_exit;
static lv_obj_t *label_focus_scene;
static int       pomo_mins    = 25;
static int       pomo_seconds = 0, pomo_total = 0;
static uint32_t  pomo_last_tick = 0, pomo_remainder_ms = 0;

/* ===================================================================
 *  8. Settings 控件
 * =================================================================== */
static lv_obj_t *slider_bright, *label_bright_val;
static lv_obj_t *label_wifi_status, *label_mqtt_status, *label_ip;

/* ===================================================================
 *  9. 运行上下文
 * =================================================================== */
static float  g_temp = 0, g_humi = 0, g_cpu_temp = 0;
static bool g_environment_valid = false;
static float  g_cpu_load = 0, g_mem_load = 0;
static int    g_mode = 0;
static bool   g_wifi = false, g_mqtt = false;
static bool   g_pc_online = false, g_cpu_valid = false, g_mem_valid = false, g_cpu_temp_valid = false;
static char   g_ip[20] = "";

static ui_macro_cb_t      cb_macro  = NULL;
static ui_action_cb_t     cb_action = NULL;
static ui_brightness_cb_t cb_bright = NULL;
static lv_group_t        *g_enc_grp = NULL;
static ui_touch_read_cb_t cb_touch = NULL;
static ui_encoder_read_cb_t cb_encoder = nullptr;
static ui_encoder_reset_cb_t cb_encoder_reset = nullptr;
static lv_indev_t* g_encoder_indev = nullptr;
static lv_indev_t *g_touch_indev = NULL;
static lv_point_t touch_last = {0, 0};
static bool touch_blocked = false;

/* ===================================================================
 *  10. 样式初始化
 * =================================================================== */
static void init_styles(void) {
    lv_style_init(&sty_bg);
    lv_style_set_bg_color(&sty_bg, C_BG);
    lv_style_set_bg_opa(&sty_bg, LV_OPA_COVER);
    lv_style_set_border_width(&sty_bg, 0);
    lv_style_set_pad_all(&sty_bg, 0);
    lv_style_set_text_font(&sty_bg, &lv_font_montserrat_14);

    lv_style_init(&sty_text);
    lv_style_set_text_color(&sty_text, C_WHITE);

    lv_style_init(&sty_text_cyan);
    lv_style_set_text_color(&sty_text_cyan, C_CYAN);

    lv_style_init(&sty_text_big);
    lv_style_set_text_color(&sty_text_big, C_CYAN);
    lv_style_set_text_font(&sty_text_big, &lv_font_montserrat_14);

    lv_style_init(&sty_text_huge);
    lv_style_set_text_color(&sty_text_huge, C_CYAN);
    lv_style_set_text_font(&sty_text_huge, &lv_font_montserrat_28);

    lv_style_init(&sty_text_title);
    lv_style_set_text_color(&sty_text_title, C_GREY);

    /* 卡片 */
    lv_style_init(&sty_card);
    lv_style_set_bg_color(&sty_card, C_CARD);
    lv_style_set_bg_opa(&sty_card, LV_OPA_COVER);
    lv_style_set_radius(&sty_card, 16);
    lv_style_set_border_width(&sty_card, 1);
    lv_style_set_border_color(&sty_card, C_DIM);
    lv_style_set_pad_all(&sty_card, 6);

    /* 卡片聚焦 */
    lv_style_init(&sty_card_focus);
    lv_style_set_bg_color(&sty_card_focus, lv_color_hex(0x0A2830));
    lv_style_set_border_width(&sty_card_focus, 2);
    lv_style_set_border_color(&sty_card_focus, C_CYAN);
    lv_style_set_shadow_width(&sty_card_focus, 20);
    lv_style_set_shadow_color(&sty_card_focus, C_CYAN);
    lv_style_set_shadow_opa(&sty_card_focus, LV_OPA_60);
    lv_style_set_shadow_ofs_x(&sty_card_focus, 0);
    lv_style_set_shadow_ofs_y(&sty_card_focus, 0);

    /* 圆弧 */
    lv_style_init(&sty_arc_bg);
    lv_style_set_arc_color(&sty_arc_bg, C_DIM);
    lv_style_set_arc_width(&sty_arc_bg, 10);

    lv_style_init(&sty_arc_cyan);
    lv_style_set_arc_color(&sty_arc_cyan, C_CYAN);
    lv_style_set_arc_width(&sty_arc_cyan, 10);
    lv_style_set_shadow_width(&sty_arc_cyan, 16);
    lv_style_set_shadow_color(&sty_arc_cyan, C_CYAN);
    lv_style_set_shadow_opa(&sty_arc_cyan, LV_OPA_60);
    lv_style_set_shadow_ofs_x(&sty_arc_cyan, 0);
    lv_style_set_shadow_ofs_y(&sty_arc_cyan, 0);

    lv_style_init(&sty_arc_red);
    lv_style_set_arc_color(&sty_arc_red, C_RED);
    lv_style_set_arc_width(&sty_arc_red, 10);
    lv_style_set_shadow_width(&sty_arc_red, 16);
    lv_style_set_shadow_color(&sty_arc_red, C_RED);
    lv_style_set_shadow_opa(&sty_arc_red, LV_OPA_60);
    lv_style_set_shadow_ofs_x(&sty_arc_red, 0);
    lv_style_set_shadow_ofs_y(&sty_arc_red, 0);

    /* 通用按钮 */
    lv_style_init(&sty_btn);
    lv_style_set_bg_color(&sty_btn, C_CARD);
    lv_style_set_bg_opa(&sty_btn, LV_OPA_COVER);
    lv_style_set_radius(&sty_btn, 14);
    lv_style_set_border_width(&sty_btn, 2);
    lv_style_set_border_color(&sty_btn, C_DIM);
    lv_style_set_text_color(&sty_btn, C_WHITE);
    lv_style_set_pad_all(&sty_btn, 6);

    /* 按钮聚焦 */
    lv_style_init(&sty_btn_focus);
    lv_style_set_bg_color(&sty_btn_focus, lv_color_hex(0x0A2830));
    lv_style_set_border_width(&sty_btn_focus, 2);
    lv_style_set_border_color(&sty_btn_focus, C_CYAN);
    lv_style_set_shadow_width(&sty_btn_focus, 22);
    lv_style_set_shadow_color(&sty_btn_focus, C_CYAN);
    lv_style_set_shadow_opa(&sty_btn_focus, LV_OPA_60);
    lv_style_set_shadow_ofs_x(&sty_btn_focus, 0);
    lv_style_set_shadow_ofs_y(&sty_btn_focus, 0);

    /* 返回按钮 */
    lv_style_init(&sty_back_btn);
    lv_style_set_bg_color(&sty_back_btn, lv_color_hex(0x1A1A24));
    lv_style_set_radius(&sty_back_btn, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&sty_back_btn, 1);
    lv_style_set_border_color(&sty_back_btn, C_DIM);
    lv_style_set_text_color(&sty_back_btn, C_CYAN);
    lv_style_set_pad_all(&sty_back_btn, 0);
    lv_style_set_shadow_width(&sty_back_btn, 8);
    lv_style_set_shadow_color(&sty_back_btn, C_CYAN);
    lv_style_set_shadow_opa(&sty_back_btn, LV_OPA_40);

    /* 进度条 */
    lv_style_init(&sty_bar_bg);
    lv_style_set_bg_color(&sty_bar_bg, C_DIM);
    lv_style_set_bg_opa(&sty_bar_bg, LV_OPA_COVER);
    lv_style_set_radius(&sty_bar_bg, 4);
}

/* ---- 创建返回按钮 (左上角圆形) ---- */
static lv_obj_t* make_back_btn(lv_obj_t *parent) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 30, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_add_style(btn, &sty_back_btn, LV_PART_MAIN);
    lv_obj_add_style(btn, &sty_btn_focus, LV_STATE_FOCUSED);
    lv_group_add_obj(g_enc_grp, btn);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "<");
    lv_obj_center(lbl);
    return btn;
}

/* ===================================================================
 *  11. 导航引擎
 * =================================================================== */
static void render_navigation(const char* reason) {
    if(!reason) return;
    const auto& state=navigation.state();
    if(!strcmp(reason,"SCENE_CHANGE") || !strcmp(reason,"USER_UNLOCK") || !strcmp(reason,"PREVIEW_FIRST_SCENE")) {
        nexus::clearM4Feedback(); macro_status_active=false;
        nexus::refreshM4(g_pc_online,state.active_scene,g_wifi,g_environment_valid,g_temp,g_humi,
                         g_cpu_valid,g_cpu_load,g_mem_valid,g_mem_load,g_cpu_temp_valid,g_cpu_temp);
    }
    if(label_focus_scene) lv_label_set_text(label_focus_scene,nexus::sceneName(state.active_scene));
    lv_obj_t* target=nullptr;
    switch(state.current_page) {
        case nexus::Page::Home: target=state.active_scene==nexus::Scene::Normal ? page_launcher : nexus::m4Page(nexus::Page::Home,state.active_scene); break;
        case nexus::Page::Dashboard: target=page_dashboard; break;
        case nexus::Page::Focus: target=page_pomodoro; break;
        case nexus::Page::Control: target=page_macros; break;
        case nexus::Page::Settings: target=page_settings; break;
        default: target=nexus::m4Page(state.current_page,state.active_scene); break;
    }
    if(target && target!=g_active_page) {
        if(cb_encoder_reset) {
            lv_indev_wait_release(g_encoder_indev);
            cb_encoder_reset(); // Discard stale deltas/clicks and a held SW across page/scene changes.
        }
        for(auto root : {page_launcher,page_dashboard,page_pomodoro,page_macros,page_settings,
                         nexus::m4Page(nexus::Page::Standby),nexus::m4Page(nexus::Page::Home,nexus::Scene::Browser),
                         nexus::m4Page(nexus::Page::Home,nexus::Scene::Coding),nexus::m4Page(nexus::Page::Home,nexus::Scene::Media),
                         nexus::m4Page(nexus::Page::QuickActions)})
            if(root) lv_obj_add_flag(root,LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(target,LV_OBJ_FLAG_HIDDEN); g_active_page=target;
        lv_group_set_editing(g_enc_grp,false);
        lv_obj_t* first=target==page_launcher ? launcher_cards[launcher_focus] : lv_obj_get_child(target,0);
        if(first) lv_group_focus_obj(first);
    }
    if(cb_navigation) cb_navigation(state,reason);
}
static void go_to_page(lv_obj_t* target) {
    nexus::Page page=target==page_dashboard ? nexus::Page::Dashboard : target==page_pomodoro ? nexus::Page::Focus :
                     target==page_macros ? nexus::Page::Control : nexus::Page::Settings;
    render_navigation(navigation.open(page));
}
static void navigate_m4(nexus::Page page) {
    if(page==nexus::Page::Home) render_navigation(navigation.state().standby ? navigation.unlock() : navigation.back());
    else if(page==nexus::Page::Standby) render_navigation(navigation.standby());
    else render_navigation(navigation.open(page));
}
static void m4_macro(int id) { if(cb_macro) cb_macro(id); }
static void navigation_timer(lv_timer_t*) { render_navigation(navigation.observe(g_mode,g_pc_online,lv_tick_get())); }
static void go_launcher() { render_navigation(navigation.back()); }

/* ---- App 按钮回调 ---- */
static void on_app_click(lv_event_t *e) {
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id >= 0 && id < 4) launcher_focus = id;
    printf("[UI] App clicked: %d\n", id);
    switch (id) {
        case 0: go_to_page(page_dashboard); break;
        case 1: go_to_page(page_pomodoro);  break;
        case 2: go_to_page(page_macros);    break;
        case 3: go_to_page(page_settings);  break;
    }
}

/* ---- 返回按钮回调 ---- */
static void on_back_click(lv_event_t *e) {
    (void)e;
    go_launcher();
}
static void on_standby_click(lv_event_t*) { navigate_m4(nexus::Page::Standby); }

/* ---- 宏按钮回调 ---- */
static void on_macro_click(lv_event_t *e) {
    int id = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    printf("[UI] Macro clicked: %d\n", id);
    if (cb_macro) cb_macro(id);
}

/* ---- 前向声明 ---- */
static void pomo_timer_cb(lv_timer_t *t);

static void pomo_controls(bool running) {
    if (running) {
        lv_obj_add_state(btn_pomo_minus, LV_STATE_DISABLED);
        lv_obj_add_state(btn_pomo_plus, LV_STATE_DISABLED);
        lv_obj_add_flag(btn_pomo_start, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(label_pomo_min, "Running");
    } else {
        lv_obj_clear_state(btn_pomo_minus, LV_STATE_DISABLED);
        lv_obj_clear_state(btn_pomo_plus, LV_STATE_DISABLED);
        lv_obj_clear_flag(btn_pomo_start, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pomo_ready_text(void) {
    lv_label_set_text_fmt(label_pomo, "%02d:00", pomo_mins);
    lv_label_set_text_fmt(label_pomo_min, "%d min", pomo_mins);
}

/* ---- 番茄钟启动回调 ---- */
static void on_pomo_start_click(lv_event_t *e) {
    (void)e;
    pomo_seconds = pomo_mins * 60;
    pomo_total   = pomo_seconds;
    pomo_last_tick = lv_tick_get();
    pomo_remainder_ms = 0;
    lv_arc_set_value(arc_pomo, 100);
    lv_label_set_text_fmt(label_pomo, "%02d:00", pomo_mins);
    lv_obj_remove_style(arc_pomo, &sty_arc_red, LV_PART_INDICATOR);
    lv_obj_add_style(arc_pomo, &sty_arc_cyan, LV_PART_INDICATOR);
    if (timer_pomo) lv_timer_del(timer_pomo);
    timer_pomo = lv_timer_create(pomo_timer_cb, 1000, NULL);
    pomo_controls(true);
    lv_group_set_editing(g_enc_grp, false);
    lv_group_focus_obj(btn_pomo_exit);
}
/* ---- 番茄钟 +/- 回调 ---- */
static void on_pomo_plus(lv_event_t *e) {
    (void)e;
    if (timer_pomo) return;
    if (pomo_mins < 60) pomo_mins += 5;
    pomo_ready_text();
}
static void on_pomo_minus(lv_event_t *e) {
    (void)e;
    if (timer_pomo) return;
    if (pomo_mins > 5) pomo_mins -= 5;
    pomo_ready_text();
}
/* ---- 番茄钟退出回调 ---- */
static void on_pomo_exit_click(lv_event_t *e) {
    (void)e;
    if (timer_pomo) { lv_timer_del(timer_pomo); timer_pomo = NULL; }
    pomo_seconds = 0;
    /* 恢复 UI */
    pomo_controls(false);
    lv_arc_set_value(arc_pomo, 100);
    pomo_ready_text();
    lv_obj_remove_style(arc_pomo, &sty_arc_red, LV_PART_INDICATOR);
    lv_obj_add_style(arc_pomo, &sty_arc_cyan, LV_PART_INDICATOR);
    go_launcher();
}

/* ---- 番茄钟倒计时 ---- */
static void pomo_timer_cb(lv_timer_t *t) {
    (void)t;
    const uint32_t now = lv_tick_get();
    const uint32_t elapsed = now - pomo_last_tick; // Unsigned subtraction survives tick wrap.
    pomo_last_tick = now;
    const uint64_t accumulated = (uint64_t)pomo_remainder_ms + elapsed;
    const uint32_t seconds = (uint32_t)(accumulated / 1000U);
    pomo_remainder_ms = (uint32_t)(accumulated % 1000U);
    if (seconds >= (uint32_t)pomo_seconds) {
        if (timer_pomo) { lv_timer_del(timer_pomo); timer_pomo = NULL; }
        pomo_seconds = 0;
        /* 恢复 UI */
        pomo_controls(false);
        lv_arc_set_value(arc_pomo, 0);
        lv_label_set_text(label_pomo, "00:00");
        lv_label_set_text(label_pomo_min, "Done");
        // Update the hidden Focus page too, without interrupting another page.
        if (g_active_page == page_pomodoro) {
            lv_group_set_editing(g_enc_grp, false);
            lv_group_focus_obj(btn_pomo_start);
        }
        return;
    }
    pomo_seconds -= (int)seconds;
    int pct = (pomo_total > 0) ? (pomo_seconds * 100 / pomo_total) : 0;
    lv_arc_set_value(arc_pomo, pct);
    int m = pomo_seconds / 60, s = pomo_seconds % 60;
    lv_label_set_text_fmt(label_pomo, "%02d:%02d", m, s);
    if (pomo_seconds <= 60) {
        lv_obj_remove_style(arc_pomo, &sty_arc_cyan, LV_PART_INDICATOR);
        lv_obj_add_style(arc_pomo, &sty_arc_red, LV_PART_INDICATOR);
    }
}

/* ---- 设置滑条回调 ---- */
static void on_brightness_changed(lv_event_t *e) {
    int val = (int)lv_slider_get_value(lv_event_get_target(e));
    lv_label_set_text_fmt(label_bright_val, "%d%%", val);
    if (cb_bright) cb_bright(val);
}

/* ---- 设置操作按钮回调 ---- */
static void on_action_click(lv_event_t *e) {
    const char *act = (const char *)lv_event_get_user_data(e);
    printf("[UI] Action: %s\n", act);
    if (cb_action) cb_action(act);
}

/* ===================================================================
 *  12. Launcher 主界面
 * =================================================================== */
static void create_page_launcher(lv_obj_t *p) {
    lv_obj_set_size(p, 320, 240);
    lv_obj_add_style(p, &sty_bg, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 状态栏 ---- */
    lv_obj_t *bar = lv_obj_create(p);
    lv_obj_set_size(bar, 320, 55);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    label_context = lv_label_create(bar);
    lv_label_set_text(label_context, "NORMAL");
    lv_obj_align(label_context, LV_ALIGN_TOP_LEFT, 48, 10);
    lv_obj_add_style(label_context, &sty_text_cyan, LV_PART_MAIN);

    label_status_icons = lv_label_create(bar);
    lv_label_set_text(label_status_icons, "PC Offline W - M -");
    lv_obj_align(label_status_icons, LV_ALIGN_TOP_LEFT, 10, 35);
    lv_obj_add_style(label_status_icons, &sty_text_title, LV_PART_MAIN);

    /* ---- 2x2 App 网格 ---- */
    lv_obj_t *grid = lv_obj_create(p);
    lv_obj_set_size(grid, 300, 90);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 103);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, 0, LV_PART_MAIN);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_style_pad_column(grid, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, 10, LV_PART_MAIN);

    const char *icons[] = {"Monitor", "Focus", "Control", "Settings"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *card = lv_btn_create(grid);
        launcher_cards[i] = card;
        lv_obj_set_grid_cell(card, LV_GRID_ALIGN_STRETCH, i % 2, 1,
                                    LV_GRID_ALIGN_STRETCH, i / 2, 1);
        lv_obj_add_style(card, &sty_card, LV_PART_MAIN);
        lv_obj_add_style(card, &sty_card_focus, LV_STATE_FOCUSED);
        lv_obj_set_user_data(card, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, on_app_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(g_enc_grp, card);

        /* 图标 + 文字垂直排列 */
        lv_obj_set_layout(card, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                                     LV_FLEX_ALIGN_CENTER,
                                     LV_FLEX_ALIGN_CENTER);

        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, icons[i]);
        lv_obj_add_style(icon, &sty_text_cyan, LV_PART_MAIN);
    }
    auto standby_button=make_back_btn(p);
    lv_obj_add_event_cb(standby_button,on_standby_click,LV_EVENT_CLICKED,nullptr);
    label_normal_metrics=lv_label_create(p); lv_obj_set_pos(label_normal_metrics,10,59);
    lv_label_set_text(label_normal_metrics,"CPU: N/A  RAM: N/A\nTEMP: N/A");
    lv_obj_add_style(label_normal_metrics,&sty_text,0);
    const char* shortcut_names[]={"Win+E","Alt+Tab","Win+D"};
    const int shortcut_ids[]={nexus::Explorer,nexus::SwitchWindow,nexus::ShowDesktop};
    for(int i=0;i<3;++i) {
        auto shortcut=lv_btn_create(p); lv_obj_set_pos(shortcut,10+i*103,201); lv_obj_set_size(shortcut,94,31);
        lv_obj_add_style(shortcut,&sty_btn,0); lv_obj_add_style(shortcut,&sty_btn_focus,LV_STATE_FOCUSED);
        lv_obj_set_user_data(shortcut,reinterpret_cast<void*>(intptr_t(shortcut_ids[i])));
        lv_obj_add_event_cb(shortcut,on_macro_click,LV_EVENT_CLICKED,nullptr); lv_group_add_obj(g_enc_grp,shortcut);
        auto text=lv_label_create(shortcut); lv_label_set_text(text,shortcut_names[i]); lv_obj_center(text);
    }

}

/* ===================================================================
 *  13. Dashboard 子应用
 * =================================================================== */
static void on_dash_click(lv_event_t *e) {
    (void)e;
    dash_state = (dash_state + 1) % 3;
}

static void create_page_dashboard(lv_obj_t *p) {
    lv_obj_set_size(p, 320, 240);
    lv_obj_add_style(p, &sty_bg, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_back_btn(p);
    lv_obj_add_event_cb(back, on_back_click, LV_EVENT_CLICKED, NULL);

    label_dash_status = lv_label_create(p);
    lv_obj_set_pos(label_dash_status, 52, 8);
    lv_obj_add_style(label_dash_status, &sty_text_cyan, LV_PART_MAIN);
    lv_label_set_text(label_dash_status, "PC Offline  OFFLINE");

    /* 左半区：触控变色圆环 130x130 */
    arc_dash = lv_arc_create(p);
    lv_obj_set_size(arc_dash, 130, 130);
    lv_obj_align(arc_dash, LV_ALIGN_LEFT_MID, 15, 0);
    lv_arc_set_rotation(arc_dash, 135);
    lv_arc_set_bg_angles(arc_dash, 0, 270);
    lv_arc_set_range(arc_dash, 0, 100);
    lv_obj_remove_style(arc_dash, NULL, LV_PART_KNOB);
    lv_obj_add_style(arc_dash, &sty_arc_bg,   LV_PART_MAIN);
    lv_obj_add_style(arc_dash, &sty_arc_cyan, LV_PART_INDICATOR);

    lv_obj_add_flag(arc_dash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(arc_dash, on_dash_click, LV_EVENT_CLICKED, NULL);

    label_dash_val = lv_label_create(p);
    lv_obj_align_to(label_dash_val, arc_dash, LV_ALIGN_CENTER, 0, -5);
    lv_obj_add_style(label_dash_val, &sty_text_huge, LV_PART_MAIN);
    lv_label_set_text(label_dash_val, "N/A");

    label_dash_title = lv_label_create(p);
    lv_obj_align_to(label_dash_title, label_dash_val, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_add_style(label_dash_title, &sty_text_title, LV_PART_MAIN);
    lv_label_set_text(label_dash_title, "CPU TEMP");

    /* 右上角：CPU 负载折线图 */
    chart_cpu = lv_chart_create(p);
    lv_obj_set_size(chart_cpu, 140, 60);
    lv_obj_align(chart_cpu, LV_ALIGN_TOP_RIGHT, -10, 42);
    lv_chart_set_type(chart_cpu, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart_cpu, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(chart_cpu, 10);
    lv_obj_set_style_bg_opa(chart_cpu, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart_cpu, 0, LV_PART_MAIN);
    ser_cpu = lv_chart_add_series(chart_cpu, C_CYAN, LV_CHART_AXIS_PRIMARY_Y);

    /* Y轴手写刻度 (LVGL 8.3 内置不靠谱，自己画) */
    static const char *yticks[] = {"100", "50", "0"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *yt = lv_label_create(p);
        lv_label_set_text(yt, yticks[i]);
        lv_obj_set_style_text_color(yt, C_GREY, LV_PART_MAIN);
        lv_obj_set_style_text_font(yt, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align_to(yt, chart_cpu, LV_ALIGN_OUT_LEFT_TOP, -2,
                        (i * 30) - 8);
    }

    /* 折线图右上角悬浮数值 */
    label_chart_val = lv_label_create(p);
    lv_obj_add_style(label_chart_val, &sty_text_title, LV_PART_MAIN);
    lv_label_set_text(label_chart_val, "LOAD: N/A");
    lv_obj_align_to(label_chart_val, chart_cpu, LV_ALIGN_OUT_TOP_RIGHT, 0, -2);

    /* 右下角：环境温湿度 */
    label_env = lv_label_create(p);
    lv_obj_set_pos(label_env, 174, 176);
    lv_obj_add_style(label_env, &sty_text_big, LV_PART_MAIN);
    lv_label_set_text(label_env, "ENV --\nRH --");

    label_pc_metrics = lv_label_create(p);
    lv_obj_set_pos(label_pc_metrics, 174, 114);
    lv_obj_add_style(label_pc_metrics, &sty_text, LV_PART_MAIN);
    lv_label_set_text(label_pc_metrics, "CPU: N/A\nRAM: N/A\nTEMP: N/A");

    // A button, rather than an editable arc, provides the same action to encoders/keypads.
    lv_obj_t *metric_button = lv_btn_create(p);
    lv_obj_set_size(metric_button, 130, 28);
    lv_obj_set_pos(metric_button, 15, 202);
    lv_obj_add_style(metric_button, &sty_btn, LV_PART_MAIN);
    lv_obj_add_style(metric_button, &sty_btn_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(metric_button, on_dash_click, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(g_enc_grp, metric_button);
    lv_obj_t *metric_label = lv_label_create(metric_button);
    lv_label_set_text(metric_label, "METRIC >");
    lv_obj_center(metric_label);
}

/* ===================================================================
 *  14. Pomodoro 子应用
 * =================================================================== */
static void create_page_pomodoro(lv_obj_t *p) {
    lv_obj_set_size(p, 320, 240);
    lv_obj_add_style(p, &sty_bg, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_back_btn(p);
    lv_obj_add_event_cb(back, on_back_click, LV_EVENT_CLICKED, NULL);

    label_focus_scene=lv_label_create(p);lv_obj_set_pos(label_focus_scene,239,8);
    lv_obj_add_style(label_focus_scene,&sty_text_title,0);lv_label_set_text(label_focus_scene,"NORMAL");
    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, "FOCUS  MODE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_add_style(title, &sty_text_big, LV_PART_MAIN);

    /* 倒计时弧 */
    arc_pomo = lv_arc_create(p);
    lv_obj_set_size(arc_pomo, 130, 130);
    lv_obj_align(arc_pomo, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_rotation(arc_pomo, 0);
    lv_arc_set_bg_angles(arc_pomo, 0, 360);
    lv_arc_set_range(arc_pomo, 0, 100);
    lv_arc_set_value(arc_pomo, 100);
    lv_obj_remove_style(arc_pomo, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc_pomo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(arc_pomo, &sty_arc_bg,   LV_PART_MAIN);
    lv_obj_add_style(arc_pomo, &sty_arc_cyan, LV_PART_INDICATOR);

    /* 倒计时数字 */
    label_pomo = lv_label_create(p);
    lv_label_set_text(label_pomo, "25:00");
    lv_obj_align(label_pomo, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_style(label_pomo, &sty_text_huge, LV_PART_MAIN);

    /* 时间调整：[-] [min] [+] */
    lv_obj_t *btn_minus = lv_btn_create(p);
    btn_pomo_minus = btn_minus;
    lv_obj_set_size(btn_minus, 36, 36);
    lv_obj_align(btn_minus, LV_ALIGN_CENTER, -105, 40);
    lv_obj_add_style(btn_minus, &sty_btn, LV_PART_MAIN);
    lv_obj_add_style(btn_minus, &sty_btn_focus, LV_STATE_FOCUSED);
    lv_obj_set_style_opa(btn_minus, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(btn_minus, on_pomo_minus, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(g_enc_grp, btn_minus);
    lv_obj_t *lbl_m = lv_label_create(btn_minus); lv_label_set_text(lbl_m, "-"); lv_obj_center(lbl_m);

    label_pomo_min = lv_label_create(p);
    lv_label_set_text_fmt(label_pomo_min, "%d min", pomo_mins);
    lv_obj_align(label_pomo_min, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_style(label_pomo_min, &sty_text_title, LV_PART_MAIN);

    lv_obj_t *btn_plus = lv_btn_create(p);
    btn_pomo_plus = btn_plus;
    lv_obj_set_size(btn_plus, 36, 36);
    lv_obj_align(btn_plus, LV_ALIGN_CENTER, 105, 40);
    lv_obj_add_style(btn_plus, &sty_btn, LV_PART_MAIN);
    lv_obj_add_style(btn_plus, &sty_btn_focus, LV_STATE_FOCUSED);
    lv_obj_set_style_opa(btn_plus, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(btn_plus, on_pomo_plus, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(g_enc_grp, btn_plus);
    lv_obj_t *lbl_p = lv_label_create(btn_plus); lv_label_set_text(lbl_p, "+"); lv_obj_center(lbl_p);

    /* START / EXIT 双按钮 */
    btn_pomo_start = lv_btn_create(p);
    lv_obj_set_size(btn_pomo_start, 130, 32);
    lv_obj_align(btn_pomo_start, LV_ALIGN_BOTTOM_MID, -70, -10);
    lv_obj_add_style(btn_pomo_start, &sty_btn, LV_PART_MAIN);
    lv_obj_add_style(btn_pomo_start, &sty_btn_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_pomo_start, on_pomo_start_click, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(g_enc_grp, btn_pomo_start);
    label_pomo_start = lv_label_create(btn_pomo_start);
    lv_label_set_text(label_pomo_start, "START"); lv_obj_center(label_pomo_start);

    lv_obj_t *btn_exit = lv_btn_create(p);
    btn_pomo_exit = btn_exit;
    lv_obj_set_size(btn_exit, 130, 32);
    lv_obj_align(btn_exit, LV_ALIGN_BOTTOM_MID, 70, -10);
    lv_obj_add_style(btn_exit, &sty_btn, LV_PART_MAIN);
    lv_obj_add_style(btn_exit, &sty_btn_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn_exit, on_pomo_exit_click, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(g_enc_grp, btn_exit);
    lv_obj_t *lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, "EXIT"); lv_obj_center(lbl_exit);
}

/* ===================================================================
 *  15. Macros 子应用
 * =================================================================== */
static void create_page_macros(lv_obj_t *p) {
    lv_obj_set_size(p, 320, 240);
    lv_obj_add_style(p, &sty_bg, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_back_btn(p);
    lv_obj_add_event_cb(back, on_back_click, LV_EVENT_CLICKED, NULL);

    label_macro_mode = lv_label_create(p);
    lv_label_set_text(label_macro_mode, "MACRO DECK");
    lv_obj_align(label_macro_mode, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_add_style(label_macro_mode, &sty_text_big, LV_PART_MAIN);

    lv_obj_t *grid = lv_obj_create(p);
    lv_obj_set_size(grid, 300, 170);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, 0, LV_PART_MAIN);

    static lv_coord_t cd[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rd[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, cd, rd);
    lv_obj_set_style_pad_column(grid, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, 10, LV_PART_MAIN);

    for (int i = 0; i < 4; i++) {
        btns_m[i] = lv_btn_create(grid);
        lv_obj_set_grid_cell(btns_m[i], LV_GRID_ALIGN_STRETCH, i % 2, 1,
                                         LV_GRID_ALIGN_STRETCH, i / 2, 1);
        lv_obj_add_style(btns_m[i], &sty_btn, LV_PART_MAIN);
        lv_obj_add_style(btns_m[i], &sty_btn_focus, LV_STATE_FOCUSED);
        lv_obj_set_user_data(btns_m[i], (void *)(intptr_t)i);
        lv_obj_add_event_cb(btns_m[i], on_macro_click, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(g_enc_grp, btns_m[i]);

        labels_m[i] = lv_label_create(btns_m[i]);
        lv_label_set_text(labels_m[i], macro_labels[i]);
        lv_obj_center(labels_m[i]);
    }
}

/* ===================================================================
 *  16. Settings 子应用
 * =================================================================== */
static void create_page_settings(lv_obj_t *p) {
    lv_obj_set_size(p, 320, 240);
    lv_obj_add_style(p, &sty_bg, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_back_btn(p);
    lv_obj_add_event_cb(back, on_back_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_add_style(title, &sty_text_big, LV_PART_MAIN);

    /* 亮度 */
    lv_obj_t *card1 = lv_obj_create(p);
    lv_obj_set_size(card1, 290, 50);
    lv_obj_align(card1, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_add_style(card1, &sty_card, LV_PART_MAIN);
    lv_obj_clear_flag(card1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_bl = lv_label_create(card1);
    lv_label_set_text(lbl_bl, "Brightness");
    lv_obj_align(lbl_bl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_style(lbl_bl, &sty_text, LV_PART_MAIN);

    slider_bright = lv_slider_create(card1);
    lv_obj_set_size(slider_bright, 110, 6);
    lv_obj_align(slider_bright, LV_ALIGN_RIGHT_MID, -50, 0);
    lv_slider_set_range(slider_bright, 10, 100);
    lv_slider_set_value(slider_bright, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_bright, C_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_bright, C_CYAN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider_bright, on_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(g_enc_grp, slider_bright);

    label_bright_val = lv_label_create(card1);
    lv_label_set_text(label_bright_val, "80%");
    lv_obj_align(label_bright_val, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_style(label_bright_val, &sty_text_cyan, LV_PART_MAIN);

    /* 网络状态 */
    lv_obj_t *card2 = lv_obj_create(p);
    lv_obj_set_size(card2, 290, 80);
    lv_obj_align(card2, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_add_style(card2, &sty_card, LV_PART_MAIN);
    lv_obj_clear_flag(card2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card2, 4, LV_PART_MAIN);

    label_wifi_status = lv_label_create(card2);
    lv_label_set_text(label_wifi_status, "WiFi  --");
    lv_obj_add_style(label_wifi_status, &sty_text, LV_PART_MAIN);

    label_ip = lv_label_create(card2);
    lv_label_set_text(label_ip, "IP   ---");
    lv_obj_add_style(label_ip, &sty_text, LV_PART_MAIN);

    label_mqtt_status = lv_label_create(card2);
    lv_label_set_text(label_mqtt_status, "MQTT --");
    lv_obj_add_style(label_mqtt_status, &sty_text, LV_PART_MAIN);

    /* 重启 */
    lv_obj_t *btn_rst = lv_btn_create(p);
    lv_obj_set_size(btn_rst, 130, 32);
    lv_obj_align(btn_rst, LV_ALIGN_BOTTOM_LEFT, 14, -10);
    lv_obj_add_style(btn_rst, &sty_btn, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_rst, on_action_click, LV_EVENT_CLICKED, (void *)"restart");
    lv_group_add_obj(g_enc_grp, btn_rst);
    lv_obj_t *lbl_rst = lv_label_create(btn_rst);
    lv_label_set_text(lbl_rst, "Restart");
    lv_obj_center(lbl_rst);

    lv_obj_t *btn_wifi = lv_btn_create(p);
    lv_obj_set_size(btn_wifi, 130, 32);
    lv_obj_align(btn_wifi, LV_ALIGN_BOTTOM_RIGHT, -14, -10);
    lv_obj_add_style(btn_wifi, &sty_btn, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_wifi, on_action_click, LV_EVENT_CLICKED, (void *)"reset_wifi");
    lv_group_add_obj(g_enc_grp, btn_wifi);
    lv_obj_t *lbl_wifi = lv_label_create(btn_wifi);
    lv_label_set_text(lbl_wifi, "Reset WiFi");
    lv_obj_center(lbl_wifi);
}

/* ===================================================================
 *  17. 全局刷新 (1Hz)
 * =================================================================== */
static void refresh_timer_cb(lv_timer_t *t) {
    nexus::refreshM4(g_pc_online, navigation.state().active_scene, g_wifi, g_environment_valid, g_temp, g_humi,
                     g_cpu_valid, g_cpu_load, g_mem_valid, g_mem_load, g_cpu_temp_valid, g_cpu_temp);
    (void)t;
    const char *mode = g_pc_online ? nexus::sceneName(navigation.state().active_scene) : "OFFLINE";
    if (label_context) {
        lv_label_set_text(label_context, "NORMAL");
        lv_obj_set_style_text_color(label_context, g_pc_online ? C_CYAN : C_GREY, LV_PART_MAIN);
    }
    if(label_normal_metrics) {
        char cpu[12]="N/A",ram[12]="N/A",temperature[24]="N/A",text[96];
        if(g_pc_online && g_cpu_valid) snprintf(cpu,sizeof(cpu),"%d%%",int(g_cpu_load));
        if(g_pc_online && g_mem_valid) snprintf(ram,sizeof(ram),"%d%%",int(g_mem_load));
        if(g_pc_online && g_cpu_temp_valid) snprintf(temperature,sizeof(temperature),"%.1fC",g_cpu_temp);
        snprintf(text,sizeof(text),"CPU: %s  RAM: %s\nTEMP: %s",cpu,ram,temperature);
        lv_label_set_text(label_normal_metrics,text);
    }

    /* Dashboard — 单环触控变色流 */
    if (arc_dash) {
        float metric = 0;
        bool available = false;
        const char *title = "CPU TEMP";
        switch (dash_state) {
            case 0: metric = g_cpu_temp; available = g_pc_online && g_cpu_temp_valid; title = "CPU TEMP"; break;
            case 1: metric = g_mem_load; available = g_pc_online && g_mem_valid; title = "RAM"; break;
            case 2: metric = g_cpu_load; available = g_pc_online && g_cpu_valid; title = "CPU LOAD"; break;
            default: break;
        }
        int val = available ? (int)(metric > 100 ? 100 : (metric < 0 ? 0 : metric)) : 0;
        lv_arc_set_value(arc_dash, val);
        if (!available) lv_label_set_text(label_dash_val, "N/A");
        else if (dash_state == 0) {
            char value[48];
            snprintf(value, sizeof(value), "%.1f", g_cpu_temp);
            lv_label_set_text(label_dash_val, value);
        }
        else lv_label_set_text_fmt(label_dash_val, "%d", val);
        lv_label_set_text(label_dash_title, title);
        lv_obj_update_layout(label_dash_val);
        lv_obj_align_to(label_dash_val, arc_dash, LV_ALIGN_CENTER, 0, -5);
        lv_obj_align_to(label_dash_title, label_dash_val, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

        /* 语义化变色：直接用 lv_obj_set_style 动态改颜色 + 发光 */
        lv_color_t color;
        if (!available)   color = C_GREY;
        else if (val < 60) color = C_CYAN;
        else if (val < 80) color = C_AMBER;
        else               color = C_RED;
        lv_obj_set_style_arc_color(arc_dash, color, LV_PART_INDICATOR);
        lv_obj_set_style_shadow_color(arc_dash, color, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(label_dash_val, color, LV_PART_MAIN);

        /* 折线图跟随变色 + 环境数据 */
        if (chart_cpu) {
            lv_chart_set_next_value(chart_cpu, ser_cpu,
                                   g_pc_online && g_cpu_valid ? (int)g_cpu_load : LV_CHART_POINT_NONE);
            lv_chart_set_series_color(chart_cpu, ser_cpu, color);
        }
        if (label_chart_val) {
            if (g_pc_online && g_cpu_valid)
                lv_label_set_text_fmt(label_chart_val, "LOAD: %d%%", (int)g_cpu_load);
            else lv_label_set_text(label_chart_val, "LOAD: N/A");
            lv_obj_align_to(label_chart_val, chart_cpu, LV_ALIGN_OUT_TOP_RIGHT, 0, -2);
            lv_obj_set_style_text_color(label_chart_val, color, LV_PART_MAIN);
        }
        if (label_dash_status)
            lv_label_set_text_fmt(label_dash_status, "PC %s  %s", g_pc_online ? "Online" : "Offline", mode);
        if (label_pc_metrics) {
            char cpu[16] = "N/A", mem[16] = "N/A", temp[48] = "N/A", metrics[112];
            if (g_pc_online && g_cpu_valid) snprintf(cpu, sizeof(cpu), "%d%%", (int)g_cpu_load);
            if (g_pc_online && g_mem_valid) snprintf(mem, sizeof(mem), "%d%%", (int)g_mem_load);
            if (g_pc_online && g_cpu_temp_valid) snprintf(temp, sizeof(temp), "%.1fC", g_cpu_temp);
            snprintf(metrics, sizeof(metrics), "CPU: %s\nRAM: %s\nTEMP: %s", cpu, mem, temp);
            lv_label_set_text(label_pc_metrics, metrics);
        }
        if (label_env) {
            // LV_SPRINTF_USE_FLOAT is disabled; use the C formatter for floats.
            char info[128];
            snprintf(info, sizeof(info), "ENV %.1fC\nRH  %.0f%%", g_temp, g_humi);
            lv_label_set_text(label_env, info);
        }
    }

    /* Fixed macros: labels must match the actions in every context mode. */
    if (macro_status_active && lv_tick_elaps(macro_status_at) >= 2000) {
        macro_status_active = false;
    }
    if (label_macro_mode && !macro_status_active)
        lv_label_set_text_fmt(label_macro_mode, "DECK: %s", g_pc_online ? mode : "OFFLINE");

    /* 状态栏 */
    if (label_status_icons) {
        char buf[32];
        snprintf(buf, sizeof(buf), "PC %s W %s M %s",
                 g_pc_online ? "Online" : "Offline", g_wifi ? "+" : "-", g_mqtt ? "+" : "-");
        lv_label_set_text(label_status_icons, buf);
        lv_obj_set_style_text_color(label_status_icons, C_GREY, LV_PART_MAIN);
    }

    /* Settings 页状态 */
    if (label_wifi_status)
        lv_label_set_text(label_wifi_status, g_wifi ? "WiFi  OK" : "WiFi  --");
    if (label_ip)
        lv_label_set_text_fmt(label_ip, "IP   %s", g_wifi ? g_ip : "---");
    if (label_mqtt_status)
        lv_label_set_text(label_mqtt_status, g_mqtt ? "MQTT OK" : "MQTT --");
}

/* ===================================================================
 *  18. indev + 公开 API
 * =================================================================== */
static void touch_cb(lv_indev_drv_t *d, lv_indev_data_t *data) {
    (void)d;
    lv_point_t point = touch_last;
    const ui_touch_state_t state = cb_touch ? cb_touch(&point) : UI_TOUCH_RELEASED;
    const bool invalid = state == UI_TOUCH_PRESSED &&
                         (point.x < 0 || point.x >= 320 || point.y < 0 || point.y >= 240);
    if (invalid || (state != UI_TOUCH_RELEASED && state != UI_TOUCH_PRESSED)) {
        // Cancel the current contact without generating a click on the old point.
        touch_blocked = true;
        lv_indev_wait_release(g_touch_indev);
    } else if (state == UI_TOUCH_RELEASED) {
        touch_blocked = false;
    }
    data->state = LV_INDEV_STATE_REL;
    data->continue_reading = false; // One bounded backend read per LVGL poll.
    if (state == UI_TOUCH_PRESSED && !invalid && !touch_blocked) {
        touch_last = point;
        data->state = LV_INDEV_STATE_PR;
    }
    data->point = touch_last; // A release need not carry coordinates from the controller.
}
static void enc_cb(lv_indev_drv_t *d, lv_indev_data_t *data) {
    (void)d;ui_encoder_data_t sample;
    if(cb_encoder)cb_encoder(&sample);
    data->enc_diff=sample.diff;
    data->state=sample.pressed?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;
}

lv_group_t* ui_get_encoder_group(void) { return g_enc_grp; }

void ui_set_encoder_reader(ui_encoder_read_cb_t reader,ui_encoder_reset_cb_t reset) {
    lv_indev_wait_release(g_encoder_indev);
    cb_encoder=reader;cb_encoder_reset=reset;
    if(cb_encoder_reset)cb_encoder_reset();
}
void ui_encoder_back() {
    lv_indev_wait_release(g_encoder_indev);
    lv_group_set_editing(g_enc_grp,false);
    render_navigation(navigation.back()); // Ordinary back preserves the Focus timer and scene parent.
}

void ui_set_touch_reader(ui_touch_read_cb_t reader) {
    // Replacing/disabling a backend must not turn an unfinished press into a macro.
    lv_indev_wait_release(g_touch_indev);
    touch_blocked = true;
    cb_touch = reader;
}

void ui_init(void) {
    init_styles();
    lv_obj_add_style(lv_scr_act(), &sty_bg, LV_PART_MAIN);

    static lv_indev_drv_t dt, de;
    lv_indev_drv_init(&dt); dt.type = LV_INDEV_TYPE_POINTER;
    dt.read_cb = touch_cb; g_touch_indev = lv_indev_drv_register(&dt);
    lv_indev_drv_init(&de); de.type = LV_INDEV_TYPE_ENCODER;
    de.read_cb = enc_cb; g_enc_grp = lv_group_create();
    g_encoder_indev=lv_indev_drv_register(&de);
    lv_indev_set_group(g_encoder_indev, g_enc_grp);

    page_launcher  = lv_obj_create(lv_scr_act());
    page_dashboard = lv_obj_create(lv_scr_act());
    page_pomodoro  = lv_obj_create(lv_scr_act());
    page_macros    = lv_obj_create(lv_scr_act());
    page_settings  = lv_obj_create(lv_scr_act());

    create_page_launcher(page_launcher);
    create_page_dashboard(page_dashboard);
    create_page_pomodoro(page_pomodoro);
    create_page_macros(page_macros);
    create_page_settings(page_settings);
    nexus::createM4Pages(g_enc_grp, navigate_m4, m4_macro);
    navigation = nexus::Navigation();
    g_active_page=nullptr;
    render_navigation("BOOT");
    lv_timer_create(navigation_timer, 50, nullptr);
    lv_timer_create(refresh_timer_cb, 1000, NULL);
}

void ui_update_sensor(float t, float h, float ct, float cl, float ml) {
    g_temp = t; g_humi = h; g_cpu_temp = ct; g_cpu_load = cl; g_mem_load = ml;
}
void ui_set_mode(int mode) {
    g_mode = (mode >= 0 && mode <= 3) ? mode : 0;
    // Timestamp the received snapshot immediately; the 50ms timer also advances
    // debounce when inputs are sent only once (SDL) instead of every UI loop.
    render_navigation(navigation.observe(g_mode,g_pc_online,lv_tick_get()));
}
void ui_set_environment_valid(bool valid) { g_environment_valid = valid; }
nexus::Page ui_current_page(void) { return navigation.page(); }
const nexus::NavState& ui_navigation_state() { return navigation.state(); }
void ui_set_navigation_callback(ui_navigation_cb_t cb) { cb_navigation=cb; }
#if defined(NEXUS_ENCODER_PREVIEW) && NEXUS_ENCODER_PREVIEW
bool ui_preview_unlock_scene(int mode) {
    const auto& state=navigation.state();
    if(mode<0 || mode>3 || !state.pc_online || state.stable_scene!=nexus::Scene(mode)) return false;
    const char* reason=navigation.unlock();
    render_navigation(reason ? "PREVIEW_FIRST_SCENE" : nullptr);
    return !navigation.state().standby;
}
#endif
void ui_set_pc_state(bool online, bool cpu_valid, bool mem_valid, bool temperature_valid) {
    g_pc_online = online;
    g_cpu_valid = cpu_valid; g_mem_valid = mem_valid; g_cpu_temp_valid = temperature_valid;
}

void ui_set_wifi_status(bool ok, const char *ip) {
    g_wifi = ok;
    if (ip) snprintf(g_ip, sizeof(g_ip), "%s", ip);
}
void ui_set_mqtt_status(bool ok) { g_mqtt = ok; }
void ui_set_macro_callback(ui_macro_cb_t cb)      { cb_macro  = cb; }
void ui_set_macro_status(const char *message) {
    nexus::m4MacroStatus(message);
    if (!label_macro_mode || !message) return;
    lv_label_set_text(label_macro_mode, message);
    macro_status_at = lv_tick_get();
    macro_status_active = true;
}
void ui_set_action_callback(ui_action_cb_t cb)    { cb_action = cb; }
void ui_set_brightness_callback(ui_brightness_cb_t cb) { cb_bright = cb; }

/* 废弃旧接口，保留定义不报错 */
void ui_enter_pomodoro(int minutes) { (void)minutes; }
void ui_exit_pomodoro(void) {}
