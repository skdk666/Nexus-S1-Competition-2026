#pragma once
#include "Arduino.h"
#define TFT_CS 10
#define TFT_DC 9
#define TFT_RST 14
#define TFT_BL 13
#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_MISO 8 // Mirrors temporary dev-board configuration; IO below is still mocked.
#define SPI_PORT 2 // ESP32-S3 FSPI register index, not a physical SPI device.
class TFT_eSPI {
public:
    bool busy=false; int finishes=0; uint8_t rotation=1;
    uint32_t windows=0, pushes=0, pushedPixels=0;
    void setAddrWindow(int x,int y,uint32_t w,uint32_t h) {
        assert(!hostSPI.active && x>=0 && y>=0 && x+w<=320 && y+h<=240);++windows;
    }
    void pushColors(uint16_t*,uint32_t count,bool swap) {
        assert(!hostSPI.active && swap);++pushes;pushedPixels+=count;
    }
    bool dmaBusy() { return busy; }
    void endWrite() { ++finishes;pinLevels[TFT_CS]=HIGH; }
    static SPIClass& getSPIinstance() { return hostSPI; }
    uint8_t getRotation() { return rotation; }
    int width() { return 320; } int height() { return 240; }
};
