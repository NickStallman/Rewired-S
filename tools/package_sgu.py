#!/usr/bin/env python3
"""Package an ESPHome app image (firmware.ota.bin) as a signed Sungrow `.sgu` for the dongle's
own update routes (the stock web UI upload or the LAN pull route served by
tools/fake_iconfig_server.py), with the checks a WiNet image must pass.

    python3 -I tools/package_sgu.py --app .esphome/build/winet-s2/build/firmware.ota.bin \
        --board s2 --build 901 --key keys/rewired-s_priv.pem --out-dir dist --zip

Two package shapes:

- **App-only** (default): one type-2 part with the app. The updater writes it to the inactive
  OTA slot and leaves the `oemdata` partition (the stock web UI) as it is. This is what the
  release packages are: nothing from a stock package is needed or included.
- **With the stock web UI** (`--stock-sgu PATH`): the type-3 part is copied unchanged from a
  stock package you provide, so the updater rewrites `oemdata` with identical content. Use it
  if your dongle refuses an app-only package.

Every package carries the in-file signed trailer: the WiNet-S2 refuses a package without one and
the WiNet-S ignores it. The package is signed with YOUR key (`make sgu-keys`, or
`tools/sgu.py keygen`); the dongle accepts it only after that public key was imported
(`tools/deploy_debug.py import-key`, docs/install_ota.md).

Checks before signing: ESP image magic, chip id (ESP32 = 0 for the S, ESP32-S3 = 9 for the S2),
the 16 MB flash size in the app header (IDF sizes the usable flash from it), the app size against
the smaller of the two OTA slots, and a package version above the installed one (the updater
refuses a downgrade). The container format: tools/sgu.py and SG-Docs
https://github.com/NickStallman/SG-Docs/blob/main/reference/firmware-update/sgu-container.md.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sgu  # noqa: E402

BOARDS = {
    # version prefix, chip id, app size limit (the smaller OTA slot), stock version floor
    "s": ("WINET-SV200.001.00", 0, 0x309000, "WINET-SV200.001.00.P046"),
    "s2": ("WINET-SV300.001.03", 9, 0x33B000, "WINET-SV300.001.03.P032"),
}
FLASH_16MB = 4
DEFAULT_KEY = os.path.join("keys", "rewired-s_priv.pem")
DEFAULT_PUB = os.path.join("keys", "rewired-s_pub.pem")


def version_name(board: str, build: int) -> str:
    if not 1 <= build <= 999:
        raise ValueError("build number 1..999")
    return f"{BOARDS[board][0]}.P{build:03d}"


def check_app(app: bytes, board: str):
    """Problems (empty = OK) for an app image meant for `board`."""
    problems = []
    _, chip, slot_min, _ = BOARDS[board]
    if len(app) < 24 or app[0] != 0xE9:
        return ["not an ESP app image (magic != 0xE9)"]
    size_nibble = app[3] >> 4
    if size_nibble != FLASH_16MB:
        problems.append(f"app header flash size nibble {size_nibble} != 4 (16MB): build with esp32: flash_size: 16MB")
    chip_id = struct.unpack_from("<H", app, 12)[0]
    if chip_id != chip:
        problems.append(f"chip_id {chip_id} != {chip} ({'ESP32' if chip == 0 else 'ESP32-S3'})")
    if len(app) > slot_min:
        problems.append(f"app is {len(app)} B > the {slot_min} B OTA slot limit")
    magic = struct.unpack_from("<I", app, 0x20)[0] if len(app) >= 0x24 else 0
    if magic != 0xABCD5432:
        problems.append("no esp_app_desc at 0x20 (not an IDF app?)")
    return problems


def stock_part2(sgu_blob: bytes) -> bytes:
    """The type-3 (web UI) part of a stock package, with its 6-byte header."""
    parts = sgu.walk_parts(sgu_blob)
    for p in parts:
        if p["type"] == sgu.PART_OEMDATA and p["len"]:
            return sgu_blob[p["hdr_off"]:p["data_off"] + p["len"]]
    raise ValueError("the stock .sgu has no type-3 (web UI) part")


def walk_tlv(sgu_blob: bytes):
    """[(type, offset_of_data, len)] of the package body."""
    return [(p["type"], p["data_off"], p["len"]) for p in sgu.walk_parts(sgu_blob)]


def check_trailer(sgu_blob: bytes):
    """Problems (empty = OK) with the in-file trailer the WiNet-S2 requires."""
    info = sgu.parse(sgu_blob)
    tr = info["trailer"]
    if tr is None:
        return ["no signed trailer after the body (the WiNet-S2 refuses this package: error 4104)"]
    problems = []
    if not tr["consistent"]:
        problems.append(f"trailer header inconsistent (total {tr['trailer_total']}, sig {tr['siglen']}, "
                        f"key {tr['keylen']}, x {tr['xlen']})")
    if not info["md5_ok"]:
        problems.append("MD5 at 0x00 does not cover [0x20, EOF)")
    return problems


def verify_package(sgu_blob: bytes, app: bytes, part2: bytes = b"", trailer: bool = True):
    """Problems (empty = OK): body = [type 2 = our app] then, when part2 is given, [type 3 =
    that part, byte-identical], then (unless `trailer` is False) a valid signed trailer."""
    try:
        parts = walk_tlv(sgu_blob)
    except ValueError as e:
        return [str(e)]
    problems = check_trailer(sgu_blob) if trailer else []
    want = 2 if part2 else 1
    if len(parts) != want:
        problems.append(f"{len(parts)} parts, expected {want}")
    if parts and (parts[0][0] != 2 or sgu_blob[parts[0][1]:parts[0][1] + parts[0][2]] != app):
        problems.append("part 1 is not type 2 with our app image")
    if part2 and len(parts) > 1:
        t, o, n = parts[1]
        if t != 3 or sgu_blob[o - 6:o + n] != part2:
            problems.append("part 2 is not the stock type-3 web UI, byte-identical")
    return problems


def stock_version(sgu_blob: bytes) -> str:
    return sgu.parse(sgu_blob)["app_ver"]


def main(argv=None):
    ap = argparse.ArgumentParser(description="Signed .sgu from an ESPHome app image (rewired-s-sgu / tools/sgu.py)")
    ap.add_argument("--app", required=True, help="firmware.ota.bin (app image, 0xE9)")
    ap.add_argument("--board", choices=sorted(BOARDS), default="s")
    ap.add_argument("--build", type=int, default=901, help="P-number of the package version (default 901)")
    ap.add_argument("--stock-sgu", help="optional stock .sgu of the same board to copy the web-UI part from "
                                        "(default: app-only package)")
    ap.add_argument("--key", default=DEFAULT_KEY, help=f"private key PEM (default {DEFAULT_KEY})")
    ap.add_argument("--pubkey", help=f"public key PEM (default {DEFAULT_PUB} if present, else derived from --key)")
    ap.add_argument("--out-dir", default="dist")
    ap.add_argument("--zip", action="store_true", help="also write the zip the stock web UI / pull route expect")
    ap.add_argument("--force", action="store_true", help="package even if the app checks fail")
    args = ap.parse_args(argv)

    with open(args.app, "rb") as f:
        app = f.read()
    problems = check_app(app, args.board)
    for p in problems:
        print(f"CHECK FAILED: {p}", file=sys.stderr)
    if problems and not args.force:
        return 2

    name = version_name(args.board, args.build)
    floor = BOARDS[args.board][3]
    part2 = b""
    if args.stock_sgu:
        with open(args.stock_sgu, "rb") as f:
            stock = f.read()
        part2 = stock_part2(stock)
        floor = max(floor, stock_version(stock))
    if name <= floor:
        print(f"version {name} is not above {floor}: the dongle refuses a downgrade", file=sys.stderr)
        return 2

    if not os.path.exists(args.key):
        print(f"no signing key at {args.key}: run `make sgu-keys` (tools/sgu.py keygen) first", file=sys.stderr)
        return 2
    with open(args.key, "rb") as f:
        priv = f.read()
    pub = None
    pub_path = args.pubkey or (DEFAULT_PUB if os.path.exists(DEFAULT_PUB) and args.key == DEFAULT_KEY else None)
    if pub_path:
        with open(pub_path, "rb") as f:
            pub = f.read()

    print(f"app {len(app)} B" + (f", web UI part {len(part2)} B from {os.path.basename(args.stock_sgu)}"
                                 if part2 else ", app-only") + f" -> {name}")
    r = sgu.pack(app, name, priv, args.out_dir, oemdata_image=part2, public_key_pem=pub,
                 embed_trailer=True, make_zip=args.zip)
    if not r["ok"]:
        return 3
    problems = verify_package(r["sgu"], app, part2)
    for p in problems:
        print(f"PACKAGE CHECK FAILED: {p}", file=sys.stderr)
    if problems:
        return 3
    print("package check: body = [type 2: app]" + (" [type 3: stock web UI, byte-identical]" if part2 else "")
          + " + signed trailer OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
