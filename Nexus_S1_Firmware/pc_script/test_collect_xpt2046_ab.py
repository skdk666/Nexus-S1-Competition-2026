"""Synthetic logs only: actual RAW snapshots, experiment identity and statistics."""
import unittest
from pathlib import Path
import json
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
from collect_xpt2046_ab import PROFILES, capture, summarize


def snapshot(exp="B", frame_id=1, data="MIXED", ok=1, contact="PRESSED", z1="400,400", z2="3500,3500"):
    hz, size, protocol = PROFILES[exp]
    return [f"TOUCH CFG id={frame_id} spi=FSPI CLK=12 DIN=11 DO=8 TCS=7 LCDCS=10 "
            f"hz={hz} mode=0 MSB started=1 n={size} read={protocol} exp={exp}",
            f"TOUCH ADC id={frame_id} ok={ok} ack=NA data={data} x=1000,1001,999 "
            f"y=2000,2001,1999 z1={z1} z2={z2} contact={contact} raw=1",
            f"TOUCH QUALITY id={frame_id} issue=NONE dx=2 dy=2 limit=20"]


class SnapshotTests(unittest.TestCase):
    def test_capture_only_reads_preserves_lines_and_refuses_overwrite(self):
        packet = "\n".join(snapshot())+"\nTOUCH ADC incomplete"
        class FakeSerial:
            def __init__(self, **kwargs):
                self.options = kwargs
                self.data = iter([b"discarded during settling\n", packet.encode("ascii")])
            def __enter__(self):
                self.assertions = (self.dtr, self.rts, self.port, self.options)
                return self
            def __exit__(self, *args):
                pass
            def read(self, size):
                assert size == 512
                return next(self.data)
            def reset_input_buffer(self):
                pass
            # No write method: attempting to send a command fails the test.
        fake = FakeSerial(port=None, baudrate=115200, timeout=0.2, write_timeout=0.2)
        root = Path(__file__).resolve().parents[1]/".pio/xpt2046_test"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as temp:
            path = Path(temp)/"capture.log"
            with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=lambda **kwargs: fake)}), \
                 patch("collect_xpt2046_ab.time.monotonic", side_effect=[0, 0, 4, 10, 10, 16]):
                capture("MOCK_PORT", 5, path)
            self.assertEqual(fake.assertions[:3], (True, False, "MOCK_PORT"))
            self.assertEqual(path.read_text(encoding="utf-8"), "\n".join(snapshot())+"\n")
            with self.assertRaises(FileExistsError):
                capture("MOCK_PORT", 5, path)

    def test_offline_cli_outputs_phase_and_fails_without_records(self):
        root = Path(__file__).resolve().parents[1]/".pio/xpt2046_test"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as temp:
            path = Path(temp)/"synthetic.log"
            path.write_text("\n".join(snapshot()), encoding="utf-8")
            command = [sys.executable, str(Path(__file__).with_name("collect_xpt2046_ab.py")),
                       "--log", str(path), "--experiment", "B", "--phase", "pressed"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["phase"], "pressed")
            path.write_text("MAIN HEARTBEAT\n", encoding="utf-8")
            result = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("No complete CFG/ADC/QUALITY snapshots", result.stdout)

    def test_all_profiles_and_spans(self):
        for exp in PROFILES:
            with self.subTest(exp=exp):
                r = summarize(snapshot(exp), exp)
                self.assertEqual(r["counts"]["records"], 1)
                self.assertEqual(r["counts"]["z_plausible"], 1)
                self.assertEqual(r["counts"]["pressed_candidate"], 1)
                self.assertEqual(r["mixed_span_x"]["max"], 2)
                self.assertEqual(r["pressed_candidate_median_x"]["median"], 1000)

    def test_ff_zero_never_plausible_even_if_pressed_label_is_wrong(self):
        lines = snapshot(data="FF")+snapshot(frame_id=2, data="ZERO")
        r = summarize(lines, "B")
        self.assertEqual(r["counts"]["FF"], 1)
        self.assertEqual(r["counts"]["ZERO"], 1)
        self.assertEqual(r["counts"]["MIXED"], 0)
        self.assertEqual(r["counts"]["pressed_candidate"], 0)
        self.assertIsNone(r["z_plausible_span_x"]["max"])

    def test_zero_or_reversed_z_never_trusted(self):
        for z1, z2 in [("0,0", "0,0"), ("4,22", "0,0"), ("400,400", "300,300"), ("0,0", "3000,3000")]:
            r = summarize(snapshot(z1=z1, z2=z2), "B")
            self.assertEqual(r["counts"]["z_plausible"], 0)
            self.assertEqual(r["mixed_span_y"]["max"], 2)

    def test_plausible_release_is_not_pressed(self):
        r = summarize(snapshot(z1="0,0", z2="4095,4095", contact="RELEASED"), "B")
        self.assertEqual(r["counts"]["z_plausible"], 1)
        self.assertEqual(r["counts"]["pressed_candidate"], 0)

    def test_wrong_profile_frequency_pins_and_old_firmware_rejected(self):
        for old, new in [("exp=B", "exp=A"), ("hz=1000000", "hz=250000"), ("DO=8", "DO=6"),
                         ("n=63", "n=30"), ("mode=0", "mode=1"), (" exp=B", "")]:
            lines = snapshot()
            lines[0] = lines[0].replace(old, new)
            with self.assertRaises(ValueError):
                summarize(lines, "B")

    def test_partial_out_of_order_and_duplicate_records(self):
        lines = snapshot()
        r = summarize([lines[1], lines[0], lines[2]], "B")
        self.assertEqual(r["counts"]["records"], 0)
        self.assertEqual(r["counts"]["incomplete"], 1)
        r = summarize([lines[0], lines[2], lines[1], lines[1], lines[2]], "B")
        self.assertEqual(r["counts"]["records"], 1)
        with self.assertRaises(ValueError):
            summarize(lines+lines, "B")  # Reset/concatenation is not another independent sample.

    def test_malformed_and_no_logs_are_not_good_results(self):
        lines = snapshot()
        lines[1] = lines[1].replace("x=1000,1001,999", "x=5000,1001,999")
        r = summarize(lines, "B")
        self.assertEqual(r["counts"]["malformed"], 1)
        self.assertEqual(r["counts"]["pressed_candidate"], 0)
        self.assertEqual(summarize(["MAIN HEARTBEAT", "garbage"], "B")["counts"]["records"], 0)


if __name__ == "__main__":
    unittest.main()
