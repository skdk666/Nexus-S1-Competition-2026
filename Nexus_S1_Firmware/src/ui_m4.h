#pragma once
#include "ui.h"
#include "ui_navigation.h"

// Widgets only; navigation and hardware services remain outside this module.
namespace nexus {
void createM4Pages(lv_group_t* group, void (*navigate)(Page), ui_macro_cb_t macro);
lv_obj_t* m4Page(Page page, Scene active=Scene::Normal);
void refreshM4(bool online, Scene active, bool wifi, bool env_valid, float temp, float humidity,
               bool cpu_valid, float cpu, bool ram_valid, float ram, bool temp_valid, float cpu_temp);
void m4MacroStatus(const char* message);
void clearM4Feedback();
}
