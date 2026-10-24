#include "ambient_esp32.h"
#include "ambient_config.h"
#include "ui.h"
#include <Arduino.h>
#include <time.h>
#include <esp_sntp.h>
#include <atomic>
#include <stdio.h>

namespace {
std::atomic<bool> synchronized{false};
void onSync(struct timeval*) { synchronized.store(true); }
}
void serviceAmbientClock(bool connected) {
    static bool configured = false, published = false;
    static uint32_t last = 0;
    // ESP-IDF SNTP performs DNS/time exchange in lwIP; configTzTime does not wait.
    if (connected && !configured) {
        sntp_set_time_sync_notification_cb(onSync);
        configTzTime(NEXUS_TIMEZONE, NEXUS_NTP_SERVER, "time.google.com");
        configured = true;
    }
    uint32_t now = millis();
    if (published && uint32_t(now - last) < 1000) return;
    published = true; last = now;
    time_t epoch = time(nullptr);
    if (!synchronized.load() || epoch < 1704067200) {
        ui_set_ambient_clock(false, nullptr, nullptr, nullptr);
        return;
    }
    struct tm date{}; localtime_r(&epoch, &date);
    char clock[8], calendar[32], source[48];
    strftime(clock, sizeof(clock), "%H:%M", &date);
    strftime(calendar, sizeof(calendar), "%a  %Y-%m-%d", &date);
    snprintf(source, sizeof(source), "%s  %s", NEXUS_TIMEZONE_LABEL, connected ? "NTP synced" : "holdover");
    ui_set_ambient_clock(true, clock, calendar, source);
}
