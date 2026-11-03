#!/usr/bin/env python3
"""Fail when an app image does not fit the smaller OTA slot of its board (CI and `make compile`).

    python3 -I tools/check_image_size.py .esphome/build/<name>/build/firmware.ota.bin winet-s2
"""
import os
import sys

LIMITS = {"winet-s": 0x309000, "winet-s2": 0x33B000}

if __name__ == "__main__":
    path, entry = sys.argv[1], sys.argv[2]
    board = "winet-s2" if entry.startswith("winet-s2") else "winet-s"
    size = os.path.getsize(path)
    limit = LIMITS[board]
    print(f"{entry}: {size:,} B of {limit:,} B ({size * 100 // limit}% of the {board} OTA slot)")
    sys.exit(1 if size > limit else 0)
