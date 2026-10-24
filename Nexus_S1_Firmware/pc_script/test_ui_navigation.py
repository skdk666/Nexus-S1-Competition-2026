"""Compile and run the production navigation state machine without hardware."""
from pathlib import Path
import os
import shutil
import subprocess

project = Path(__file__).resolve().parents[1]
work = project / '.pio/navigation_test'
work.mkdir(parents=True, exist_ok=True)
override = os.environ.get('NEXUS_HOST_CXX_DIR')
compiler = str(Path(override)/'g++.exe') if override else shutil.which('g++')
if not compiler:
    raise SystemExit('Set NEXUS_HOST_CXX_DIR or put MinGW g++ on PATH')
env = os.environ.copy()
env['PATH'] = str(Path(compiler).parent)+os.pathsep+env.get('PATH','')
exe = work/'navigation_test.exe'
subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror','-pedantic',
                '-I'+str(project/'src'),
                str(project/'test/test_ui_navigation.cpp'),'-o',str(exe)],env=env,check=True)
subprocess.run([str(exe)],env=env,check=True,timeout=10)
