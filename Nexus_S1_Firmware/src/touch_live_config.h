#pragma once
// Separate build opt-in. Production and A/B/C remain RAW with HID unchanged.
#ifndef NEXUS_TOUCH_LIVE_PREVIEW
#define NEXUS_TOUCH_LIVE_PREVIEW 0
#endif
#ifndef NEXUS_TOUCH_LIVE_XY_ONLY
#define NEXUS_TOUCH_LIVE_XY_ONLY 0
#endif
static_assert(NEXUS_TOUCH_LIVE_PREVIEW == 0 || NEXUS_TOUCH_LIVE_PREVIEW == 1, "Invalid live preview switch");
static_assert(NEXUS_TOUCH_LIVE_XY_ONLY == 0 || NEXUS_TOUCH_LIVE_XY_ONLY == 1, "Invalid XY experiment switch");
static_assert(NEXUS_TOUCH_LIVE_XY_ONLY == 0 || NEXUS_TOUCH_LIVE_XY_ONLY == 1, "Invalid XY experiment switch");
static_assert(!NEXUS_TOUCH_LIVE_XY_ONLY || NEXUS_TOUCH_LIVE_PREVIEW, "XY experiment requires isolated live preview");
