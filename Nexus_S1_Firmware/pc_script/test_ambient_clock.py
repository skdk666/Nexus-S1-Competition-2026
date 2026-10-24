"""Exercise the actual SNTP/UI adapter without opening a network connection."""
from pathlib import Path
import os
import shutil
import subprocess

project = Path(__file__).resolve().parents[1]
work = project / '.pio/ambient_test'
work.mkdir(parents=True, exist_ok=True)
override = os.environ.get('NEXUS_HOST_CXX_DIR')
compiler = str(Path(override)/'g++.exe') if override else shutil.which('g++')
if not compiler:
    raise SystemExit('Set NEXUS_HOST_CXX_DIR or put MinGW g++ on PATH')
env = os.environ.copy()
env['PATH'] = str(Path(compiler).parent)+os.pathsep+env.get('PATH','')
# Shim headers are generated only inside the isolated build directory.
(work/'Arduino.h').write_text('''#pragma once
#include <stdint.h>
uint32_t millis();
void configTzTime(const char*,const char*,const char*);
''',encoding='utf-8')
(work/'esp_sntp.h').write_text('''#pragma once
struct timeval;
typedef void (*sntp_sync_time_cb_t)(struct timeval*);
void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t);
''',encoding='utf-8')
(work/'controlled_time.h').write_text('''#pragma once
#include <time.h>
inline struct tm* localtime_r(const time_t* value,struct tm* result) {
    return localtime_s(result,value)==0 ? result : nullptr;
}
time_t nexusTestTime(time_t*);
#define time nexusTestTime
''',encoding='utf-8')
exe = work/'ambient_test.exe'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic',
                '-DLV_CONF_INCLUDE_SIMPLE','-include',str(work/'controlled_time.h'),
                '-I'+str(work),'-I'+str(project/'src'),'-I'+str(project/'include'),
                '-isystem',str(project/'.pio/libdeps/esp32-s3-devkitc-1/lvgl'),
                str(project/'test/test_ambient_clock.cpp'),str(project/'src/ambient_esp32.cpp'),
                '-o',str(exe)],env=env,check=True)
subprocess.run([str(exe)],env=env,check=True,timeout=10)
