"""Run production encoder logic/ESP32 adapter with real LVGL and synthetic GPIO/SPI/CDC/HID."""
import ast
import os
from pathlib import Path
import re
import subprocess

project=Path(__file__).resolve().parents[1]
work=project/'.pio/encoder_test';work.mkdir(parents=True,exist_ok=True)
compiler=Path(os.environ['NEXUS_HOST_CXX_DIR']);env=os.environ.copy()
env['PATH']=str(compiler)+os.pathsep+env.get('PATH','')
unit=work/'core.exe'
subprocess.run([str(compiler/'g++.exe'),'-std=c++11','-Wall','-Wextra','-Werror','-pedantic',
                '-I'+str(project/'src'),str(project/'test/test_encoder.cpp'),'-o',str(unit)],env=env,check=True)
subprocess.run([str(unit)],env=env,check=True,timeout=10)
lvgl=project/'.pio/libdeps/esp32-s3-devkitc-1/lvgl'
objects=sorted((project/'.pio/offline_link_test').glob('*.o'))
assert len(objects)==len(list((lvgl/'src').rglob('*.c'))),'Run check_firmware_link.py first'
assert min(p.stat().st_mtime for p in objects)>=(project/'include/lv_conf.h').stat().st_mtime
tree=ast.parse((project/'pc_script/check_firmware_link.py').read_text(encoding='utf-8'))
prefix=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign)
            and isinstance(n.targets[0],ast.Name) and n.targets[0].id=='prefix')
prefix=prefix.replace('uint32_t millis() { return lv_tick_get(); }','')
prefix=re.sub(r'struct portMUX_TYPE .*?#define portEXIT_CRITICAL\(p\).*?\n','',prefix,flags=re.S)
prefix='#include "Arduino.h"\n'+prefix
source=(project/'src/main.cpp').read_text(encoding='utf-8')
enum=source[source.index('typedef enum {'):source.index('} SystemMode_t;')+len('} SystemMode_t;')]
runtime=source[source.index('// Protected by dataMutex.'):source.index('// Macro requests may come')]
semaphores=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign)
                and isinstance(n.targets[0],ast.Name) and n.targets[0].id=='semaphores')
macro=source[source.index('// Macro requests may come'):source.index('// void broadcastData')]
harness=work/'harness.cpp'
harness.write_text(prefix+semaphores+enum+runtime+macro+(project/'test/test_encoder_lvgl.cpp').read_text(encoding='utf-8'),encoding='utf-8')
sdk=Path.home()/'.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/qio_qspi/include/sdkconfig.h'
fifo=re.search(r'^#define CONFIG_TINYUSB_CDC_TX_BUFSIZE\s+(\d+)\s*$',sdk.read_text(),re.M)[1]
exe=work/'encoder.exe'
command=[str(compiler/'g++.exe'),'-std=c++17','-O0','-pthread','-Wall','-Wextra',
         '-DNEXUS_ENCODER_PREVIEW=1','-DNEXUS_HID_OUTPUT_DISABLED=1','-DNEXUS_TOUCH_RAW_EXPERIMENT=0',
         '-DLV_CONF_INCLUDE_SIMPLE',f'-DNEXUS_TEST_CDC_FIFO_SIZE={fifo}',
         '-I'+str(project/'test/encoder_host'),'-I'+str(project/'test/xpt2046_host'),
         '-I'+str(project/'.pio/libdeps/esp32-s3-devkitc-1/ArduinoJson/src'),
         '-I'+str(project/'include'),'-I'+str(project/'src'),'-isystem',str(lvgl),str(harness),
         *[str(project/'src'/p) for p in ('encoder_esp32.cpp','touch_esp32.cpp','xpt2046.cpp','ui.cpp','ui_m4.cpp')],
         *map(str,objects),'-o',str(exe)]
compiled=subprocess.run(command,env=env,capture_output=True,text=True)
if compiled.returncode:
    print(compiled.stdout+compiled.stderr);raise SystemExit(compiled.returncode)
from test_agent_startup import capture_scene_frames
(work/'scene_frames.jsonl').write_bytes(b''.join(capture_scene_frames()))
subprocess.run([str(exe)],cwd=work,env=env,check=True,timeout=30)
for first in range(4):
    # Fresh process per first scene: boot-time fallback and its one-shot latch are real production code.
    subprocess.run([str(exe),str(first)],cwd=work,env=env,check=True,timeout=30)
preprocessed=subprocess.run([*command[:1],*command[1:command.index(str(harness))],'-E','-P',str(harness)],
                            env=env,capture_output=True,text=True,check=True)
assert 'macroHid.SendReport(' not in preprocessed.stdout,'HID calls must be compiled OUT of independent preview'
# The authorized on-device profile is tested against a USB spy, never Windows APIs.
hid_exe=work/'encoder_hid.exe'
hid_command=[arg.replace('-DNEXUS_HID_OUTPUT_DISABLED=1','-DNEXUS_HID_OUTPUT_DISABLED=0')
             if arg!=str(exe) else str(hid_exe) for arg in command]
hid_command.insert(1,'-DNEXUS_ENCODER_HID_ENABLED=1')
compiled=subprocess.run(hid_command,env=env,capture_output=True,text=True)
if compiled.returncode:
    print(compiled.stdout+compiled.stderr);raise SystemExit(compiled.returncode)
subprocess.run([str(hid_exe),'--hid'],cwd=work,env=env,check=True,timeout=30)
for first in range(4):
    subprocess.run([str(hid_exe),str(first)],cwd=work,env=env,check=True,timeout=30)
for bad in ('-DNEXUS_HID_OUTPUT_DISABLED=0','-DNEXUS_ENCODER_EDGES_PER_STEP=1'):
    args=[str(compiler/'g++.exe'),'-std=c++11','-fsyntax-only','-x','c++','-DNEXUS_ENCODER_PREVIEW=1',
          '-I'+str(project/'src')]
    if 'EDGES' in bad:args+=['-DNEXUS_HID_OUTPUT_DISABLED=1']
    result=subprocess.run([*args,bad,'-'],input='#include "encoder_config.h"\n',env=env,capture_output=True,text=True)
    assert result.returncode,'Unsafe/invalid preview configuration was accepted'
print('PASS encoder preview: disabled profile compiles HID calls out; authorized HID-enabled profile tested with synthetic USB; unapproved HID/invalid steps rejected; no physical hardware used')
