#!/usr/bin/env python3
"""Back up a WiNet dongle running the DEBUG build over the network (READ-ONLY on the device).

The debug build (winet-s-debug.yaml, common/debug.yaml, sungrow_debug `flash_dump: true`) serves
read-only flash dumps on its web server, behind the web server's auth:
    GET /dump/index            JSON: regions (offset, size, sha256)
    GET /dump/<region>         raw bytes; headers Content-Length, X-SHA256, X-Region-Offset
This tool downloads every region, verifies the SHA-256s, writes a manifest, and assembles a
full internal-flash image that restores STOCK (our running slot replaced by the stock app or
0xFF, otadata pointed at the stock slot). Stdlib only. Usage:

    python3 -I tools/flash_backup.py download --host 192.0.2.50 --user admin --out backup/
        (password: --password, $WINET_WEB_PASSWORD or a prompt)
    python3 -I tools/flash_backup.py assemble --out backup/ \
        [--stock-sgu ../firmware/WINET-SV200.001.00.P046.sgu | --stock-app app.bin] [--otadata stock|keep]
    python3 -I tools/flash_backup.py all ...   (download, then assemble)

Outputs in --out: <region>.bin, manifest.json, restore_full_<size>.bin, RESTORE.txt.
"""
import argparse
import getpass
import hashlib
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request

TOOL_VERSION = 1
# Regions not downloaded by default: "flash" is the whole internal chip (also available with --full).
DEFAULT_SKIP = {"flash"}
APP_TYPE = 0x00
OTA_SUBTYPE_MIN, OTA_SUBTYPE_MAX = 0x10, 0x1F
OTA_STATE_VALID = 2


# ---- small pure helpers (host-tested in tests/test_flash_backup.py) ---------------------------
def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _rom_crc(crc: int, data: bytes) -> int:
    """esp_rom_crc32_le(crc, data): CRC-32 (zlib polynomial) continuing from crc; otadata passes 0xFFFFFFFF."""
    c = (~crc) & 0xFFFFFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (0xEDB88320 if c & 1 else 0)
    return (~c) & 0xFFFFFFFF


def parse_partition_table(buf: bytes):
    """[(label, type, subtype, offset, size, flags)], md5_ok (None = no MD5 entry)."""
    out, md5_ok = [], None
    for off in range(0, len(buf) - 31, 32):
        e = buf[off:off + 32]
        magic = struct.unpack_from("<H", e)[0]
        if magic == 0x50AA:
            t, st, o, s = struct.unpack_from("<BBII", e, 2)
            label = e[12:28].split(b"\0", 1)[0].decode("ascii", "replace")
            out.append((label, t, st, o, s, struct.unpack_from("<I", e, 28)[0]))
        elif magic == 0xEBEB:
            md5_ok = hashlib.md5(buf[:off]).digest() == e[16:32]
            break
        else:
            break
    return out, md5_ok


def otadata_entry(seq: int, state: int = OTA_STATE_VALID) -> bytes:
    """One 32-byte esp_ota_select_entry_t (seq, seq_label 0xFF, state, crc)."""
    head = struct.pack("<I", seq)
    return head + b"\xff" * 20 + struct.pack("<I", state) + struct.pack("<I", _rom_crc(0xFFFFFFFF, head))


def parse_otadata(buf: bytes):
    ents = []
    for i in range(2):
        e = buf[i * 0x1000:i * 0x1000 + 32]
        if len(e) < 32:
            ents.append(None)
            continue
        seq, state, crc = struct.unpack_from("<I", e)[0], struct.unpack_from("<I", e, 24)[0], struct.unpack_from("<I", e, 28)[0]
        valid = seq != 0xFFFFFFFF and state not in (3, 4) and crc == _rom_crc(0xFFFFFFFF, e[:4])
        ents.append({"seq": seq, "state": state, "crc": crc, "valid": valid})
    return ents


def make_otadata(size: int, slot_index: int, app_count: int, prev_seq: int = 0) -> bytes:
    """otadata selecting ota_<slot_index>: seq > prev_seq with (seq - 1) % app_count == slot_index."""
    seq = prev_seq + 1
    while (seq - 1) % app_count != slot_index:
        seq += 1
    img = bytearray(b"\xff" * size)
    img[0:32] = otadata_entry(seq)
    return bytes(img)


def sgu_app(blob: bytes) -> bytes:
    """Part 1 (the ESP app image) of a Sungrow .sgu: u16 type @0xA4 (2 = app), u32 len @0xA6, data @0xAA."""
    if len(blob) < 0xAA:
        raise ValueError("too short for an .sgu")
    ptype, plen = struct.unpack_from("<HI", blob, 0xA4)
    app = blob[0xAA:0xAA + plen]
    if ptype != 2 or len(app) != plen or app[:1] != b"\xe9":
        raise ValueError(f"no ESP app image as part 1 (type {ptype}, len {plen})")
    return app


def assemble(manifest: dict, region_data: dict, stock_app: bytes = None, otadata_mode: str = "stock",
             full_dump: bytes = None):
    """Build a restore image of the internal flash. Returns (image, notes).

    region_data: name -> bytes for every downloaded internal region. The running slot (this debug
    build) is replaced by stock_app (padded with 0xFF) or erased; with otadata_mode "stock",
    otadata is rewritten to boot the other OTA slot (the stock app stock-OTA left there)."""
    size = manifest["flash_size"]
    img = bytearray(full_dump if full_dump is not None else b"\xff" * size)
    if len(img) != size:
        raise ValueError("full dump size != flash_size")
    notes = []
    regions = {r["name"]: r for r in manifest["regions"]}
    for r in manifest["regions"]:
        if r["external"] or r["name"] in ("flash", "running_app") or r["name"].startswith("raw_"):
            continue
        data = region_data.get(r["name"])
        if data is None:
            notes.append(f"{r['name']}: not downloaded, left as {'full dump' if full_dump else '0xFF'}")
            continue
        if len(data) != r["size"]:
            raise ValueError(f"{r['name']}: {len(data)} bytes, manifest says {r['size']}")
        img[r["offset"]:r["offset"] + r["size"]] = data
    run = regions.get("running_app")
    if run is not None:
        slot = bytearray(b"\xff" * run["size"])
        if stock_app is not None:
            if stock_app[:1] != b"\xe9":
                raise ValueError("stock app does not start with 0xE9")
            if len(stock_app) > run["size"]:
                raise ValueError(f"stock app ({len(stock_app)} B) does not fit the slot ({run['size']} B)")
            slot[:len(stock_app)] = stock_app
            notes.append(f"running slot {run['label']} @0x{run['offset']:x}: stock app ({len(stock_app)} B)")
        else:
            notes.append(f"running slot {run['label']} @0x{run['offset']:x}: erased (0xFF); stock boots from the other slot")
        img[run["offset"]:run["offset"] + run["size"]] = slot
    od = regions.get("otadata")
    if otadata_mode == "stock" and od is not None:
        apps = sorted((r for r in manifest["regions"] if r.get("type") == APP_TYPE and
                       OTA_SUBTYPE_MIN <= r.get("subtype", -1) <= OTA_SUBTYPE_MAX), key=lambda r: r["subtype"])
        inactive = regions.get("inactive_app")
        if inactive is not None and apps:
            prev = 0
            cur = region_data.get("otadata")
            if cur is not None:
                prev = max([e["seq"] for e in parse_otadata(cur) if e and e["valid"]] or [0])
            idx = inactive["subtype"] - OTA_SUBTYPE_MIN
            img[od["offset"]:od["offset"] + od["size"]] = make_otadata(od["size"], idx, len(apps), prev)
            notes.append(f"otadata rewritten: boots ota_{idx} ({inactive['label']}, the stock app)")
        else:
            notes.append("otadata kept (no inactive OTA slot / app list in the manifest)")
    elif od is not None:
        notes.append("otadata kept as dumped (boots the running slot)")
    return bytes(img), notes


def restore_note(manifest: dict, image_name: str, notes) -> str:
    regs = "\n".join(f"  {r['name']:<14} @0x{r['offset']:06x} size 0x{r['size']:06x}  {r.get('file', '-')}"
                     f"  sha256 {r.get('sha256', '')[:16]}..." + ("  [EXTERNAL NOR]" if r["external"] else "")
                     for r in manifest["regions"])
    return f"""WiNet flash backup ({manifest.get('host', '?')}, {manifest.get('time', '?')})
flash size {manifest['flash_size']} B, flash_id {manifest.get('flash_id', '?')}, running {manifest.get('running', '?')}

Regions:
{regs}

Assembled restore image: {image_name}
{chr(10).join('  - ' + n for n in notes)}

RESTORE (serial, ROM download mode: GPIO0 low at reset, UART0 115200):
  Prefer the full serial backup taken before the first install (docs/debug_build.md step 1) if
  you made one:
    esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x0 full_16MB.bin
  Otherwise this network-assembled image (bootloader, table, nvs, otadata, stock slot, oemdata,
  coredump exactly as dumped; our slot replaced as noted above):
    esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x0 {image_name}
  Without serial access: in the debug build turn on "Allow boot switch to other slot" and press
  "Boot other slot (stock app)" (otadata switch only), see docs/debug_build.md.

The external SPI NOR (identity, usrdataout) is in extflash.bin. It is NOT part of the internal
image and is never written by any of this tooling. Keep it safe: it holds the dongle's SN/identity.
"""


# ---- network -----------------------------------------------------------------------------------
def make_opener(host: str, user: str, password: str):
    base = f"http://{host}"
    pm = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    pm.add_password(None, base, user, password)
    return urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(pm),
                                       urllib.request.HTTPBasicAuthHandler(pm)), base


def fetch(opener, url: str, timeout: float = 120.0):
    with opener.open(url, timeout=timeout) as r:
        return r.read(), {k.lower(): v for k, v in r.headers.items()}


def download(opener, base: str, out_dir: str, include_full: bool = False, only=None, retries: int = 3,
             fetch_fn=fetch, log=print):
    os.makedirs(out_dir, exist_ok=True)
    body, _ = fetch_fn(opener, f"{base}/dump/index")
    index = json.loads(body)
    ptable = None
    manifest = {"tool_version": TOOL_VERSION, "host": base, "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                "flash_size": index["flash_size"], "flash_id": index.get("flash_id"),
                "running": index.get("running"), "regions": []}
    for r in index["regions"]:
        name = r["name"]
        entry = dict(r)
        if (name in DEFAULT_SKIP and not include_full) or (only and name not in only):
            entry["file"] = None
            manifest["regions"].append(entry)
            continue
        ok = False
        for attempt in range(1, retries + 1):
            data, hdr = fetch_fn(opener, f"{base}/dump/{name}", 600.0)
            got = sha256_hex(data)
            want = hdr.get("x-sha256", "")
            ok = len(data) == r["size"] and got == want
            if ok:
                break
            log(f"  {name}: attempt {attempt}: size {len(data)}/{r['size']} sha {got[:12]} vs {want[:12]}"
                " (a live region like nvs can change between hash and read)")
        path = os.path.join(out_dir, f"{name}.bin")
        with open(path, "wb") as f:
            f.write(data)
        entry.update(file=f"{name}.bin", sha256=got, verified=ok,
                     index_sha256_match=(r.get("sha256") in ("", None) or r.get("sha256") == got))
        manifest["regions"].append(entry)
        log(f"{name:<14} @0x{r['offset']:06x} {r['size']:>9} B  {'OK' if ok else 'UNVERIFIED'}")
        if name == "ptable":
            ptable = data
    if ptable is not None:
        parts, md5_ok = parse_partition_table(ptable)
        manifest["partition_table"] = [dict(label=p[0], type=p[1], subtype=p[2], offset=p[3], size=p[4], flags=p[5])
                                       for p in parts]
        manifest["partition_table_md5_ok"] = md5_ok
        by_off = {(p[3], p[4]): p for p in parts}
        for e in manifest["regions"]:
            p = by_off.get((e["offset"], e["size"]))
            if p is not None and not e["external"]:
                e.setdefault("type", p[1])
                e.setdefault("subtype", p[2])
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    return manifest


def load_saved(out_dir: str):
    with open(os.path.join(out_dir, "manifest.json")) as f:
        manifest = json.load(f)
    data = {}
    for r in manifest["regions"]:
        if r.get("file"):
            with open(os.path.join(out_dir, r["file"]), "rb") as f:
                b = f.read()
            if r.get("sha256") and sha256_hex(b) != r["sha256"]:
                raise ValueError(f"{r['file']}: sha256 differs from manifest")
            data[r["name"]] = b
    return manifest, data


def cmd_assemble(args):
    manifest, data = load_saved(args.out)
    stock = None
    if args.stock_app:
        with open(args.stock_app, "rb") as f:
            stock = f.read()
    elif args.stock_sgu:
        with open(args.stock_sgu, "rb") as f:
            stock = sgu_app(f.read())
    img, notes = assemble(manifest, data, stock, args.otadata, data.get("flash") if args.from_full else None)
    name = f"restore_full_{manifest['flash_size'] >> 20}MB.bin"
    with open(os.path.join(args.out, name), "wb") as f:
        f.write(img)
    with open(os.path.join(args.out, "RESTORE.txt"), "w") as f:
        f.write(restore_note(manifest, name, notes))
    print(f"{name}: {len(img)} B, sha256 {sha256_hex(img)}")
    for n in notes:
        print("  -", n)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=["download", "assemble", "all"])
    ap.add_argument("--host")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", help="default: $WINET_WEB_PASSWORD, else prompt")
    ap.add_argument("--out", required=True)
    ap.add_argument("--full", action="store_true", help="also download /dump/flash (whole internal chip)")
    ap.add_argument("--only", nargs="*", help="only these regions")
    ap.add_argument("--stock-app", help="stock app image (.bin, 0xE9) for the running slot")
    ap.add_argument("--stock-sgu", help="stock .sgu: its app part fills the running slot")
    ap.add_argument("--otadata", choices=["stock", "keep"], default="stock")
    ap.add_argument("--from-full", action="store_true", help="start from flash.bin instead of 0xFF")
    args = ap.parse_args(argv)
    if args.command in ("download", "all"):
        if not args.host:
            ap.error("--host is required")
        pw = args.password or os.environ.get("WINET_WEB_PASSWORD") or getpass.getpass("web password: ")
        opener, base = make_opener(args.host, args.user, pw)
        try:
            download(opener, base, args.out, args.full, args.only)
        except urllib.error.HTTPError as e:
            print(f"HTTP {e.code}: {e.reason} (wrong password? debug build with flash_dump?)", file=sys.stderr)
            return 1
    if args.command in ("assemble", "all"):
        cmd_assemble(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
