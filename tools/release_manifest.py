#!/usr/bin/env python3
"""Collect a compiled entry into a release directory with an ESP Web Tools / ESPHome manifest.

    python3 -I tools/release_manifest.py --entry winet-s-prebuilt --build-dir .esphome/build/rewired-s \
        --version 0.9.1 --out-dir release/winet-s-prebuilt --base-url https://nickstallman.github.io/Rewired-S/firmware/winet-s-prebuilt

Writes:
    <out-dir>/<entry>.ota.bin        the app image (ESPHome OTA, stock pull-OTA packaging, serial write to an OTA slot)
    <out-dir>/<entry>.factory.bin    the full image (bootloader + partition table + app); kept for reference only
    <out-dir>/blank_otadata.bin      0x2000 bytes of 0xFF (resets the OTA selection so the bootloader starts ota_0)
    <out-dir>/manifest.json          one manifest that both ESP Web Tools and ESPHome's http_request update read

The ESP Web Tools `parts` describe the WiNet-S serial route from docs/install_serial.md: the app
goes to the first OTA slot at 0x110000 and otadata at 0x109000 is blanked. The bootloader and
partition table of the dongle are never written. ESPHome's `update: platform: http_request`
reads `builds[].ota` (path, md5, summary, release_url).
"""
import argparse
import hashlib
import json
import os
import shutil

CHIP = {"winet-s": "ESP32", "winet-s2": "ESP32-S3"}
OTADATA_OFF = 0x109000
OTADATA_LEN = 0x2000
OTA0_OFF = 0x110000


def board_of(entry: str) -> str:
    return "winet-s2" if entry.startswith("winet-s2") else "winet-s"


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--entry", required=True, help="entry name, e.g. winet-s-prebuilt")
    ap.add_argument("--build-dir", required=True, help="ESPHome build dir holding build/firmware.ota.bin")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--base-url", default="", help="URL prefix for the paths in the manifest (default: relative)")
    ap.add_argument("--release-url", default="")
    ap.add_argument("--summary", default="")
    args = ap.parse_args(argv)

    src = os.path.join(args.build_dir, "build")
    ota = os.path.join(src, "firmware.ota.bin")
    factory = os.path.join(src, "firmware.factory.bin")
    if not os.path.exists(ota):
        raise SystemExit(f"missing {ota}")
    os.makedirs(args.out_dir, exist_ok=True)
    ota_name = f"{args.entry}.ota.bin"
    shutil.copyfile(ota, os.path.join(args.out_dir, ota_name))
    if os.path.exists(factory):
        shutil.copyfile(factory, os.path.join(args.out_dir, f"{args.entry}.factory.bin"))
    with open(os.path.join(args.out_dir, "blank_otadata.bin"), "wb") as f:
        f.write(b"\xff" * OTADATA_LEN)
    with open(ota, "rb") as f:
        md5 = hashlib.md5(f.read()).hexdigest()

    prefix = args.base_url.rstrip("/") + "/" if args.base_url else ""
    chip = CHIP[board_of(args.entry)]
    build = {
        "chipFamily": chip,
        "ota": {"path": prefix + ota_name, "md5": md5,
                "summary": args.summary, "release_url": args.release_url},
    }
    if chip == "ESP32":
        # Serial route (WiNet-S only): app into ota_0, otadata blanked, nothing else touched.
        build["parts"] = [
            {"path": prefix + "blank_otadata.bin", "offset": OTADATA_OFF},
            {"path": prefix + ota_name, "offset": OTA0_OFF},
        ]
    manifest = {
        "name": f"Rewired-S {args.entry}",
        "version": args.version,
        "home_assistant_domain": "esphome",
        "new_install_prompt_erase": False,
        "new_install_improv_wait_time": 15,
        "builds": [build],
    }
    with open(os.path.join(args.out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"{args.out_dir}: {ota_name} ({os.path.getsize(ota):,} B, md5 {md5}), manifest for {chip}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
