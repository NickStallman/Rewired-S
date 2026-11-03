#!/usr/bin/env python3
"""Sungrow WiNet `.sgu` firmware packages: inspect, extract, verify, build and sign.

The container format and the signature scheme are documented in SG-Docs:
https://github.com/NickStallman/SG-Docs/blob/main/reference/firmware-update/sgu-container.md
(`#byte-layout`, `#the-signature-trailer`, `#verification`, `#the-winet-s2-signature-rule`,
`#distribution`) and the update channels in
https://github.com/NickStallman/SG-Docs/blob/main/reference/firmware-update/update-channels.md.

Layout (all little-endian):

    0x00  32 B   ASCII-hex MD5 of [0x20, EOF)
    0x20  64 B   app version, e.g. "WINET-SV300.001.03.P032", NUL padded
    0x60  64 B   bootloader version, e.g. "WINET-SV300.001.03.B001", NUL padded
    0xA0  u32    total_len: length of the part list that starts at 0xA4
    0xA4  parts  {u16 type, u32 len, data[len]} ... tiling [0xA4, 0xA4 + total_len) exactly
          type 2 = ESP app image (written to the inactive OTA slot)
          type 3 = raw image for the `oemdata` partition (the stock web UI, 3 MB LittleFS)
    ----  optional signature trailer, not covered by total_len
    +0    u16    0x0002
    +2    u32    trailer_total = 12 + siglen + keylen + xlen
    +6    u32    siglen (256 for RSA-2048)
    +10   u32    keylen (embedded PEM public key, may be 0)
    +14   u32    xlen (0)
    +18   siglen B  RSA-2048 PKCS#1 v1.5 signature over the body [0x20, 0xA4 + total_len)
    +..   keylen B  embedded PEM public key

Signed message: the SHA-256 of the region, rendered as 64 lowercase ASCII-hex characters, signed
with PKCS#1 v1.5 and no DigestInfo. The package is distributed as a zip holding the `.sgu`, a
detached `.bin` (the same signature scheme over the whole `.sgu` file [0, EOF)) and
`Version_signature.xml`; the dongle checks the detached signature against its stored public key
and, on the WiNet-S2, also requires the in-file trailer.

This tool signs only with a key you supply (see `keygen`). It never ships, embeds or reproduces any
Sungrow key or image; stock packages are inputs you provide. Pure Python with the `cryptography`
package for RSA.

Usage:
    sgu.py info    PKG.sgu [...]            # header, parts, integrity, trailer (--json)
    sgu.py extract PKG.sgu --out-dir DIR    # <version>.app.bin and <version>.oemdata.bin
    sgu.py verify  PKG.sgu [--sig PKG.bin] [--pubkey KEY.pem]
    sgu.py pack    --app APP.bin [--oemdata IMG] --name WINET-SV300.001.03.P901 --key PRIV.pem
                   [--pubkey PUB.pem] [--embed-trailer] [--out-dir dist] [--zip]
    sgu.py keygen  [--out-dir keys] [--name rewired-s]
    sgu.py repack-check PKG.sgu [--app-ver VER]
"""

import argparse
import hashlib
import io
import json
import os
import struct
import sys
import zipfile

APPVER_OFF = 0x20
BOOTVER_OFF = 0x60
TOTLEN_OFF = 0xA0
BODY_OFF = 0xA4            # first part's header (u16 type, u32 len)
TLV_HDR = 6
HASH_START = 0x20
MD5_HEX_LEN = 32
VER_FIELD = 0x40           # version fields are 64 bytes, NUL padded
SIGLEN = 0x100             # RSA-2048
TRAILER_HDR = 18           # u16 magic + four u32 fields

PART_APP = 2               # ESP app image -> inactive OTA slot
PART_OEMDATA = 3           # raw image -> `oemdata` partition (stock: the LittleFS web UI)
KNOWN_PART_TYPES = {PART_APP: "app", PART_OEMDATA: "oemdata"}
OEMDATA_STOCK_LEN = 0x300000   # the stock web-UI image fills the partition (LittleFS block count)

TRAILER_MAGIC = 0x0002

XML_TEMPLATE = (
    "<?xml version='1.0' encoding='utf-8'?>\n"
    '<root type="communication">\n'
    '    <point name="{base}.bin" type="communication" update_seq="1" />\n'
    '    <point name="{base}.sgu" type="communication" update_seq="1" />\n'
    '    <point name="{pubkey_file}" type="publickey" />\n'
    "</root>"
)


# ---------------------------------------------------------------- container


def _ver_bytes(s: str) -> bytes:
    b = s.encode("ascii")
    if len(b) >= VER_FIELD:
        raise ValueError(f"version string too long ({len(b)} >= {VER_FIELD}): {s!r}")
    return b + b"\x00" * (VER_FIELD - len(b))


def body_hash_region(blob: bytes):
    """(start, end) of the region the in-file trailer signs: [0x20, 0xA4 + total_len)."""
    total_len = struct.unpack_from("<I", blob, TOTLEN_OFF)[0]
    return HASH_START, BODY_OFF + total_len


def walk_parts(blob: bytes):
    """Walk the part list and require that it tiles [0xA4, 0xA4 + total_len) exactly.

    Returns [{"type", "hdr_off", "data_off", "len"}]; raises ValueError on a truncated or
    overrunning part or a body that runs past EOF."""
    if len(blob) < BODY_OFF:
        raise ValueError("too short to be an SGU")
    _, end = body_hash_region(blob)
    if end > len(blob):
        raise ValueError(f"body end 0x{end:x} (0xA4 + total_len) is beyond EOF 0x{len(blob):x}")
    off, parts = BODY_OFF, []
    while off < end:
        if off + TLV_HDR > end:
            raise ValueError(f"truncated part header at 0x{off:x} (body ends at 0x{end:x})")
        ptype, plen = struct.unpack_from("<HI", blob, off)
        if off + TLV_HDR + plen > end:
            raise ValueError(f"part type {ptype} at 0x{off:x} (len 0x{plen:x}) overruns the body end 0x{end:x}")
        parts.append({"type": ptype, "hdr_off": off, "data_off": off + TLV_HDR, "len": plen})
        off += TLV_HDR + plen
    return parts


def check_parts(parts):
    """Problems (empty = OK) that make the dongle's updater refuse or skip the package."""
    problems = []
    if not parts:
        problems.append("body has no parts")
    for p in parts:
        if p["type"] not in KNOWN_PART_TYPES:
            problems.append(f"part at 0x{p['hdr_off']:x} has type {p['type']}: the updater refuses unknown types")
        elif p["len"] == 0:
            problems.append(f"{KNOWN_PART_TYPES[p['type']]} part at 0x{p['hdr_off']:x} is empty: the updater skips it")
    if not any(p["type"] in KNOWN_PART_TYPES and p["len"] for p in parts):
        problems.append("no non-empty app (2) or oemdata (3) part: nothing to install")
    return problems


def parse(blob: bytes) -> dict:
    """Parse a package into a dict. Does not verify signatures.

    parts   : from walk_parts (None when the walk failed; the reason is in tlv_error)
    app     : data of the first type-2 part (b"" if none)
    oemdata : data of the first type-3 part (b"" if none)"""
    if len(blob) < BODY_OFF + TLV_HDR:
        raise ValueError("too short to be an SGU")
    md5_hex = blob[0:MD5_HEX_LEN].decode("ascii", "replace")
    app_ver = blob[APPVER_OFF:APPVER_OFF + VER_FIELD].split(b"\x00", 1)[0].decode("ascii", "replace")
    boot_ver = blob[BOOTVER_OFF:BOOTVER_OFF + VER_FIELD].split(b"\x00", 1)[0].decode("ascii", "replace")
    total_len = struct.unpack_from("<I", blob, TOTLEN_OFF)[0]
    h_start, h_end = body_hash_region(blob)
    try:
        parts, tlv_error = walk_parts(blob), None
    except ValueError as e:
        parts, tlv_error = None, str(e)

    def data(p):
        return blob[p["data_off"]:p["data_off"] + p["len"]]

    plist = parts or []
    first_of = lambda t: next((data(p) for p in plist if p["type"] == t), b"")  # noqa: E731
    out = {
        "md5_hex": md5_hex,
        "app_ver": app_ver,
        "boot_ver": boot_ver,
        "total_len": total_len,
        "first_part_type": struct.unpack_from("<H", blob, BODY_OFF)[0],
        "parts": parts,
        "tlv_error": tlv_error,
        "part_problems": check_parts(parts) if parts is not None else [tlv_error],
        "hash_region": (h_start, h_end),
        "app": first_of(PART_APP),
        "oemdata": first_of(PART_OEMDATA),
        "md5_ok": None,
        "sha256_hex": hashlib.sha256(blob[h_start:min(h_end, len(blob))]).hexdigest(),
        "trailer": None,
    }
    # The MD5 covers [0x20, EOF): the trailer too, when there is one.
    md5_calc = hashlib.md5(blob[h_start:]).hexdigest()
    out["md5_calc"] = md5_calc
    out["md5_ok"] = md5_calc == md5_hex.lower()

    t = h_end
    if t + 2 <= len(blob):
        magic = struct.unpack_from("<H", blob, t)[0]
        if magic == TRAILER_MAGIC and t + TRAILER_HDR <= len(blob):
            trailer_total, siglen, keylen, xlen = struct.unpack_from("<IIII", blob, t + 2)
            sig = blob[t + TRAILER_HDR:t + TRAILER_HDR + siglen]
            key = blob[t + TRAILER_HDR + siglen:t + TRAILER_HDR + siglen + keylen]
            out["trailer"] = {
                "magic": magic,
                "trailer_total": trailer_total,
                "siglen": siglen,
                "keylen": keylen,
                "xlen": xlen,
                "signature": sig,
                "embedded_pubkey_pem": key.decode("ascii", "replace") if keylen else "",
                "consistent": (siglen == SIGLEN and trailer_total == 12 + siglen + keylen + xlen
                               and t + TLV_HDR + trailer_total == len(blob)),
            }
    return out


def app_descriptor(app: bytes) -> dict:
    """The ESP-IDF app descriptor at 0x20 of an app image (version, project name, build date)."""
    if len(app) < 0x20 + 0x100 or struct.unpack_from("<I", app, 0x20)[0] != 0xABCD5432:
        return {}
    d = 0x20
    field = lambda off, n: app[d + off:d + off + n].split(b"\x00", 1)[0].decode("ascii", "replace")  # noqa: E731
    return {
        "version": field(16, 32),
        "project_name": field(48, 32),
        "time": field(80, 16),
        "date": field(96, 16),
        "idf_ver": field(112, 32),
    }


# ---------------------------------------------------------------- signatures


def _sha256_hex_ascii(region: bytes) -> bytes:
    """The signed message: lowercase 64-character ASCII hex of SHA-256(region)."""
    return hashlib.sha256(region).hexdigest().encode("ascii")


def sign_rsa(region: bytes, private_key_pem: bytes) -> bytes:
    """256-byte RSA-2048 PKCS#1 v1.5 signature over the 64 ASCII-hex bytes of SHA-256(region),
    with no DigestInfo (the hex string itself is the padded message)."""
    from cryptography.hazmat.primitives.serialization import load_pem_private_key
    priv = load_pem_private_key(private_key_pem, password=None)
    nums = priv.private_numbers()
    pub = priv.public_key().public_numbers()
    n, d = pub.n, nums.d
    k = (n.bit_length() + 7) // 8
    msg = _sha256_hex_ascii(region)
    if len(msg) > k - 11:
        raise ValueError("message too long for PKCS#1 v1.5")
    em = b"\x00\x01" + b"\xff" * (k - len(msg) - 3) + b"\x00" + msg
    return pow(int.from_bytes(em, "big"), d, n).to_bytes(k, "big")


def verify_rsa(region: bytes, signature: bytes, public_key_pem: bytes) -> bool:
    """Verify a signature made by sign_rsa."""
    from cryptography.hazmat.primitives.serialization import load_pem_public_key
    pub = load_pem_public_key(public_key_pem)
    nums = pub.public_numbers()
    n, e = nums.n, nums.e
    k = (n.bit_length() + 7) // 8
    if len(signature) != k:
        return False
    em = pow(int.from_bytes(signature, "big"), e, n).to_bytes(k, "big")
    msg = _sha256_hex_ascii(region)
    if len(msg) > k - 11:
        return False
    return em == b"\x00\x01" + b"\xff" * (k - len(msg) - 3) + b"\x00" + msg


def public_pem_from_private(private_key_pem: bytes) -> bytes:
    from cryptography.hazmat.primitives.serialization import (Encoding, PublicFormat,
                                                              load_pem_private_key)
    return load_pem_private_key(private_key_pem, password=None).public_key().public_bytes(
        Encoding.PEM, PublicFormat.SubjectPublicKeyInfo)


def generate_keypair():
    """(private_pem, public_pem) for a fresh RSA-2048 signing key."""
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives.serialization import (Encoding, NoEncryption,
                                                              PrivateFormat, PublicFormat)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    priv = key.private_bytes(Encoding.PEM, PrivateFormat.TraditionalOpenSSL, NoEncryption())
    pub = key.public_key().public_bytes(Encoding.PEM, PublicFormat.SubjectPublicKeyInfo)
    return priv, pub


# ---------------------------------------------------------------- build


def oemdata_payload(image: bytes):
    """(raw_image, was_wrapped) for a type-3 input: a raw image, or a type-3 part copied with
    its 6-byte header ({u16 3, u32 len} where len == size - 6)."""
    if (len(image) >= TLV_HDR and struct.unpack_from("<H", image, 0)[0] == PART_OEMDATA
            and struct.unpack_from("<I", image, 2)[0] == len(image) - TLV_HDR):
        return image[TLV_HDR:], True
    return image, False


def build_body(parts) -> bytes:
    """Serialise [(type, data), ...] as the part list (from 0xA4)."""
    out = io.BytesIO()
    for ptype, data in parts:
        if ptype not in KNOWN_PART_TYPES:
            raise ValueError(f"part type {ptype}: the updater only accepts {sorted(KNOWN_PART_TYPES)}")
        if not data:
            raise ValueError(f"empty {KNOWN_PART_TYPES[ptype]} part: the updater would skip it")
        out.write(struct.pack("<HI", ptype, len(data)))
        out.write(data)
    if out.tell() == 0:
        raise ValueError("no parts")
    return out.getvalue()


def build(app_image: bytes, oemdata_image: bytes, app_ver: str, boot_ver: str,
          private_key_pem: bytes = None, embed_pubkey_pem: bytes = None,
          embed_trailer: bool = False, parts=None):
    """Assemble a package and, with a key, its detached 256-byte signature.

    Returns (sgu_bytes, detached_signature_or_None). Body = [type 2 app] then [type 3 oemdata]
    when given (the stock order); an app-only package has only the type-2 part. `parts` is an
    explicit [(type, data), ...] list that replaces the two images.

    Two checks happen on the dongle: the in-file trailer (SHA-256 over [0x20, 0xA4 + total_len),
    verified with the key embedded in the trailer; the WiNet-S2 requires it) and the detached
    signature over the whole file, verified with the public key stored on the dongle."""
    if parts is None:
        parts = []
        if app_image:
            parts.append((PART_APP, app_image))
        if oemdata_image:
            parts.append((PART_OEMDATA, oemdata_payload(oemdata_image)[0]))
    body_tlv = build_body(parts)
    total_len = len(body_tlv)

    buf = io.BytesIO()
    buf.write(b"\x00" * MD5_HEX_LEN)
    buf.write(_ver_bytes(app_ver))
    buf.write(_ver_bytes(boot_ver))
    buf.write(struct.pack("<I", total_len))
    buf.write(body_tlv)
    blob = bytearray(buf.getvalue())

    h_start, h_end = HASH_START, BODY_OFF + total_len
    assert h_end == len(blob), (h_end, len(blob))
    body = bytes(blob[h_start:h_end])

    if embed_trailer and private_key_pem is not None:
        emb_sig = sign_rsa(body, private_key_pem)
        keyb = embed_pubkey_pem or b""
        xlen = 0
        trailer_total = 12 + SIGLEN + len(keyb) + xlen
        tr = io.BytesIO()
        tr.write(struct.pack("<H", TRAILER_MAGIC))
        tr.write(struct.pack("<IIII", trailer_total, SIGLEN, len(keyb), xlen))
        tr.write(emb_sig)
        tr.write(keyb)
        blob += tr.getvalue()

    md5_hex = hashlib.md5(bytes(blob[h_start:])).hexdigest().encode("ascii")
    blob[0:MD5_HEX_LEN] = md5_hex

    detached = sign_rsa(bytes(blob), private_key_pem) if private_key_pem is not None else None
    return bytes(blob), detached


def derive_boot_ver(app_ver: str) -> str:
    """'WINET-SV300.001.03.P901' -> 'WINET-SV300.001.03.B001'."""
    head, _, _ = app_ver.rpartition(".")
    return f"{head}.B001" if head else app_ver


def diff_ranges(a: bytes, b: bytes):
    """[(start, end)] of differing byte runs over the common length."""
    out, i, n = [], 0, min(len(a), len(b))
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


def repack_check(blob: bytes, app_ver: str = None, boot_ver: str = None) -> dict:
    """Rebuild a package from its parsed parts (unsigned, no trailer) and compare it with the
    original's [0, 0xA4 + total_len): a correct model of the container reproduces every byte
    except the MD5 field and any version field that was changed."""
    info = parse(blob)
    if info["parts"] is None:
        raise ValueError(f"part walk failed: {info['tlv_error']}")
    rebuilt, _ = build(None, None, app_ver or info["app_ver"], boot_ver or info["boot_ver"],
                       parts=[(p["type"], blob[p["data_off"]:p["data_off"] + p["len"]])
                              for p in info["parts"]])
    h_end = info["hash_region"][1]
    orig = blob[:h_end]
    diffs = diff_ranges(orig, rebuilt)
    allowed = lambda s, e: (e <= MD5_HEX_LEN or  # noqa: E731
                            (app_ver and s >= APPVER_OFF and e <= APPVER_OFF + VER_FIELD) or
                            (boot_ver and s >= BOOTVER_OFF and e <= BOOTVER_OFF + VER_FIELD))
    ok = len(rebuilt) == len(orig) and all(allowed(s, e) for s, e in diffs)
    return {"rebuilt": rebuilt, "diffs": diffs, "len_orig_body": len(orig),
            "len_rebuilt": len(rebuilt), "trailer_bytes_not_rebuilt": len(blob) - h_end, "ok": ok}


# ---------------------------------------------------------------- high level


def pack(app_image: bytes, name: str, private_key_pem: bytes, out_dir: str, *,
         oemdata_image: bytes = b"", public_key_pem: bytes = None, app_ver: str = None,
         boot_ver: str = None, embed_trailer: bool = True, pubkey_version: str = "V1",
         make_zip: bool = False, log=print) -> dict:
    """Build, sign, write and self-verify a package. Returns the paths and the check list."""
    app_ver = app_ver or name
    boot_ver = boot_ver or derive_boot_ver(app_ver)
    public_key_pem = public_key_pem or public_pem_from_private(private_key_pem)
    os.makedirs(out_dir, exist_ok=True)
    oemdata_raw, wrapped = oemdata_payload(oemdata_image) if oemdata_image else (b"", False)

    sgu_bytes, detached = build(app_image, oemdata_raw, app_ver, boot_ver,
                                private_key_pem=private_key_pem,
                                embed_pubkey_pem=public_key_pem if embed_trailer else None,
                                embed_trailer=embed_trailer)
    pubkey_file = f"publicKey_{pubkey_version}.pem"
    paths = {
        "sgu": os.path.join(out_dir, f"{name}.sgu"),
        "sig": os.path.join(out_dir, f"{name}.bin"),
        "xml": os.path.join(out_dir, "Version_signature.xml"),
        "pubkey": os.path.join(out_dir, pubkey_file),
        "zip": None,
    }
    with open(paths["sgu"], "wb") as f:
        f.write(sgu_bytes)
    with open(paths["sig"], "wb") as f:
        f.write(detached)
    with open(paths["xml"], "w") as f:
        f.write(XML_TEMPLATE.format(base=name, pubkey_file=pubkey_file))
    with open(paths["pubkey"], "wb") as f:
        f.write(public_key_pem)

    checks = self_check(sgu_bytes, detached, public_key_pem,
                        want=[(PART_APP, app_image)] + ([(PART_OEMDATA, oemdata_raw)] if oemdata_raw else []),
                        expect_trailer=embed_trailer)
    if make_zip:
        paths["zip"] = os.path.join(out_dir, f"WINET_B000.{name}.zip")
        with zipfile.ZipFile(paths["zip"], "w", zipfile.ZIP_DEFLATED) as z:
            z.write(paths["sgu"], f"{name}.sgu")
            z.write(paths["sig"], f"{name}.bin")
            z.write(paths["xml"], "Version_signature.xml")
            z.write(paths["pubkey"], pubkey_file)

    log(f"app_ver     : {app_ver}")
    log(f"boot_ver    : {boot_ver}")
    log(f"app image   : {len(app_image):,} bytes (part type 2)")
    log(f"oemdata     : {len(oemdata_raw):,} bytes (part type 3{', input carried its header' if wrapped else ''})"
        if oemdata_raw else "oemdata     : none (app-only package)")
    log(f".sgu        : {paths['sgu']}  ({len(sgu_bytes):,} bytes)")
    log(f"detached sig: {paths['sig']}  ({len(detached)} bytes)")
    log(f"manifest    : {paths['xml']}")
    log(f"public key  : {paths['pubkey']}  (import it on the dongle once)")
    if paths["zip"]:
        log(f"package zip : {paths['zip']}")
    log("self-verification:")
    ok = True
    for label, good in checks:
        ok = ok and bool(good)
        log(f"  [{'OK' if good else 'FAIL'}] {label}")
    return {"paths": paths, "checks": checks, "ok": ok, "sgu": sgu_bytes, "sig": detached}


def self_check(sgu_bytes: bytes, detached: bytes, public_key_pem: bytes, want=None,
               expect_trailer: bool = True):
    """Reproduce the dongle's checks on a finished package. Returns [(label, ok)]."""
    info = parse(sgu_bytes)
    checks = []
    parts = info["parts"]
    checks.append(("part list tiles [0xA4, 0xA4 + total_len) exactly"
                   + (f" ({info['tlv_error']})" if parts is None else ""), parts is not None))
    if parts is not None:
        if want is not None:
            got = [(p["type"], sgu_bytes[p["data_off"]:p["data_off"] + p["len"]]) for p in parts]
            checks.append(("parts = " + " + ".join(f"type {t} ({KNOWN_PART_TYPES[t]}, {len(d):,} B)" for t, d in want)
                           + ", byte-identical to the inputs", got == want))
        checks.append(("first part at 0xA4 is the app (type 2)", info["first_part_type"] == PART_APP))
        checks.append(("the updater accepts the part list"
                       + (f" ({'; '.join(info['part_problems'])})" if info["part_problems"] else ""),
                       not info["part_problems"]))
    checks.append(("MD5 at 0x00 covers [0x20, EOF)", info["md5_ok"]))
    if detached is not None:
        checks.append(("detached signature verifies over the whole file with the public key",
                       verify_rsa(sgu_bytes, detached, public_key_pem)))
    if expect_trailer:
        tr = info["trailer"]
        checks.append(("in-file trailer present and consistent (required by the WiNet-S2)",
                       tr is not None and tr["consistent"]))
        if tr:
            h0, h1 = info["hash_region"]
            checks.append(("trailer signature verifies over [0x20, 0xA4 + total_len) with the embedded key",
                           bool(tr["embedded_pubkey_pem"]) and
                           verify_rsa(sgu_bytes[h0:h1], tr["signature"], tr["embedded_pubkey_pem"].encode())))
    return checks


# ---------------------------------------------------------------- CLI


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def cmd_info(args):
    rc = 0
    for path in args.sgu:
        blob = _read(path)
        info = parse(blob)
        desc = app_descriptor(info["app"]) if info["app"] else {}
        if args.json:
            out = dict(info)
            for k in ("app", "oemdata"):
                out[k] = len(out[k])
            if out["trailer"]:
                out["trailer"] = dict(out["trailer"], signature=out["trailer"]["signature"][:8].hex() + "...")
            out["app_descriptor"] = desc
            print(json.dumps(out, indent=2))
        else:
            print(f"=== {path} ({len(blob):,} bytes)")
            print(f"app version     : {info['app_ver']}")
            print(f"boot version    : {info['boot_ver']}")
            print(f"MD5 [0x20, EOF) : {'OK' if info['md5_ok'] else 'MISMATCH'}")
            if info["parts"] is None:
                print(f"parts           : INVALID ({info['tlv_error']})")
            for p in info["parts"] or []:
                print(f"part type {p['type']} ({KNOWN_PART_TYPES.get(p['type'], 'unknown')}): "
                      f"{p['len']:,} bytes at 0x{p['data_off']:x}")
            if desc:
                print(f"app descriptor  : version {desc['version']!r}, project {desc['project_name']!r}, "
                      f"built {desc['date']} {desc['time']}, IDF {desc['idf_ver']}")
            tr = info["trailer"]
            print("trailer         : " + (f"present, {tr['siglen']}-byte signature, "
                                         f"{'embedded key' if tr['keylen'] else 'no key'}, "
                                         f"{'consistent' if tr['consistent'] else 'INCONSISTENT'}"
                                         if tr else "none"))
            for prob in info["part_problems"]:
                print(f"problem         : {prob}")
        if info["parts"] is None or info["part_problems"]:
            rc = 1
    return rc


def cmd_extract(args):
    blob = _read(args.sgu)
    info = parse(blob)
    if info["parts"] is None:
        print(f"cannot walk the parts: {info['tlv_error']}", file=sys.stderr)
        return 1
    os.makedirs(args.out_dir, exist_ok=True)
    base = args.name or info["app_ver"] or "package"
    written = []
    for p in info["parts"]:
        kind = KNOWN_PART_TYPES.get(p["type"])
        if kind is None or (args.app_only and p["type"] != PART_APP):
            continue
        out = os.path.join(args.out_dir, f"{base}.{kind}.bin")
        with open(out, "wb") as f:
            f.write(blob[p["data_off"]:p["data_off"] + p["len"]])
        written.append(out)
        print(f"{kind:8}: {out} ({p['len']:,} bytes)")
    if info["app"]:
        desc = app_descriptor(info["app"])
        if desc:
            print(f"app descriptor: version {desc['version']!r}, project {desc['project_name']!r}")
    return 0 if written else 1


def cmd_verify(args):
    blob = _read(args.sgu)
    sig = _read(args.sig) if args.sig else None
    pub = _read(args.pubkey) if args.pubkey else None
    if sig is not None and pub is None:
        print("--sig needs --pubkey", file=sys.stderr)
        return 2
    checks = self_check(blob, sig if pub else None, pub, expect_trailer=not args.no_trailer)
    ok = True
    for label, good in checks:
        ok = ok and bool(good)
        print(f"  [{'OK' if good else 'FAIL'}] {label}")
    return 0 if ok else 1


def cmd_pack(args):
    app = _read(args.app)
    if app[:1] != b"\xe9":
        print(f"WARNING: {args.app} does not start with 0xE9 (ESP app-image magic)", file=sys.stderr)
    oem = _read(args.oemdata) if args.oemdata else b""
    if oem and oemdata_payload(oem)[0].__len__() != OEMDATA_STOCK_LEN:
        print(f"WARNING: oemdata image is not {OEMDATA_STOCK_LEN:#x} bytes; LittleFS needs the image to "
              "match the partition size", file=sys.stderr)
    priv = _read(args.key)
    pub = _read(args.pubkey) if args.pubkey else None
    r = pack(app, args.name, priv, args.out_dir, oemdata_image=oem, public_key_pem=pub,
             app_ver=args.app_ver, boot_ver=args.boot_ver, embed_trailer=not args.no_trailer,
             pubkey_version=args.pubkey_version, make_zip=args.zip)
    if not r["ok"]:
        print("ERROR: self-verification failed", file=sys.stderr)
        return 1
    print("All checks passed. The dongle accepts the package when its stored public key is this one "
          "and the version is above the installed one.")
    return 0


def cmd_keygen(args):
    os.makedirs(args.out_dir, exist_ok=True)
    priv_path = os.path.join(args.out_dir, f"{args.name}_priv.pem")
    pub_path = os.path.join(args.out_dir, f"{args.name}_pub.pem")
    if os.path.exists(priv_path) and not args.force:
        print(f"{priv_path} exists; use --force to overwrite", file=sys.stderr)
        return 2
    priv, pub = generate_keypair()
    fd = os.open(priv_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(priv)
    with open(pub_path, "wb") as f:
        f.write(pub)
    print(f"private key: {priv_path} (mode 600, keep it secret and backed up)")
    print(f"public key : {pub_path} (import on the dongle: tools/deploy_debug.py import-key)")
    return 0


def cmd_repack_check(args):
    rc = 0
    for path in args.sgu:
        r = repack_check(_read(path), app_ver=args.app_ver)
        print(f"{path}: body {r['len_orig_body']} B vs rebuilt {r['len_rebuilt']} B; differing ranges "
              f"{[(hex(s), hex(e)) for s, e in r['diffs']]}; trailer bytes not rebuilt: "
              f"{r['trailer_bytes_not_rebuilt']} -> {'OK' if r['ok'] else 'FAIL'}")
        rc |= 0 if r["ok"] else 1
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description="Sungrow WiNet .sgu packages: inspect, extract, verify, build, sign.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="show the header, parts and integrity of packages")
    p.add_argument("sgu", nargs="+")
    p.add_argument("--json", action="store_true")
    p.set_defaults(fn=cmd_info)

    p = sub.add_parser("extract", help="write the app and oemdata parts as separate files")
    p.add_argument("sgu")
    p.add_argument("--out-dir", default=".")
    p.add_argument("--name", help="base name of the output files (default: the app version)")
    p.add_argument("--app-only", action="store_true")
    p.set_defaults(fn=cmd_extract)

    p = sub.add_parser("verify", help="check a package's integrity, trailer and detached signature")
    p.add_argument("sgu")
    p.add_argument("--sig", help="detached .bin signature")
    p.add_argument("--pubkey", help="public key PEM to verify --sig against")
    p.add_argument("--no-trailer", action="store_true", help="do not require the in-file trailer")
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("pack", help="build and sign a package from an app image")
    p.add_argument("--app", required=True, help="ESP-IDF app image (firmware.ota.bin)")
    p.add_argument("--oemdata", help="optional type-3 image for the oemdata partition (raw, or a part with its header)")
    p.add_argument("--name", required=True, help="package name and app version, e.g. WINET-SV300.001.03.P901")
    p.add_argument("--app-ver", help="app version string (default: --name)")
    p.add_argument("--boot-ver", help="bootloader version string (default: derived .B001)")
    p.add_argument("--key", required=True, help="RSA-2048 private key PEM")
    p.add_argument("--pubkey", help="public key PEM (default: derived from --key)")
    p.add_argument("--pubkey-version", default="V1")
    p.add_argument("--no-trailer", action="store_true", help="omit the in-file trailer (the WiNet-S2 refuses such packages)")
    p.add_argument("--out-dir", default="dist")
    p.add_argument("--zip", action="store_true", help="also write the zip the stock web UI and pull route expect")
    p.set_defaults(fn=cmd_pack)

    p = sub.add_parser("keygen", help="generate an RSA-2048 signing key pair")
    p.add_argument("--out-dir", default="keys")
    p.add_argument("--name", default="rewired-s")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_keygen)

    p = sub.add_parser("repack-check", help="rebuild packages from their parts and compare byte for byte")
    p.add_argument("sgu", nargs="+")
    p.add_argument("--app-ver")
    p.set_defaults(fn=cmd_repack_check)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
