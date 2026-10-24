#pragma once
#include <stdint.h>

namespace nexus {
enum class Scene : int { Unknown=-1, Normal=0, Coding=1, Browser=2, Media=3 };
enum class Page { Standby, Home, Dashboard, Focus, Control, Settings, QuickActions };
inline const char* sceneName(Scene s) {
    switch(s) { case Scene::Normal:return "NORMAL"; case Scene::Coding:return "CODING";
        case Scene::Browser:return "BROWSER"; case Scene::Media:return "MEDIA"; default:return "UNKNOWN"; }
}
inline const char* pageName(Page p) {
    switch(p) { case Page::Standby:return "STANDBY"; case Page::Home:return "HOME";
        case Page::Dashboard:return "MONITOR"; case Page::Focus:return "FOCUS";
        case Page::Control:return "CONTROL"; case Page::Settings:return "SETTINGS"; default:return "QUICK_ACTIONS"; }
}
struct Target { Scene scene; Page page; Target(Scene s=Scene::Normal, Page p=Page::Home):scene(s),page(p) {} };
struct NavState {
    Scene detected_scene=Scene::Unknown, stable_scene=Scene::Unknown, active_scene=Scene::Normal;
    Page current_page=Page::Standby;
    Target return_parent;
    bool standby=true, pc_online=false;
};
class Navigation {
    NavState state_;
    Scene candidate_=Scene::Unknown;
    uint32_t since_=0;
    Target parents_[3];
    unsigned depth_=0;
    void resetParent() { depth_=0; state_.return_parent={state_.active_scene,Page::Home}; }
public:
    static constexpr uint32_t debounce_ms=800;
    const NavState& state() const { return state_; }
    Page page() const { return state_.current_page; }
    const char* observe(int mode,bool online,uint32_t now) {
        Scene detected=(online && mode>=0 && mode<=3) ? static_cast<Scene>(mode) : Scene::Unknown;
        bool changed=state_.pc_online!=online || state_.detected_scene!=detected;
        state_.pc_online=online; state_.detected_scene=detected;
        if (detected==Scene::Unknown) {
            candidate_=Scene::Unknown; state_.stable_scene=Scene::Unknown;
            return changed ? "PC_OFFLINE_OR_INVALID" : nullptr;
        }
        if (detected==state_.stable_scene) { candidate_=Scene::Unknown; return changed ? "DETECTED" : nullptr; }
        if (candidate_!=detected) { candidate_=detected; since_=now; return "DETECTED"; }
        if (uint32_t(now-since_)<debounce_ms) return changed ? "DETECTED" : nullptr;
        state_.stable_scene=detected; candidate_=Scene::Unknown;
        if (!state_.standby && state_.active_scene!=detected) {
            state_.active_scene=detected; state_.current_page=Page::Home; resetParent();
            return "SCENE_CHANGE";
        }
        return "STABLE";
    }
    const char* unlock() {
        if (!state_.standby) return nullptr;
        state_.standby=false;
        state_.active_scene=state_.pc_online && state_.stable_scene!=Scene::Unknown ? state_.stable_scene : Scene::Normal;
        state_.current_page=Page::Home; resetParent(); return "USER_UNLOCK";
    }
    const char* standby() {
        if (state_.standby) return nullptr;
        state_.standby=true; state_.current_page=Page::Standby; resetParent(); return "USER_STANDBY";
    }
    const char* open(Page page) {
        if (state_.standby || page==Page::Home || page==Page::Standby || page==state_.current_page || depth_==3) return nullptr;
        parents_[depth_++]={state_.active_scene,state_.current_page};
        state_.return_parent=parents_[depth_-1]; state_.current_page=page; return "USER_OPEN";
    }
    const char* back() {
        if (state_.standby) return nullptr;
        if (state_.current_page==Page::Home) return standby();
        if (depth_) { Target parent=parents_[--depth_]; state_.active_scene=parent.scene; state_.current_page=parent.page; }
        else state_.current_page=Page::Home;
        state_.return_parent=depth_ ? parents_[depth_-1] : Target{state_.active_scene,Page::Home};
        return "USER_BACK";
    }
};
}
