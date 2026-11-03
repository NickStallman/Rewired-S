#!/usr/bin/env python3
"""Print the CHANGELOG.md section of one version (release notes for the GitHub release).

    python3 -I tools/changelog_section.py 0.9.0 [CHANGELOG.md]
"""
import re
import sys


def section(text: str, version: str) -> str:
    m = re.search(r"^## \[" + re.escape(version) + r"\][^\n]*\n(.*?)(?=^## \[|\Z)", text, re.S | re.M)
    if not m:
        raise SystemExit(f"no section for {version} in the changelog")
    return m.group(1).strip() + "\n"


if __name__ == "__main__":
    path = sys.argv[2] if len(sys.argv) > 2 else "CHANGELOG.md"
    with open(path, encoding="utf-8") as f:
        sys.stdout.write(section(f.read(), sys.argv[1]))
