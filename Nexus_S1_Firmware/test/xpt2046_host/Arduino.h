#pragma once
// Host-only spies for the production ESP32 adapter. No Windows GPIO/USB APIs.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <cassert>
#include <vector>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define MSBFIRST 1
#define SPI_MODE0 0
using TaskHandle_t=void*;
extern TaskHandle_t hostTask;
extern bool hostISR;
extern uint32_t hostMillis;
extern int gpioWrites, pinModes, pinLevels[49];
inline bool xPortInIsrContext() { return hostISR; }
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return hostTask; }
inline uint32_t millis() { return hostMillis; }
inline void digitalWrite(int pin,int value) { assert(pin>=0 && pin<49);++gpioWrites;pinLevels[pin]=value; }
inline void pinMode(int,int mode) { assert(mode==OUTPUT);++pinModes; }
inline int digitalRead(int pin) { assert(pin>=0 && pin<49);return pinLevels[pin]; }
struct SPISettings {
    uint32_t hz; int order,mode;
    SPISettings(uint32_t h,int o,int m):hz(h),order(o),mode(m) {}
};
struct SPIClass {
    int begins=0,ends=0,bytes=0,transactionBytes=0;
    bool settled=false,noisy=false;
    bool active=false,touched=false,floating=false,zero=false,paddingOnly=false;
    uint16_t touchZ1=400,touchZ2=3500;
    uint16_t touchX=1000,touchY=2000;
    uint32_t lastHz=0;
    void beginTransaction(const SPISettings& s) {
        assert(!active && (s.hz==1000000 || s.hz==250000) && s.order==MSBFIRST && s.mode==SPI_MODE0);
        lastHz=s.hz;
        assert(pinLevels[10]==HIGH && pinLevels[7]==HIGH);active=true;++begins;transactionBytes=0;
    }
    void endTransaction() { assert(active && pinLevels[7]==HIGH);active=false;++ends; }
    uint8_t transfer(uint8_t value) {
        assert(active && pinLevels[7]==LOW && pinLevels[10]==HIGH);
        const int n=transactionBytes++,phase=n%3,conversion=n/3;++bytes;
        if(n==0)settled=value==0xB1;
        const uint8_t commands[]={0xB0,0xC0,0xD0,0x90,0xD0,0x90,0xD0,0x90,0xB0,0xC0};
        const int sample=settled?conversion/2:conversion;
        assert(conversion<(settled?21:10));
        const uint8_t command=settled?(conversion==20?0x90:uint8_t(commands[sample]|1)):commands[sample];
        assert(value==(phase==0?command:0));
        if(floating)return 0xFF;
        if(zero)return 0;
        if(paddingOnly)return phase==2?7:0; // RX is nonzero but all decoded ADC values are zero.
        uint16_t adc=sample==0 || sample==8 ? (touched?touchZ1:0) :
            sample==1 || sample==9 ? (touched?touchZ2:4095) : sample%2==0?touchX:touchY;
        if(noisy && sample>=2 && sample<=7)adc=sample%2==0?uint16_t(1000+sample*200):uint16_t(2000+sample*100);
        if(settled && conversion<20 && conversion%2==0)adc=123; // Deliberately different discarded first result.
        return phase==0?0:uint8_t(phase==1?(adc<<3)>>8:(adc<<3)&255);
    }
    SPIClass* bus() { return this; } // Mock non-null Arduino HAL bus handle.
};
extern SPIClass hostSPI;
struct SerialSpy {
    void println(const char*) {}
};
extern SerialSpy Serial;
