"""
Real-time PIR sensor state monitor for the Lantern Brain ESP32.

Polls /status at a configurable interval and prints every change to sensor
state, motion mode, direction, or segment brightness.

Usage:
    python tests/monitor_sensors.py
    python tests/monitor_sensors.py --ip 192.168.1.239 --interval 0.5
    python tests/monitor_sensors.py --log events.jsonl   # also write JSONL log
"""

import argparse
import json
import sys
import time
from datetime import datetime

import requests

COLUMNS = (
    f"{'Timestamp':12} {'Uptime':>7}  {'F':1} {'M':1} {'B':1}  "
    f"{'Motion':6}  {'Dir':6}  {'Idle':>5}  Segments"
)
DIVIDER = "-" * 72


def fetch_status(base_url: str, timeout: int = 5) -> dict | None:
    try:
        r = requests.get(f"{base_url}/status", timeout=timeout)
        r.raise_for_status()
        return r.json()
    except requests.RequestException as exc:
        return {"_error": str(exc)}


def seg_bar(segs_bright: list[bool]) -> str:
    """Return a compact visual string: ■ = bright, □ = dim."""
    return "".join("■" if b else "□" for b in segs_bright)


def has_changed(prev: dict, curr: dict) -> bool:
    if prev is None:
        return True
    return any(
        prev.get(k) != curr.get(k)
        for k in ("pir_f", "pir_m", "pir_b", "motion", "dir", "segs_bright")
    )


def format_row(data: dict, changed: bool) -> str:
    marker = "◄" if changed else " "
    segs = seg_bar(data.get("segs_bright", []))
    motion_str = "MOTION" if data.get("motion") else "idle"
    return (
        f"{datetime.now().strftime('%H:%M:%S'):12} "
        f"{data.get('uptime_s', '?'):>5}s  "
        f"{data.get('pir_f', '?'):1} "
        f"{data.get('pir_m', '?'):1} "
        f"{data.get('pir_b', '?'):1}  "
        f"{motion_str:6}  "
        f"{data.get('dir', '?'):6}  "
        f"{data.get('idle_s', 0):>3}s  "
        f"{segs} {marker}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Monitor Lantern Brain PIR sensor states in real time."
    )
    parser.add_argument("--ip",       default="192.168.1.239",
                        help="Brain ESP32 IP address (default: 192.168.1.239)")
    parser.add_argument("--interval", type=float, default=0.5,
                        help="Poll interval in seconds (default: 0.5)")
    parser.add_argument("--log",      default=None,
                        help="Optional path to write a JSONL event log")
    parser.add_argument("--changes-only", action="store_true",
                        help="Print only rows where something changed")
    args = parser.parse_args()

    base_url = f"http://{args.ip}"
    log_file  = open(args.log, "a") if args.log else None

    print(f"Monitoring {base_url}  (interval={args.interval}s)  Ctrl-C to stop\n")
    print(COLUMNS)
    print(DIVIDER)

    prev: dict | None = None
    try:
        while True:
            data = fetch_status(base_url)

            if "_error" in data:
                print(f"[{datetime.now().strftime('%H:%M:%S')}] ERROR: {data['_error']}")
            else:
                changed = has_changed(prev, data)
                if not args.changes_only or changed:
                    print(format_row(data, changed))

                if log_file and changed:
                    record = {"ts": datetime.utcnow().isoformat(), **data}
                    log_file.write(json.dumps(record) + "\n")
                    log_file.flush()

                prev = data

            time.sleep(args.interval)

    except KeyboardInterrupt:
        print("\nMonitor stopped.")
    finally:
        if log_file:
            log_file.close()


if __name__ == "__main__":
    sys.exit(main())
