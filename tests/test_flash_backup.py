#!/usr/bin/env python3
"""Host tests for tools/flash_backup.py: manifest/download logic (fake transport, no sockets),
partition table + otadata helpers, restore-image assembly. Run: python3 -I tests/test_flash_backup.py"""
import hashlib
import json
import os
import pathlib
import struct
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import flash_backup as fb  # noqa: E402

FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print("FAIL", msg)


MB = 1 << 20
FLASH = 16 * MB
LAYOUT = [  # name, label, type, subtype, offset, size, external
    ("nvs", "nvs", 1, 2, 0x9000, 0x4000, False),
    ("otadata", "otadata", 1, 0, 0xD000, 0x2000, False),
    ("phy", "phy_init", 1, 1, 0xF000, 0x1000, False),
    ("inactive_app", "ota_0", 0, 0x10, 0x10000, 0x400000, False),
    ("running_app", "ota_1", 0, 0x11, 0x410000, 0x400000, False),
    ("oemdata", "oemdata", 1, 0x81, 0x810000, 0x300000, False),
    ("coredump", "coredump", 1, 3, 0xB10000, 0x10000, False),
    ("extflash", "usrdataout", 1, 0x81, 0, 0x100000, True),
]


def ptable_bytes():
    b = bytearray()
    for _, label, t, st, off, size, ext in LAYOUT:
        if ext:
            continue
        b += struct.pack("<HBBII", 0x50AA, t, st, off, size) + label.encode().ljust(16, b"\0") + b"\0" * 4
    b += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(bytes(b)).digest()
    return bytes(b) + b"\xff" * (0xC00 - len(b) - 0)


class FakeDevice:
    """Serves /dump/index and /dump/<region> like the firmware (deterministic contents)."""

    def __init__(self, flaky=None):
        self.regions = [("bootloader", "", 0, 0, 0x1000, 0x7000, False), ("ptable", "", 0, 0, 0x8000, 0xC00, False)]
        self.regions += [(n, l, t, st, o, s, e) for n, l, t, st, o, s, e in LAYOUT]
        self.regions.append(("flash", "", 0, 0, 0, FLASH, False))
        self.data = {}
        for n, l, t, st, o, s, e in self.regions:
            if n == "ptable":
                self.data[n] = ptable_bytes()
            elif n == "flash":
                continue
            elif n == "otadata":
                d = bytearray(b"\xff" * s)
                d[0:32] = fb.otadata_entry(1)
                d[0x1000:0x1020] = fb.otadata_entry(2)  # running = ota_1 (seq 2)
                self.data[n] = bytes(d)
            else:
                self.data[n] = bytes([(o >> 12) & 0xFF]) * s
        self.flaky = dict(flaky or {})
        self.calls = []

    def fetch(self, opener, url, timeout=0):
        name = url.rsplit("/", 1)[1]
        self.calls.append(name)
        if name == "index":
            regs = [dict(name=n, label=l, offset=o, size=s, external=e,
                         sha256=hashlib.sha256(self.data[n]).hexdigest() if n in self.data else "", note="")
                    for n, l, t, st, o, s, e in self.regions]
            return json.dumps(dict(flash_size=FLASH, flash_id="0x204018", running="ota_1",
                                   running_offset=0x410000, regions=regs)).encode(), {}
        d = self.data[name]
        sha = hashlib.sha256(d).hexdigest()
        if self.flaky.get(name, 0) > 0:  # live region changed between hash and read
            self.flaky[name] -= 1
            d = b"\x00" + d[1:]
        return d, {"x-sha256": sha}


def test_helpers():
    check(fb._rom_crc(0xFFFFFFFF, struct.pack("<I", 1)) == 0x4743989A, "otadata crc seq 1")
    parts, md5_ok = fb.parse_partition_table(ptable_bytes())
    check(len(parts) == 7 and md5_ok is True, "ptable parse + md5")
    check(parts[4][0] == "ota_1" and parts[4][3] == 0x410000, "ptable entry")
    bad = bytearray(ptable_bytes())
    bad[12] ^= 1
    check(fb.parse_partition_table(bytes(bad))[1] is False, "ptable md5 mismatch")
    od = fb.make_otadata(0x2000, 0, 2, prev_seq=2)
    e = fb.parse_otadata(od)
    check(e[0]["valid"] and e[0]["seq"] == 3 and (e[0]["seq"] - 1) % 2 == 0, "make_otadata selects ota_0")
    check(e[1]["valid"] is False, "second otadata sector erased")
    od1 = fb.parse_otadata(fb.make_otadata(0x2000, 1, 2, prev_seq=2))
    check(od1[0]["seq"] == 4, "make_otadata selects ota_1")
    app = b"\xe9" + b"A" * 99
    web = struct.pack("<HI", 3, 5) + b"WEBUI"
    body = struct.pack("<HI", 2, len(app)) + app + web
    sgu = b"0" * 32 + b"\0" * 128 + struct.pack("<I", len(body)) + body
    check(fb.sgu_app(sgu) == app, "sgu_app extracts part 1")
    try:
        fb.sgu_app(sgu[:0xA4] + struct.pack("<H", 1) + sgu[0xA6:])
        check(False, "sgu_app must reject a non-app part 1")
    except ValueError:
        pass


def test_download_and_assemble():
    dev = FakeDevice(flaky={"nvs": 1})
    logs = []
    with tempfile.TemporaryDirectory() as tmp:
        m = fb.download(None, "http://fake", tmp, fetch_fn=dev.fetch, log=logs.append)
        check("flash" not in dev.calls, "whole flash skipped by default")
        check(dev.calls.count("nvs") == 2, "nvs retried after a sha mismatch")
        regs = {r["name"]: r for r in m["regions"]}
        check(all(regs[n]["verified"] for n in regs if regs[n].get("file")), "all regions verified")
        check(regs["running_app"]["type"] == 0 and regs["inactive_app"]["subtype"] == 0x10, "types from ptable")
        check(m["partition_table_md5_ok"] is True, "manifest md5 ok")
        check(os.path.exists(os.path.join(tmp, "manifest.json")) and os.path.exists(os.path.join(tmp, "extflash.bin")),
              "files written")
        manifest, data = fb.load_saved(tmp)
        # 1) no stock app: our slot erased, otadata -> ota_0
        img, notes = fb.assemble(manifest, data)
        check(len(img) == FLASH, "image size")
        check(img[0x8000:0x8C00] == data["ptable"], "ptable placed")
        check(img[0x10000:0x410000] == data["inactive_app"], "stock slot kept")
        check(img[0x410000:0x810000] == b"\xff" * 0x400000, "running slot erased")
        check(img[0:0x1000] == b"\xff" * 0x1000, "0x0..0x1000 erased")
        e = fb.parse_otadata(img[0xD000:0xF000])
        check(e[0]["valid"] and (e[0]["seq"] - 1) % 2 == 0 and e[0]["seq"] > 2, "otadata boots ota_0")
        check(any("ota_0" in n for n in notes), "note about otadata")
        # external flash never in the internal image
        check(img[0:0x100000].count(bytes([0])) < 0x100000, "ext data not placed at 0")
        # 2) stock app fills the running slot, otadata kept
        stock = b"\xe9" + b"S" * 1000
        img2, _ = fb.assemble(manifest, data, stock, "keep")
        check(img2[0x410000:0x410000 + 1001] == stock and img2[0x410000 + 1001] == 0xFF, "stock app in our slot")
        check(img2[0xD000:0xF000] == data["otadata"], "otadata kept")
        try:
            fb.assemble(manifest, data, b"\x00bad")
            check(False, "non-image stock app must be rejected")
        except ValueError:
            pass
        note = fb.restore_note(manifest, "restore_full_16MB.bin", notes)
        check("write_flash 0x0 restore_full_16MB.bin" in note and "extflash.bin" in note, "restore note")
        # tampered file is detected
        with open(os.path.join(tmp, "nvs.bin"), "r+b") as f:
            f.write(b"\x42")
        try:
            fb.load_saved(tmp)
            check(False, "tampered region must fail")
        except ValueError:
            pass


if __name__ == "__main__":
    test_helpers()
    test_download_and_assemble()
    print(f"flash_backup: {len(FAILS)} failures")
    sys.exit(1 if FAILS else 0)
