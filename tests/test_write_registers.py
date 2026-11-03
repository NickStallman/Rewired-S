"""Checks for the write allow-list (data/write_registers.yaml -> components/sungrow_control)."""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_write_registers as g  # noqa: E402

rows = g.load()
by_key = {r["key"]: r for r in rows}

# Generated files are up to date.
rc = subprocess.call([sys.executable, "-I", os.path.join(ROOT, "tools", "gen_write_registers.py"), "--check"])
assert rc == 0, "generated sungrow_control files are stale"

# Document numbers (1-based); mkaiser's YAML wire address = address - 1 (as quoted in `mkaiser`).
EXPECT = {
    "ems_mode": 13050, "forced_cmd": 13051, "forced_power": 13052, "max_soc": 13058,
    "min_soc": 13059, "export_power_limit": 13074, "export_power_limit_mode": 13087,
    "reserved_soc_backup": 13100, "battery_max_charge_power": 33047,
    "battery_max_discharge_power": 33048, "power_limit_switch": 5007, "power_limit_pct": 5008,
}
for k, a in EXPECT.items():
    assert by_key[k]["address"] == a, (k, by_key[k]["address"])
for r in rows:
    m = re.search(r"wire (\d+)", r["mkaiser"])
    if m:
        assert int(m.group(1)) == r["address"] - 1, (r["key"], "wire != address - 1")

# mkaiser / battery-controller strings, byte for byte.
opts = dict(by_key["ems_mode"]["options"])
assert opts[0] == "Self-consumption mode (default)" and opts[2] == "Forced mode" and opts[3] == "External EMS"
assert 8 not in by_key["ems_mode"]["values"]  # MicroGrid shown, never written (as mkaiser)
opts = dict(by_key["forced_cmd"]["options"])
assert opts == {0xCC: "Stop (default)", 0xAA: "Forced charge", 0xBB: "Forced discharge"}
assert dict(by_key["export_power_limit_mode"]["options"]) == {0xAA: "Enabled", 0x55: "Disabled"}

# Start/stop follows the stock descriptor table by protocol type: 5006 by default, 30045 on
# protocol 4 (string), 13000 on protocol 5 (hybrid). 13000 / 30045 appear only as those per-protocol
# start/stop addresses; 30043 (factory reset) never.
START_STOP = {"start_stop": {4: 30045}, "hybrid_start_stop": {5: 13000}}
for r in rows:
    assert r["address"] not in (13000, 30043, 30045), r["key"]
    assert r["address_by_protocol"] == START_STOP.get(r["key"], {}), r["key"]
for k in START_STOP:
    assert by_key[k]["address"] == 5006 and by_key[k]["values"] == [0xCE, 0xCF], k
    assert by_key[k]["confidence"] == "likely", k
assert by_key["start_stop"]["profile"] == "string" and by_key["hybrid_start_stop"]["profile"] == "hybrid"
assert "setup descriptor" in by_key["hybrid_start_stop"]["evidence"]

# Generated C++: the per-protocol address and a read-back table per alt protocol.
cpp = open(os.path.join(ROOT, "components", "sungrow_control", "write_registers.cpp"), encoding="utf-8").read()
assert "static const RuleAlt k_alt_start_stop[] = {{4, 30045}};" in cpp
assert "static const RuleAlt k_alt_hybrid_start_stop[] = {{5, 13000}};" in cpp
assert "k_table_string_p4" in cpp and "k_table_hybrid_p5" in cpp

# Every key used by the device packages exists and lives in the right profile.
for pkg, prof in (("hybrid_control.yaml", "hybrid"), ("string_control.yaml", "string")):
    text = open(os.path.join(ROOT, "devices", pkg), encoding="utf-8").read()
    keys = re.findall(r"^\s+key: (\w+)", text, re.M)
    assert keys, pkg
    for k in keys:
        assert k in by_key and by_key[k]["profile"] == prof, (pkg, k)
    assert "profile: %s" % prof in text

# address_by_protocol parsing: bad entries and collisions with another row are rejected.
import tempfile  # noqa: E402

def _load_text(text):
    with tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False) as f:
        f.write("registers:\n" + text)
    try:
        return g.load(f.name)
    finally:
        os.unlink(f.name)

ROW = """- profile: string
  key: k%d
  name: x
  address: %d
  %s
  type: u16
  min: 0
  max: 1
  interval: 10
  mkaiser: "-"
  confidence: likely
  evidence: "-"
"""
ok = _load_text(ROW % (1, 5006, 'address_by_protocol: {4: 30045, 5: 13000}'))
assert ok[0]["address_by_protocol"] == {4: 30045, 5: 13000}
for bad in ('address_by_protocol: {9: 30045}', 'address_by_protocol: "4:30045"',
            'address_by_protocol: {4: 70000}'):
    try:
        _load_text(ROW % (1, 5006, bad))
    except SystemExit:
        pass
    else:
        raise AssertionError(bad)
try:
    _load_text(ROW % (1, 5006, 'address_by_protocol: {4: 5007}') + ROW % (2, 5007, "unit: \"\""))
except SystemExit:
    pass
else:
    raise AssertionError("alt address colliding with another row accepted")

# Read-back sigids stay clear of the derived range (0xF001-0xF034).
for r in rows:
    assert 0xF100 <= r["sigid"] < 0xF180, r["key"]
print("write_registers: %d entries ok" % len(rows))
