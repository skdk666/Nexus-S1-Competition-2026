#pragma once
// Independent of touch experiments. Default and existing environments are unchanged.
#ifndef NEXUS_HID_OUTPUT_DISABLED
#define NEXUS_HID_OUTPUT_DISABLED 0
#endif
static_assert(NEXUS_HID_OUTPUT_DISABLED==0 || NEXUS_HID_OUTPUT_DISABLED==1,"Invalid HID output switch");
