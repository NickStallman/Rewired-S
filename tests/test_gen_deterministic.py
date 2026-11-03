#!/usr/bin/env python3
"""gen_tables.py output must be deterministic and match the committed generated files."""
import filecmp
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, "tools", "gen_tables.py")
OUTPUTS = ["components/sungrow_datamodel/register_tables.h",
           "components/sungrow_datamodel/register_tables.cpp",
           "components/sungrow_datamodel/register_meta.py"]


def run(out):
    subprocess.run([sys.executable, GEN, "--out-root", out], check=True,
                   stdout=subprocess.DEVNULL)
    files = OUTPUTS + sorted("devices/" + f for f in os.listdir(os.path.join(out, "devices")))
    return files


def main():
    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        fa, fb = run(a), run(b)
        assert fa == fb, (fa, fb)
        bad = [f for f in fa if not filecmp.cmp(os.path.join(a, f), os.path.join(b, f), False)]
        assert not bad, "non-deterministic output: %s" % bad
        stale = [f for f in fa if not os.path.exists(os.path.join(ROOT, f)) or
                 not filecmp.cmp(os.path.join(a, f), os.path.join(ROOT, f), False)]
        assert not stale, "committed generated files are stale, rerun tools/gen_tables.py: %s" % stale
        # metadata module imports and resolves a few keys
        sys.path.insert(0, os.path.join(a, "components", "sungrow_datamodel"))
        import register_meta as m
        assert m.FAMILIES["inverter"]["active_power"][0] == 0x2080
        assert m.FAMILIES["battery"]["cell_voltage_spread"][0] == 0xF007
        assert m.FAMILIES["inverter"]["internal_temperature"][2] == "°C"
        assert m.FAMILIES["inverter"]["reactive_power"][2:4] == ("var", 1000.0)
        assert m.FAMILIES["inverter"]["battery_soc"][0] == 0x2514
        assert m.FAMILIES["inverter"]["battery_soc"][4] == "battery"
        assert m.FAMILIES["inverter"]["battery_power"][0] == 0x252C
        assert m.INFO["inverter"]["mppt1_voltage"][0] == "confirmed"
        assert m.FAMILIES["inverter"]["first_fault"][0] == 0xF065
        assert m.FAMILIES["battery"]["first_fault"][7] is True
        assert m.FAMILIES["inverter"]["state_transient"][0] == 0xF066
        assert m.INFO["inverter"]["state_transient"][5] == "no_yes"
        assert m.FAULT_CODES["inverter"][39] == "Low insulation resistance"
        assert m.FAULT_CODE_OVERRIDES == {}
        assert set(m.DEV_STATE_CLASS) == {c for c, _ in m.ENUMS["device_state"]}
        assert set(m.DEV_STATE_CLASS.values()) <= set(m.DEV_STATE_CLASSES)
        assert m.DEV_STATE_CLASS[0x8200] == "RUNNING" and m.DEV_STATE_CLASS[0x1300] == "STOPPED"
        assert 0x1900 in m.TRANSIENT_STATES["inverter"]
        assert m.TRANSIENT_STATE_EXTRAS == {"string": [0x1300]}
        assert m.INVERTER_MODEL_DEV_TYPES == {"hybrid": (35,), "string": (1, 21)}
    print("PASS gen_tables deterministic (%d files)" % len(fa))


if __name__ == "__main__":
    main()
