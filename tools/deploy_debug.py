#!/usr/bin/env python3
"""Deploy the ESPHome DEBUG build to a live stock WiNet-S/-S2 over its own web server.

Automates the two state-changing steps that otherwise need the Vue SPA (which on recent stock
firmware, e.g. WiNet-S P044, only exposes the iConfig path in its "System update" dialog):

  1. --import-key : import OUR OTA public key so the dongle will accept our signed .sgu
                    (POST /system/importPublicKey).
  2. --upload     : push the signed .sgu + detached .bin, then start the install
                    (POST /system/upgradeCheck -> /system/upgradeStart), polling progress.

Read-only --status just prints the dongle SN / versions / running inverter for a before/after
comparison. Authentication mirrors the SPA: a WebSocket connect+login on
wss://<host>:443/ws/home/overview with the admin credentials (password in cleartext, exactly as
the stock web UI sends it), which yields a session token the HTTP endpoints accept. The reference
client is winet-extractor's src/winetHandler.ts (it stays on the guest
connect token for read-only services; we add the login frame for the privileged POSTs).

Stdlib only; run isolated:  python3 -I tools/deploy_debug.py <cmd> [opts]

SAFETY
  * The password comes from --password, $WINET_WEB_PASSWORD, or an interactive prompt. It is never
    written to disk, never printed, and must never be hard-coded or committed.
  * --import-key and --upload change device state and require a typed "yes" unless --yes is given.
  * The dongle's external identity flash is never touched by anything here.
  * After a successful upload our app boots PENDING_VERIFY; safe_mode marks it valid after ~30 s,
    else the stock bootloader reverts to the stock app in the other OTA slot.

Installed from the tools package this is the `rewired-s-deploy` command (same arguments).
Build the inputs first (Ethernet dongle shown):
    .venv/bin/esphome compile winet-s-debug-ethernet.yaml
    make sgu APP=.esphome/build/winet-s-debug-eth/build/firmware.ota.bin
  -> dist/WINET-SV200.001.00.P901.sgu + .bin + publicKey_V1.pem
"""
import argparse
import base64
import getpass
import hashlib
import json
import os
import re
import socket
import ssl
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

WS_PATH = "/ws/home/overview"
LANG = "en_us"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"  # RFC 6455 WebSocket accept magic


# ============================ pure helpers (host-tested) ============================

def encode_pubkey(pem: bytes) -> str:
    """Encode a PEM public key exactly as the stock SPA does before POSTing it.

    The device URL-decodes the public_key_data field, so the client must escape the only two
    characters that the decoder would otherwise eat: '%' (escape intro) and '+' (space). Base64
    PEM bodies contain '+', so this matters. All other PEM characters ('/','=','-', newlines,
    letters, digits) survive URL-decoding unchanged and are left as-is. Mirrors winetHandler's
    importPublicKey: t.replace(/%/g,"%25").replace(/\\+/g,"%2B").
    """
    text = pem.decode("ascii")
    return text.replace("%", "%25").replace("+", "%2B")


def default_sig_path(sgu_path: str) -> str:
    """The detached signature tools/sgu.py writes next to the .sgu: same stem, .bin extension."""
    base, _ = os.path.splitext(sgu_path)
    return base + ".bin"


def spa_pubkey_version(pem_path: str, product_name: str) -> str:
    """The `public_key_version` string the stock SPA sends for a key file.

    The web UI requires the filename to match ^publicKey_V\\d+\\.pem$, then sends
    `<product_name>_<filename without .pem>`, e.g. product 'WiNet-S' + 'publicKey_V1' ->
    'WiNet-S_publicKey_V1'. The device parses the V<n> version out of that. Sending a bare 'V1'
    fails the device's version parser, so we must reproduce this exactly.
    """
    base = os.path.basename(pem_path)
    if not re.match(r"^publicKey_V\d+\.pem$", base):
        raise ValueError("public key file must be named publicKey_V<n>.pem (the release ships it as "
                         "publicKey_V1.pem, also inside the package zip), got %r" % base)
    stem = base[:-len(".pem")]
    return "%s_%s" % (product_name, stem)


def ws_accept_key(client_key: str) -> str:
    """Server's expected Sec-WebSocket-Accept for a given client key (RFC 6455)."""
    digest = hashlib.sha1((client_key + GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def encode_ws_text_frame(payload: str, mask: bytes) -> bytes:
    """A single masked client text frame (FIN=1, opcode=0x1). Clients MUST mask (RFC 6455)."""
    data = payload.encode("utf-8")
    if len(mask) != 4:
        raise ValueError("mask must be 4 bytes")
    out = bytearray([0x81])  # FIN + text
    n = len(data)
    if n < 126:
        out.append(0x80 | n)
    elif n < 65536:
        out.append(0x80 | 126)
        out += struct.pack(">H", n)
    else:
        out.append(0x80 | 127)
        out += struct.pack(">Q", n)
    out += mask
    out += bytes(b ^ mask[i & 3] for i, b in enumerate(data))
    return bytes(out)


def parse_ws_frame(buf: bytes):
    """Parse one frame from the front of buf. Returns (fin, opcode, payload, nconsumed) or None
    if buf does not yet hold a whole frame. Server->client frames are not masked."""
    if len(buf) < 2:
        return None
    b0, b1 = buf[0], buf[1]
    fin = bool(b0 & 0x80)
    opcode = b0 & 0x0F
    masked = bool(b1 & 0x80)
    ln = b1 & 0x7F
    off = 2
    if ln == 126:
        if len(buf) < off + 2:
            return None
        ln = struct.unpack_from(">H", buf, off)[0]
        off += 2
    elif ln == 127:
        if len(buf) < off + 8:
            return None
        ln = struct.unpack_from(">Q", buf, off)[0]
        off += 8
    mask = b""
    if masked:
        if len(buf) < off + 4:
            return None
        mask = buf[off:off + 4]
        off += 4
    if len(buf) < off + ln:
        return None
    data = bytearray(buf[off:off + ln])
    if masked:
        for i in range(ln):
            data[i] ^= mask[i & 3]
    return fin, opcode, bytes(data), off + ln


def multipart_body(fields: dict, files: list, boundary: str):
    """Build a multipart/form-data body. files = [(field, filename, content_type, bytes)].
    Returns (body_bytes, content_type_header)."""
    crlf = b"\r\n"
    out = bytearray()
    for name, value in fields.items():
        out += b"--" + boundary.encode() + crlf
        out += ('Content-Disposition: form-data; name="%s"' % name).encode() + crlf + crlf
        out += str(value).encode() + crlf
    for field, filename, ctype, content in files:
        out += b"--" + boundary.encode() + crlf
        out += ('Content-Disposition: form-data; name="%s"; filename="%s"'
                % (field, filename)).encode() + crlf
        out += ("Content-Type: %s" % ctype).encode() + crlf + crlf
        out += content + crlf
    out += b"--" + boundary.encode() + b"--" + crlf
    return bytes(out), "multipart/form-data; boundary=" + boundary


# ============================ WebSocket login ============================

class WinetWS:
    """Minimal RFC 6455 client over TLS for the WiNet /ws/home/overview service."""

    def __init__(self, host, port=443, timeout=15):
        self.host, self.port, self.timeout = host, port, timeout
        self.sock = None
        self.buf = b""

    def connect(self):
        raw = socket.create_connection((self.host, self.port), self.timeout)
        ctx = ssl._create_unverified_context()  # dongle serves a self-signed cert on the LAN
        self.sock = ctx.wrap_socket(raw, server_hostname=self.host)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = (
            "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: https://%s\r\n\r\n"
            % (WS_PATH, self.host, self.port, key, self.host)
        )
        self.sock.sendall(req.encode())
        # read HTTP handshake response up to the blank line
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("WebSocket handshake closed early")
            resp += chunk
        head, _, rest = resp.partition(b"\r\n\r\n")
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise RuntimeError("WebSocket upgrade failed: " + head.split(b"\r\n", 1)[0].decode("latin1"))
        m = re.search(rb"sec-websocket-accept:\s*(\S+)", head, re.I)
        if not m or m.group(1).decode() != ws_accept_key(key):
            raise RuntimeError("WebSocket accept key mismatch")
        self.buf = rest

    def send(self, obj):
        frame = encode_ws_text_frame(json.dumps(obj), os.urandom(4))
        self.sock.sendall(frame)

    def recv_json(self, deadline):
        """Return the next text message as a dict (skips ping/pong), or raise on timeout/close."""
        while True:
            parsed = parse_ws_frame(self.buf)
            if parsed is not None:
                fin, opcode, data, used = parsed
                self.buf = self.buf[used:]
                if opcode == 0x8:
                    raise RuntimeError("WebSocket closed by server")
                if opcode == 0x9:  # ping -> pong
                    self.sock.sendall(encode_ws_text_frame_pong(data))
                    continue
                if opcode in (0x1, 0x0):
                    try:
                        return json.loads(data.decode("utf-8"))
                    except ValueError:
                        continue
                continue
            if time.time() > deadline:
                raise TimeoutError("timed out waiting for a WebSocket message")
            self.sock.settimeout(max(0.5, deadline - time.time()))
            try:
                chunk = self.sock.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                raise RuntimeError("WebSocket closed by server")
            self.buf += chunk

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass


def encode_ws_text_frame_pong(payload: bytes) -> bytes:
    out = bytearray([0x8A])  # FIN + pong
    mask = os.urandom(4)
    out.append(0x80 | len(payload))
    out += mask
    out += bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    return bytes(out)


def ws_login(host, user, password, timeout=20):
    """connect -> login; returns (token, connect_info). Raises on failure."""
    ws = WinetWS(host, timeout=timeout)
    ws.connect()
    try:
        ws.send({"lang": LANG, "token": "", "service": "connect"})
        deadline = time.time() + timeout
        guest = None
        while guest is None:
            msg = ws.recv_json(deadline)
            if msg.get("result_data", {}).get("service") == "connect":
                guest = msg
        rd = guest.get("result_data", {})
        token = rd.get("token", "")
        if not token:
            raise RuntimeError("connect returned no token")
        ws.send({"lang": LANG, "token": token, "service": "login",
                 "username": user, "passwd": password})
        deadline = time.time() + timeout
        while True:
            msg = ws.recv_json(deadline)
            if msg.get("result_data", {}).get("service") == "login":
                if msg.get("result_code") == 1:
                    new_token = msg["result_data"].get("token") or token
                    return new_token, rd
                raise RuntimeError("login failed: result_code=%s msg=%s"
                                   % (msg.get("result_code"), msg.get("result_msg")))
    finally:
        ws.close()


# ============================ HTTP helpers ============================

def _http(host, path, token, method="GET", json_body=None, body=None, content_type=None,
          timeout=120):
    url = "https://%s%s?token=%s&lang=%s" % (host, path, token, LANG)
    data = None
    headers = {"Accept": "application/json, text/plain, */*"}
    if json_body is not None:
        data = json.dumps(json_body).encode("utf-8")
        headers["Content-Type"] = "application/json; charset=UTF-8"
    elif body is not None:
        data = body
        if content_type:
            headers["Content-Type"] = content_type
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    ctx = ssl._create_unverified_context()
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
        raw = r.read()
    try:
        return json.loads(raw.decode("utf-8"))
    except ValueError:
        return {"_raw": raw[:400].decode("latin1", "replace"), "_status": "non-json"}


def get_json(host, path, token, **kw):
    return _http(host, path, token, method="GET", **kw)


# ============================ commands ============================

def cmd_status(host, token):
    about = get_json(host, "/about/list", token)
    info = {}
    for row in about.get("result_data", {}).get("list", []):
        info[row.get("data_name")] = row.get("data_value")
    ov = get_json(host, "/device/overview", token).get("result_data", {})
    print("Dongle SN      :", info.get("I18N_COMMON_DEVICE_SN"))
    print("App version    :", info.get("I18N_COMMON_APPLI_SOFT_VERSION"))
    print("Build version  :", info.get("I18N_COMMON_BUILD_SOFT_VERSION"))
    print("Module version :", info.get("I18N_COMMON_VERSION"))
    mi = ov.get("module_info", {})
    if mi:
        print("Module (ov)    :", mi.get("module_sn"), mi.get("module_ver"))
    for d in ov.get("list", []):
        print("Inverter       :", d.get("dev_name"), "SN", d.get("dev_sn"),
              "link", d.get("link_status"), d.get("company"))
    return info


def cmd_import_key(host, token, pem_path, version):
    with open(pem_path, "rb") as f:
        pem = f.read()
    if b"PUBLIC KEY" not in pem:
        raise SystemExit("%s does not look like a PEM public key" % pem_path)
    body = {"import": "1", "public_key_data": encode_pubkey(pem),
            "public_key_version": version, "token": token, "lang": LANG}
    resp = _http(host, "/system/importPublicKey", token, method="POST", json_body=body)
    return report_import_key(resp, version)


# 120: the same key version is already stored (it lives on the external flash and survives stock updates).
IMPORT_KEY_ALREADY = (120, "120")


def report_import_key(resp, version):
    """Print the outcome of an importPublicKey reply; True when the key is usable afterwards."""
    code = resp.get("result_code")
    if code == 1 or code == "1":
        print("public key imported OK (version %s)" % version)
        return True
    if code in IMPORT_KEY_ALREADY:
        print("import answered result_code=120 (%s): the dongle usually says this when the same key "
              "version is already stored (it survives stock updates). Continuing; if the install "
              "later fails its signature check, import under a higher --pubkey-version."
              % resp.get("result_msg"))
        return True
    print("import FAILED: result_code=%s msg=%s" % (code, resp.get("result_msg")))
    return False


def _upload_one(host, token, path, blob):
    """One layui-style file upload to /system/upgradeCheck: multipart, field `file` + `lang`."""
    boundary = "----WiNetDeploy" + base64.b16encode(os.urandom(8)).decode()
    body, ctype = multipart_body(
        {"lang": LANG},
        [("file", os.path.basename(path), "application/octet-stream", blob)],
        boundary)
    return _http(host, "/system/upgradeCheck", token, method="POST", body=body,
                 content_type=ctype)


def cmd_upload(host, token, sgu_path, sig_path, poll_secs=240):
    """Replicates the stock SPA's system-firmware upload sequence (read from its JS).

    WiNet-S firmware P044 has no upload endpoints (/system/upgradeCheck is 404,
    /upgrade/upgradeCheck does not stage files), so this does not work there; install on P044 is
    pull-based (iConfig) or UART. For stock firmware that exposes the /system/* upload routes.
    Sequence:
      1) POST /device/upgradeSeq  (form: seq, count=2, token, lang)
      2) upload the .bin, then the .sgu, each as its own multipart POST to /system/upgradeCheck
         (field `file`; the SPA sorts the .bin first; the Version_signature.xml is client-side only)
      3) POST /upgrade/systemUpgradeStart  (form: token, lang)
      4) poll /upgrade/upgradeStatus
    """
    with open(sgu_path, "rb") as f:
        sgu = f.read()
    with open(sig_path, "rb") as f:
        sig = f.read()
    for name, blob in ((sgu_path, sgu), (sig_path, sig)):
        if len(blob) > 7864320:
            raise SystemExit("%s is %d B, over the 7.5 MB web upload cap" % (name, len(blob)))

    # 1) register the upload sequence (count = the two files we will upload)
    seq = str(int(time.time() * 1000))
    seqresp = _http(host, "/device/upgradeSeq", token, method="POST",
                    body=urllib.parse.urlencode({"seq": seq, "count": "2",
                                                 "token": token, "lang": LANG}).encode(),
                    content_type="application/x-www-form-urlencoded; charset=UTF-8")
    print("upgradeSeq ->", json.dumps(seqresp)[:200])
    if seqresp.get("result_code") != 1:
        print("upgradeSeq refused; aborting upload."); return False

    # 2) upload .bin first, then .sgu (SPA order), each on its own request
    for path, blob in ((sig_path, sig), (sgu_path, sgu)):
        print("uploading %s (%d B) to /system/upgradeCheck ..." % (os.path.basename(path), len(blob)))
        r = _upload_one(host, token, path, blob)
        print("  ->", json.dumps(r)[:300])
        if r.get("result_code") not in (1, None):
            print("  upload/check rejected %s; aborting." % os.path.basename(path))
            return False

    # 3) start the install
    start = _http(host, "/upgrade/systemUpgradeStart", token, method="POST",
                  body=urllib.parse.urlencode({"token": token, "lang": LANG}).encode(),
                  content_type="application/x-www-form-urlencoded; charset=UTF-8")
    print("systemUpgradeStart ->", json.dumps(start)[:300])
    if start.get("result_code") != 1:
        print("systemUpgradeStart failed.")
        return False
    print("install started; polling /upgrade/upgradeStatus for up to %d s ..." % poll_secs)
    deadline = time.time() + poll_secs
    last = None
    while time.time() < deadline:
        try:
            st = _http(host, "/upgrade/upgradeStatus", token, method="POST",
                       body=b"", content_type="application/x-www-form-urlencoded")
        except urllib.error.URLError:
            st = {"_note": "dongle unreachable (likely rebooting into the new image)"}
        s = json.dumps(st)
        if s != last:
            print("  status:", s[:300])
            last = s
        if st.get("result_data", {}).get("total_status") in ("2", "3", 2, 3):
            break
        time.sleep(5)
    print("Upload sequence done. Watch the ESPHome logs: the app boots PENDING_VERIFY and marks "
          "itself valid after ~30 s, else the bootloader reverts to stock.")
    return True


# ============================ cli ============================

def confirm(action, assume_yes):
    if assume_yes:
        return True
    ans = input("About to %s on the dongle. This changes device state. Type 'yes' to proceed: "
                % action).strip().lower()
    return ans == "yes"


def get_password(args):
    if args.password:
        return args.password
    env = os.environ.get("WINET_WEB_PASSWORD")
    if env:
        return env
    return getpass.getpass("WiNet web password for %s: " % args.user)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Deploy the ESPHome debug build to a stock WiNet over its web UI")
    ap.add_argument("command", choices=["status", "import-key", "upload", "all"])
    ap.add_argument("--host", required=True, help="IP address or host name of the stock dongle")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", help="admin password (prefer $WINET_WEB_PASSWORD or the prompt)")
    ap.add_argument("--sgu", help="signed .sgu to install (upload/all)")
    ap.add_argument("--sig", help="detached .bin signature (default: the .sgu name with .bin)")
    ap.add_argument("--pubkey", default="dist/publicKey_V1.pem", help="our OTA public key PEM")
    ap.add_argument("--pubkey-version", help="override the _V<n> version parsed from --pubkey")
    ap.add_argument("--yes", action="store_true", help="skip the typed confirmation for writes")
    ap.add_argument("--poll-secs", type=int, default=240)
    args = ap.parse_args(argv)

    if args.command in ("upload", "all") and not args.sgu:
        ap.error("--sgu is required for %s" % args.command)
    sig = args.sig or (default_sig_path(args.sgu) if args.sgu else None)

    password = get_password(args)
    print("logging in to %s as %s ..." % (args.host, args.user))
    token, _ = ws_login(args.host, args.user, password)
    print("authenticated.")

    if args.command == "status":
        cmd_status(args.host, token)
        return 0

    print("\n-- before --")
    cmd_status(args.host, token)

    if args.command in ("import-key", "all"):
        # The device parses the key version from a "<product>_publicKey_V<n>" string, exactly as
        # the SPA builds it. Fetch the product name so we match it (unless overridden).
        if args.pubkey_version:
            version = args.pubkey_version
        else:
            product = get_json(args.host, "/product/list", token).get("result_data", {})
            product_name = product.get("product_name") or "WiNet-S"
            version = spa_pubkey_version(args.pubkey, product_name)
        print("public_key_version =", version)
        if not confirm("import our OTA public key (overwrites the trust anchor)", args.yes):
            print("aborted."); return 1
        if not cmd_import_key(args.host, token, args.pubkey, version):
            return 2

    if args.command in ("upload", "all"):
        if not os.path.exists(sig):
            raise SystemExit("detached signature not found: %s" % sig)
        if not confirm("upload and START the firmware install", args.yes):
            print("aborted."); return 1
        if not cmd_upload(args.host, token, args.sgu, sig, poll_secs=args.poll_secs):
            return 3

    return 0


if __name__ == "__main__":
    sys.exit(main())
