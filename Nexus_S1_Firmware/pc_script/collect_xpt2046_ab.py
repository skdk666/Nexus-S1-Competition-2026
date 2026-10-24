"""Read-only RAW snapshot collection/statistics; never sends data or system keys."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import statistics
import time

PROFILES = {"A": (1000000, 30, "LEGACY30"), "B": (1000000, 63, "SETTLED24"),
            "C": (250000, 63, "SETTLED24")}


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def summarize(lines, experiment):
    """Count matching, complete CFG/ADC/QUALITY snapshots, not every SPI poll.

    'z_plausible' is the existing conservative software guard, NOT proven contact.
    FF/ZERO include discarded conversions and final power-down, matching firmware.
    """
    pending, seen = {}, set()
    counts = Counter(records=0, FF=0, ZERO=0, MIXED=0, INVALID=0,
                     z_plausible=0, pressed_candidate=0, malformed=0, incomplete=0)
    contacts = Counter()
    dx, dy, mx, my, px, py = [], [], [], [], [], []
    hz, size, protocol = PROFILES[experiment]
    for line in lines:
        if not line.startswith(("TOUCH CFG ", "TOUCH ADC ", "TOUCH QUALITY ")):
            continue
        f = fields(line)
        if "id" not in f:
            continue
        frame_id = f["id"]
        if line.startswith("TOUCH CFG "):
            expected = {"exp": experiment, "hz": str(hz), "n": str(size), "read": protocol,
                        "mode": "0", "CLK": "12", "DIN": "11", "DO": "8", "TCS": "7", "LCDCS": "10"}
            if any(f.get(k) != v for k, v in expected.items()):
                raise ValueError("CFG differs from requested experiment/pins; stop, check flashed firmware")
            if frame_id in seen or frame_id in pending:
                raise ValueError("Repeated CFG id; possible reset/concatenated runs; use a new log")
            pending[frame_id] = {"cfg": f}
            if len(pending) > 16:
                pending.pop(next(iter(pending)))
                counts["incomplete"] += 1
        elif frame_id in pending and frame_id not in seen:
            item = pending[frame_id]
            item["adc" if line.startswith("TOUCH ADC ") else "quality"] = f
            if "adc" not in item or "quality" not in item:
                continue
            a, q = item["adc"], item["quality"]
            data = a.get("data", "INVALID")
            counts["records"] += 1
            counts[data if data in ("FF", "ZERO", "MIXED") else "INVALID"] += 1
            contacts[a.get("contact", "MISSING")] += 1
            try:
                zs = [int(z) for key in ("z1", "z2") for z in a[key].split(",")]
                xs, ys = ([int(v) for v in a[key].split(",")] for key in ("x", "y"))
                sx, sy = int(q["dx"]), int(q["dy"])
                if len(zs) != 4 or len(xs) != 3 or len(ys) != 3 or any(v < 0 or v > 4095 for v in zs+xs+ys):
                    raise ValueError("Invalid ADC range/length")
                if sx != max(xs)-min(xs) or sy != max(ys)-min(ys):
                    raise ValueError("Span differs from ADC values")
                if data == "MIXED":
                    mx.append(sx); my.append(sy)
                # Match pressureIssue(): both pairs must be plausible. Never count FF/00.
                plausible = (item["cfg"].get("started") == "1" and a.get("ok") == "1" and data == "MIXED"
                             and all(zs[i+2] > zs[i] and (zs[i] > 0 or 4095-zs[i+2] <= 300) for i in (0, 1)))
                if plausible:
                    counts["z_plausible"] += 1
                    dx.append(sx); dy.append(sy)
                    if a.get("contact") == "PRESSED" and sx <= 20 and sy <= 20:
                        counts["pressed_candidate"] += 1
                        px.append(statistics.median(xs)); py.append(statistics.median(ys))
            except (KeyError, ValueError):
                counts["malformed"] += 1
            seen.add(frame_id); del pending[frame_id]
    counts["incomplete"] += len(pending)
    def stats(values):
        return {"n": len(values), "median": statistics.median(values) if values else None,
                "max": max(values) if values else None,
                "range": max(values)-min(values) if values else None}
    return {"experiment": experiment, "hz": hz, "bytes": size,
            "counts": dict(counts), "contact": dict(contacts),
            "mixed_span_x": stats(mx), "mixed_span_y": stats(my),
            "z_plausible_span_x": stats(dx), "z_plausible_span_y": stats(dy),
            "pressed_candidate_median_x": stats(px), "pressed_candidate_median_y": stats(py),
            "scope": "logged snapshots only; plausibility is NOT hardware acceptance"}


def capture(port, seconds, output):
    import serial  # Existing project dependency; offline analysis needs only stdlib.
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive file creation protects previous experiments; port opens only on explicit --port.
    with output.open("x", encoding="utf-8") as log:
        link = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=0.2)
        link.dtr, link.rts, link.port = True, False, port
        with link:
            print("Maintain selected phase; settling for 3 seconds...", flush=True)
            settle_until = time.monotonic()+3
            while time.monotonic() < settle_until:
                link.read(512)  # Drain old snapshots; no writes, no reset command.
            link.reset_input_buffer()
            end, partial = time.monotonic()+seconds, b""
            while time.monotonic() < end:
                partial += link.read(512)
                if len(partial) > 8192:
                    raise ValueError("Oversized/missing-newline CDC log")
                while b"\n" in partial:
                    line, partial = partial.split(b"\n", 1)
                    log.write(line.decode("ascii", errors="replace").rstrip("\r")+"\n")
            # Never invent a final newline for a partial CDC line.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--experiment", choices=PROFILES, required=True)
    parser.add_argument("--phase", choices=("idle", "pressed", "released"), required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="Explicit current CDC COM port; close Agent/monitor first")
    source.add_argument("--log", type=Path, help="Analyze an existing single-phase log offline")
    parser.add_argument("--output", type=Path, help="New .log path for capture; never overwritten")
    parser.add_argument("--seconds", type=float, default=60)
    args = parser.parse_args()
    if not 5 <= args.seconds <= 600:
        parser.error("--seconds must be 5..600")
    if args.port and not args.output:
        parser.error("--output is required for capture")
    path = args.log or args.output
    try:
        if args.port:
            capture(args.port, args.seconds, path)
        result = summarize(path.read_text(encoding="utf-8").splitlines(), args.experiment)
        result.update(phase=args.phase, log=str(path), seconds=args.seconds if args.port else None)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if not result["counts"].get("records"):
            raise ValueError("No complete CFG/ADC/QUALITY snapshots; do not interpret as zero failures")
        return 0
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
