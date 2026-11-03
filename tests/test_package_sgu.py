#!/usr/bin/env python3
"""Host tests for tools/sgu.py and tools/package_sgu.py: container parse/build round trips, the
part walk, app checks, version naming, and (when the `cryptography` module is available) full
signed builds with a THROWAWAY key generated here. Run: python3 -I tests/test_package_sgu.py"""
import os
import pathlib
import struct
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import package_sgu as ps  # noqa: E402
import sgu  # noqa: E402

FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print("FAIL", msg)


def fake_app(size=0x1000, flash_nibble=4, chip=0, version=b"WINET-SV200.001.00.P046B001-dir"):
    a = bytearray(size)
    a[0], a[1], a[2], a[3] = 0xE9, 3, 2, (flash_nibble << 4) | 0
    struct.pack_into("<H", a, 12, chip)
    struct.pack_into("<I", a, 0x20, 0xABCD5432)
    a[0x20 + 16:0x20 + 16 + len(version)] = version
    a[0x20 + 48:0x20 + 48 + 7] = b"WiNet-S"
    return bytes(a)


def fake_stock_sgu(app, web=b"W" * 64, ver=b"WINET-SV200.001.00.P046"):
    body = struct.pack("<HI", 2, len(app)) + app + struct.pack("<HI", 3, len(web)) + web
    blob = (b"0" * 32 + ver.ljust(64, b"\0") + b"WINET-SV200.001.00.B001".ljust(64, b"\0")
            + struct.pack("<I", len(body)) + body)
    import hashlib
    return hashlib.md5(blob[0x20:]).hexdigest().encode() + blob[32:]


def test_container():
    stock = fake_stock_sgu(fake_app(0x200))
    info = sgu.parse(stock)
    check(info["app_ver"] == "WINET-SV200.001.00.P046" and info["boot_ver"].endswith("B001"), "header parse")
    check([p["type"] for p in info["parts"]] == [2, 3], "part walk")
    check(info["md5_ok"], "md5 of a well-formed package")
    check(info["trailer"] is None and info["part_problems"] == [], "no trailer, no problems")
    check(len(info["app"]) == 0x200 and info["oemdata"] == b"W" * 64, "part data")
    desc = sgu.app_descriptor(info["app"])
    check(desc["version"] == "WINET-SV200.001.00.P046B001-dir" and desc["project_name"] == "WiNet-S", "app descriptor")
    check(sgu.repack_check(stock)["ok"], "repack reproduces the body")
    check(sgu.repack_check(stock, app_ver="WINET-SV200.001.00.P099")["ok"], "repack with a new version")
    # malformed bodies
    trunc = bytearray(stock)
    struct.pack_into("<I", trunc, 0xA0, len(stock))
    check(sgu.parse(bytes(trunc))["parts"] is None, "overrun detected")
    bad = bytearray(stock)
    struct.pack_into("<H", bad, 0xA4, 1)
    check(sgu.parse(bytes(bad))["part_problems"] != [], "unknown part type reported")
    # wrapped oemdata input
    raw = b"Z" * 32
    wrapped = struct.pack("<HI", 3, len(raw)) + raw
    check(sgu.oemdata_payload(wrapped) == (raw, True) and sgu.oemdata_payload(raw) == (raw, False), "oemdata payload")
    check(sgu.derive_boot_ver("WINET-SV300.001.03.P901") == "WINET-SV300.001.03.B001", "boot version")


def test_package_pure():
    check(ps.version_name("s", 901) == "WINET-SV200.001.00.P901", "version name")
    check(ps.version_name("s2", 7) == "WINET-SV300.001.03.P007", "version name s2")
    check(ps.check_app(fake_app(), "s") == [], "good app")
    check(any("16MB" in p for p in ps.check_app(fake_app(flash_nibble=2), "s")), "4MB header rejected")
    check(any("chip_id" in p for p in ps.check_app(fake_app(chip=9), "s")), "S3 image rejected for S")
    check(any("slot" in p for p in ps.check_app(fake_app(size=0x309001), "s")), "oversize rejected")
    check(ps.check_app(b"\x00" * 64, "s") != [], "bad magic rejected")
    stock = fake_stock_sgu(fake_app(0x200))
    check([p[0] for p in ps.walk_tlv(stock)] == [2, 3], "TLV walk")
    part2 = ps.stock_part2(stock)
    check(part2[:2] == b"\x03\x00" and len(part2) == 70, "stock part 2 incl. header")
    check(ps.verify_package(stock, fake_app(0x200), part2, trailer=False) == [], "verify stock-shaped body")
    check(ps.verify_package(stock, fake_app(0x200), part2) != [], "trailerless package rejected")
    check(ps.stock_version(stock) == "WINET-SV200.001.00.P046", "stock version")


def _keypair(tmp):
    priv, pub = sgu.generate_keypair()
    kp, pp = os.path.join(tmp, "k.pem"), os.path.join(tmp, "p.pem")
    with open(kp, "wb") as f:
        f.write(priv)
    with open(pp, "wb") as f:
        f.write(pub)
    return kp, pp, priv, pub


def test_full_build():
    try:
        import cryptography  # noqa: F401
    except ImportError:
        print("skip full build: no cryptography module")
        return
    with tempfile.TemporaryDirectory() as tmp:
        kp, pp, priv, pub = _keypair(tmp)
        # library level: sign + verify both regions
        blob, det = sgu.build(fake_app(0x300), b"", "WINET-SV200.001.00.P901", "WINET-SV200.001.00.B001",
                              private_key_pem=priv, embed_pubkey_pem=pub, embed_trailer=True)
        info = sgu.parse(blob)
        check(info["trailer"] is not None and info["trailer"]["consistent"], "trailer built")
        check(sgu.verify_rsa(blob, det, pub), "detached signature over the whole file")
        h0, h1 = info["hash_region"]
        check(sgu.verify_rsa(blob[h0:h1], info["trailer"]["signature"], pub), "trailer signature over the body")
        check(not sgu.verify_rsa(blob[h0:h1], det, pub), "regions are distinct")
        check(all(ok for _, ok in sgu.self_check(blob, det, pub)), "self check passes")

        # app-only package through package_sgu.py (the release shape)
        app = fake_app(0x3000)
        ap = os.path.join(tmp, "app.bin")
        with open(ap, "wb") as f:
            f.write(app)
        out = os.path.join(tmp, "dist")
        rc = ps.main(["--app", ap, "--key", kp, "--pubkey", pp, "--out-dir", out, "--build", "901", "--zip"])
        check(rc == 0, f"app-only package build rc {rc}")
        sgu_path = os.path.join(out, "WINET-SV200.001.00.P901.sgu")
        produced = open(sgu_path, "rb").read()
        check(ps.verify_package(produced, app) == [], "app-only layout + trailer")
        check(os.path.exists(os.path.join(out, "WINET_B000.WINET-SV200.001.00.P901.zip")), "zip written")
        check(ps.main(["--app", ap, "--key", kp, "--out-dir", out, "--build", "46"]) == 2, "downgrade refused")
        check(ps.main(["--app", ap, "--key", os.path.join(tmp, "missing.pem"), "--out-dir", out]) == 2,
              "missing key reported")

        # CLI: info / verify / extract on the produced package
        check(sgu.main(["info", sgu_path]) == 0, "info exits 0")
        check(sgu.main(["verify", sgu_path, "--sig", os.path.join(out, "WINET-SV200.001.00.P901.bin"),
                        "--pubkey", pp]) == 0, "verify exits 0")
        ex = os.path.join(tmp, "ex")
        check(sgu.main(["extract", sgu_path, "--out-dir", ex]) == 0, "extract exits 0")
        check(open(os.path.join(ex, "WINET-SV200.001.00.P901.app.bin"), "rb").read() == app, "extracted app identical")

        # with the stock web UI part (S2 shape)
        app2 = fake_app(0x3000, chip=9)
        with open(ap, "wb") as f:
            f.write(app2)
        stock2 = fake_stock_sgu(fake_app(0x200, chip=9), web=b"Y" * 4096, ver=b"WINET-SV300.001.03.P032")
        sp = os.path.join(tmp, "stock.sgu")
        with open(sp, "wb") as f:
            f.write(stock2)
        rc = ps.main(["--app", ap, "--board", "s2", "--stock-sgu", sp, "--key", kp, "--pubkey", pp,
                      "--out-dir", out, "--build", "34"])
        check(rc == 0, f"S2 package build rc {rc}")
        sgu2 = open(os.path.join(out, "WINET-SV300.001.03.P034.sgu"), "rb").read()
        check(ps.verify_package(sgu2, app2, ps.stock_part2(stock2)) == [], "S2 package with the web UI part")
        cut = bytes(sgu2[:0xA4 + struct.unpack_from("<I", sgu2, 0xA0)[0]])
        check(ps.check_trailer(cut) != [], "trailerless package rejected")
        ex2 = os.path.join(tmp, "ex2")
        check(sgu.main(["extract", sp, "--out-dir", ex2]) == 0, "extract a stock-shaped package")
        check(open(os.path.join(ex2, "WINET-SV300.001.03.P032.oemdata.bin"), "rb").read() == b"Y" * 4096,
              "extracted oemdata identical")

        # keygen
        kd = os.path.join(tmp, "keys")
        check(sgu.main(["keygen", "--out-dir", kd]) == 0, "keygen")
        check(oct(os.stat(os.path.join(kd, "rewired-s_priv.pem")).st_mode & 0o777) == "0o600", "private key mode 600")
        check(sgu.main(["keygen", "--out-dir", kd]) == 2, "keygen refuses to overwrite")


if __name__ == "__main__":
    test_container()
    test_package_pure()
    test_full_build()
    print(f"package_sgu: {len(FAILS)} failures")
    sys.exit(1 if FAILS else 0)
