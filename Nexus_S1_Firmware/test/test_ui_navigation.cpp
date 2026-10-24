#include "ui_navigation.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace nexus;
static void stable(Navigation& nav,int mode,uint32_t at) {
    auto page=nav.page(); auto active=nav.state().active_scene;
    nav.observe(mode,true,at); nav.observe(mode,true,at+799);
    assert(nav.page()==page && nav.state().active_scene==active);
    nav.observe(mode,true,at+800);
}
int main() {
    Navigation nav; assert(nav.state().standby && nav.page()==Page::Standby);
    stable(nav,1,0); assert(nav.state().standby && nav.state().stable_scene==Scene::Coding);
    nav.unlock(); assert(nav.state().active_scene==Scene::Coding && nav.page()==Page::Home);
    nav.open(Page::Focus); assert(nav.state().return_parent.scene==Scene::Coding);
    nav.back(); assert(nav.page()==Page::Home && nav.state().active_scene==Scene::Coding);
    for(int mode:{2,1,3,0,2}) {
        stable(nav,mode,1000+mode*1000); assert(nav.page()==Page::Home && int(nav.state().active_scene)==mode);
        assert(nav.observe(mode,true,10000)==nullptr); // Repeated status has no navigation event.
    }
    nav.open(Page::QuickActions); nav.open(Page::Control);
    assert(nav.state().return_parent.page==Page::QuickActions);
    nav.back(); assert(nav.page()==Page::QuickActions);
    nav.back(); assert(nav.page()==Page::Home && nav.state().active_scene==Scene::Browser);
    for(auto page:{Page::Dashboard,Page::Focus,Page::Control,Page::Settings,Page::QuickActions}) {
        nav.open(page); nav.back(); assert(nav.page()==Page::Home && nav.state().active_scene==Scene::Browser);
        nav.open(page); stable(nav,1,20000); assert(nav.page()==Page::Home && nav.state().active_scene==Scene::Coding);
        nav.back(); assert(nav.state().standby); // Obsolete parent was cleared on scene change.
        stable(nav,2,22000); assert(nav.state().standby); nav.unlock();
    }
    nav.open(Page::Focus); auto parent=nav.state().return_parent;
    nav.observe(2,false,30000); assert(nav.state().stable_scene==Scene::Unknown && nav.state().active_scene==Scene::Browser && nav.page()==Page::Focus);
    stable(nav,2,31000); assert(nav.page()==Page::Focus && nav.state().return_parent.scene==parent.scene);
    nav.observe(1,true,32000); nav.observe(2,true,32200); nav.observe(2,true,34000);
    assert(nav.page()==Page::Focus); // Window flicker does not navigate.
    nav.observe(1,true,35000); nav.observe(1,false,35500);
    stable(nav,1,36000); assert(nav.page()==Page::Home && nav.state().active_scene==Scene::Coding);
    nav.standby(); stable(nav,3,38000); stable(nav,0,40000); stable(nav,2,42000);
    assert(nav.state().standby); nav.unlock(); assert(nav.state().active_scene==Scene::Browser);
    nav.standby(); nav.observe(2,false,44000); nav.unlock();
    assert(nav.state().active_scene==Scene::Normal && !nav.state().pc_online);
    stable(nav,3,0xFFFFFF00U); assert(nav.state().active_scene==Scene::Media); // Timer wrap.
    nav.observe(99,true,900); assert(nav.state().detected_scene==Scene::Unknown && nav.state().active_scene==Scene::Media);
    for(int i=0;i<10000;++i) { nav.open(Page::QuickActions); nav.open(Page::Settings); nav.back(); nav.back(); }
    assert(nav.page()==Page::Home && nav.state().active_scene==Scene::Media);
    puts("PASS M4.1 navigation: all scenes, stable unlock, parent stack, child interruption, repeat/flicker/offline/reconnect/wrap, 10000 navigation cycles");
}
