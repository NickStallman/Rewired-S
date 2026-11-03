#!/usr/bin/env python3
"""Host unit tests for the pure helpers in tools/deploy_node.py and tools/syslog_receiver.py.

    python3 -I tests/test_deploy_node.py
"""
import datetime as dt
import json
import os
import sys
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

import deploy_node as dn  # noqa: E402
import syslog_receiver as sr  # noqa: E402


def check(name, cond):
    if not cond:
        raise AssertionError("FAILED: " + name)
    print("ok:", name)


def test_hash():
    check("version text sensor -> hash",
          dn.parse_version_hash("2026.9.1 (config hash 0xBFDD4F7F)") == "0xbfdd4f7f")
    check("no hash -> None", dn.parse_version_hash("2026.9.1") is None)
    check("format_hash pads", dn.format_hash(0xa8262a64) == "0xa8262a64" and dn.format_hash(0x1f) == "0x0000001f")
    check("build_info int hash", dn.format_hash(2821073508) == "0xa8262a64")
    name = dn.archive_name(1792529046, "0xA8262A64")
    check("archive name ends with lower-case hash", name.endswith("_0xa8262a64") and len(name.split("_")[0]) == 15)


def test_crash_lines():
    log = "\n".join([
        "07:53:45 [E][esp32.crash:436]: *** CRASH DETECTED ON PREVIOUS BOOT ***",
        "07:53:45 [E][esp32.crash:450]:   PC:  0x42012345  (fault location)",
        "07:53:45 [E][esp32.crash:359]:   BT0: 0x4037ABCD  (stack)",
        "07:53:45 [E][esp32.crash:404]:   bt1: 0x42012345",
        "Backtrace: 0x40375a2e:0x3fcf1230 0x4200f00d:0x3fcf1250",
        "07:53:46 [D][sungrow_modbus:067]: dev 0 addr 1 online 0x42000000",  # no crash keyword
        "07:53:46 [I][app]: heap at 0x3fc90000 panic",                     # keyword, no code address
    ])
    lines = dn.crash_lines(log)
    check("crash lines picked", len(lines) == 4)
    addrs = dn.code_addresses(lines)
    check("unique code addresses in order",
          addrs == ["0x42012345", "0x4037abcd", "0x40375a2e", "0x4200f00d"])
    check("stack pointers (0x3fc..) are not code", all(not a.startswith("0x3f") for a in addrs))


def test_build_refs_and_pick():
    text = "journal: build 0xA8262A64 build_time 1792529046\nconfig hash 0xbfdd4f7f"
    hashes, times = dn.build_refs(text)
    check("hash refs", {"0xa8262a64", "0xbfdd4f7f"} <= hashes)
    check("time refs", times == {1792529046})
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        a = root / "20261021-074406_0xa8262a64"
        b = root / "20261021-073017_0x11111111"
        c = root / "20261021-070000_0x22222222"
        for p in (a, b, c):
            p.mkdir()
        (b / "build_info.json").write_text(json.dumps({"build_time": 1792529046}))
        picked = dn.pick_archives([a, b, c], {"0xa8262a64"}, {1792529046})
        check("pick by hash and by build time", picked == [a, b])
        check("no refs -> none", dn.pick_archives([a, b, c], set(), set()) == [])


def test_secrets():
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "secrets.yaml"
        p.write_text('web_username: "admin"\nweb_password: s3cr3t\n# comment\n')
        s = dn.read_secrets(p)
        check("quoted and bare values", s == {"web_username": "admin", "web_password": "s3cr3t"})


def test_syslog_format():
    now = dt.datetime(2026, 10, 21, 7, 55, 9, 698000)
    line = sr.format_line(b"<135>Oct 21 07:55:09 rewired-s sungrow_modbus: dev 9 online\x00", now)
    check("pri 135 -> DEBUG", " DEBUG " in line)
    check("payload kept, PRI stripped", line.endswith("rewired-s sungrow_modbus: dev 9 online\n"))
    check("receive timestamp", line.startswith("2026-10-21 07:55:09.698"))
    check("no PRI -> '-'", sr.format_line(b"plain", now).split()[2] == "-")
    with tempfile.TemporaryDirectory() as d:
        sink = sr.Sink(Path(d))
        sink.write("192.0.2.4", "a\n", now)
        sink.write("192.0.2.4", "b\n", now + dt.timedelta(days=1))
        files = sorted(p.name for p in (Path(d) / "192.0.2.4").iterdir())
        check("daily rotation per host", files == ["2026-10-21.log", "2026-10-22.log"])


if __name__ == "__main__":
    test_hash()
    test_crash_lines()
    test_build_refs_and_pick()
    test_secrets()
    test_syslog_format()
    print("all deploy_node / syslog_receiver tests passed")
