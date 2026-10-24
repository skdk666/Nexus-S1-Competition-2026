"""USB-first Nexus Agent. Old data.py/text_context.py remain reference scripts."""

import argparse
import json
import logging
import math
import os
import re
import threading
import time

import psutil
import pygetwindow as gw
import requests
import serial
import serial.tools.list_ports


LOG = logging.getLogger("nexus_agent")
APP_RULES = {
    "coding": ("Visual Studio Code", "PyCharm", "Keil"),
    "browser": ("Edge", "Chrome", "Firefox"),
    "media": ("Bilibili", "网易云音乐", "Spotify"),
}
TEMP_NAMES = {"Core (Tctl/Tdie)", "CPU Package", "Core Max", "CCDs Max (Tdie)", "CPU Core"}
# Keep the Arduino TinyUSB profile and the user-measured S3 CDC interface.
# PID 1001 alone cannot identify the application or distinguish ROM mode.
NEXUS_USB_VID = 0x303A
NEXUS_USB_PROFILES = {0x0002: None, 0x1001: 1}
PROCESS_RULES = {
    "coding": {"code.exe", "code - insiders.exe", "pycharm.exe", "pycharm64.exe", "uv4.exe"},
    "browser": {"chrome.exe", "msedge.exe", "firefox.exe"},
    "media": {"spotify.exe", "cloudmusic.exe", "vlc.exe", "bilibili.exe"},
}


def classify_window(title, process_name=None):
    process = process_name.casefold() if isinstance(process_name, str) else ""
    for status, names in PROCESS_RULES.items():
        if process in names:
            return status
    title = title.casefold() if isinstance(title, str) else ""
    for status, names in APP_RULES.items():
        if any(name.casefold() in title for name in names):
            return status
    return "normal"


def foreground_process_name(window):
    """Resolve the same PyGetWindow HWND; process names survive changing titles.

    A media foreground executable is a scene hint, not proof of music playback.
    """
    hwnd = getattr(window, "_hWnd", None)
    if os.name != "nt" or not hwnd:
        return None
    import ctypes
    from ctypes import wintypes
    try:
        query = ctypes.windll.user32.GetWindowThreadProcessId
        query.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        query.restype = wintypes.DWORD
        pid = wintypes.DWORD()
        if query(hwnd, ctypes.byref(pid)) and pid.value:
            return psutil.Process(pid.value).name()
    except (OSError, psutil.Error):
        pass  # A closing/inaccessible process falls back to existing title rules.
    return None


def foreground_status():
    try:
        window = gw.getActiveWindow()
        return classify_window(window.title if window else None, foreground_process_name(window))
    except Exception:
        LOG.debug("Foreground window unavailable", exc_info=True)
        return "normal"


def temperature_value(value):
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, str):
        value = value.replace("°C", "").replace(",", ".").strip()
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        return None
    return number if math.isfinite(number) else None


def find_temperature(node):
    if not isinstance(node, dict):
        return None
    text = node.get("Text")
    if isinstance(text, str) and text in TEMP_NAMES:
        value = temperature_value(node.get("Value"))
        if value is not None:
            return value
    children = node.get("Children", [])
    if isinstance(children, list):
        for child in children:
            value = find_temperature(child)
            if value is not None:
                return value
    return None


class LatestState:
    """Latest snapshot only: reconnect never drains an old packet backlog."""

    def __init__(self):
        self.lock = threading.Lock()
        self.telemetry = None
        self.cpu_temp = None

    def update(self, cpu, mem, status):
        with self.lock:
            self.telemetry = {"cpu": cpu, "mem": mem, "status": status}

    def set_temperature(self, value):
        with self.lock:
            self.cpu_temp = temperature_value(value)

    def snapshot(self):
        with self.lock:
            if self.telemetry is None:
                return None
            return {"cpu": self.telemetry["cpu"], "mem": self.telemetry["mem"],
                    "cpu_temp": self.cpu_temp, "status": self.telemetry["status"]}


def encode_frame(snapshot):
    return (json.dumps(snapshot, allow_nan=False, separators=(",", ":")) + "\n").encode("utf-8")


def choose_port(explicit_port=None):
    if explicit_port:
        return explicit_port
    candidates = [port.device for port in nexus_ports()]
    if len(candidates) > 1:
        raise RuntimeError("Multiple candidate ports; select the business CDC using --port")
    return candidates[0] if candidates else None


def usb_interface_number(port):
    # pyserial 3.5 Windows leaves .interface unset, but appends the parsed MI
    # number to .location as ':x.N'. Match only that suffix, not the hub path.
    match = re.search(r":x\.(\d+)$", getattr(port, "location", None) or "")
    return int(match[1]) if match else None


def is_nexus_port(port):
    if (getattr(port, "vid", None) != NEXUS_USB_VID
            or getattr(port, "pid", None) not in NEXUS_USB_PROFILES
            or not getattr(port, "serial_number", None)
            or not (getattr(port, "hwid", None) or "").upper().startswith("USB ")
            or not re.fullmatch(r"COM\d+", getattr(port, "device", ""), re.I)):
        return False
    required_interface = NEXUS_USB_PROFILES[port.pid]
    return required_interface is None or usb_interface_number(port) == required_interface


def nexus_ports(serial_number=None, pid=None, interface=None):
    """Enumerate only; supported VID/PID/CDC role plus enrolled serial/profile."""
    return [port for port in serial.tools.list_ports.comports()
            if is_nexus_port(port)
            and (serial_number is None or port.serial_number == serial_number)
            and (pid is None or port.pid == pid)
            and (interface is None or usb_interface_number(port) == interface)]


def choose_nexus_port(serial_number, pid=None, interface=None):
    candidates = nexus_ports(serial_number, pid, interface)
    if len(candidates) > 1:
        raise RuntimeError("Enrolled USB serial has multiple CDC ports; refusing ambiguous selection")
    return candidates[0].device if candidates else None


def open_cdc(port):
    connection = serial.Serial(port=None, baudrate=115200, timeout=0, write_timeout=0.2)
    connection.dtr = True  # Native CDC uses DTR; do not request an RTS reset.
    connection.rts = False
    connection.port = port
    try:
        connection.open()
    except Exception:
        connection.close()
        raise
    return connection


class SerialSender(threading.Thread):
    """Single owner of the CDC handle; port waits never run in the window loop."""

    def __init__(self, state, stop, interval=1.0, port=None,
                 port_selector=choose_port, opener=open_cdc, retry_interval=1.0):
        super().__init__(name="cdc_sender", daemon=True)
        self.state, self.stop = state, stop
        self.interval, self.port = interval, port
        self.port_selector, self.opener = port_selector, opener
        self.retry_interval = retry_interval

    def run(self):
        connection = None
        needs_separator = False
        try:
            while not self.stop.is_set():
                try:
                    if connection is None:
                        port = self.port_selector(self.port)
                        if port is None:
                            self.stop.wait(self.retry_interval)
                            continue
                        connection = self.opener(port)
                        LOG.info("CDC connected: %s", port)
                    snapshot = self.state.snapshot()
                    if snapshot is not None:
                        # End a potentially truncated line before the first recovered frame.
                        payload = (b"\n" if needs_separator else b"") + encode_frame(snapshot)
                        if connection.write(payload) != len(payload):
                            raise serial.SerialTimeoutException("Incomplete CDC write")
                        needs_separator = False
                        LOG.debug("CDC TX (%d bytes): %s", len(payload), payload.decode("utf-8").strip())
                    # Drain bounded debug bytes without waiting for a newline.
                    waiting = connection.in_waiting
                    if waiting:
                        data = connection.read(min(waiting, 512))
                        LOG.debug("CDC RX: %s", data.decode("utf-8", errors="replace").rstrip())
                    self.stop.wait(self.interval)
                except (serial.SerialException, OSError, RuntimeError) as error:
                    LOG.warning("CDC unavailable: %s; retrying", error)
                    if connection is not None:
                        try:
                            connection.close()
                        except (serial.SerialException, OSError):
                            pass
                    connection = None
                    needs_separator = True
                    self.stop.wait(self.retry_interval)
        finally:
            if connection is not None:
                try:
                    connection.close()
                except (serial.SerialException, OSError):
                    pass


class TemperatureSampler(threading.Thread):
    def __init__(self, state, stop, url="http://127.0.0.1:8085/data.json"):
        super().__init__(name="lhm_sampler", daemon=True)
        self.state, self.stop, self.url = state, stop, url

    def run(self):
        failures, next_report = 0, 0.0
        with requests.Session() as session:
            session.trust_env = False  # Local temperature service must not depend on a proxy.
            while not self.stop.is_set():
                value = None
                try:
                    with session.get(self.url, timeout=(0.3, 0.5)) as response:
                        response.raise_for_status()
                        value = find_temperature(response.json())
                    if failures:
                        LOG.info("LHM request recovered after %d failed polls; cpu_temp=%s", failures, value)
                    failures, next_report = 0, 0.0
                except (requests.RequestException, ValueError, RecursionError) as error:
                    failures += 1
                    now = time.monotonic()
                    if now >= next_report:
                        LOG.warning("LHM unavailable (%s: %s); cpu_temp=null; consecutive failures=%d; "
                                    "retrying (log at most every 30s)", type(error).__name__, error, failures)
                        next_report = now + 30.0
                self.state.set_temperature(value)
                self.stop.wait(1.0)


def collect(state, stop, interval=1.0, dry_run=False, duration=None,
            status_reader=foreground_status, stats=psutil):
    stats.cpu_percent(interval=None)  # Prime the nonblocking sampler; do not send its first result.
    start = time.monotonic()
    next_sample = start + 1.0
    next_output = next_sample
    cpu = mem = None
    while not stop.is_set():
        now = time.monotonic()
        if duration is not None and now - start >= duration:
            break
        status = status_reader()
        if now >= next_sample:
            cpu = stats.cpu_percent(interval=None)
            mem = stats.virtual_memory().percent
            next_sample = now + 1.0
        if cpu is not None:
            state.update(cpu, mem, status)
            if dry_run and now >= next_output:
                print(encode_frame(state.snapshot()).decode("utf-8"), end="", flush=True)
                next_output = now + interval
        stop.wait(0.1)  # Window classification continues even with unchanged titles or no CDC.


def positive_seconds(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("Must be a finite positive number")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description="Nexus USB-first telemetry/context Agent (Windows)")
    parser.add_argument("--port", help="Business CDC COM port; auto-detect only if unambiguous")
    parser.add_argument("--usb-serial", help="Enrolled native USB serial; tracks COM changes (background monitor)")
    parser.add_argument("--usb-pid", type=lambda value: int(value, 0), choices=tuple(NEXUS_USB_PROFILES),
                        help="Enrolled PID, e.g. 0x1001; requires --usb-serial")
    parser.add_argument("--usb-interface", type=int, help="Enrolled CDC interface number; requires --usb-serial")
    parser.add_argument("--interval", type=positive_seconds, default=1.0, help="Frame interval in seconds")
    parser.add_argument("--duration", type=positive_seconds, help="Exit after this many seconds")
    parser.add_argument("--dry-run", action="store_true", help="Print real snapshots; do not open a serial port")
    parser.add_argument("--no-temperature", action="store_true", help="Skip LHM; cpu_temp is null")
    parser.add_argument("--list-ports", action="store_true", help="List ports without opening them")
    parser.add_argument("--verbose", action="store_true", help="Include CDC TX frames and bounded debug reads")
    args = parser.parse_args(argv)
    if args.port and args.usb_serial:
        parser.error("Use either --port or --usb-serial")
    if (args.usb_pid is not None or args.usb_interface is not None) and not args.usb_serial:
        parser.error("USB profile filters require --usb-serial")
    if args.usb_interface is not None and not 0 <= args.usb_interface <= 255:
        parser.error("USB interface must be 0..255")
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    # Verbose Agent diagnostics must not enable urllib3's per-request HTTP noise.
    LOG.setLevel(logging.DEBUG if args.verbose else logging.INFO)
    if args.list_ports:
        for port in serial.tools.list_ports.comports():
            print(f"{port.device}: {port.description} VID={port.vid} PID={port.pid} "
                  f"SER={port.serial_number} IF={usb_interface_number(port)}")
        return 0
    stop, state = threading.Event(), LatestState()
    workers = []
    if not args.no_temperature:
        workers.append(TemperatureSampler(state, stop))
    if not args.dry_run:
        options = {"port_selector": lambda _: choose_nexus_port(args.usb_serial, args.usb_pid, args.usb_interface)} if args.usb_serial else {}
        workers.append(SerialSender(state, stop, interval=args.interval, port=args.port, **options))
    for worker in workers:
        worker.start()
    try:
        collect(state, stop, args.interval, args.dry_run, args.duration)
    except KeyboardInterrupt:
        LOG.info("Agent stopped")
    finally:
        stop.set()
        for worker in workers:
            worker.join(timeout=0.75)  # Never wait indefinitely for a Windows driver/service.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
