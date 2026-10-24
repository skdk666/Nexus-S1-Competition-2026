"""D0/D1 real-LVGL flush/adapter regression. Synthetic SPI/CDC; never opens a port."""
import ast
import os
from pathlib import Path
import re
import subprocess

project=Path(__file__).resolve().parents[1]
work=project/'.pio/touch_spi_diag_test'
work.mkdir(parents=True,exist_ok=True)
compiler=Path(os.environ['NEXUS_HOST_CXX_DIR'])
env=os.environ.copy();env['PATH']=str(compiler)+os.pathsep+env.get('PATH','')
lvgl=project/'.pio/libdeps/esp32-s3-devkitc-1/lvgl'
objects=sorted((project/'.pio/offline_link_test').glob('*.o'))
assert len(objects)==len(list((lvgl/'src').rglob('*.c'))),'Run check_firmware_link.py first'
assert min(p.stat().st_mtime for p in objects)>=(project/'include/lv_conf.h').stat().st_mtime
sdk=Path.home()/'.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/qio_qspi/include/sdkconfig.h'
fifo=re.search(r'^#define CONFIG_TINYUSB_CDC_TX_BUFSIZE\s+(\d+)\s*$',sdk.read_text(),re.M)[1]
source=(project/'src/main.cpp').read_text(encoding='utf-8')
flush=source[source.index('// D0/D1 display gate:'):source.index('// End D0/D1 display gate.')]
assert source.count('lv_timer_handler();\n#if NEXUS_TOUCH_SPI_DIAGNOSTIC\n    nexusTouchDisplayDiagnostic().afterHandler();')==2
harness=work/'harness.cpp'
harness.write_text('#include "Arduino.h"\n#include "TFT_eSPI.h"\n#include "ui.h"\n#include "touch_spi_diag.h"\nTFT_eSPI tft;\n'+flush+
                   (project/'test/test_touch_spi_diag.cpp').read_text(encoding='utf-8'),encoding='utf-8')

def check_log(path,freeze):
    groups={}
    for line in path.read_text().splitlines():
        assert len(line)<255,(len(line),line)
        m=re.match(r'TOUCH (CFG|ADC|TX|RX|QUALITY|DISPLAY) id=(\d+) ',line)
        if m:
            kind,id=m.groups();groups.setdefault(id,{})[kind]=line
    complete=[g for g in groups.values() if len(g)==6]
    assert len(complete)>=10
    statuses=set()
    expected=[v for cmd in (0xB0,0xC0,0xD0,0x90,0xD0,0x90,0xD0,0x90,0xB0,0xC0) for v in (cmd,0,0)]
    for g in complete:
        assert f'exp=D{freeze}_LIVE_PRESSURE' in g['CFG']
        assert 'hz=1000000 mode=0 MSB cs=1/0/1 lcd=1/1 started=1 n=30 read=LEGACY30' in g['CFG']
        tx=[int(v,16) for v in g['TX'].split('hex=')[1].split()]
        rx=[int(v,16) for v in g['RX'].split('hex=')[1].split()]
        assert tx==expected and len(rx)==30
        adc=[((rx[i+1]<<8|rx[i+2])>>3)&4095 for i in range(0,30,3)]
        for key,indices in [('x',(2,4,6)),('y',(3,5,7)),('z1',(0,8)),('z2',(1,9))]:
            actual=re.search(r'\b'+key+r'=([\d,]+)',g['ADC'])[1]
            assert actual==','.join(str(adc[i]) for i in indices),(g,adc)
        statuses.add(re.search(r'\bdata=(\w+)',g['ADC'])[1])
        assert f'frozen={freeze}' in g['DISPLAY'] and 'first=1 pending=0' in g['DISPLAY']
    assert statuses>={'FF','ZERO','MIXED'}
    print(f'PASS D{freeze}: {len(complete)} complete same-ID CFG/ADC/TX/RX/QUALITY/DISPLAY groups; RX decoded independently, failed RX retained')

for freeze in (0,1):
    exe=work/f'd{freeze}.exe';log=work/f'd{freeze}.log'
    command=[str(compiler/'g++.exe'),'-std=c++17','-O0','-Wall','-Wextra',
             '-DNEXUS_TOUCH_LIVE_PREVIEW=1','-DNEXUS_TOUCH_RAW_EXPERIMENT=0',
             '-DNEXUS_TOUCH_SPI_DIAGNOSTIC=1',f'-DNEXUS_TOUCH_SPI_FREEZE_TFT={freeze}',
             '-DLV_CONF_INCLUDE_SIMPLE',f'-DNEXUS_TEST_CDC_FIFO_SIZE={fifo}',
             '-I'+str(project/'test/xpt2046_host'),'-I'+str(project/'include'),'-I'+str(project/'src'),
             '-isystem',str(lvgl),str(harness),
             *[str(project/'src'/p) for p in ('touch_esp32.cpp','xpt2046.cpp','touch_live_ui.cpp','ui.cpp','ui_m4.cpp')],
             *map(str,objects),'-o',str(exe)]
    compiled=subprocess.run(command,env=env,capture_output=True,text=True)
    if compiled.returncode:
        print(compiled.stdout+compiled.stderr);raise SystemExit(compiled.returncode)
    subprocess.run([str(exe),str(log)],env=env,check=True,timeout=30)
    check_log(log,freeze)
