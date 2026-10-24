"""Host-only tests: synthetic ports/windows, owned workspace shortcut, no CDC open."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import agent
import agent_monitor as monitor

WORK = Path(__file__).resolve().parents[1] / ".pio/agent_startup_test"
WORK.mkdir(parents=True, exist_ok=True)


def port(device="COM4", vid=0x303A, pid=0x0002, serial_number="ENROLLED",
         location="1-2.1:x.1", hwid=None):
    if hwid is None:
        hwid = f"USB VID:PID={vid:04X}:{pid:04X} SER={serial_number}" if vid is not None and pid is not None else "BTHENUM\\SIMULATED"
    return SimpleNamespace(device=device, vid=vid, pid=pid, serial_number=serial_number,
                           location=location, hwid=hwid, interface=None)


def capture_scene_frames():
    """Actual foreground classifier/collector/SerialSender with fake OS/I/O only."""
    state, stop, guard = agent.LatestState(), threading.Event(), threading.Lock()
    frames, closed = {}, []
    scenarios = [("Desktop", "explorer.exe"), ("workspace", "Code.exe"),
                 ("document", "msedge.exe"), ("artist - track", "Spotify.exe")]

    def window():
        with guard:
            title, process = scenarios[min(len(frames), 3)]
        return SimpleNamespace(title=title, process=process)

    class Connection:
        in_waiting = 0

        def write(self, payload):
            with guard:
                frames.setdefault(json.loads(payload)["status"], payload)
                if len(frames) == 4:
                    stop.set()
            return len(payload)

        def close(self):
            closed.append(True)

    worker = agent.SerialSender(state, stop, interval=0.02,
                                port_selector=lambda _: "MOCK_CDC", opener=lambda _: Connection())
    stats = SimpleNamespace(cpu_percent=lambda interval: 20.0,
                            virtual_memory=lambda: SimpleNamespace(percent=50.0))
    with patch.object(agent.gw, "getActiveWindow", side_effect=window), \
         patch.object(agent, "foreground_process_name", side_effect=lambda item: item.process):
        worker.start()
        try:
            agent.collect(state, stop, duration=4, status_reader=agent.foreground_status, stats=stats)
        finally:
            stop.set()
            worker.join(timeout=1)
    assert not worker.is_alive() and closed == [True]
    assert set(frames) == {"normal", "coding", "browser", "media"}, frames
    return [frames[scene] for scene in ("normal", "coding", "browser", "media")]


class StartupTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows PowerShell parser")
    def test_installer_static_syntax(self):
        installer = Path(__file__).with_name("install_agent.ps1")
        with tempfile.TemporaryDirectory(dir=WORK) as temporary:
            probe = Path(temporary) / "syntax.ps1"
            probe.write_text('$tokens=$null; $errors=$null; '
                             '[void][System.Management.Automation.Language.Parser]::ParseFile($args[0],[ref]$tokens,[ref]$errors); '
                             'if($errors.Count){$errors|Out-String|Write-Error; exit 1}; Write-Output "SYNTAX OK"', encoding="utf-8")
            result = subprocess.run(["powershell.exe", "-NoProfile", "-File", str(probe), str(installer)],
                                    check=True, capture_output=True, text=True, timeout=15)
            self.assertIn("SYNTAX OK", result.stdout)

    @unittest.skipUnless(os.name == "nt", "Windows Python candidate selection")
    def test_python_candidates_are_separate_and_dependency_output_is_not_a_path(self):
        installer = Path(__file__).with_name("install_agent.ps1")
        with tempfile.TemporaryDirectory(prefix="python candidate test ", dir=WORK) as temporary:
            folder = Path(temporary)
            # Host-only wrappers: real Python with site-packages disabled must
            # fail imports; a successful interpreter may emit an unrelated banner.
            bad, noisy = folder / "missing deps.cmd", folder / "noisy python.cmd"
            bad.write_text(f'@echo off\n"{sys.executable}" -S %*\n', encoding="utf-8")
            noisy.write_text(f'@echo off\necho BANNER_MUST_NOT_BECOME_A_PATH\n"{sys.executable}" %*\n', encoding="utf-8")
            (folder / "pythonw.exe").write_bytes(b"")  # Existence-only fixture, never executed.
            probe = folder / "candidates.ps1"
            probe.write_text(r'''
param($Installer,$GoodPython,$BadPython,$NoisyPython)
$ErrorActionPreference='Stop'; $WarningPreference='SilentlyContinue'
$tokens=$null; $errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile($Installer,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Invalid installer syntax'}
# Load only helper definitions, never execute the install/USB/Startup body.
foreach($definition in $ast.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst]},$false)) {
    . ([ScriptBlock]::Create($definition.Extent.Text))
}
$defaults=@(Get-PythonCandidates)
$explicit=@(Get-PythonCandidates -ExplicitPython $GoodPython)
$selected=Resolve-AgentPython -Candidates @((Join-Path $PSScriptRoot 'absent/python.exe'),$BadPython,$GoodPython)
$noisy=Resolve-AgentPython -Candidates @($NoisyPython)
@{defaults=$defaults;explicit=$explicit;selected=$selected;selectedType=$selected.GetType().FullName;
  noisy=$noisy;noisyType=$noisy.GetType().FullName}|ConvertTo-Json -Depth 4 -Compress
''', encoding="utf-8")
            result = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(probe),
                                     str(installer), sys.executable, str(bad), str(noisy)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            selected = json.loads(result.stdout)
            self.assertGreaterEqual(len(selected["defaults"]), 2)
            self.assertEqual(Path(selected["defaults"][0]), Path(os.environ["USERPROFILE"])/"anaconda3/python.exe")
            self.assertTrue(all(isinstance(item, str) and "\n" not in item for item in selected["defaults"]))
            self.assertEqual(selected["explicit"], [sys.executable])
            self.assertEqual(Path(selected["selected"]), Path(sys.executable))
            self.assertEqual(selected["selectedType"], "System.String")
            self.assertEqual(Path(selected["noisy"]), noisy)
            self.assertEqual(selected["noisyType"], "System.String")
            self.assertNotIn("BANNER_MUST_NOT_BECOME_A_PATH", result.stdout)

    @unittest.skipUnless(os.name == "nt", "Windows dependency failure guard")
    def test_invalid_python_or_dependencies_cannot_create_startup(self):
        installer = Path(__file__).with_name("install_agent.ps1")
        with tempfile.TemporaryDirectory(prefix="dependency guard test ", dir=WORK) as temporary:
            folder = Path(temporary)
            bad = folder / "missing deps.cmd"
            bad.write_text(f'@echo off\n"{sys.executable}" -S %*\n', encoding="utf-8")
            (folder / "pythonw.exe").write_bytes(b"")  # Never executed.
            for candidate in (folder / "absent/python.exe", bad):
                with self.subTest(candidate=candidate):
                    state, startup = folder / "state", folder / "Startup"
                    result = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                             str(installer), "-Python", str(candidate), "-SerialNumber", "SIMULATED_USB_SERIAL",
                                             "-StateDirectory", str(state), "-StartupDirectory", str(startup), "-SkipStart"],
                                            capture_output=True, text=True, timeout=20)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("No usable Python", result.stdout + result.stderr)
                    self.assertFalse(state.exists())
                    self.assertFalse(startup.exists())

    def test_foreground_process_names_and_title_fallback(self):
        for process, scene in (("Code.exe", "coding"), ("chrome.exe", "browser"),
                               ("Spotify.exe", "media"), ("explorer.exe", "normal")):
            self.assertEqual(agent.classify_window("untitled", process), scene)
        self.assertEqual(agent.classify_window("Visual Studio Code", None), "coding")
        self.assertEqual(agent.classify_window("Spotify - Chrome", "chrome.exe"), "browser")
        if os.name == "nt":
            import ctypes

            def query(hwnd, pointer):
                self.assertEqual(hwnd, 0x123456789AB)  # No 32-bit HWND truncation.
                pointer._obj.value = 99
                return 1

            with patch.object(ctypes.windll.user32, "GetWindowThreadProcessId", side_effect=query), \
                 patch.object(agent.psutil, "Process") as process:
                process.return_value.name.return_value = "Code.exe"
                self.assertEqual(agent.foreground_process_name(SimpleNamespace(_hWnd=0x123456789AB)), "Code.exe")
                process.assert_called_once_with(99)

    def test_native_identity_rejects_bluetooth_rom_uart_and_other_usb(self):
        ports = [port("COM1", None, None, None), port("COM2", 0x10C4, 0xEA60),
                 port("COM3", pid=0x1001, location="1-2:x.0"),  # Non-CDC/JTAG role.
                 port("COM6", pid=0x1001, location=None),  # Unknown/ROM role: cannot attest CDC.
                 port("COM7", 0x239A, 0x0002), port("COM8", pid=0x1002),
                 port("COM9", pid=0x1001, hwid="BTHENUM\\SPOOFED"),
                 port("COM5", pid=0x1001, serial_number="OTHER"),
                 port(pid=0x1001, serial_number="A1B2C3D4E5F6")]
        with patch.object(agent.serial.tools.list_ports, "comports", return_value=ports):
            self.assertEqual(agent.choose_nexus_port("A1B2C3D4E5F6", 0x1001, 1), "COM4")
            self.assertIsNone(agent.choose_nexus_port("MISSING"))
            self.assertEqual(agent.nexus_ports("A1B2C3D4E5F6")[0].interface, None)
        with patch.object(agent.serial.tools.list_ports, "comports", return_value=[port(), port("COM10")]):
            with self.assertRaises(RuntimeError):
                agent.choose_nexus_port("ENROLLED")

    def test_supported_profiles_missing_serial_and_duplicates(self):
        measured = port(pid=0x1001, serial_number="A1B2C3D4E5F6")
        tinyusb = port("COM10", serial_number="A1B2C3D4E5F6", location="3-8:x.0")
        with patch.object(agent.serial.tools.list_ports, "comports", return_value=[measured, tinyusb]):
            self.assertEqual(agent.choose_nexus_port("A1B2C3D4E5F6", 0x1001, 1), "COM4")
            self.assertEqual(agent.choose_nexus_port("A1B2C3D4E5F6", 0x0002, 0), "COM10")
            with self.assertRaises(RuntimeError):
                agent.choose_nexus_port("A1B2C3D4E5F6")
            with self.assertRaises(RuntimeError):
                agent.choose_port()  # Automatic selection uses the same strict profiles.
        for ports in ([], [port(pid=0x1001, serial_number=None)]):
            with patch.object(agent.serial.tools.list_ports, "comports", return_value=ports):
                self.assertIsNone(agent.choose_nexus_port("A1B2C3D4E5F6", 0x1001, 1))
        with patch.object(agent.serial.tools.list_ports, "comports", return_value=[measured, measured]):
            with self.assertRaises(RuntimeError):
                agent.choose_nexus_port("A1B2C3D4E5F6", 0x1001, 1)

    def test_enrollment_persists_measured_pid_interface_and_migrates_old_profile(self):
        measured = port(pid=0x1001, serial_number="A1B2C3D4E5F6")
        with tempfile.TemporaryDirectory(dir=WORK) as temporary:
            folder = Path(temporary)
            # The former installer could save an assumed 0002 even for this serial.
            (folder / "config.json").write_text(json.dumps({"owner": monitor.OWNER, "usb_serial": measured.serial_number,
                                                          "vid": 0x303A, "pid": 0x0002}))
            with patch.object(agent.serial.tools.list_ports, "comports", return_value=[measured]), \
                 patch.object(agent.serial, "Serial") as opened:
                config = monitor.configure(folder, measured.serial_number)
                opened.assert_not_called()
            self.assertEqual((config["vid"], config["pid"], config["usb_interface"], config["usb_serial"]),
                             (0x303A, 0x1001, 1, "A1B2C3D4E5F6"))
            before = (folder / "config.json").read_bytes()
            with patch.object(agent.serial.tools.list_ports, "comports", return_value=[]):
                self.assertEqual(monitor.configure(folder)["pid"], 0x1001)  # Unplugged reinstall retains identity.
                before = (folder / "config.json").read_bytes()
                with self.assertRaises(ValueError):
                    monitor.configure(folder, "OTHER_SERIAL")
            self.assertEqual((folder / "config.json").read_bytes(), before)
            with patch.object(agent.serial.tools.list_ports, "comports", return_value=[measured, measured]):
                with self.assertRaises(ValueError):
                    monitor.configure(folder)
            self.assertEqual((folder / "config.json").read_bytes(), before)
        with tempfile.TemporaryDirectory(dir=WORK) as temporary, \
             patch.object(agent.serial.tools.list_ports, "comports", return_value=[]):
            with self.assertRaises(ValueError):
                monitor.configure(temporary, "A1B2C3D4E5F6")  # Explicit serial is not an identity bypass.
            self.assertFalse((Path(temporary) / "config.json").exists())

    def test_enrollment_does_not_open_ports_or_overwrite_foreign_config(self):
        with tempfile.TemporaryDirectory(dir=WORK) as temporary:
            folder = Path(temporary)
            with patch.object(agent, "nexus_ports", return_value=[port()]), patch.object(agent.serial, "Serial") as opened:
                self.assertEqual(monitor.configure(folder)["usb_serial"], "ENROLLED")
                opened.assert_not_called()
            self.assertEqual(monitor.load_config(folder)["vid"], 0x303A)
            self.assertEqual(monitor.configure(folder)["usb_serial"], "ENROLLED")
            (folder / "config.json").write_text('{"owner":"someone else"}')
            with self.assertRaises(ValueError):
                monitor.configure(folder, "NEW")
            self.assertEqual(json.loads((folder / "config.json").read_text())["owner"], "someone else")
        with tempfile.TemporaryDirectory(dir=WORK) as temporary, \
             patch.object(agent, "nexus_ports", return_value=[]):
            with self.assertRaises(ValueError):
                monitor.configure(temporary)

    @unittest.skipUnless(os.name == "nt", "Windows byte locking")
    def test_single_instance_lock_released_after_close(self):
        with tempfile.TemporaryDirectory(dir=WORK) as temporary:
            first, second = [monitor.InstanceLock(Path(temporary) / "monitor.lock") for _ in range(2)]
            try:
                self.assertTrue(first.acquire())
                self.assertFalse(second.acquire())
                first.close()
                self.assertTrue(second.acquire())
                second.close()
                self.assertTrue(monitor.request_stop(temporary, timeout=0.5))
            finally:
                first.close()
                second.close()

    def test_supervisor_one_child_waits_restarts_and_stops(self):
        child = SimpleNamespace(poll=lambda: None, terminate=lambda: None, wait=lambda timeout: 0)
        with patch.object(agent, "choose_nexus_port", return_value=None) as select, \
             patch.object(subprocess, "Popen", return_value=child) as spawn:
            supervisor = monitor.Supervisor(lambda: select("ENROLLED"), spawn)
            supervisor.tick(0)
            spawn.assert_not_called()
            select.return_value = "COM4"
            supervisor.tick(1)
            supervisor.tick(2)
            self.assertEqual(spawn.call_count, 1)
            child.poll = lambda: 7
            supervisor.tick(3)
            select.return_value = "COM10"
            supervisor.tick(7.9)
            self.assertEqual(spawn.call_count, 1)
            child.poll = lambda: None
            supervisor.tick(8)
            self.assertEqual(spawn.call_count, 2)
            with patch.object(child, "terminate") as terminate:
                supervisor.close()
                terminate.assert_called_once()

    @unittest.skipUnless(os.name == "nt", "Windows monitor lock")
    def test_monitor_passes_the_bound_usb_profile_to_agent(self):
        with tempfile.TemporaryDirectory(dir=WORK) as temporary:
            folder = Path(temporary)
            (folder / "config.json").write_text(json.dumps({"owner": monitor.OWNER, "usb_serial": "A1B2C3D4E5F6",
                                                          "vid": 0x303A, "pid": 0x1001, "usb_interface": 1}))
            child = SimpleNamespace(poll=lambda: None, terminate=lambda: None, wait=lambda timeout: 0)
            handlers = list(monitor.LOG.handlers)
            try:
                with patch.object(agent, "choose_nexus_port", return_value="COM10") as select, \
                     patch.object(subprocess, "Popen", return_value=child) as spawn, \
                     patch.object(monitor.time, "sleep", side_effect=lambda _: (folder / "stop.request").write_text("stop")), \
                     patch.object(agent.serial, "Serial") as opened:
                    self.assertEqual(monitor.run(folder), 0)
                    select.assert_called_once_with("A1B2C3D4E5F6", 0x1001, 1)
                    command = spawn.call_args.args[0]
                    self.assertEqual(command[2:], ["--usb-serial", "A1B2C3D4E5F6", "--usb-pid", "0x1001", "--usb-interface", "1"])
                    opened.assert_not_called()
            finally:
                for handler in list(monitor.LOG.handlers):
                    if handler not in handlers:
                        handler.close()
                        monitor.LOG.removeHandler(handler)

    def test_real_sender_reselects_enrolled_com_after_unplug(self):
        state, stop = agent.LatestState(), threading.Event()
        state.update(20, 50, "coding")
        writes, opened, closed = [], [], []

        class Connection:
            in_waiting = 0

            def __init__(self, name):
                self.name = name

            def write(self, payload):
                if self.name == "COM4":
                    state.update(25, 51, "browser")
                    raise agent.serial.SerialException("simulated unplug")
                writes.append(payload)
                stop.set()
                return len(payload)

            def close(self):
                closed.append(self.name)

        def open_port(name):
            opened.append(name)
            return Connection(name)

        with patch.object(agent.serial.tools.list_ports, "comports", side_effect=[
                [port(pid=0x1001, serial_number="A1B2C3D4E5F6")],
                [port("COM10", pid=0x1001, serial_number="A1B2C3D4E5F6", location="4-9:x.1")]]):
            agent.SerialSender(state, stop, port_selector=lambda _: agent.choose_nexus_port("A1B2C3D4E5F6", 0x1001, 1),
                               opener=open_port, retry_interval=0).run()
        self.assertEqual(opened, ["COM4", "COM10"])
        self.assertEqual(closed, ["COM4", "COM10"])
        self.assertEqual(json.loads(writes[0])["status"], "browser")
        self.assertTrue(writes[0].startswith(b"\n") and writes[0].endswith(b"\n"))

    def test_main_uses_serial_bound_selector(self):
        with patch.object(agent, "SerialSender") as sender, \
             patch.object(agent, "choose_nexus_port", return_value="COM10") as select, \
             patch.object(agent, "collect", side_effect=KeyboardInterrupt):
            self.assertEqual(agent.main(["--usb-serial", "ENROLLED", "--usb-pid", "0x1001", "--usb-interface", "1", "--no-temperature"]), 0)
            self.assertEqual(sender.call_args.kwargs["port_selector"](None), "COM10")
            select.assert_called_once_with("ENROLLED", 0x1001, 1)
            sender.return_value.join.assert_called_once_with(timeout=0.75)

    def test_windows_to_collector_to_actual_serial_writes_four_scenes(self):
        for payload, scene in zip(capture_scene_frames(), ("normal", "coding", "browser", "media")):
            self.assertEqual(json.loads(payload), {"cpu": 20.0, "mem": 50.0, "cpu_temp": None, "status": scene})
            self.assertTrue(payload.endswith(b"\n"))

    @unittest.skipUnless(os.name == "nt", "Windows shortcut component test")
    def test_install_uninstall_in_workspace_only_no_monitor_started(self):
        scripts = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix="launcher test ", dir=WORK) as temporary:
            state, startup = Path(temporary) / "state", Path(temporary) / "Startup test"
            # Child Python processes run the real installer/monitor with synthetic
            # enumeration only. Opening a serial handle is forbidden by this fixture.
            (Path(temporary) / "sitecustomize.py").write_text('''
from types import SimpleNamespace
import serial, serial.tools.list_ports
serial.tools.list_ports.comports = lambda: [SimpleNamespace(device="COM4",vid=0x303A,pid=0x1001,
    serial_number="SIMULATED_USB_SERIAL",location="1-2.1:x.1",hwid="USB VID:PID=303A:1001",interface=None)]
def no_open(*args,**kwargs):
    raise AssertionError("Installer tests must never open a serial port")
serial.Serial = no_open
''', encoding="utf-8")
            test_env = dict(os.environ, PYTHONPATH=temporary + os.pathsep + os.environ.get("PYTHONPATH", ""))
            common = ["-Python", sys.executable, "-StateDirectory", str(state), "-StartupDirectory", str(startup)]
            installer = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(scripts / "install_agent.ps1"),
                         *common, "-SerialNumber", "SIMULATED_USB_SERIAL", "-SkipStart"]
            for _ in range(2):
                subprocess.run(installer, env=test_env, check=True, capture_output=True, text=True, timeout=30)
            self.assertEqual(len(list(startup.glob("*.lnk"))), 1)
            config = json.loads((state / "config.json").read_text())
            self.assertEqual((config["pid"], config["usb_interface"]), (0x1001, 1))
            probe = Path(temporary) / "probe.ps1"
            probe.write_text('$s=(New-Object -ComObject WScript.Shell).CreateShortcut($args[0]); '
                             '@{target=$s.TargetPath;arguments=$s.Arguments;owner=$s.Description}|ConvertTo-Json -Compress', encoding="utf-8")
            data = subprocess.check_output(["powershell.exe", "-NoProfile", "-File", str(probe), str(startup / "NexusS1Agent.lnk")], text=True)
            shortcut = json.loads(data)
            self.assertEqual(Path(shortcut["target"]), Path(sys.executable).with_name("pythonw.exe"))
            self.assertIn('"' + str(scripts / "agent_monitor.py") + '"', shortcut["arguments"])
            self.assertIn('"' + str(state) + '"', shortcut["arguments"])
            self.assertEqual(shortcut["owner"], monitor.OWNER)
            self.assertFalse((state / "monitor.lock").exists())  # SkipStart did not launch anything.
            subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            str(scripts / "uninstall_agent.ps1"), *common], env=test_env, check=True, capture_output=True, text=True, timeout=30)
            self.assertFalse((startup / "NexusS1Agent.lnk").exists())
            self.assertTrue((state / "config.json").exists())


if __name__ == "__main__":
    unittest.main()
