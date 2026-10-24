#pragma once
#include "Arduino.h"
#include <string>
#ifndef NEXUS_TEST_CDC_FIFO_SIZE
#error Set NEXUS_TEST_CDC_FIFO_SIZE from the actual ESP32-S3 SDK configuration.
#endif
#define CFG_TUD_CDC_TX_BUFSIZE NEXUS_TEST_CDC_FIFO_SIZE
extern std::string hostCdcOutput;
extern bool hostCdcConnected;
extern bool hostCdcPartial;
extern uint32_t hostCdcSpace,hostCdcWrites,hostCdcFlushes;
extern uint32_t hostCdcMaxWrite;
extern uint8_t hostCdcLineState;
inline bool tud_cdc_n_connected(uint8_t itf) { assert(itf==0);return hostCdcConnected; }
inline uint8_t tud_cdc_n_get_line_state(uint8_t itf) { assert(itf==0);return hostCdcLineState; }
inline uint32_t tud_cdc_n_write_available(uint8_t itf) { assert(itf==0);return hostCdcSpace; }
inline uint32_t tud_cdc_n_write(uint8_t itf,const void* data,uint32_t size) {
    assert(!hostSPI.active); // Production diagnostics must never print inside SPI transactions.
    assert(itf==0);++hostCdcWrites;
    assert(size<=hostCdcSpace && hostCdcSpace<=CFG_TUD_CDC_TX_BUFSIZE);
    if(size>hostCdcMaxWrite)hostCdcMaxWrite=size;
    const uint32_t written=hostCdcPartial?size/2:size;
    hostCdcOutput.append(static_cast<const char*>(data),written);
    hostCdcSpace-=written; // Flush does not magically drain a busy host endpoint.
    return written;
}
inline uint32_t tud_cdc_n_write_flush(uint8_t itf) { assert(itf==0);++hostCdcFlushes;return 0; }
