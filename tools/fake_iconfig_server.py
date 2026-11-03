#!/usr/bin/env python3
r"""Minimal fake "iConfig" package server so a stock WiNet-S pulls and installs OUR signed .sgu.

Background: on WiNet-S firmware P044 the browser has no .sgu upload; the dongle instead PULLS a
firmware package over HTTPS from an iConfig server whose IP the web UI is given. The dongle then
verifies the .sgu with the SAME on-device RSA check (against our imported key) and esp_ota-flashes
it.

This server impersonates iConfig: it listens HTTPS on :20001 and answers the five /v2/web/* requests
the dongle makes, serving our files from dist/ by path (it ignores the dongle's time/sign/file_id).

IMPORTANT: get_packages must serve a .ZIP, not a bare .sgu. After the
dongle downloads the package, the web UI re-fetches it, unzips it in the browser (JSZip), and
re-submits each member to /upgrade/upgradeCheck?upgrademode=1&public_key_version=... (le() in the
SPA). A bare .sgu makes the UI throw "Please Select Zip File" / crash on a `/\.zip$/` path match.
The zip holds the three files the updater actually ingests + verifies: <base>.sgu, its detached
<base>.bin signature, and Version_signature.xml (the pubkey comes from the device trust store, not
the zip). The on-device RSA check then verifies the .sgu vs the .bin against our
imported key, and esp_ota flashes it.

    python3 -I tools/fake_iconfig_server.py --dist dist --bind 0.0.0.0 --port 20001

Then in the dongle web UI (admin): System update -> iConfig update package -> enter THIS host's LAN
IP. The dongle discovers the port (20001 first), downloads our key + .sgu, verifies and flashes.

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
         "dev_model": "WiNet-S", "with_key": True}


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
        path = u.path.rstrip("/")
        sys.stderr.write("%s %s\n" % (method, self.path))
        if path.endswith("/v2/web/get_time"):
            return self._json({"result_code": "1", "result_data": {"time": int(time.time())}})
        if path.endswith("/v2/web/select_packages"):
            entry = {
                "file_id": STATE["file_id"],
                "file_name": os.path.basename(STATE["pkg"]),
                "dev_model": STATE["dev_model"],
                "version": os.path.splitext(os.path.basename(STATE["pkg"]))[0],
                "file_size": os.path.getsize(STATE["pkg"]),
                "create_time": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            }
            if STATE["with_key"] and STATE["pem"]:
                entry["license_name"] = os.path.basename(STATE["pem"])
            return self._json({"result_code": "1",
                               "result_data": {"rowCount": 1, "pageList": [entry]}})
        if path.endswith("/v2/web/get_key"):
            if not STATE["pem"]:
                return self._json({"result_code": "0", "result_msg": "no key"}, 404)
            return self._file(STATE["pem"])
        if path.endswith("/v2/web/get_packages"):
            return self._file(STATE["pkg"])
        if path.endswith("/v2/web/result"):
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else b""
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
    ap.add_argument("--dev-model", default="WiNet-S")
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
    print("point the dongle's 'System update -> iConfig' at this host's LAN IP.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
