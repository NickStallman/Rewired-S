#!/usr/bin/env python3
"""Every entry validates when ESPHome fetches it as a remote package.

A user's whole configuration can be `packages: {x: github://NickStallman/Rewired-S/<entry>.yaml@<ref>}`
in an otherwise empty directory. ESPHome then resolves `!include`, file paths and the local
`components` path next to the fetched file, not under the user's directory. Including each entry by
absolute path from a temporary directory makes ESPHome resolve paths the same way, with no network.

    python3 -I tests/test_remote_package.py [entry.yaml ...]     (default: every entry)
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESPHOME = os.environ.get("ESPHOME", os.path.join(ROOT, ".venv", "bin", "esphome"))
if os.sep not in ESPHOME:
    ESPHOME = shutil.which(ESPHOME) or ESPHOME
ENTRIES = ["winet-s.yaml", "winet-s-ethernet.yaml", "winet-s2.yaml", "winet-s2-ethernet.yaml",
           "winet-s-prebuilt.yaml", "winet-s-ethernet-prebuilt.yaml", "winet-s2-prebuilt.yaml",
           "winet-s2-ethernet-prebuilt.yaml"]

# A valid secrets.yaml for the compiled entries (the pre-built ones read none).
SECRETS = """wifi_ssid: "example"
wifi_password: "example-password"
fallback_ap_password: "example-ap-pw"
api_encryption_key: "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA="
web_username: "admin"
web_password: "example-web-pw"
mqtt_broker: "192.0.2.20"
mqtt_username: "mqtt"
mqtt_password: "example-mqtt-pw"
"""


def check(entry):
    with tempfile.TemporaryDirectory(prefix="rewired-remote-") as d:
        with open(os.path.join(d, "secrets.yaml"), "w", encoding="utf-8") as f:
            f.write(SECRETS)
        site = os.path.join(d, "site.yaml")
        with open(site, "w", encoding="utf-8") as f:
            f.write("packages:\n  entry: !include %s\n" % os.path.join(ROOT, entry))
        r = subprocess.run([ESPHOME, "config", site], cwd=d, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
        if r.returncode != 0:
            tail = "\n".join(r.stderr.strip().splitlines()[-12:])
            print("FAIL %s as a remote package:\n%s" % (entry, tail))
            return False
        print("ok %s" % entry)
        return True


def main(argv):
    entries = argv or ENTRIES
    if not os.path.exists(ESPHOME):
        print("SKIP remote package check (no esphome at %s; set ESPHOME=)" % ESPHOME)
        return 0
    ok = all([check(e) for e in entries])
    print("PASS every entry validates as a remote package" if ok else "FAIL remote package check")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
