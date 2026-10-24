"""Compile/run production XPT2046 logic and wire tests; no ports, hardware or new packages."""
from pathlib import Path
import os
import shutil
import subprocess
import re

project = Path(__file__).resolve().parents[1]
work = project / ".pio" / "xpt2046_test"
work.mkdir(parents=True, exist_ok=True)
override = os.environ.get("NEXUS_HOST_CXX_DIR")
compiler = str(Path(override) / "g++.exe") if override else shutil.which("g++")
if not compiler:
    raise SystemExit("Set NEXUS_HOST_CXX_DIR or put MinGW g++ on PATH")
env = os.environ.copy()
env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
exe = work / "xpt2046_test.exe"
for experiment in range(3):
    subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                f"-DNEXUS_TOUCH_RAW_EXPERIMENT={experiment}",
                "-I" + str(project / "src"), str(project / "test/test_xpt2046.cpp"),
                str(project / "src/xpt2046.cpp"), "-o", str(exe)], env=env, check=True)
    subprocess.run([str(exe)], env=env, cwd=work, check=True, timeout=30)

invalid = subprocess.run([compiler, "-std=c++11", "-fsyntax-only", "-DNEXUS_TOUCH_RAW_EXPERIMENT=3",
                          "-I"+str(project / "src"), str(project / "test/test_xpt2046.cpp")],
                         env=env, capture_output=True, text=True)
assert invalid.returncode != 0 and "XPT RAW experiment must be" in invalid.stderr
print("PASS invalid experiment rejected at compile time", flush=True)

# Compile the actual ESP32 adapter against small SDK/TFT spies. This exercises
# the temporary board configuration without accessing any physical GPIO/USB.
lvgl = project / ".pio/libdeps/esp32-s3-devkitc-1/lvgl"
adapter = work / "touch_esp32_test.exe"
packages = Path(os.environ.get("PLATFORMIO_PACKAGES_DIR",
    Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home()/".platformio"))/"packages"))
sdk = packages / "framework-arduinoespressif32/tools/sdk/esp32s3/qio_qspi/include/sdkconfig.h"
fifo_match = re.search(r"^#define CONFIG_TINYUSB_CDC_TX_BUFSIZE\s+(\d+)\s*$",
                       sdk.read_text(encoding="utf-8"), re.MULTILINE)
assert fifo_match, "CDC TX FIFO capacity missing from the actual qio_qspi SDK"
fifo_size = int(fifo_match[1])
print(f"Actual ESP32-S3 qio_qspi SDK CDC TX FIFO: {fifo_size} bytes", flush=True)
for experiment in range(3):
    subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                f"-DNEXUS_TOUCH_RAW_EXPERIMENT={experiment}",
                f"-DNEXUS_TEST_CDC_FIFO_SIZE={fifo_size}",
                "-DLV_CONF_INCLUDE_SIMPLE", "-I" + str(project / "test/xpt2046_host"),
                "-I" + str(project / "src"), "-I" + str(project / "include"), "-isystem", str(lvgl),
                str(project / "test/test_touch_esp32.cpp"), str(project / "src/touch_esp32.cpp"),
                str(project / "src/xpt2046.cpp"), "-o", str(adapter)], env=env, check=True)
    subprocess.run([str(adapter)], env=env, cwd=work, check=True, timeout=30)
    print(f"PASS experiment {'ABC'[experiment]}: configured SPI frequency, exact CFG/30-or-63 TX/RX, RAW cannot click", flush=True)

from calibrate_xpt2046 import fit_calibration, target_points
for rotation in range(4):
    for swap in (False, True):
        for invert_x in (False, True):
            for invert_y in (False, True):
                raw = []
                for x, y in target_points(rotation, 20):
                    u, v = [(x, y), (y, 319-x), (239-x, 319-y), (239-y, x)][rotation]
                    a, b = round(100+3800*u/239), round(100+3800*v/319)
                    if invert_x: a = 4000-a
                    if invert_y: b = 4000-b
                    raw.append((b, a) if swap else (a, b))
                cal = fit_calibration(raw, rotation, 20)
                assert cal["swap_xy"] == swap
                for key, expected in {"x0": 3900 if invert_x else 100, "x1": 100 if invert_x else 3900,
                                      "y0": 3900 if invert_y else 100, "y1": 100 if invert_y else 3900}.items():
                    assert abs(cal[key]-expected) <= 1, (raw, cal)
for invalid in ([(0,0)]*4, [(5000,0)]*4, [(0,0)]*3,
                [(100,3900),(100,100),(3900,100),(3300,3900)]):
    try:
        fit_calibration(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid calibration accepted: "+str(invalid))
print("PASS calibration calculator: 32 rotation/swap/inversion cases and degenerate/range/noise rejection; synthetic data only")
