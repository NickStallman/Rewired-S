#!/usr/bin/env python3
"""Print the ESPHome build directory of an entry (where build/firmware.ota.bin lands).

    python3 -I tools/build_dir.py winet-s-prebuilt.yaml   -> .esphome/build/winet-s-prebuilt

ESPHome builds under `.esphome/build/<esphome.name>` unless the entry sets `esphome: build_path`
(the pre-built entries do, because they share one name). The directory is read from
`esphome config`, so it is right for either case. Pass `--esphome` to use a specific binary.
"""
import argparse
import os
import subprocess
import sys

import yaml


class _Loader(yaml.SafeLoader):
    """ESPHome's dump carries tags such as !secret and !lambda; their values are irrelevant here."""


_Loader.add_multi_constructor("!", lambda loader, suffix, node: None)


def build_dir(entry: str, esphome: str = "esphome") -> str:
    out = subprocess.run([esphome, "config", entry], capture_output=True, text=True, check=True).stdout
    cfg = yaml.load(out, Loader=_Loader)
    core = cfg["esphome"]
    path = core.get("build_path")
    if path:
        return os.path.normpath(os.path.join(".esphome", path)) if not os.path.isabs(path) else path
    return os.path.join(".esphome", "build", core["name"])


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("entry")
    ap.add_argument("--esphome", default="esphome")
    a = ap.parse_args()
    print(build_dir(a.entry, a.esphome))
