#!/usr/bin/env python3
"""Fail CI when the application image leaves less than 10% headroom."""
from pathlib import Path
import sys

MAX_APP_BYTES = 0x480000  # 4.5 MiB; partition is 0x500000 (10% reserve)

def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "build/NomadCast.bin")
    size = path.stat().st_size
    print(f"{path}: {size} bytes (limit {MAX_APP_BYTES})")
    if size > MAX_APP_BYTES:
        print("ERROR: firmware image exceeds the release headroom threshold", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
