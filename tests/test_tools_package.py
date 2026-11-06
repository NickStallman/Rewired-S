#!/usr/bin/env python3
"""tools/pyproject.toml names real scripts: every module exists, every console script resolves to
its `main`, the version matches (tests/test_version.py checks the value)."""
import importlib
import os
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")


def main():
    with open(os.path.join(TOOLS, "pyproject.toml"), "rb") as f:
        meta = tomllib.load(f)
    modules = meta["tool"]["setuptools"]["py-modules"]
    for m in modules:
        assert os.path.exists(os.path.join(TOOLS, m + ".py")), "py-modules names a missing file: %s" % m
    sys.path.insert(0, TOOLS)
    scripts = meta["project"]["scripts"]
    assert scripts, "no console scripts"
    for cmd, target in scripts.items():
        mod, _, fn = target.partition(":")
        assert mod in modules, "%s -> %s is not in py-modules" % (cmd, mod)
        assert cmd.startswith("rewired-s-"), cmd
        module = importlib.import_module(mod)
        assert callable(getattr(module, fn, None)), "%s has no %s()" % (mod, fn)
    assert meta["project"]["requires-python"] == ">=3.11"
    assert any(d.startswith("cryptography") for d in meta["project"]["dependencies"])
    print("PASS tools package: %d console scripts" % len(scripts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
