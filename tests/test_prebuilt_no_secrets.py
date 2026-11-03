#!/usr/bin/env python3
"""No `!secret` is reachable from the pre-built entries.

The pre-built images (winet-s-prebuilt.yaml and friends, docs/prebuilt.md) are built without a
secrets.yaml: Wi-Fi, the web login, the MQTT broker and the API key are provisioned at runtime.
This walks every `!include` (bare and `{file: ...}` form) from each entry and fails on the first
`!secret` tag in any file reached. Stdlib only; the YAML is scanned textually, which is enough for
the `!include` and `!secret` tags ESPHome uses.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PREBUILT = [
    "winet-s-prebuilt.yaml",
    "winet-s-ethernet-prebuilt.yaml",
    "winet-s2-prebuilt.yaml",
    "winet-s2-ethernet-prebuilt.yaml",
]
INCLUDE_RE = re.compile(r"!include\s+(?:\{\s*file:\s*)?([A-Za-z0-9_./-]+\.yaml)")
SECRET_RE = re.compile(r"!secret\b")


def strip_comment(line):
    # A '#' that starts a comment is preceded by whitespace or starts the line.
    m = re.match(r"^(.*?)(?:(?<=\s)#.*|^#.*)?$", line)
    return m.group(1) if m else line


def walk(path, seen, problems):
    if path in seen:
        return
    seen.add(path)
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = strip_comment(raw.rstrip("\n"))
            if SECRET_RE.search(line):
                problems.append(f"{os.path.relpath(path, ROOT)}:{n}: {line.strip()}")
            for inc in INCLUDE_RE.findall(line):
                target = os.path.normpath(os.path.join(os.path.dirname(path), inc))
                if not os.path.isfile(target):
                    problems.append(f"{os.path.relpath(path, ROOT)}:{n}: include not found: {inc}")
                    continue
                walk(target, seen, problems)


def main():
    failures = 0
    for entry in PREBUILT:
        path = os.path.join(ROOT, entry)
        if not os.path.isfile(path):
            print(f"FAIL {entry}: missing")
            failures += 1
            continue
        seen, problems = set(), []
        walk(path, seen, problems)
        if problems:
            failures += 1
            print(f"FAIL {entry} ({len(seen)} files):")
            for p in problems:
                print("  " + p)
        else:
            print(f"PASS {entry}: no !secret in {len(seen)} files")
    # The regular entries DO use secrets (the test would be vacuous otherwise).
    seen, problems = set(), []
    walk(os.path.join(ROOT, "winet-s.yaml"), seen, problems)
    if not any("!secret" in p for p in problems):
        print("FAIL winet-s.yaml: expected !secret tags in the compiled entry's includes")
        failures += 1
    else:
        print("PASS winet-s.yaml: compiled entry still reads secrets")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
