#!/usr/bin/env python3
r"""Minimal fake "iConfig" package server so a stock WiNet-S / WiNet-S2 pulls and installs OUR signed .sgu.

Background: the stock web UI has no .sgu upload on recent firmware; the dongle instead PULLS a
firmware package over HTTPS from an iConfig server. The dongle then verifies the .sgu with the
SAME on-device RSA check (against our imported key) and esp_ota-flashes it.

This server impersonates iConfig: it listens HTTPS on :20001 and answers the five requests the
dongle makes (`get_time`, `select_packages`, `get_key`, `get_packages`, `result`), serving our
files from dist/ by path (it ignores the dongle's time/sign/file_id). Stock P033 and the WiNet-S
P044 request `/v2/web/<name>`; older stock (P016) requests `/web/<name>`; both are matched on the
suffix.

Which server the dongle contacts: the stock UI sends the dongle the address of the machine the
browser runs on (`window.testIp`), not an address you type, on every version seen so far. Run this
server on the computer whose browser is logged in to the dongle.

SAFETY: the stock web UI has two iConfig dialogs on the same server. "System -> System update ->
iConfig update package" asks with dev_model=WiNet-S / WiNet-S2 and installs on the dongle; the
*device* update dialog asks with the inverter's model (dev_model=SH15T, `["SH15T"]` on P016) and
pushes what it gets to the inverter over RS485. A request that does not name a WiNet gets an
empty package list, and get_key / get_packages are refused until a dongle has asked.

IMPORTANT: get_packages must serve a .ZIP, not a bare .sgu. After the
dongle downloads the package, the web UI re-fetches it, unzips it in the browser (JSZip), and
re-submits each member to /upgrade/upgradeCheck?upgrademode=1&public_key_version=... (le() in the
SPA). A bare .sgu makes the UI throw "Please Select Zip File" / crash on a `/\.zip$/` path match.
The zip holds the three files the updater actually ingests + verifies: <base>.sgu, its detached
<base>.bin signature, and Version_signature.xml (the pubkey comes from the device trust store, not
the zip). The on-device RSA check then verifies the .sgu vs the .bin against our
imported key, and esp_ota flashes it.

    python3 -I tools/fake_iconfig_server.py --dist dist --bind 0.0.0.0 --port 20001

Then in the dongle web UI (admin): System -> System update -> iConfig update package -> start. The
dongle discovers the port (20001 first), downloads our key + .sgu, verifies and flashes.

Stdlib plus `cryptography` (tools/requirements.txt) for the self-signed certificate it generates on first run
(openssl is used instead when `cryptography` is missing)
(the dongle does not verify the server cert). Keep this on the LAN; firewall the dongle off the
internet so its post-update telemetry to auto.isolarcloud.com cannot leave.
"""
import argparse
import datetime
import json
import os
import ssl
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

STATE = {"dist": "dist", "sgu": None, "sig": None, "pem": None, "pkg": None, "file_id": 901,
         "dev_model": "WiNet-S", "with_key": True,
         # The dev_model of the last select_packages that was answered with the package: get_key
         # and get_packages are served only after a dongle has asked.
         "last_dev_model": ""}

ROUTES = ("get_time", "select_packages", "get_key", "get_packages", "result")


def route_name(path):
    """Which iConfig request `path` is: the name after the last `/web/` (`/v2/web/<name>` on
    current stock, `/web/<name>` on P016), or None."""
    path = path.rstrip("/")
    for name in ROUTES:
        if path.endswith("/web/" + name):
            return name
    return None


def dev_model_from(query, body=b""):
    """The dev_model the dongle asks for, from the query string or a form / JSON POST body.
    P016 sends it as a JSON array string (`["SH15T"]`); the brackets and quotes are stripped."""
    q = parse_qs(query or "", keep_blank_values=True)
    values = q.get("dev_model") or []
    if not values and body:
        text = body.decode("utf-8", "replace")
        values = parse_qs(text, keep_blank_values=True).get("dev_model") or []
        if not values:
            try:
                obj = json.loads(text)
            except ValueError:
                obj = None
            if isinstance(obj, dict) and "dev_model" in obj:
                values = [obj["dev_model"] if isinstance(obj["dev_model"], str) else json.dumps(obj["dev_model"])]
    if not values:
        return ""
    return values[0].strip().strip("[]").strip().strip("\"'").strip()


def is_dongle(dev_model):
    """True when the request comes from the dongle's own updater (WiNet-S, WiNet-S2, winet...),
    False for an inverter / battery model from the device-update dialog."""
    return "winet" in (dev_model or "").lower()


def _find_artifacts(dist):
    sgu = sig = pem = pkg = None
    for fn in sorted(os.listdir(dist)):
        p = os.path.join(dist, fn)
        if fn.endswith(".sgu"):
            sgu = p
        elif fn.endswith(".zip"):
            pkg = p
        elif fn.endswith(".bin") and fn[:1].isalpha():
            sig = p
        elif fn.endswith(".pem") and fn.startswith("publicKey_V"):
            pem = p
    return sgu, sig, pem, pkg


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[%s] %s - %s\n" % (time.strftime("%H:%M:%S"), self.client_address[0],
                                             fmt % args))

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json;charset=UTF-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _file(self, path):
        with open(path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        sys.stderr.write("    -> served %s (%d bytes)\n" % (os.path.basename(path), len(data)))

    def _route(self, method):
        u = urlparse(self.path)
        sys.stderr.write("%s %s\n" % (method, self.path))
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        name = route_name(u.path)
        if name == "get_time":
            return self._json({"result_code": "1", "result_data": {"time": int(time.time())}})
        if name == "select_packages":
            asked = dev_model_from(u.query, body)
            if not is_dongle(asked):
                STATE["last_dev_model"] = ""
                sys.stderr.write("    !! refused: dev_model=%r is not the dongle (the device-update dialog "
                                 "targets the inverter); nothing offered\n" % asked)
                return self._json({"result_code": "1", "result_data": {"rowCount": 0, "pageList": []}})
            STATE["last_dev_model"] = asked
            entry = {
                "file_id": STATE["file_id"],
                "file_name": os.path.basename(STATE["pkg"]),
                "dev_model": asked or STATE["dev_model"],
                "version": os.path.splitext(os.path.basename(STATE["pkg"]))[0],
                "file_size": os.path.getsize(STATE["pkg"]),
                "create_time": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            }
            if STATE["with_key"] and STATE["pem"]:
                entry["license_name"] = os.path.basename(STATE["pem"])
            return self._json({"result_code": "1",
                               "result_data": {"rowCount": 1, "pageList": [entry]}})
        if name in ("get_key", "get_packages"):
            if not is_dongle(STATE.get("last_dev_model")):
                sys.stderr.write("    !! refused: no dongle has asked for the package yet (last dev_model=%r)\n"
                                 % STATE.get("last_dev_model"))
                return self._json({"result_code": "0", "result_msg": "refused"}, 403)
            if name == "get_key":
                if not STATE["pem"]:
                    return self._json({"result_code": "0", "result_msg": "no key"}, 404)
                return self._file(STATE["pem"])
            return self._file(STATE["pkg"])
        if name == "result":
            text = body.decode("utf-8", "replace")
            sys.stderr.write("    result report (%d bytes):\n%s\n----end result----\n"
                             % (len(body), text))
            try:
                with open(os.path.join(STATE["dist"], "last_result.json"), "w") as f:
                    f.write(text)
                sys.stderr.write("    (saved full result to dist/last_result.json)\n")
            except OSError as e:
                sys.stderr.write("    (could not save result: %s)\n" % e)
            return self._json({"result_code": "1"})  # MUST be a string: stock parses it with atof(valuestring); a JSON int leaves cJSON valuestring=NULL -> NULL-deref crash in systemUpgradeStart before esp_ota
        # Unknown path
        sys.stderr.write("    !! unhandled path\n")
        return self._json({"result_code": "0", "result_msg": "not found"}, 404)

    def do_GET(self):
        self._route("GET")

    def do_POST(self):
        self._route("POST")


def _selfsigned_with_cryptography(cert, key):
    """Self-signed certificate without openssl on the PATH (Windows): uses the `cryptography`
    package from tools/requirements.txt."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    priv = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "iconfig.local")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert_obj = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                .public_key(priv.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(days=1))
                .not_valid_after(now + datetime.timedelta(days=825))
                .sign(priv, hashes.SHA256()))
    with open(key, "wb") as f:
        f.write(priv.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                   serialization.NoEncryption()))
    with open(cert, "wb") as f:
        f.write(cert_obj.public_bytes(serialization.Encoding.PEM))


def ensure_cert(cert, key, workdir):
    if cert and key and os.path.exists(cert) and os.path.exists(key):
        return cert, key
    cert = cert or os.path.join(workdir, "iconfig_fake_cert.pem")
    key = key or os.path.join(workdir, "iconfig_fake_key.pem")
    if os.path.exists(cert) and os.path.exists(key):
        return cert, key
    print("generating a self-signed cert at %s" % cert)
    try:
        _selfsigned_with_cryptography(cert, key)
    except ImportError:
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", key, "-out", cert, "-days", "825",
                        "-subj", "/CN=iconfig.local"], check=True)
    return cert, key


def main(argv=None):
    ap = argparse.ArgumentParser(description="Fake iConfig package server for WiNet .sgu install")
    ap.add_argument("--dist", default="dist", help="dir holding the .sgu / .bin / publicKey_V*.pem")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=20001)
    ap.add_argument("--cert", help="server cert PEM (self-signed generated if omitted)")
    ap.add_argument("--key", help="server key PEM")
    ap.add_argument("--file-id", type=int, default=901)
    ap.add_argument("--dev-model", default="WiNet-S",
                    help="dev_model echoed when the dongle's own request carries none")
    ap.add_argument("--no-key", action="store_true",
                    help="omit license_name so the dongle skips get_key and uses the already-imported key")
    args = ap.parse_args(argv)

    dist = os.path.abspath(args.dist)
    sgu, sig, pem, pkg = _find_artifacts(dist)
    if not pkg:
        sys.exit("no package .zip found in %s (the dongle's SPA unzips it; build it with "
                 "`make sgu` / package_sgu.py --zip, or zip the .sgu+.bin+Version_signature.xml)"
                 % dist)
    STATE.update(dist=dist, sgu=sgu, sig=sig, pem=pem, pkg=pkg, file_id=args.file_id,
                 dev_model=args.dev_model, with_key=not args.no_key)
    print("serving package : %s (%d bytes)" % (pkg, os.path.getsize(pkg)))
    print("  inner .sgu    : %s" % (sgu or "(none)"))
    print("public key      : %s" % (pem or "(none — relies on already-imported key)"))
    print("detached sig     : %s" % (sig or "(none)"))

    cert, key = ensure_cert(args.cert, args.key, dist)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)

    httpd = ThreadingHTTPServer((args.bind, args.port), Handler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    print("fake iConfig server on https://%s:%d  (Ctrl-C to stop)" % (args.bind, args.port))
    print("open the dongle's web UI from THIS computer (the dongle contacts the browser's address),")
    print("then System -> System update -> iConfig update package. NOT the device-update dialog:")
    print("that one targets the inverter; requests that do not name a WiNet are refused here.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
