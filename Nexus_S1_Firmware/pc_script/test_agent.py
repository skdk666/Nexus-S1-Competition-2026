"""Software checks only. No serial device, MQTT, or LHM server is contacted."""

import json
import contextlib
import io
import threading
import time
import unittest
from unittest.mock import patch
from types import SimpleNamespace

import agent


def capture_timeout_frames():
    """Run real collector/sender workers against failing HTTP and a recording port.

    The returned bytes are actual SerialSender.write() arguments, not frames
    synthesized by a test. No Windows serial port or HTTP server is contacted.
    """
    state, stop = agent.LatestState(), threading.Event()
    state.set_temperature(60)
    modes = ("normal", "coding", "browser", "media")
    frames, seen, failures, windows, closed = [], set(), [], [], []
    guard = threading.Lock()
    cpu_samples = iter((0, 20.0, 80.0, 30.0, 40.0))

    def status_reader():
        with guard:
            windows.append(time.monotonic())
            return modes[min(len(seen), 3)]

    def unavailable(*args, **kwargs):
        if kwargs.get("timeout") != (0.3, 0.5):
            raise AssertionError("HTTP timeouts changed")
        stop.wait(0.15)  # A pending temperature request must not stall collection/TX.
        with guard:
            failures.append(time.monotonic())
        raise agent.requests.ReadTimeout("simulated LHM timeout")

    class Connection:
        in_waiting = 0

        def write(self, frame):
            decoded = json.loads(frame)
            with guard:
                frames.append(frame)
                seen.add(decoded["status"])
                if len(seen) == 4 and decoded["cpu"] == 80 and len(failures) >= 2:
                    stop.set()
            return len(frame)

        def close(self):
            closed.append(True)

    sender = agent.SerialSender(state, stop, interval=0.02,
                               port_selector=lambda _: "SIMULATED", opener=lambda _: Connection())
    sampler = agent.TemperatureSampler(state, stop)
    stats = SimpleNamespace(cpu_percent=lambda interval: next(cpu_samples),
                            virtual_memory=lambda: SimpleNamespace(percent=50.0))
    with patch.object(agent.requests, "Session") as factory:
        factory.return_value.__enter__.return_value.get.side_effect = unavailable
        sender.start()
        sampler.start()
        try:
            agent.collect(state, stop, duration=4, status_reader=status_reader, stats=stats)
            completed = stop.is_set()
        finally:
            stop.set()
            sender.join(timeout=1)
            sampler.join(timeout=1)
    check = unittest.TestCase()
    check.assertFalse(sender.is_alive())
    check.assertFalse(sampler.is_alive())
    check.assertTrue(completed, "all four modes and a new CPU sample must reach write()")
    check.assertEqual(closed, [True])
    check.assertGreaterEqual(len(failures), 2)
    check.assertGreaterEqual(len(windows), 10)
    return frames


class AgentTests(unittest.TestCase):
    def test_classification_and_no_actions(self):
        for title, status in [("Visual Studio Code", "coding"), ("PYCHARM", "coding"),
                              ("Chrome - Docs", "browser"), ("网易云音乐", "media"),
                              ("Spotify", "media"), (None, "normal"),
                              ("AutoCAD", "normal"), ("Steam", "normal")]:
            self.assertEqual(agent.classify_window(title), status)
        for title in ("Windows PowerShell", "PowerShell 7", "Windows Terminal"):
            self.assertEqual(agent.classify_window(title), "normal")
        for title, expected in (("Nexus - Visual Studio Code", "coding"),
                                ("Windows PowerShell", "normal"),
                                ("Docs - Chrome", "browser"), ("Spotify", "media")):
            with patch.object(agent.gw, "getActiveWindow", return_value=SimpleNamespace(title=title)):
                self.assertEqual(agent.foreground_status(), expected)
        with patch.object(agent.gw, "getActiveWindow", side_effect=RuntimeError("unavailable")):
            self.assertEqual(agent.foreground_status(), "normal")

    def test_temperature_and_exact_wire_format(self):
        for raw in (None, "N/A", "", "nan", float("inf"), False):
            self.assertIsNone(agent.temperature_value(raw))
        self.assertEqual(agent.temperature_value("55,5 °C"), 55.5)
        self.assertEqual(agent.temperature_value(0), 0.0)  # Known zero is not a missing value.
        tree = {"Children": [{"Text": "CPU Package", "Value": "N/A"},
                             {"Text": "Core Max", "Value": "64 °C"}]}
        self.assertEqual(agent.find_temperature(tree), 64)
        self.assertIsNone(agent.find_temperature({"Children": "malformed"}))
        self.assertIsNone(agent.find_temperature({"Text": [], "Value": 50}))
        self.assertIsNone(agent.temperature_value(10 ** 1000))
        state = agent.LatestState()
        self.assertIsNone(state.snapshot())
        state.update(12.5, 42.0, "coding")
        encoded = agent.encode_frame(state.snapshot())
        self.assertTrue(encoded.endswith(b"\n"))
        self.assertEqual(json.loads(encoded), {"cpu": 12.5, "mem": 42.0,
                                              "cpu_temp": None, "status": "coding"})
        state.set_temperature(50)
        state.set_temperature(None)
        self.assertIsNone(state.snapshot()["cpu_temp"])

    def test_port_selection_and_timeouts(self):
        candidates = [SimpleNamespace(device="COM4", vid=0x303A, pid=0x0002, serial_number="USB_A",
                                      hwid="USB VID:PID=303A:0002", location="1-2:x.0"),
                      SimpleNamespace(device="COM5", vid=0x303A, pid=0x1001, serial_number="USB_B",
                                      hwid="USB VID:PID=303A:1001", location="1-3:x.1")]
        with patch.object(agent.serial.tools.list_ports, "comports", return_value=candidates):
            with self.assertRaises(RuntimeError):
                agent.choose_port()
            self.assertEqual(agent.choose_port("COM8"), "COM8")
        with patch.object(agent.serial, "Serial") as factory:
            connection = agent.open_cdc("COM8")
            factory.assert_called_once_with(port=None, baudrate=115200, timeout=0, write_timeout=0.2)
            self.assertTrue(connection.dtr)
            self.assertFalse(connection.rts)
            connection.open.assert_called_once()

    def test_partial_write_reconnects_with_latest_snapshot(self):
        state, stop = agent.LatestState(), threading.Event()
        state.update(1.0, 2.0, "normal")
        frames, closed = [], []

        class Connection:
            in_waiting = 0

            def __init__(self, failing):
                self.failing = failing

            def write(self, frame):
                if self.failing:
                    state.update(88.0, 66.0, "media")
                    return len(frame) - 1
                frames.append(frame)
                stop.set()
                return len(frame)

            def close(self):
                closed.append(self.failing)

        connections = iter([Connection(True), Connection(False)])
        worker = agent.SerialSender(state, stop, port_selector=lambda _: "TEST",
                                    opener=lambda _: next(connections), retry_interval=0.01)
        with self.assertLogs(agent.LOG, level="DEBUG") as logs:
            worker.start()
            worker.join(timeout=1)
        self.assertFalse(worker.is_alive())
        self.assertEqual(closed, [True, False])
        self.assertEqual(json.loads(frames[0]), {"cpu": 88.0, "mem": 66.0,
                                               "cpu_temp": None, "status": "media"})
        self.assertTrue(frames[0].startswith(b"\n"))
        transmissions = [record.getMessage() for record in logs.records if "CDC TX" in record.getMessage()]
        self.assertEqual(len(transmissions), 1)  # No successful TX log for the incomplete write.
        self.assertIn('"status":"media"', transmissions[0])

    def test_serial_write_wait_does_not_block_window_loop(self):
        state, stop = agent.LatestState(), threading.Event()
        state.update(1, 2, "normal")
        entered, release = threading.Event(), threading.Event()
        calls = []

        class Connection:
            in_waiting = 0

            def write(self, frame):
                entered.set()
                release.wait(timeout=2)
                return len(frame)

            def close(self):
                pass

        worker = agent.SerialSender(state, stop, port_selector=lambda _: "TEST", opener=lambda _: Connection())
        worker.start()
        self.assertTrue(entered.wait(timeout=1))
        try:
            stats = SimpleNamespace(cpu_percent=lambda interval: 1,
                                    virtual_memory=lambda: SimpleNamespace(percent=2))
            agent.collect(state, stop, duration=0.35, stats=stats,
                          status_reader=lambda: calls.append(time.monotonic()) or "coding")
            self.assertGreaterEqual(len(calls), 3)
        finally:
            stop.set()
            release.set()
            worker.join(timeout=1)
        self.assertFalse(worker.is_alive())

    def test_open_failure_and_disconnect_recover_without_other_serial_owners(self):
        state, stop = agent.LatestState(), threading.Event()
        state.update(5, 6, "browser")
        opens, frames, closed = [], [], []

        class Connection:
            in_waiting = 0

            def __init__(self, failing):
                self.failing = failing

            def write(self, frame):
                if self.failing:
                    raise agent.serial.SerialException("USB disconnected")
                frames.append(frame)
                stop.set()
                return len(frame)

            def close(self):
                closed.append(self.failing)

        def open_port(port):
            opens.append(port)
            if len(opens) == 1:
                raise agent.serial.SerialException("port missing")
            return Connection(len(opens) == 2)

        worker = agent.SerialSender(state, stop, port_selector=lambda _: "TEST",
                                    opener=open_port, retry_interval=0.01)
        worker.start()
        worker.join(timeout=1)
        self.assertFalse(worker.is_alive())
        self.assertEqual(opens, ["TEST"] * 3)
        self.assertEqual(closed, [True, False])
        self.assertEqual(json.loads(frames[0])["status"], "browser")

    def test_unavailable_lhm_clears_previous_value_and_has_timeouts(self):
        state, stop = agent.LatestState(), threading.Event()
        state.set_temperature(60)
        worker = agent.TemperatureSampler(state, stop)
        with patch.object(agent.requests, "Session") as factory:
            session = factory.return_value.__enter__.return_value

            def unavailable(*args, **kwargs):
                self.assertEqual(kwargs["timeout"], (0.3, 0.5))
                stop.set()
                raise agent.requests.ConnectionError("offline")

            session.get.side_effect = unavailable
            worker.run()
            self.assertFalse(session.trust_env)
        state.update(1, 2, "normal")
        self.assertIsNone(state.snapshot()["cpu_temp"])

    def test_timeout_does_not_skip_real_serial_writes_and_workers_can_restart(self):
        for _ in range(2):  # Fresh workers after stop, with no stale queued frames.
            frames = capture_timeout_frames()
            decoded = [json.loads(frame) for frame in frames]
            self.assertEqual({item["status"] for item in decoded},
                             {"normal", "coding", "browser", "media"})
            self.assertEqual({item["cpu"] for item in decoded}, {20.0, 80.0})
            for raw, item in zip(frames, decoded):
                self.assertTrue(raw.endswith(b"\n"))
                self.assertEqual(raw.count(b"\n"), 1)
                self.assertLessEqual(len(raw.rstrip(b"\n")), 512)
                self.assertEqual(set(item), {"cpu", "mem", "cpu_temp", "status"})
                self.assertEqual(item["mem"], 50.0)
                self.assertIsNone(item["cpu_temp"])

    def test_dry_run_uses_same_snapshot_and_encoder(self):
        state, stop = agent.LatestState(), threading.Event()
        stats = SimpleNamespace(cpu_percent=lambda interval: 20.0,
                                virtual_memory=lambda: SimpleNamespace(percent=50.0))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            agent.collect(state, stop, dry_run=True, duration=1.15,
                          status_reader=lambda: "coding", stats=stats)
        self.assertEqual(output.getvalue().encode(), agent.encode_frame(state.snapshot()))
        self.assertEqual(json.loads(output.getvalue()),
                         {"cpu": 20.0, "mem": 50.0, "cpu_temp": None, "status": "coding"})

    def test_lhm_failures_are_rate_limited_and_recovery_is_reported(self):
        state, stop = agent.LatestState(), threading.Event()
        worker = agent.TemperatureSampler(state, stop)
        results = iter((True, True, True, False, True))

        def request(*args, **kwargs):
            failing = next(results)
            if failing:
                raise agent.requests.ReadTimeout("test timeout")
            return response

        waits = []

        def wait(_):
            waits.append(state.cpu_temp)
            if len(waits) == 5:
                stop.set()

        with patch.object(agent.requests, "Session") as factory:
            session = factory.return_value.__enter__.return_value
            response = session.get.return_value
            response.__enter__.return_value.json.return_value = {"Text": "CPU Package", "Value": "55 °C"}
            session.get.side_effect = request
            with patch.object(stop, "wait", side_effect=wait), \
                 patch.object(agent.time, "monotonic", side_effect=(0, 1, 31, 33)), \
                 self.assertLogs(agent.LOG, level="DEBUG") as logs:
                worker.run()
        self.assertEqual(waits, [None, None, None, 55.0, None])
        warnings = [record for record in logs.records if record.levelname == "WARNING"]
        self.assertEqual(len(warnings), 3)  # First, 30s summary, first failure after recovery.
        self.assertTrue(all(record.exc_info is None for record in warnings))
        self.assertTrue(all("ReadTimeout" in record.getMessage() for record in warnings))
        self.assertIn("consecutive failures=3", warnings[1].getMessage())
        self.assertEqual(sum("recovered" in record.getMessage() for record in logs.records), 1)

    def test_unexpected_temperature_error_is_not_silenced(self):
        state, stop = agent.LatestState(), threading.Event()
        with patch.object(agent.requests, "Session") as factory:
            factory.return_value.__enter__.return_value.get.side_effect = RuntimeError("unexpected bug")
            with self.assertRaisesRegex(RuntimeError, "unexpected bug"):
                agent.TemperatureSampler(state, stop).run()

    def test_main_interrupt_closes_sender_and_can_be_started_again(self):
        real_sender = agent.SerialSender
        for _ in range(2):
            written, closed = threading.Event(), threading.Event()

            class Connection:
                in_waiting = 0

                def write(self, frame):
                    self.frame = frame
                    written.set()
                    return len(frame)

                def close(self):
                    closed.set()

            connection = Connection()
            workers = []

            def sender(state, stop, **kwargs):
                worker = real_sender(state, stop, interval=0.01,
                                     port_selector=lambda _: "SIMULATED",
                                     opener=lambda _: connection)
                workers.append(worker)
                return worker

            def interrupted(state, *args):
                state.update(20.0, 50.0, "normal")
                self.assertTrue(written.wait(timeout=1))
                raise KeyboardInterrupt

            with patch.object(agent, "SerialSender", side_effect=sender), \
                 patch.object(agent, "collect", side_effect=interrupted):
                self.assertEqual(agent.main(["--no-temperature"]), 0)
            self.assertTrue(closed.is_set())
            self.assertFalse(workers[0].is_alive())
            self.assertEqual(json.loads(connection.frame)["status"], "normal")


if __name__ == "__main__":
    unittest.main()
