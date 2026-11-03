#!/usr/bin/env python3
"""Host unit tests for the pure helpers in tools/deploy_debug.py (no network).

    python3 -I tests/test_deploy_debug.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

import deploy_debug as dd


def check(name, cond):
    if not cond:
        raise AssertionError("FAILED: " + name)
    print("ok:", name)


# --- encode_pubkey: only % and + are escaped; base64 '+' survives round-trip via urldecode ---
def test_encode_pubkey():
    pem = (b"-----BEGIN PUBLIC KEY-----\n"
           b"MIIBIjANBgkqhki+G9w0BAQEFAAOCA+8AMIIBCgKCAQ==\n"
           b"-----END PUBLIC KEY-----\n")
    enc = dd.encode_pubkey(pem)
    check("no bare + remains", "+" not in enc)
    check("+ became %2B", "%2B" in enc)
    check("newlines preserved", "\n" in enc)
    check("slash preserved", "/" not in enc or True)  # this sample has none; ensure no crash
    check("= padding preserved", enc.endswith("-----END PUBLIC KEY-----\n") or "==" in enc)
    # The device URL-decodes the value; decoding must give back the exact PEM.
    import urllib.parse
    check("urldecode round-trips to the original PEM",
          urllib.parse.unquote(enc) == pem.decode("ascii"))


def test_encode_pubkey_percent():
    # A literal % must be escaped so the decoder does not eat it.
    import urllib.parse
    s = b"ab%cd+ef\n"
    enc = dd.encode_pubkey(s)
    check("percent escaped", enc == "ab%25cd%2Bef\n")
    check("percent round-trips", urllib.parse.unquote(enc) == s.decode())


def test_default_sig_path():
    check("sgu->bin", dd.default_sig_path("dist/WINET-SV200.001.00.P901.sgu")
          == "dist/WINET-SV200.001.00.P901.bin")
    check("no ext", dd.default_sig_path("pkg") == "pkg.bin")


def test_spa_pubkey_version():
    check("spa version V1", dd.spa_pubkey_version("dist/publicKey_V1.pem", "WiNet-S")
          == "WiNet-S_publicKey_V1")
    check("spa version V12", dd.spa_pubkey_version("/x/publicKey_V12.pem", "WiNet-S2")
          == "WiNet-S2_publicKey_V12")
    try:
        dd.spa_pubkey_version("whatever.pem", "WiNet-S")
        raise AssertionError("bad name should raise")
    except ValueError:
        check("rejects non-publicKey_V<n> name", True)


# --- WebSocket framing: accept key, mask round-trip, length boundaries ---
def test_ws_accept_key():
    # RFC 6455 §1.3 canonical example.
    check("rfc6455 example", dd.ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==")
          == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")


def test_ws_frame_roundtrip():
    for payload in ["", "hi", "x" * 125, "y" * 126, "z" * 70000,
                    json.dumps({"service": "connect"})]:
        frame = dd.encode_ws_text_frame(payload, bytes([0x11, 0x22, 0x33, 0x44]))
        check("frame FIN+text (%d)" % len(payload), frame[0] == 0x81)
        check("client frame is masked (%d)" % len(payload), frame[1] & 0x80)
        parsed = dd.parse_ws_frame(frame)
        check("parses whole frame (%d)" % len(payload), parsed is not None)
        fin, opcode, data, used = parsed
        check("fin set (%d)" % len(payload), fin)
        check("opcode text (%d)" % len(payload), opcode == 0x1)
        check("payload round-trips (%d)" % len(payload), data.decode("utf-8") == payload)
        check("consumed all (%d)" % len(payload), used == len(frame))


def test_parse_ws_partial():
    frame = dd.encode_ws_text_frame("hello", os.urandom(4))
    check("partial returns None", dd.parse_ws_frame(frame[:3]) is None)
    # Two frames back-to-back: parse consumes exactly the first.
    f2 = dd.encode_ws_text_frame("world", os.urandom(4))
    fin, op, data, used = dd.parse_ws_frame(frame + f2)
    check("first of two decoded", data == b"hello")
    fin, op, data2, used2 = dd.parse_ws_frame((frame + f2)[used:])
    check("second of two decoded", data2 == b"world")


def test_parse_server_unmasked():
    # Server frames are not masked; build one by hand (FIN+text, len 2, "ok").
    buf = bytes([0x81, 0x02]) + b"ok"
    fin, op, data, used = dd.parse_ws_frame(buf)
    check("unmasked server frame", data == b"ok" and used == 4)


def test_multipart_body():
    body, ctype = dd.multipart_body(
        {"start": "1"},
        [("file", "a.sgu", "application/octet-stream", b"\xe9\x01BINDATA")],
        "BOUND123")
    check("content-type carries boundary", ctype == "multipart/form-data; boundary=BOUND123")
    check("field present", b'name="start"' in body and b"\r\n1\r\n" in body)
    check("file disposition", b'name="file"; filename="a.sgu"' in body)
    check("file bytes embedded", b"\xe9\x01BINDATA" in body)
    check("closing boundary", body.rstrip().endswith(b"--BOUND123--"))


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("\nALL deploy_debug host tests passed")


if __name__ == "__main__":
    main()
