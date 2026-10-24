"""Compile live filter + real ESP32 adapter/LVGL/main HID guard; no real ports or keys."""
import ast
import os
import re
from pathlib import Path
import subprocess

project = Path(__file__).resolve().parents[1]
work = project / '.pio/touch_live_test'
work.mkdir(parents=True, exist_ok=True)
compiler = Path(os.environ['NEXUS_HOST_CXX_DIR'])
env = os.environ.copy()
env['PATH'] = str(compiler)+os.pathsep+env.get('PATH', '')
exe = work/'filter.exe'
subprocess.run([str(compiler/'g++.exe'),'-std=c++11','-Wall','-Wextra','-Werror','-pedantic',
                '-I'+str(project/'src'),str(project/'test/test_touch_live.cpp'),'-o',str(exe)],env=env,check=True)
subprocess.run([str(exe)],env=env,check=True,timeout=10)

# Reuse the regression's real LVGL C objects; require check_firmware_link.py to
# build them from this lv_conf.h first. Compile all live/UI/adapter C++ afresh.
lvgl = project/'.pio/libdeps/esp32-s3-devkitc-1/lvgl'
objects = sorted((project/'.pio/offline_link_test').glob('*.o'))
assert len(objects)==len(list((lvgl/'src').rglob('*.c'))), 'Run check_firmware_link.py first'
assert min(p.stat().st_mtime for p in objects)>= (project/'include/lv_conf.h').stat().st_mtime
packages=Path(os.environ.get('PLATFORMIO_PACKAGES_DIR',
    Path(os.environ.get('PLATFORMIO_CORE_DIR',Path.home()/'.platformio'))/'packages'))
sdk=packages/'framework-arduinoespressif32/tools/sdk/esp32s3/qio_qspi/include/sdkconfig.h'
fifo=re.search(r'^#define CONFIG_TINYUSB_CDC_TX_BUFSIZE\s+(\d+)\s*$',sdk.read_text(encoding='utf-8'),re.MULTILINE)
assert fifo, 'Real SDK FIFO configuration required'
tree = ast.parse((project/'pc_script/check_firmware_link.py').read_text(encoding='utf-8'))
prefix = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign)
              and isinstance(n.targets[0],ast.Name) and n.targets[0].id=='prefix')
prefix = '#include "Arduino.h"\n'+prefix.replace('uint32_t millis() { return lv_tick_get(); }','')
source = (project/'src/main.cpp').read_text(encoding='utf-8')
macro = source[source.index('// Macro requests may come'):source.index('// void broadcastData')]
harness = work/'harness.cpp'
harness.write_text(prefix+macro+(project/'test/test_touch_live_lvgl.cpp').read_text(encoding='utf-8'),encoding='utf-8')
for xy in (0, 1):
    exe = work/f'live_{xy}.exe'
    command = [str(compiler/'g++.exe'),'-std=c++17','-O0','-pthread','-Wall','-Wextra',
               '-DNEXUS_TOUCH_LIVE_PREVIEW=1',f'-DNEXUS_TOUCH_LIVE_XY_ONLY={xy}',
               '-DNEXUS_TOUCH_RAW_EXPERIMENT=0','-DLV_CONF_INCLUDE_SIMPLE',
               f'-DNEXUS_TEST_CDC_FIFO_SIZE={fifo[1]}',
               '-I'+str(project/'test/xpt2046_host'),'-I'+str(project/'include'),'-I'+str(project/'src'),
               '-isystem',str(lvgl),str(harness),
               *[str(project/'src'/p) for p in ('touch_esp32.cpp','xpt2046.cpp','touch_live_ui.cpp','ui.cpp','ui_m4.cpp')],
               *[str(p) for p in objects],'-o',str(exe)]
    compiled=subprocess.run(command,env=env,capture_output=True,text=True)
    if compiled.returncode:
        print(compiled.stdout+compiled.stderr)
        raise SystemExit(compiled.returncode)
    subprocess.run([str(exe)],env=env,cwd=work,check=True,timeout=30)
    print(f'PASS live preview policy xy={xy}',flush=True)
try:
    import struct
    from PIL import Image
    for path in work.glob('*.rgb565'):
        values=struct.unpack('<76800H',path.read_bytes())
        rgb=bytes(c for v in values for c in (((v>>11)&31)*255//31,((v>>5)&63)*255//63,(v&31)*255//31))
        Image.frombytes('RGB',(320,240),rgb).save(path.with_suffix('.png'))
    print('Rendered actual LVGL diagnostic screenshots with SYNTHETIC data under',work)
except ImportError:
    print('Pillow unavailable; raw RGB565 screenshots retained')
