#include "touch_live_config.h"
#if NEXUS_TOUCH_LIVE_PREVIEW
#include "touch_live_ui.h"
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace {
lv_obj_t *panel=nullptr,*text=nullptr,*cursor=nullptr;
uint32_t clicks=0;
void observed(lv_event_t* e) {
    if(lv_event_get_target(e)!=lv_event_get_current_target(e)) return;
    auto indev=lv_event_get_indev(e);
    if(indev && lv_indev_get_type(indev)==LV_INDEV_TYPE_POINTER) ++clicks;
}
void observeTree(lv_obj_t* root) {
    lv_obj_add_event_cb(root,observed,LV_EVENT_CLICKED,nullptr);
    for(uint32_t i=0;i<lv_obj_get_child_cnt(root);++i) observeTree(lv_obj_get_child(root,i));
}
void fold(lv_event_t*) {
    if(lv_obj_has_flag(panel,LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(panel,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(panel,LV_OBJ_FLAG_HIDDEN);
}
}
void touchLiveUiBegin() {
    if(panel) return;
    observeTree(lv_scr_act()); // Existing pages only; no synthetic events or navigation changes.
    panel=lv_obj_create(lv_layer_top());lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel,126,20);lv_obj_set_size(panel,194,190);
    lv_obj_clear_flag(panel,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel,lv_color_hex(0x101018),0);
    lv_obj_set_style_bg_opa(panel,LV_OPA_90,0);
    text=lv_label_create(panel);lv_obj_set_pos(text,3,1);lv_obj_set_width(text,188);
    lv_obj_set_style_text_font(text,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(text,lv_color_hex(0xFFCC66),0);
    lv_obj_set_style_text_letter_space(text,-1,0);
    lv_obj_clear_flag(text,LV_OBJ_FLAG_CLICKABLE);
    auto toggle=lv_btn_create(lv_layer_top());lv_obj_remove_style_all(toggle);
    lv_obj_set_pos(toggle,277,0);lv_obj_set_size(toggle,43,20);
    lv_obj_set_style_bg_color(toggle,lv_color_hex(0x123341),0);
    lv_obj_set_style_bg_opa(toggle,LV_OPA_COVER,0);lv_obj_set_style_text_color(toggle,lv_color_hex(0x00E5FF),0);
    lv_obj_set_style_text_font(toggle,&lv_font_montserrat_14,0);
    lv_obj_add_event_cb(toggle,fold,LV_EVENT_CLICKED,nullptr);
    auto label=lv_label_create(toggle);lv_label_set_text(label,"DIAG");lv_obj_center(label);
    cursor=lv_obj_create(lv_layer_top());lv_obj_remove_style_all(cursor);lv_obj_set_size(cursor,7,7);
    lv_obj_set_style_radius(cursor,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(cursor,LV_OPA_COVER,0);
    lv_obj_clear_flag(cursor,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cursor,LV_OBJ_FLAG_HIDDEN);
}
void touchLiveUiUpdate(const xpt2046::LiveResult& r,const char* rx,uint32_t now) {
    if(!panel) return;
    char line[400],raw[32],spread[32],z[64],pressure[48],xy[48];
    if(r.raw_available) {
        snprintf(raw,sizeof(raw),"RAW:%u,%u",unsigned(r.x),unsigned(r.y));
        snprintf(spread,sizeof(spread),"dx:%u dy:%u",unsigned(r.dx),unsigned(r.dy));
        snprintf(z,sizeof(z),"Z1:%u,%u\nZ2:%u,%u",unsigned(r.z1_before),unsigned(r.z1_after),
            unsigned(r.z2_before),unsigned(r.z2_after));
        snprintf(pressure,sizeof(pressure),"P:%u,%u %s",unsigned(r.pressure_before),unsigned(r.pressure_after),
            xpt2046::livePressureName(r.pressure));
    } else {
        snprintf(raw,sizeof(raw),"RAW:N/A");snprintf(spread,sizeof(spread),"dx:N/A dy:N/A");
        snprintf(z,sizeof(z),"Z1:N/A\nZ2:N/A");snprintf(pressure,sizeof(pressure),"P:N/A INVALID");
    }
    if(r.mapped) snprintf(xy,sizeof(xy),"PX:%d,%d NOW",r.pixel.x,r.pixel.y);
    else if(r.history_valid) snprintf(xy,sizeof(xy),"HIST:%d,%d %lums",r.history_pixel.x,r.history_pixel.y,
        (unsigned long)uint32_t(now-r.history_at));
    else snprintf(xy,sizeof(xy),"PX:N/A");
    snprintf(line,sizeof(line),"UNCAL / HID OFF\n%s\nRX:%s S:%c M:%c\n%s\n%s\n%s\n%s\n%s\nCLICK:%lu %s\n%s",
        NEXUS_TOUCH_LIVE_XY_ONLY?"XY_ONLY_EXPERIMENTAL":"PRESSURE ONLY",rx,r.xy_stable?'Y':'N',r.mapped?'Y':'N',
        raw,spread,z,pressure,xy,(unsigned long)clicks,
        r.state==xpt2046::State::Pressed?"PR":r.state==xpt2046::State::Cancel?"CANCEL":"REL",r.reason);
    lv_label_set_text(text,line);
    if(r.mapped) {
        lv_obj_set_pos(cursor,r.pixel.x>313?313:r.pixel.x,r.pixel.y>233?233:r.pixel.y);
        lv_obj_set_style_bg_color(cursor,lv_color_hex(r.state==xpt2046::State::Pressed?0x00E5FF:0xFFCC66),0);
        lv_obj_clear_flag(cursor,LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(cursor,LV_OBJ_FLAG_HIDDEN);
}
#endif
