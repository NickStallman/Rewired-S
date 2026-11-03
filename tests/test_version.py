#!/usr/bin/env python3
"""The release version agrees everywhere it is written down.

common/base.yaml `project_version` (the ESPHome project version), the SPA's package.json
`version` and the newest released `## [x.y.z]` heading of CHANGELOG.md must be the same string.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def base_version():
    with open(os.path.join(ROOT, "common", "base.yaml"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r'\s+project_version:\s*"?([^"\s#]+)"?', line)
            if m:
                return m.group(1)
    raise AssertionError("common/base.yaml has no project_version substitution")


def web_version():
    with open(os.path.join(ROOT, "components", "sungrow_web", "web", "package.json"), encoding="utf-8") as f:
        return json.load(f)["version"]


def changelog_version():
    with open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"## \[(\d+\.\d+\.\d+)\]", line)
            if m:
                return m.group(1)
    raise AssertionError("CHANGELOG.md has no released version heading")


def main():
    versions = {"common/base.yaml": base_version(),
                "web/package.json": web_version(),
                "CHANGELOG.md": changelog_version()}
    if len(set(versions.values())) != 1:
        print("FAIL version mismatch: %s" % ", ".join("%s %s" % kv for kv in versions.items()))
        return 1
    print("PASS version %s" % versions["CHANGELOG.md"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
