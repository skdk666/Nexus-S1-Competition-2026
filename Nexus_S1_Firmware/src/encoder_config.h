#pragma once
#include "hid_output_config.h"
#ifndef NEXUS_ENCODER_PREVIEW
#define NEXUS_ENCODER_PREVIEW 0
#endif
#ifndef NEXUS_ENCODER_EDGES_PER_STEP
#define NEXUS_ENCODER_EDGES_PER_STEP 4
#endif
#ifndef NEXUS_ENCODER_DIRECTION
#define NEXUS_ENCODER_DIRECTION 1
#endif
#ifndef NEXUS_ENCODER_HID_ENABLED
#define NEXUS_ENCODER_HID_ENABLED 0
#endif
static_assert(NEXUS_ENCODER_PREVIEW==0 || NEXUS_ENCODER_PREVIEW==1,"Invalid encoder preview switch");
static_assert(NEXUS_ENCODER_EDGES_PER_STEP==2 || NEXUS_ENCODER_EDGES_PER_STEP==4,"Encoder edges per step must be 2 or 4; verify actual module detents");
static_assert(NEXUS_ENCODER_DIRECTION==1 || NEXUS_ENCODER_DIRECTION==-1,"Encoder direction must be +/-1");
static_assert(NEXUS_ENCODER_HID_ENABLED==0 || NEXUS_ENCODER_HID_ENABLED==1,"Invalid encoder HID opt-in");
static_assert(!NEXUS_ENCODER_HID_ENABLED || (NEXUS_ENCODER_PREVIEW && !NEXUS_HID_OUTPUT_DISABLED),"Encoder HID opt-in requires preview and HID output enabled");
static_assert(!NEXUS_ENCODER_PREVIEW || NEXUS_HID_OUTPUT_DISABLED || NEXUS_ENCODER_HID_ENABLED,"Encoder HID output requires explicit opt-in");
namespace nexus_encoder {
// Confirmed DevKit wiring: rotary A/B on 1/2, rotary common C to GND.
// The separate push switch connects pin_sw to GND; this is not the rotary C.
// Temporary flying wires, not the final PCB mapping.
constexpr int pin_a=1,pin_b=2,pin_sw=21;
}
