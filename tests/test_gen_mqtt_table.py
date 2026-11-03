#!/usr/bin/env python3
"""tools/gen_mqtt_table.py output must be deterministic and match the committed files."""
import filecmp
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, "tools", "gen_mqtt_table.py")
FILES = ["mqtt_point_table.h", "mqtt_point_table.cpp"]


def main():
    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        for d in (a, b):
            subprocess.run([sys.executable, "-I", GEN, "--out-dir", d], check=True, stdout=subprocess.DEVNULL)
        for f in FILES:
            assert filecmp.cmp(os.path.join(a, f), os.path.join(b, f), False), "non-deterministic: " + f
            committed = os.path.join(ROOT, "components", "isolarcloud", f)
            assert os.path.exists(committed) and filecmp.cmp(os.path.join(a, f), committed, False), \
                "committed %s is stale, rerun tools/gen_mqtt_table.py" % f
        src = open(os.path.join(a, "mqtt_point_table.cpp")).read()
        # Spot checks against the stock rundata rows.
        assert "{1, 0x2020, 2, 0x2, 9, 4," in src
        assert "{2, 0x2021, 8, 0x2, 10, 8," in src
        assert "{0x23, 14, " in src and "{0x15, 1, " in src and "{0x02, 22, " in src
    print("gen_mqtt_table: OK")


if __name__ == "__main__":
    main()
