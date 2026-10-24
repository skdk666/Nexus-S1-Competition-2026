#pragma once
// Encoder-only SDK additions; existing touch SDK spies remain unchanged.
#include "../xpt2046_host/Arduino.h"
#include <mutex>
#define INPUT_PULLUP 2
#define CHANGE 3
#define IRAM_ATTR
struct portMUX_TYPE { std::recursive_mutex lock; };
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) ((p)->lock.lock())
#define portEXIT_CRITICAL(p) ((p)->lock.unlock())
#define portENTER_CRITICAL_ISR(p) (assert(hostISR),(p)->lock.lock())
#define portEXIT_CRITICAL_ISR(p) ((p)->lock.unlock())
extern int encoderPinMode[49];
extern void (*encoderIrq[49])();
inline void encoderPinModeSet(int pin,int mode) {
    assert(!hostISR && pin>=0 && pin<49 && (mode==OUTPUT || mode==INPUT_PULLUP));
    encoderPinMode[pin]=mode;++pinModes;
}
#define pinMode encoderPinModeSet
inline void attachInterrupt(int pin,void (*callback)(),int mode) {
    assert(!hostISR && (pin==1 || pin==2) && mode==CHANGE);encoderIrq[pin]=callback;
}
