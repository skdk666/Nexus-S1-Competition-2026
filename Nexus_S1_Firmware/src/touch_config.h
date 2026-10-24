#pragma once
#include "xpt2046.h"
#include "touch_live_config.h"

// Build-time A/B/C only; default retains the checkpoint's B sampling path.
// A -> B changes sampling protocol only. B -> C changes SPI frequency only.
#ifndef NEXUS_TOUCH_RAW_EXPERIMENT
#define NEXUS_TOUCH_RAW_EXPERIMENT 1
#endif
static_assert(NEXUS_TOUCH_RAW_EXPERIMENT >= 0 && NEXUS_TOUCH_RAW_EXPERIMENT <= 2,
              "XPT RAW experiment must be 0=A, 1=B, or 2=C");
static_assert(!NEXUS_TOUCH_LIVE_PREVIEW || NEXUS_TOUCH_RAW_EXPERIMENT == 0,
              "Live preview must use the isolated LEGACY30/1MHz experiment");
static_assert(!NEXUS_TOUCH_LIVE_PREVIEW || NEXUS_TOUCH_RAW_EXPERIMENT == 0,
              "Live preview must use the isolated LEGACY30/1MHz experiment");
inline const char* nexusTouchExperiment(const xpt2046::Config& c) {
#if NEXUS_TOUCH_LIVE_PREVIEW
    if(c.mode==xpt2046::Mode::RawDiagnostics) return NEXUS_TOUCH_LIVE_XY_ONLY?"LIVE_XY":"LIVE_PRESSURE";
#endif
    if (c.mode != xpt2046::Mode::RawDiagnostics) return "CUSTOM";
    if (!c.raw_settled_probe && c.spi_hz == 1000000) return "A";
    if (c.raw_settled_probe && c.spi_hz == 1000000) return "B";
    if (c.raw_settled_probe && c.spi_hz == 250000) return "C";
    return "CUSTOM";
}

// User-approved temporary DevKitC-1 flying wires (2026-10-21), NOT final PCB nets.
// T_CLK=12/T_DIN=11 share TFT FSPI; T_IRQ is unused (polling).
// TFT_MISO in platformio.ini must match miso_pin so TFT.begin() routes SPI input.
inline xpt2046::Config nexusTouchConfig() {
    xpt2046::Config c;
    c.mode = xpt2046::Mode::RawDiagnostics; // Always reports RELEASED to LVGL.
    c.pins_confirmed = true; // Temporary wiring authorized, not physically verified.
    c.cs_pin = 7;
    c.miso_pin = 8;
    c.raw_settled_probe = NEXUS_TOUCH_RAW_EXPERIMENT != 0;
    c.spi_hz = NEXUS_TOUCH_RAW_EXPERIMENT == 2 ? 250000 : 1000000;
    c.rotation = 1; // Must match existing tft.setRotation(1), 320x240.
    c.calibration.confirmed = false;
    c.calibration.swap_xy = false;
    c.calibration.x0 = 0; c.calibration.x1 = 0;
    c.calibration.y0 = 0; c.calibration.y1 = 0;
    return c;
}
