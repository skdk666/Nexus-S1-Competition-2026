#pragma once
#include <stdint.h>

#ifndef NEXUS_TOUCH_SPI_DIAGNOSTIC
#define NEXUS_TOUCH_SPI_DIAGNOSTIC 0
#endif
#ifndef NEXUS_TOUCH_SPI_FREEZE_TFT
#define NEXUS_TOUCH_SPI_FREEZE_TFT 0
#endif

#if NEXUS_TOUCH_SPI_DIAGNOSTIC
// Only D0/D1 use this gate. SPI sampling and LVGL scheduling are untouched.
struct TouchSpiDisplayDiagnostic {
    uint32_t handlers=0, flushes=0, writes=0, skipped=0, pixels=0;
    uint16_t next_row=0;
    bool pending=false, first_complete=false, frozen=false;

    void flushed(int x1,int y1,int x2,int y2,bool last,bool written) {
        ++flushes;
        if(written) { ++writes;pixels+=uint32_t(x2-x1+1)*uint32_t(y2-y1+1); }
        else ++skipped;
        if(first_complete || pending) return;
        // The 320x10 draw buffer splits the initial full-screen invalidation
        // into full-width consecutive rows. An unrelated partial refresh must
        // never freeze an incomplete first screen.
        if(written && x1==0 && x2==319 && y1==next_row && y2>=y1 && y2<240)
            next_row=uint16_t(y2+1);
        else next_row=0;
        if(last) {
            pending=next_row==240;
            next_row=0;
        }
    }
    void afterHandler() {
        ++handlers;
        if(pending) {
            first_complete=true;
            pending=false;
            frozen=NEXUS_TOUCH_SPI_FREEZE_TFT!=0;
        }
    }
};
TouchSpiDisplayDiagnostic& nexusTouchDisplayDiagnostic();
#endif
