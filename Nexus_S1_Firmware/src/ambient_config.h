#pragma once
// User-confirmed Beijing time, UTC+8, without daylight saving time.
// POSIX TZ offsets use the opposite sign: CST-8 means UTC+8.
#ifndef NEXUS_TIMEZONE
#define NEXUS_TIMEZONE "CST-8"
#endif
#ifndef NEXUS_TIMEZONE_LABEL
#define NEXUS_TIMEZONE_LABEL "UTC+8"
#endif
#ifndef NEXUS_NTP_SERVER
#define NEXUS_NTP_SERVER "pool.ntp.org"
#endif
