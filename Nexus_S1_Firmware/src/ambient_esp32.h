#pragma once
// Call only from the UI loop. Does not wait for WiFi, DNS or time synchronization.
void serviceAmbientClock(bool wifi_connected);
