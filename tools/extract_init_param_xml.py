#!/usr/bin/env python3
"""Extract the grid-code configuration XML files (web_<family>_config.xml) from a WiNet
external-flash (/usrdataout) LittleFS dump.

The dongle keeps one XML file per settings family: the country / grid-code picker with its
register sets and the parameter pages. They persist across a factory reset and a reflash of
the app, so they can be recovered offline from an ext-flash dump. The XML files themselves are
not part of this repository; `--to-project-format` turns the extracted set into
data/grid_profiles/<family>.yaml (tools/grid_profiles_xml.py, docs/data_format.md) so it can be
compared with the committed data (tests/test_xml_drift.py) or adopted.

The dump is a raw LittleFS image of the whole external SPI-NOR chip (block_size 4096), as pulled over the
debug build's `GET /dump/extflash` endpoint (with its companion .hdr).

Usage:
    python3 tools/extract_init_param_xml.py                 # defaults (verify SHA, extract the 4 XML)
    python3 tools/extract_init_param_xml.py --list          # just list /log/init_param_xml
    python3 tools/extract_init_param_xml.py --bin DUMP --out DIR
    python3 tools/extract_init_param_xml.py --to-project-format [--profiles-out DIR] [--labels FILE]
        also convert the extracted XML to the project format (default DIR: <out>/grid_profiles/;
        --labels: a key=text label file, else the committed labels are reused by register)

Requires: littlefs-python (pip install littlefs-python); PyYAML for --to-project-format.
"""
import argparse
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_BIN = os.path.join(REPO, "dist", "flash_backup", "extflash.bin")  # flash_backup.py output
DEFAULT_OUT = os.path.join(REPO, "data", "fetched", "extracted_xml")  # git-ignored
XML_DIR = "/log/init_param_xml"
BLOCK_SIZE = 4096  # stock WiNet /usrdataout LittleFS geometry (sungrow_extflash.cpp verifies == 4096)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def expected_sha(bin_path):
    """Read X-SHA256 from the sibling .hdr (the HTTP response that produced the dump), if present."""
    hdr = os.path.splitext(bin_path)[0] + ".hdr"
    if not os.path.exists(hdr):
        return None
    m = re.search(r"(?im)^X-SHA256:\s*([0-9a-f]{64})", open(hdr, "r", errors="replace").read())
    return m.group(1).lower() if m else None


def mount(bin_path):
    try:
        from littlefs import LittleFS
    except ImportError:
        sys.exit("error: littlefs-python not installed (pip install littlefs-python)")
    data = open(bin_path, "rb").read()
    if len(data) % BLOCK_SIZE:
        sys.exit(f"error: image size {len(data)} is not a multiple of block_size {BLOCK_SIZE}")
    block_count = len(data) // BLOCK_SIZE
    fs = LittleFS(block_size=BLOCK_SIZE, block_count=block_count, read_size=256, prog_size=256,
                  cache_size=256, lookahead_size=32, mount=False)
    fs.context.buffer[:] = data
    fs.mount()  # raises on corruption
    return fs


def list_xml(fs):
    try:
        names = fs.listdir(XML_DIR)
    except Exception as e:
        sys.exit(f"error: cannot list {XML_DIR}: {e}")
    out = []
    for n in sorted(names):
        st = fs.stat(f"{XML_DIR}/{n}")
        out.append((n, st.size))
    return out


def read_file(fs, path):
    with fs.open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=DEFAULT_BIN, help="ext-flash LittleFS dump (.bin)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output directory for extracted XML")
    ap.add_argument("--list", action="store_true", help="list the init_param_xml dir and exit")
    ap.add_argument("--no-verify", action="store_true", help="skip the SHA-256 integrity check")
    ap.add_argument("--to-project-format", action="store_true",
                    help="also write data/grid_profiles-format YAML from the extracted XML")
    ap.add_argument("--profiles-out", default=None, help="output directory for the YAML (default <out>/grid_profiles)")
    ap.add_argument("--labels", default=None, help="key=text label file for --to-project-format")
    args = ap.parse_args()

    if not os.path.exists(args.bin):
        sys.exit(f"error: dump not found: {args.bin}")

    if not args.no_verify:
        want = expected_sha(args.bin)
        got = sha256_of(args.bin)
        if want is None:
            print(f"warning: no .hdr next to the dump; cannot verify. sha256={got}")
        elif want != got:
            sys.exit(f"error: SHA-256 mismatch!\n  expected {want}\n  got      {got}")
        else:
            print(f"SHA-256 OK: {got}")

    fs = mount(args.bin)
    files = list_xml(fs)
    if not files:
        sys.exit(f"error: no files found in {XML_DIR}")
    print(f"{XML_DIR}:")
    for n, sz in files:
        print(f"  {n}  ({sz} B)")
    if args.list:
        return

    os.makedirs(args.out, exist_ok=True)
    for n, _ in files:
        data = read_file(fs, f"{XML_DIR}/{n}")
        dst = os.path.join(args.out, n)
        with open(dst, "wb") as f:
            f.write(data)
        print(f"  -> {os.path.relpath(dst, REPO)}  ({len(data)} B)")
    print(f"\nextracted {len(files)} file(s) to {os.path.relpath(args.out, REPO)}")
    if args.to_project_format:
        out_dir = args.profiles_out or os.path.join(args.out, "grid_profiles")
        paths = convert_to_project_format(args.out, out_dir, args.labels)
        print("project format: %s" % ", ".join(os.path.relpath(p, REPO) for p in paths))


def convert_to_project_format(xml_dir, out_dir, labels_path=None):
    """Convert web_<family>_config.xml files in xml_dir to data/grid_profiles YAML in out_dir
    (shared with tools/fetch_grid_profiles.py). Returns the paths written."""
    sys.path.insert(0, HERE)
    import datalib
    import gen_installer_rules
    import grid_profiles_xml as gpx
    index, docs = datalib.load_grid_profiles()
    label = gpx.labels_from_properties(labels_path) if labels_path else gpx.labels_from_committed(docs)
    converted = gpx.convert(xml_dir, index, label, gen_installer_rules.load()["dtypes"])
    if not converted:
        sys.exit(f"error: no web_<family>_config.xml in {xml_dir}")
    return gpx.write_profiles(out_dir, converted)


if __name__ == "__main__":
    main()
