#!/usr/bin/env python3
"""components/sungrow_mqtt/ha_meta_gen.py: deterministic output that matches the committed header."""
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, "components", "sungrow_mqtt", "ha_meta_gen.py")
OUT = os.path.join(ROOT, "components", "sungrow_mqtt", "ha_meta_gen.h")


def load():
    spec = importlib.util.spec_from_file_location("ha_meta_gen", GEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    g = load()
    a, b = g.gen_header(), g.gen_header()
    assert a == b, "non-deterministic output"
    with open(OUT, encoding="utf-8") as f:
        committed = f.read()
    assert committed == a, "ha_meta_gen.h is stale, run components/sungrow_mqtt/ha_meta_gen.py"
    # --check agrees
    r = subprocess.run([sys.executable, "-I", GEN, "--check"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    # the device_state enum carries the standby code and the derived rows are published
    assert "0xA100" in a
    assert '"state_transient"' not in a and '"transient_state"' in a  # slug from the display name
    assert "{0xF066, \"Transient state\"" in a and "&HA_ENUM_no_yes" in a
    assert "{0xF065, \"First active fault\"" in a
    assert "Temperature or microgrid derating" in a
    # static identity rows are diagnostic in every family that has them
    for line in a.splitlines():
        if '"serial_number"' in line or '"device_model"' in line:
            assert line.rstrip(",").endswith("true}"), line
    print("PASS ha_meta_gen deterministic and current")


if __name__ == "__main__":
    main()
