#pragma once

#include "lvgl.h"
#include "ui_navigation.h"

/* Legacy numeric aliases retained for source compatibility.
 * Current navigation is nexus::Page; boot starts on Standby. */
#define UI_STATE_IDLE     0
#define UI_STATE_MACROS   1
#define UI_STATE_POMODORO 2

/* ---- 初始化 ---- */
lv_group_t* ui_get_encoder_group(void);
void ui_init(void);

/* Optional hardware backend, called only by the LVGL/UI context (never an ISR).
 * Supply calibrated logical pixels for the current 320x240 landscape display.
 * Reads must be bounded; CANCEL means a read failure, not a finger release.
 * nullptr disables touch without accessing any bus/GPIO. A new reader must
 * observe RELEASED before its first accepted press. No controller is selected here.
 */
enum ui_touch_state_t { UI_TOUCH_RELEASED, UI_TOUCH_PRESSED, UI_TOUCH_CANCEL };
typedef ui_touch_state_t (*ui_touch_read_cb_t)(lv_point_t *point);
void ui_set_touch_reader(ui_touch_read_cb_t reader);

// Optional real encoder backend; callbacks and navigation run only on the UI task.
struct ui_encoder_data_t { int16_t diff=0; bool pressed=false; };
typedef void (*ui_encoder_read_cb_t)(ui_encoder_data_t* data);
typedef void (*ui_encoder_reset_cb_t)();
void ui_set_encoder_reader(ui_encoder_read_cb_t reader,ui_encoder_reset_cb_t reset);
void ui_encoder_back();

/* ---- 数据更新（仅 UI 执行上下文调用）---- */
void ui_update_sensor(float temp, float humi, float cpu_temp,
                      float cpu_load, float mem_load);
void ui_set_pc_state(bool online, bool cpu_valid, bool mem_valid, bool temperature_valid);
void ui_set_environment_valid(bool valid);
void ui_set_ambient_clock(bool valid, const char* time, const char* date, const char* source);
// Optional provider hook; no HTTP/weather dependency is required to boot.
void ui_set_ambient_weather(const char* summary, bool valid, bool cached);
nexus::Page ui_current_page(void);
const nexus::NavState& ui_navigation_state();
typedef void (*ui_navigation_cb_t)(const nexus::NavState&, const char* reason);
void ui_set_navigation_callback(ui_navigation_cb_t callback);
#if defined(NEXUS_ENCODER_PREVIEW) && NEXUS_ENCODER_PREVIEW
// Demo fallback only: caller must supply a fresh, parsed and debounced CDC status.
bool ui_preview_unlock_scene(int mode);
#endif

/* ---- 状态控制 ---- */
void ui_set_mode(int mode);            /* 0=normal, 1=coding, 2=browser, 3=media */
void ui_enter_pomodoro(int minutes);   /* 强制切到番茄钟，min=分钟数 */
void ui_exit_pomodoro(void);           /* 退出番茄钟，回到对应状态 */

/* ---- 状态栏 ---- */
void ui_set_wifi_status(bool connected, const char* ip);
void ui_set_mqtt_status(bool connected);

/* ---- 回调 ---- */
typedef void (*ui_macro_cb_t)(int action_id);   /* hid_actions.h; original IDs 0~3 retained */
typedef void (*ui_action_cb_t)(const char* action);
typedef void (*ui_brightness_cb_t)(int value);

void ui_set_macro_callback(ui_macro_cb_t cb);
void ui_set_macro_status(const char* message); /* Call only from the UI context. */
void ui_set_action_callback(ui_action_cb_t cb);
void ui_set_brightness_callback(ui_brightness_cb_t cb);
