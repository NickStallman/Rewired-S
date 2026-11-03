#!/usr/bin/env python3
"""Write the GitHub Pages index: ESP Web Tools install buttons for the WiNet-S serial route and
links to the manifests the ESPHome update entity polls.

    python3 -I tools/pages_index.py --version 0.9.0 --out site/index.html
"""
import argparse

ENTRIES = [
    ("winet-s-prebuilt", "WiNet-S, Wi-Fi", True),
    ("winet-s-ethernet-prebuilt", "WiNet-S, Ethernet", True),
    ("winet-s2-prebuilt", "WiNet-S2, Wi-Fi", False),
    ("winet-s2-ethernet-prebuilt", "WiNet-S2, Ethernet", False),
    ("winet-s-debug", "WiNet-S debug build (Wi-Fi)", True),
    ("winet-s-debug-ethernet", "WiNet-S debug build (Ethernet)", True),
    ("winet-s2-debug", "WiNet-S2 debug build", False),
]

PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Rewired-S firmware {version}</title>
<style>
body{{font:16px/1.5 system-ui,sans-serif;max-width:52rem;margin:2rem auto;padding:0 1rem;color:#1b1f23;background:#fff}}
h1{{font-size:1.6rem}} table{{border-collapse:collapse;width:100%}} td,th{{padding:.5rem;border-bottom:1px solid #ddd;text-align:left;vertical-align:top}}
.warn{{background:#fff4d6;border-left:4px solid #e0a800;padding:.75rem 1rem;margin:1rem 0}}
code{{background:#f3f4f6;padding:.1rem .3rem;border-radius:3px}}
</style>
<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
</head><body>
<h1>Rewired-S firmware {version}</h1>
<p>Open-source replacement firmware for the Sungrow WiNet-S and WiNet-S2 dongles.
<a href="https://github.com/NickStallman/Rewired-S">Project, documentation and releases</a>.</p>
<div class="warn"><strong>Alpha software.</strong> Read <a href="https://github.com/NickStallman/Rewired-S/blob/main/docs/install_serial.md">the serial install guide</a>
before flashing. The buttons below write only the app slot and the OTA selector of a <strong>WiNet-S</strong> over its UART pads;
they never touch the bootloader, the partition table or the external flash that holds the dongle's identity.
The WiNet-S2 has no accessible UART and is installed through the stock updater with a signed package from the release page.</div>
<table><tr><th>Image</th><th>Serial install (WiNet-S)</th><th>Files</th></tr>
{rows}
</table>
<p>Each <code>manifest.json</code> is also what the firmware's <em>Update</em> entity polls, so Home Assistant offers new releases to pre-built dongles.</p>
</body></html>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    rows = []
    for entry, label, serial in ENTRIES:
        button = (f'<esp-web-install-button manifest="firmware/{entry}/manifest.json"></esp-web-install-button>'
                  if serial else "stock updater only")
        rows.append(f"<tr><td><strong>{label}</strong><br><code>{entry}</code></td><td>{button}</td>"
                    f'<td><a href="firmware/{entry}/{entry}.ota.bin">{entry}.ota.bin</a><br>'
                    f'<a href="firmware/{entry}/manifest.json">manifest.json</a></td></tr>')
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(PAGE.format(version=args.version, rows="\n".join(rows)))


if __name__ == "__main__":
    main()
