// Linked to the actual ESP32 adapter with fake Arduino/SNTP and a controlled clock.
#include "ambient_esp32.h"
#include "ambient_config.h"
#include <Arduino.h>
#include <esp_sntp.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <cstdlib>

static uint32_t now_ms = 0;
static time_t epoch = 1792540800; // 2026-10-21 00:00 UTC, synthetic.
static int configured = 0, updates = 0;
static bool valid = false;
static std::string clock_text, date_text, source_text;
static sntp_sync_time_cb_t callback;
uint32_t millis() { return now_ms; }
time_t nexusTestTime(time_t*) { return epoch; }
void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb) { callback = cb; }
void configTzTime(const char* tz, const char* server, const char*) {
    assert(std::string(tz)==NEXUS_TIMEZONE && std::string(server)==NEXUS_NTP_SERVER);
    ++configured; _putenv_s("TZ",tz); _tzset();
}
void ui_set_ambient_clock(bool ok, const char* clock, const char* date, const char* source) {
    ++updates; valid=ok;
    clock_text=clock ? clock : ""; date_text=date ? date : ""; source_text=source ? source : "";
}
int main() {
    serviceAmbientClock(false);
    assert(configured==0 && updates==1 && !valid); // Even a plausible RTC epoch is not proof of sync.
    serviceAmbientClock(true);
    assert(configured==1 && callback && updates==1);
    now_ms=1000; serviceAmbientClock(true);
    assert(!valid && updates==2); // Starting SNTP does not fabricate success.
    callback(nullptr); now_ms=2000; serviceAmbientClock(true);
    assert(valid && clock_text=="08:00" && date_text=="Wed  2026-10-21");
    assert(source_text=="UTC+8  NTP synced");
    now_ms=3000; serviceAmbientClock(false);
    assert(valid && source_text=="UTC+8  holdover");
    now_ms=4000; serviceAmbientClock(true);
    assert(configured==1); // No repeated restart while reconnecting.
    epoch=1792540800+17*3600; now_ms=5000; serviceAmbientClock(true);
    assert(valid && clock_text=="01:00" && date_text=="Thu  2026-10-22"); // Beijing date rolls over before UTC.
    epoch=0; now_ms=6000; serviceAmbientClock(true); assert(!valid);
    epoch=1792540800;
    now_ms=0xFFFFFF00U; serviceAmbientClock(false); int before=updates;
    now_ms=0x10U; serviceAmbientClock(false); assert(updates==before);
    now_ms=0x400U; serviceAmbientClock(false); assert(updates==before+1 && valid);
    puts("PASS ambient clock MOCK SDK: no-WiFi/unsynced, async init once, actual sync gate, Beijing UTC+8 date/day rollover, holdover, invalid epoch, 1Hz/wrap");
}
