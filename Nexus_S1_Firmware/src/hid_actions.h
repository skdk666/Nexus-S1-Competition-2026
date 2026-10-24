#pragma once
#include <stdint.h>

namespace nexus {
// IDs 0..3 are the original M1 contract. Values are USB HID usages, not ASCII.
enum ActionId : uint8_t {
    Copy, Paste, SwitchWindow, ShowDesktop, Explorer,
    CommandPalette, QuickOpen, Terminal, CommentLine,
    NewTab, CloseTab, ReopenTab, AddressBar, Refresh, BrowserBack,
    PlayPause, PreviousTrack, NextTrack, VolumeUp, VolumeDown, Mute,
#if defined(NEXUS_ENCODER_PREVIEW) && NEXUS_ENCODER_PREVIEW
    CtrlD, // Appended: retain all existing IDs, especially M1 0..3 and Consumer 15..20.
#endif
    ActionCount
};
enum class HidKind { Keyboard, Consumer };
struct HidAction { const char* name; HidKind kind; uint8_t modifiers, key; uint16_t usage; };
inline const HidAction* hidAction(int id) {
    static const HidAction actions[] = {
        {"Ctrl+C",HidKind::Keyboard,1,6,0}, {"Ctrl+V",HidKind::Keyboard,1,25,0},
        {"Alt+Tab",HidKind::Keyboard,4,43,0}, {"Win+D",HidKind::Keyboard,8,7,0},
        {"Win+E",HidKind::Keyboard,8,8,0},
        {"Ctrl+Shift+P",HidKind::Keyboard,3,19,0}, {"Ctrl+P",HidKind::Keyboard,1,19,0},
        {"Ctrl+`",HidKind::Keyboard,1,53,0}, {"Ctrl+/",HidKind::Keyboard,1,56,0},
        {"Ctrl+T",HidKind::Keyboard,1,23,0}, {"Ctrl+W",HidKind::Keyboard,1,26,0},
        {"Ctrl+Shift+T",HidKind::Keyboard,3,23,0}, {"Ctrl+L",HidKind::Keyboard,1,15,0},
        {"Ctrl+R",HidKind::Keyboard,1,21,0}, {"Alt+Left",HidKind::Keyboard,4,80,0},
        {"Play/Pause",HidKind::Consumer,0,0,0xCD}, {"Previous track",HidKind::Consumer,0,0,0xB6},
        {"Next track",HidKind::Consumer,0,0,0xB5}, {"Volume +",HidKind::Consumer,0,0,0xE9},
        {"Volume -",HidKind::Consumer,0,0,0xEA}, {"Mute",HidKind::Consumer,0,0,0xE2},
#if defined(NEXUS_ENCODER_PREVIEW) && NEXUS_ENCODER_PREVIEW
        {"Ctrl+D",HidKind::Keyboard,1,7,0},
#endif
    };
    static_assert(sizeof(actions)/sizeof(actions[0])==ActionCount,"Every ID needs a binding");
    return id>=0 && id<ActionCount ? &actions[id] : nullptr;
}
}
