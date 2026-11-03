#!/usr/bin/env python3
"""Parse a dumped WiNet sigdata file (dev_file_247_<productType>_2) and print the cloud identity.

The file is the stock DM "logger" device file from the external NOR (/usrdataout/log/sigdata/,
docs/cloud_identity.md): WiNet-S2 `dev_file_247_1824_2`, WiNet-S `dev_file_247_1813_2`.
Concatenated records `u16 id | u16 len | data[len]`, little-endian, no header/CRC, file <= 0xFFFFE.
Record length cap per board (--board): S2 0x420, S 0x100; an over-cap header is skipped stock-style
(len := 0, continue 4 B on). A record is used only when its len equals the sig's declared size; the
first match wins; missing records fall back to the stock defaults.
Same semantics as components/sungrow_extflash/sigdata.cpp.

Input is either the raw file, or (--from-log) an ESPHome log capture of the
"Dump identity file" button (lines `DUMP BEGIN <path> ...`, `DUMP <offset>: <hex>`, `DUMP END ...`).

Stdlib only. Run with `python3 -I tools/read_identity.py <file>`.

Examples:
  python3 -I tools/read_identity.py dev_file_247_1824_2
  python3 -I tools/read_identity.py --from-log esphome.log --save dev_file_247_1824_2
  python3 -I tools/read_identity.py dev_file_247_1824_2 --secrets >> secrets.yaml
  python3 -I tools/read_identity.py --board s dev_file_247_1813_2
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass, field

# Per-board stock constants.
BOARDS = {
    "s2": {"product_type": 0x720, "max_record_len": 0x420},  # WiNet-S2 (SV300), 1824
    "s": {"product_type": 0x715, "max_record_len": 0x100},  # WiNet-S (V200), 1813
}
MAX_RECORD_LEN = BOARDS["s2"]["max_record_len"]


def sigdata_name(product_type: int) -> str:
    return f"dev_file_247_{product_type}_2"

MAX_FILE_SIZE = 0xFFFFE
DEFAULT_MQTT_KEY = b"Sun#0*2Pow925Mdk"

# id -> (name, declared len, kind, default). kind: str | u16 | u32 | key
SIGS: dict[int, tuple[str, int, str, object]] = {
    0x0002: ("sn", 20, "str", None),
    0x0004: ("product_type", 2, "u16", 0x720),
    0x0020: ("encrypt_mode", 2, "u16", 0),
    0x0021: ("key_number", 2, "u16", 0),
    0x0022: ("compress_mode", 2, "u16", 1),
    0x0023: ("ssl_flag", 2, "u16", 2),
    0x0024: ("old_ssl_flag", 2, "u16", 2),
    0x1013: ("mqtt_key", 16, "key", DEFAULT_MQTT_KEY),
    0x1015: ("rundata_period", 2, "u16", 5),
    0x1017: ("silence_enable", 2, "u16", 0),
    0x1018: ("silence_start", 2, "u16", 0),
    0x1019: ("silence_end", 2, "u16", 0),
    0x1071: ("broker_host", 64, "str", "iot.isolarcloud.com"),
    0x1072: ("broker_port", 2, "u16", 16668),
    0x107A: ("timezone_id", 2, "u16", 8),
    0x1957: ("old_broker_host", 64, "str", "iot.isolarcloud.com"),
    0x1958: ("old_broker_port", 2, "u16", 16668),
    0x1959: ("old_mqtt_key", 16, "key", DEFAULT_MQTT_KEY),
    0x6492: ("remote_ctl_mask", 4, "u32", 0x4FF),
}

# Other 0xF7 sigs worth naming in the record table (sizes from ext_flash.md §2.3; not validated).
OTHER_NAMES = {
    0x0001: "model",
    0x0005: "product_name",
    0x1000: "app_version",
    0x1014: "web_key",
    0x1053: "wifi_sta_ssid",
    0x1054: "wifi_sta_password",
    0x1057: "wifi_ap_password",
    0x1058: "wifi_ap_ssid",
    0x1073: "alt_link_host",
    0x1074: "alt_link_port",
    0x1077: "ntp_host",
    0x1966: "wifi_sta_ssid_2",
    0x1967: "wifi_sta_password_2",
    0x1A11: "pbkdf2_salt",
    0x1A12: "pbkdf2_salt_2",
}


@dataclass
class Record:
    id: int
    length: int
    offset: int
    data: bytes
    ignored: bool = False  # len != declared size
    shadowed: bool = False  # a previous valid record with this id wins


@dataclass
class Sigdata:
    records: list[Record] = field(default_factory=list)
    truncated: bool = False
    oversize: bool = False
    resyncs: int = 0  # over-cap headers skipped (len := 0, +4 B)
    size: int = 0
    parsed: int = 0

    def find(self, sig_id: int) -> Record | None:
        for r in self.records:
            if r.id == sig_id and not r.ignored:
                return r
        return None


def parse(data: bytes, max_record_len: int = MAX_RECORD_LEN) -> Sigdata:
    out = Sigdata(size=len(data))
    if len(data) > MAX_FILE_SIZE:
        data = data[:MAX_FILE_SIZE]
        out.oversize = True
    pos = 0
    seen: set[int] = set()
    while pos < len(data):
        if len(data) - pos < 4:
            out.truncated = True
            break
        sig_id, length = struct.unpack_from("<HH", data, pos)
        if length > max_record_len:  # stock: treat len as 0 and resync 4 B on
            out.resyncs += 1
            pos += 4
            continue
        if len(data) - pos - 4 < length:
            out.truncated = True
            break
        rec = Record(sig_id, length, pos + 4, bytes(data[pos + 4 : pos + 4 + length]))
        if sig_id in SIGS and SIGS[sig_id][1] != length:
            rec.ignored = True
        elif sig_id in seen:
            rec.shadowed = True
        else:
            seen.add(sig_id)
        out.records.append(rec)
        pos += 4 + length
    out.parsed = pos
    return out


def nul_trim(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("latin-1")


def sn_plausible(sn: str) -> bool:
    return 0 < len(sn) <= 20 and all(0x20 < ord(c) < 0x7F for c in sn)


def decode_value(kind: str, b: bytes):
    if kind == "u16":
        return struct.unpack("<H", b)[0]
    if kind == "u32":
        return struct.unpack("<I", b)[0]
    if kind == "str":
        return nul_trim(b)
    return b  # key


def identity(sd: Sigdata, default_product_type: int = 0x720) -> dict[str, dict]:
    """name -> {value, source ('file'|'default'), id}"""
    out: dict[str, dict] = {}
    for sig_id, (name, _length, kind, default) in SIGS.items():
        if name == "product_type":
            default = default_product_type
        rec = sd.find(sig_id)
        value, source = default, "default"
        if rec is not None:
            v = decode_value(kind, rec.data)
            # sigdata.cpp keeps the default for an all-NUL host string (sn is taken as-is). Deliberate
            # difference from stock, which would connect to "" (docs/cloud_identity.md, I8).
            if not (kind == "str" and v == "" and name != "sn"):
                value, source = v, "file"
        out[name] = {"id": sig_id, "value": value, "source": source}
    return out


def fmt_value(v) -> str:
    if isinstance(v, bytes):
        txt = v.decode("latin-1")
        printable = all(0x20 <= c < 0x7F for c in v)
        return f"{v.hex()}" + (f' ("{txt}")' if printable else "")
    if isinstance(v, int):
        return f"{v} (0x{v:X})"
    return repr(v)


def preview(rec: Record) -> str:
    b = rec.data
    if b and all(0x20 <= c < 0x7F or c == 0 for c in b) and b[0] != 0:
        return '"' + nul_trim(b) + '"'
    if len(b) == 2:
        return str(struct.unpack("<H", b)[0])
    if len(b) == 4:
        return f"0x{struct.unpack('<I', b)[0]:08X}"
    h = b[:24].hex()
    return h + ("..." if len(b) > 24 else "")


def print_report(sd: Sigdata, ident: dict, show_records: bool) -> None:
    print(f"file: {sd.size} B, {len(sd.records)} records, parsed {sd.parsed} B"
          + (", TRUNCATED/CORRUPT TAIL" if sd.truncated else "")
          + (f", {sd.resyncs} over-cap header(s) skipped" if sd.resyncs else "")
          + (", OVERSIZE (>0xFFFFE)" if sd.oversize else ""))
    if show_records:
        print("\nrecords:")
        print(f"  {'off':>6}  {'id':>6}  {'len':>4}  {'name':<20} value")
        for r in sd.records:
            name = SIGS[r.id][0] if r.id in SIGS else OTHER_NAMES.get(r.id, "")
            flag = " [IGNORED: len != %d]" % SIGS[r.id][1] if r.ignored else (" [shadowed]" if r.shadowed else "")
            print(f"  {r.offset - 4:6X}  0x{r.id:04X}  {r.length:4d}  {name:<20} {preview(r)}{flag}")
    sn = ident["sn"]["value"]
    print("\nidentity:")
    for name, e in ident.items():
        print(f"  0x{e['id']:04X} {name:<16} {fmt_value(e['value']) if e['value'] is not None else '-':<44} {e['source']}")
    print(f"\nSN valid: {'yes' if sn and sn_plausible(sn) else 'NO'}")


def yaml_str(s: str) -> str:
    return json.dumps(s)  # JSON strings are valid YAML double-quoted scalars


def secrets_snippet(ident: dict, include_defaults: bool, source: str = "the sigdata file") -> str:
    lines = [f"# iSolarCloud identity read from {source} by tools/read_identity.py"]

    def emit(key: str, name: str, conv=lambda v: v):
        e = ident[name]
        if e["value"] is None:
            return
        val = conv(e["value"])
        text = yaml_str(val) if isinstance(val, str) else str(val)
        prefix = "" if (e["source"] == "file" or include_defaults) else "# (default) "
        lines.append(f"{prefix}isolarcloud_{key}: {text}")

    emit("sn", "sn")
    emit("broker_host", "broker_host")
    emit("broker_port", "broker_port")
    emit("mqtt_key", "mqtt_key", lambda b: b.decode("latin-1"))
    emit("ssl_flag", "ssl_flag")
    emit("encrypt", "encrypt_mode")
    emit("keynum", "key_number")
    emit("compress", "compress_mode")
    emit("product_type", "product_type")
    emit("timezone_id", "timezone_id")
    emit("old_broker_host", "old_broker_host")
    emit("old_broker_port", "old_broker_port")
    emit("old_mqtt_key", "old_mqtt_key", lambda b: b.decode("latin-1"))
    emit("old_ssl_flag", "old_ssl_flag")
    return "\n".join(lines) + "\n"


_LOG_BEGIN = re.compile(r"DUMP BEGIN (\S+)")
_LOG_LINE = re.compile(r"DUMP ([0-9a-fA-F]{6,8}): ([0-9a-fA-F]*)")
_LOG_END = re.compile(r"DUMP END (\S+) bytes=(\d+)")


def from_log(text: str) -> tuple[bytes, str]:
    """Reassemble the last complete dump in an ESPHome log capture."""
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)
    dumps: list[tuple[str, bytes]] = []
    cur: bytearray | None = None
    path = ""
    for line in text.splitlines():
        if m := _LOG_BEGIN.search(line):
            cur, path = bytearray(), m.group(1)
        elif cur is not None and (m := _LOG_LINE.search(line)):
            off = int(m.group(1), 16)
            if off != len(cur):
                raise SystemExit(f"log dump of {path}: gap/out-of-order at offset 0x{off:X} (have 0x{len(cur):X})")
            cur += bytes.fromhex(m.group(2))
        elif cur is not None and (m := _LOG_END.search(line)):
            if int(m.group(2)) != len(cur):
                raise SystemExit(f"log dump of {path}: END says {m.group(2)} bytes, got {len(cur)}")
            if "READ ERROR" in line:
                raise SystemExit(f"log dump of {path}: device reported a read error")
            dumps.append((path, bytes(cur)))
            cur = None
    if not dumps:
        raise SystemExit("no complete DUMP BEGIN/END block found in the log")
    path, data = dumps[-1]
    return data, path


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("file", help="sigdata file, or log capture with --from-log ('-' = stdin)")
    ap.add_argument("--from-log", action="store_true", help="input is an ESPHome log with a DUMP block")
    ap.add_argument("--save", metavar="OUT", help="write the reassembled/raw file bytes to OUT")
    ap.add_argument("--records", action="store_true", help="also list every record")
    ap.add_argument("--secrets", action="store_true", help="print only a secrets.yaml snippet")
    ap.add_argument("--include-defaults", action="store_true",
                    help="with --secrets: emit default values uncommented too")
    ap.add_argument("--json", action="store_true", help="print the identity as JSON")
    ap.add_argument("--board", choices=sorted(BOARDS), default="s2",
                    help="dongle the file came from: sets the record length cap and default productType "
                         "(s2: 0x420 / 1824, s: 0x100 / 1813; default s2)")
    ap.add_argument("--product-type", type=lambda s: int(s, 0), default=None,
                    help="default productType when 0x0004 is absent (default: from --board)")
    args = ap.parse_args(argv)

    if args.file == "-":
        raw = sys.stdin.buffer.read()
    else:
        with open(args.file, "rb") as f:
            raw = f.read()
    src = args.file
    if args.from_log:
        raw, src = from_log(raw.decode("utf-8", "replace"))
        print(f"# reassembled {len(raw)} B of {src} from log", file=sys.stderr)
    if args.save:
        with open(args.save, "wb") as f:
            f.write(raw)
        print(f"# wrote {args.save}", file=sys.stderr)

    board = BOARDS[args.board]
    product_type = board["product_type"] if args.product_type is None else args.product_type
    sd = parse(raw, board["max_record_len"])
    ident = identity(sd, product_type)
    if args.secrets:
        name = src.rsplit("/", 1)[-1] if src not in ("-", "") else sigdata_name(product_type)
        sys.stdout.write(secrets_snippet(ident, args.include_defaults, name))
    elif args.json:
        out = {k: {**v, "value": v["value"].decode("latin-1") if isinstance(v["value"], bytes) else v["value"]}
               for k, v in ident.items()}
        print(json.dumps({"source": src, "board": args.board, "truncated": sd.truncated, "resyncs": sd.resyncs,
                          "identity": out}, indent=2))
    else:
        print_report(sd, ident, args.records)
    sn = ident["sn"]["value"]
    return 0 if sn and sn_plausible(sn) else 2


if __name__ == "__main__":
    sys.exit(main())
