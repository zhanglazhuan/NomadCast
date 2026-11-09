"""Capture CST836U USB serial logs. Run with ESP-IDF Python (pyserial installed)."""
import argparse
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser()
parser.add_argument("--port", default="COM12")
parser.add_argument("--seconds", type=float, default=30)
parser.add_argument("--output", default="capture.log")
parser.add_argument("--reset", action="store_true", help="Reset ESP32 via RTS before capture")
args = parser.parse_args()
if args.seconds <= 0:
    parser.error("--seconds must be positive")
with serial.Serial(args.port, 115200, timeout=0.2) as connection:
    if args.reset:
        connection.dtr = False
        connection.rts = True
        time.sleep(0.15)
        connection.rts = False
    until = time.monotonic() + args.seconds
    with Path(args.output).open("wb") as output:
        while time.monotonic() < until:
            data = connection.read(connection.in_waiting or 1)
            if data:
                output.write(data)
                output.flush()
                print(data.decode("utf-8", errors="replace"), end="", flush=True)
