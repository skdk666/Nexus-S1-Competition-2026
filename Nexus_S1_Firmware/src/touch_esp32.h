#pragma once
#include "xpt2046.h"
class TFT_eSPI;
// Set a confirmed touch CS inactive before TFT initialization starts shared clocks.
// Disabled/invalid configuration has no side effects.
bool prepareNexusTouchPins();
bool prepareNexusTouchPins(const xpt2046::Config& config);
// Called once after TFT and ui_init(), on the same task that runs LVGL.
// Disabled/invalid config returns without touching any GPIO or SPI register.
bool initNexusTouch(TFT_eSPI& display);
bool initNexusTouch(TFT_eSPI& display, const xpt2046::Config& config);
// Main-loop-only bounded CDC TX service and 1Hz heartbeat, independent of LVGL/SPI.
void serviceNexusTouchDiagnostics();
#if defined(NEXUS_ENCODER_PREVIEW) && NEXUS_ENCODER_PREVIEW
// Encoder-preview main loop only: share the existing bounded CDC line queue.
// False means busy/offline; callers retry later, never wait or write in an ISR.
bool tryNexusEncoderDiagnosticLine(const char* text, uint32_t length);
#endif
