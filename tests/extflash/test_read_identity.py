"""Tests for tools/read_identity.py (stdlib unittest). Run: python3 -I tests/extflash/test_read_identity.py"""

import io
import os
import struct
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import read_identity as ri  # noqa: E402


def rec(sig_id: int, data: bytes) -> bytes:
    return struct.pack("<HH", sig_id, len(data)) + data


def s(text: str, n: int) -> bytes:
    return text.encode().ljust(n, b"\0")


def u16(v: int) -> bytes:
    return struct.pack("<H", v)


SAMPLE = (
    rec(0x0002, s("B2101234567", 20))
    + rec(0x1071, s("gateway.isolarcloud.eu", 64))
    + rec(0x1072, u16(19999))
    + rec(0x1013, b"ABCDEFGHIJKLMNOP")
    + rec(0x1072, u16(1))  # duplicate, shadowed
    + rec(0x0023, b"\x01\x00\x00")  # wrong len, ignored
    + rec(0x1053, s("MyWifi", 32))
)


class ReadIdentityTest(unittest.TestCase):
    def test_parse_and_identity(self):
        sd = ri.parse(SAMPLE)
        self.assertFalse(sd.truncated)
        self.assertEqual(len(sd.records), 7)
        ident = ri.identity(sd)
        self.assertEqual(ident["sn"]["value"], "B2101234567")
        self.assertEqual(ident["broker_host"]["value"], "gateway.isolarcloud.eu")
        self.assertEqual(ident["broker_port"]["value"], 19999)
        self.assertEqual(ident["mqtt_key"]["value"], b"ABCDEFGHIJKLMNOP")
        self.assertEqual(ident["ssl_flag"], {"id": 0x23, "value": 2, "source": "default"})
        self.assertEqual(ident["compress_mode"]["value"], 1)
        self.assertTrue(sd.records[4].shadowed)
        self.assertTrue(sd.records[5].ignored)

    def test_truncated_and_empty(self):
        sd = ri.parse(SAMPLE[:-5])
        self.assertTrue(sd.truncated)
        self.assertEqual(len(sd.records), 6)
        sd = ri.parse(b"")
        self.assertEqual(sd.records, [])
        self.assertIsNone(ri.identity(sd)["sn"]["value"])

    def test_caps_and_resync(self):
        big = rec(0x177B, b"a" * 0x101) + rec(0x0002, s("AFTERBIG", 20))
        sd = ri.parse(big)  # S2 cap 0x420: fine
        self.assertEqual(sd.resyncs, 0)
        self.assertEqual(ri.identity(sd)["sn"]["value"], "AFTERBIG")
        sd = ri.parse(big, ri.BOARDS["s"]["max_record_len"])  # S cap 0x100: header skipped
        self.assertGreaterEqual(sd.resyncs, 1)
        self.assertFalse(any(r.id == 0x177B for r in sd.records))
        ok = rec(0x177B, b"a" * 0x100) + rec(0x0002, s("AFTERBIG", 20))
        sd = ri.parse(ok, 0x100)
        self.assertEqual(sd.resyncs, 0)
        self.assertEqual(ri.identity(sd)["sn"]["value"], "AFTERBIG")
        # Hidden record right after an over-cap header is found after the resync (stock behaviour).
        hidden = struct.pack("<HH", 0x1234, 0x421) + rec(0x1072, u16(4242))
        sd = ri.parse(hidden)
        self.assertEqual(sd.resyncs, 1)
        self.assertFalse(sd.truncated)
        self.assertEqual(ri.identity(sd)["broker_port"]["value"], 4242)
        # Erased tail: skipped, not truncated.
        sd = ri.parse(rec(0x1072, u16(7)) + b"\xff" * 64)
        self.assertEqual((sd.resyncs, sd.truncated, len(sd.records)), (16, False, 1))

    def test_winet_s_defaults(self):
        sd = ri.parse(rec(0x0002, s("A2291234567", 20)), 0x100)
        ident = ri.identity(sd, ri.BOARDS["s"]["product_type"])
        self.assertEqual(ident["product_type"], {"id": 4, "value": 1813, "source": "default"})
        self.assertEqual(ident["remote_ctl_mask"]["value"], 0x4FF)  # no 0x6492 on the S
        self.assertEqual(ri.sigdata_name(1813), "dev_file_247_1813_2")
        self.assertEqual(ri.sigdata_name(0x720), "dev_file_247_1824_2")

    def test_secrets(self):
        out = ri.secrets_snippet(ri.identity(ri.parse(SAMPLE)), include_defaults=False)
        self.assertIn('isolarcloud_sn: "B2101234567"', out)
        self.assertIn("isolarcloud_broker_port: 19999", out)
        self.assertIn('isolarcloud_mqtt_key: "ABCDEFGHIJKLMNOP"', out)
        self.assertIn("# (default) isolarcloud_ssl_flag: 2", out)

    def test_from_log(self):
        lines = ["[I][sungrow_extflash:1]: DUMP BEGIN /usrdataout/log/sigdata/dev_file_247_1824_2 size=%d" % len(SAMPLE)]
        for off in range(0, len(SAMPLE), 32):
            lines.append("[I][sungrow_extflash:2]: DUMP %06x: %s" % (off, SAMPLE[off : off + 32].hex()))
        lines.append("[I][sungrow_extflash:3]: DUMP END /usrdataout/log/sigdata/dev_file_247_1824_2 bytes=%d" % len(SAMPLE))
        data, path = ri.from_log("\n".join(lines))
        self.assertEqual(data, SAMPLE)
        self.assertTrue(path.endswith("dev_file_247_1824_2"))

    def test_cli(self):
        tmp = Path(os.environ.get("TMPDIR", "/tmp")) / f"ri_test_{os.getpid()}"
        tmp.write_bytes(SAMPLE)
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = ri.main([str(tmp), "--records"])
            self.assertEqual(rc, 0)
            self.assertIn("SN valid: yes", buf.getvalue())
            self.assertIn("IGNORED", buf.getvalue())
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = ri.main([str(tmp), "--board", "s", "--json"])
            self.assertEqual(rc, 0)
            out = buf.getvalue()
            self.assertIn('"board": "s"', out)
            self.assertIn('"value": 1813', out)
        finally:
            tmp.unlink()


if __name__ == "__main__":
    unittest.main(verbosity=1)
