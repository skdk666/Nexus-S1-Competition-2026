"""Opt-in current-user Nexus monitor. No port is opened by this supervisor.

The existing Agent remains the sole CDC writer. Installed by install_agent.ps1;
this file never edits Startup, services, registry or scheduled tasks itself.
"""
import argparse
import json
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import subprocess
import sys
import time

import agent

OWNER = "NexusS1AgentMonitor/v1"
LOG = logging.getLogger("nexus_monitor")


class InstanceLock:
    """OS-released byte lock; a stale lock file does not block the next start."""
    def __init__(self, path):
        self.path, self.file = Path(path), None

    def acquire(self):
        import msvcrt
        self.path.parent.mkdir(parents=True, exist_ok=True)
        handle = self.path.open("a+b")
        handle.seek(0, 2)
        if handle.tell() == 0:
            handle.write(b"0")
            handle.flush()
        handle.seek(0)
        try:
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            handle.close()
            return False
        self.file = handle
        return True

    def close(self):
        if self.file is not None:
            import msvcrt
            self.file.seek(0)
            msvcrt.locking(self.file.fileno(), msvcrt.LK_UNLCK, 1)
            self.file.close()
            self.file = None


def load_config(directory):
    config = json.loads((Path(directory) / "config.json").read_text(encoding="utf-8"))
    if (config.get("owner") != OWNER or not isinstance(config.get("usb_serial"), str)
            or not config["usb_serial"].strip()):
        raise ValueError("Unknown/invalid monitor configuration; refusing to use it")
    if (config.get("vid") != agent.NEXUS_USB_VID or config.get("pid") not in agent.NEXUS_USB_PROFILES
            or (config.get("usb_interface") is not None and
                (type(config["usb_interface"]) is not int or not 0 <= config["usb_interface"] <= 255))
            or (config.get("pid") == 0x1001 and config.get("usb_interface") != 1)):
        raise ValueError("Unsupported USB identity/interface; rerun enrollment with the device attached")
    return config


def configure(directory, serial_number=None):
    directory = Path(directory)
    previous = None
    if (directory / "config.json").exists():
        previous = load_config(directory)  # Never overwrite an unrelated file.
        if serial_number is None:
            serial_number = previous["usb_serial"]
    ports = agent.nexus_ports(serial_number)
    if len(ports) > 1:
        raise ValueError("Multiple matching CDC interfaces; refusing ambiguous enrollment")
    if ports:
        serial_number = ports[0].serial_number
        pid, interface = ports[0].pid, agent.usb_interface_number(ports[0])
    elif previous and previous["usb_serial"] == serial_number:
        # Reinstall while an already enrolled device is unplugged; keep its identity.
        pid, interface = previous["pid"], previous.get("usb_interface")
    else:
        raise ValueError("No matching CDC: connect 303A:0002 or 303A:1001 CDC interface 1 "
                         "with a stable USB serial; -SerialNumber selects, not bypasses identity checks. No port was opened")
    config = {"owner": OWNER, "usb_serial": serial_number,
              "vid": agent.NEXUS_USB_VID, "pid": pid, "usb_interface": interface,
              "agent": str(Path(agent.__file__).resolve()), "python": sys.executable}
    directory.mkdir(parents=True, exist_ok=True)
    pending = directory / "config.json.new"
    pending.write_text(json.dumps(config, indent=2), encoding="utf-8")
    pending.replace(directory / "config.json")
    return config


class Supervisor:
    def __init__(self, selector, spawn):
        self.selector, self.spawn = selector, spawn
        self.child, self.next_start = None, 0.0

    def tick(self, now):
        if self.child is not None:
            result = self.child.poll()
            if result is None:
                return  # Its SerialSender handles unplug/replug and COM changes.
            LOG.warning("Agent exited (%s); retry in 5s", result)
            self.child, self.next_start = None, now + 5.0
        if now < self.next_start:
            return
        try:
            port = self.selector()
            if port is not None:
                self.child = self.spawn()
                LOG.info("Agent launched for enrolled CDC at %s", port)
        except (OSError, RuntimeError) as error:
            LOG.warning("Waiting for enrolled CDC: %s", error)
            self.next_start = now + 5.0

    def close(self):
        if self.child is not None and self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait(timeout=3)
        self.child = None


def request_stop(directory, timeout=8):
    directory = Path(directory)
    if not (directory / "monitor.lock").exists():
        return True
    (directory / "stop.request").write_text("stop\n", encoding="ascii")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        probe = InstanceLock(directory / "monitor.lock")
        if probe.acquire():
            probe.close()
            return True
        time.sleep(0.1)
    return False


def run(directory):
    directory = Path(directory)
    config = load_config(directory)
    lock = InstanceLock(directory / "monitor.lock")
    if not lock.acquire():
        return 0  # Already running; do not start another Agent.
    supervisor = None
    try:
        (directory / "stop.request").unlink(missing_ok=True)  # Our own control flag only.
        handler = RotatingFileHandler(directory / "monitor.log", maxBytes=262144,
                                      backupCount=2, encoding="utf-8")
        LOG.addHandler(handler)
        LOG.setLevel(logging.INFO)
        handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))

        def spawn():
            output = directory / "agent.log"
            if output.exists() and output.stat().st_size > 1048576:
                output.replace(directory / "agent.previous.log")  # Previous child has exited.
            with output.open("a", encoding="utf-8") as stream:
                console_python = str(Path(sys.executable).with_name("python.exe"))
                command = [console_python, str(Path(agent.__file__).resolve()),
                           "--usb-serial", config["usb_serial"], "--usb-pid", hex(config["pid"])]
                if config.get("usb_interface") is not None:
                    command += ["--usb-interface", str(config["usb_interface"])]
                return subprocess.Popen(command,
                                        stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                                        cwd=Path(agent.__file__).resolve().parent,
                                        creationflags=subprocess.CREATE_NO_WINDOW)

        supervisor = Supervisor(lambda: agent.choose_nexus_port(config["usb_serial"], config["pid"],
                                                               config.get("usb_interface")), spawn)
        LOG.info("Monitor started; waiting for enrolled native CDC, not a fixed COM number")
        while not (directory / "stop.request").exists():
            supervisor.tick(time.monotonic())
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        if supervisor is not None:
            supervisor.close()
        lock.close()
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path,
                        default=Path(os.environ.get("LOCALAPPDATA", Path.home())) / "NexusS1Agent")
    operation = parser.add_mutually_exclusive_group()
    operation.add_argument("--configure", action="store_true")
    operation.add_argument("--stop", action="store_true")
    parser.add_argument("--serial", help="Previously verified native USB serial, for enrollment only")
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("This opt-in launcher is Windows-only")
    if args.configure:
        configure(args.state_dir, args.serial)
        print("Configured current-user native CDC enrollment; no serial port opened.")
        return 0
    if args.stop:
        return 0 if request_stop(args.state_dir) else 1
    return run(args.state_dir)


if __name__ == "__main__":
    raise SystemExit(main())
