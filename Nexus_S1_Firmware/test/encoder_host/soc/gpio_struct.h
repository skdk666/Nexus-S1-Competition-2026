#pragma once
#include <stdint.h>
struct EncoderGpioRegisters { volatile uint32_t in=0; };
extern EncoderGpioRegisters GPIO;
