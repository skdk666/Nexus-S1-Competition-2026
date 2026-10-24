"""Inspect actual PlatformIO SDK objects and linked ELF; no USB enumeration claim."""
from pathlib import Path
import os
import subprocess

project = Path(__file__).resolve().parents[1]
build = project / '.pio/build/esp32-s3-devkitc-1'
work = project / '.pio/hid_descriptor_test'
work.mkdir(parents=True, exist_ok=True)
packages = Path(os.environ.get('PLATFORMIO_PACKAGES_DIR',
    Path(os.environ.get('PLATFORMIO_CORE_DIR',Path.home()/'.platformio'))/'packages'))
bin_dir = packages/'toolchain-xtensa-esp32s3/bin'

def descriptor(object_name):
    matches = list(build.rglob(object_name+'.cpp.o'))
    assert len(matches)==1, f'Build first; expected one {object_name} object: {matches}'
    target = work/(object_name+'.bin')
    subprocess.run([str(bin_dir/'xtensa-esp32s3-elf-objcopy.exe'),
        '--dump-section',f'.rodata._ZL17report_descriptor={target}',str(matches[0])],check=True)
    data = target.read_bytes()
    assert data, 'Empty descriptor'
    inputs, pages, ids = {}, set(), set()
    values, stack = {}, []
    pos = 0
    while pos < len(data):
        prefix = data[pos];pos+=1
        assert prefix!=0xFE, 'Unexpected long HID item'
        size = (0,1,2,4)[prefix&3]
        assert pos+size<=len(data)
        value = int.from_bytes(data[pos:pos+size],'little');pos+=size
        kind, tag = (prefix>>2)&3, prefix>>4
        if kind==1:
            if tag==10: stack.append(values.copy())
            elif tag==11: values=stack.pop()
            else: values[tag]=value
            if tag==8: ids.add(value)
        elif kind==0 and tag==8:  # INPUT
            report=values.get(8,0)
            inputs[report]=inputs.get(report,0)+values.get(7,0)*values.get(9,0)
            pages.add(values.get(0,0))
    return inputs,pages,ids

keyboard = descriptor('USBHIDKeyboard')
consumer = descriptor('USBHIDConsumerControl')
assert keyboard[0]=={1:64} and keyboard[2]=={1}, keyboard
assert consumer[0]=={4:16} and consumer[2]=={4} and 12 in consumer[1], consumer
assert keyboard[2].isdisjoint(consumer[2])
symbols = subprocess.check_output([str(bin_dir/'xtensa-esp32s3-elf-nm.exe'),'-C',str(build/'firmware.elf')],text=True)
for symbol in ('USBHIDKeyboard::_onGetDescriptor','USBHIDConsumerControl::_onGetDescriptor','USBCDC::begin'):
    assert symbol in symbols, f'{symbol} missing from final ELF'
print('PASS actual SDK descriptors/ELF: keyboard ID1 8-byte input, Consumer ID4 2-byte input/page 0x0C, distinct IDs; CDC and both HID devices linked')
print('OFFLINE ONLY: this does not validate Windows enumeration, media application behavior or CDC/HID hardware coexistence')
