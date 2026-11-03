#!/usr/bin/env python3
"""Checks on the signal definitions of data/families/*.yaml as applied by tools/gen_tables.py:

- no two rows of a table share/overlap a (fn, register range) unless a `shared` entry allows it
  - catches st6100-style address collisions;
- every key is unique per family, and every core entity key is unique per device package;
- a signal that matches no table row, a duplicate key, a bad fix or a bad field fails loudly.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "components", "sungrow_datamodel"))
import gen_tables as g  # noqa: E402


def run_gen(data_dir):
    with tempfile.TemporaryDirectory() as out:
        return subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_tables.py"),
                               "--out-root", out, "--data-dir", data_dir],
                              capture_output=True, text=True)


def main():
    tables, ov, meta = g.build()

    bad = [c for c in g.address_collisions(tables, ov["shared"]) if not c[3]]
    assert not bad, "address collisions: %s" % [
        "%s 0x%04x@%d / 0x%04x@%d" % (t, a["sigid"], a["regaddr"], b["sigid"], b["regaddr"])
        for t, a, b, _ in bad]
    # every `shared` entry is still needed
    used = {(t, a["fn"], a["regaddr"]) for t, a, b, ok in g.address_collisions(
        tables, ov["shared"]) if ok}
    assert used == set(ov["shared"]), set(ov["shared"]) - used

    # st6100 fix applied
    st = {r["sigid"]: r["regaddr"] for r in
          next(t for t in tables if t["name"] == "other_dev18_st6100")["rows"]}
    assert st[0x210D] == 4161 and st[0x210E] == 4163 and st[0x2113] == 4173

    for family, sigs in meta.items():
        keys = [m["key"] for m in sigs.values()]
        assert len(keys) == len(set(keys)), "duplicate keys in %s" % family

    ncore = 0
    for dname in g.DEVICES:
        p = g.device_plan(tables, meta, ov, dname)
        allk = p["core"] + p["text"] + p["state"]
        assert len(allk) == len(set(allk)), "duplicate core key in %s" % dname
        assert not set(allk) & set(p["extra_num"] + p["extra_str"]), dname
        assert len(p["extra_num"] + p["extra_str"]) == len(set(p["extra_num"] + p["extra_str"]))
        ncore += len(allk)
    hyb = g.device_plan(tables, meta, ov, "hybrid_inverter")["core"]
    for k in ("battery_soc", "battery_power", "mppt1_voltage", "export_power", "load_power",
              "daily_import_energy", "total_export_energy", "grid_frequency", "phase_a_voltage"):
        assert k in hyb, k
    st_core = g.device_plan(tables, meta, ov, "string_inverter")["core"]
    assert "battery_soc" not in st_core and "string1_current" in st_core

    # derived core entities and key fallback groups (protocol-suffixed duplicates collapse)
    hp = g.device_plan(tables, meta, ov, "hybrid_inverter")
    sp = g.device_plan(tables, meta, ov, "string_inverter")
    for p in (hp, sp):
        for k in ("total_yield", "mppt1_power", "grid_import_power", "grid_export_power"):
            assert k in p["core"], k
        for k, members in p["groups"].items():
            assert members[0] == k and len(members) > 1
            for m in members[1:]:
                assert m not in p["core"] and m not in p["extra_num"] + p["extra_str"], m
    assert hp["groups"]["battery_power"] == ["battery_power", "battery_power_sh"]
    assert sp["groups"]["load_power"] == ["load_power", "load_power_sg"]
    assert "load_power_sg" not in sp["core"]
    # bitfield children: hidden binary keys bound to parent + bit
    ch = meta["inverter"][0x5041]
    assert ch["key"] == "sig_5022_bit1" and ch["bit_of"] == (0x5022, 1)
    import register_meta  # noqa: E402  (generated)
    assert register_meta.FAMILIES["inverter"]["sig_5022_bit1"][-2:] == (0x5022, 1)
    assert register_meta.FAMILIES["inverter"]["total_yield"][0] == 0x2021

    # protection READS: unit / str_pv layout (32318..32329), unique addresses and keys
    reads = register_meta.READS["inverter"]
    addr = {k: r[0] for k, r in reads.items()}
    assert len(set(addr.values())) == len(addr), "duplicate READ address"
    assert not set(reads) & set(register_meta.FAMILIES["inverter"]), "READ key shadows a signal"
    want = {"over_voltage_recovery": 32318, "under_voltage_recovery": 32319,
            "over_frequency_recovery": 32320, "under_frequency_recovery": 32321,
            "under_voltage_l1_trip": 32322, "over_voltage_l1_trip": 32323,
            "under_frequency_l1_trip": 32324, "over_frequency_l1_trip": 32325,
            "under_voltage_l2_trip": 32326, "over_voltage_l2_trip": 32327,
            "under_frequency_l2_trip": 32328, "over_frequency_l2_trip": 32329,
            "grid_country_code": 30500, "grid_country_code_unit": 32310, "grid_standard": 30036,
            "reconnect_time": 30092, "volt_watt_enable": 31230, "volt_var_q4": 31872}
    for k, a in want.items():
        assert addr[k] == a, (k, addr[k], a)
    assert not [a for a in addr.values() if 30600 <= a <= 30700], "res-family protection block"
    # (regaddr, name, unit, mult, dc, sc, accuracy, fn, group, priority, enum, scale, dtype)
    assert reads["under_voltage_l1_trip"][2:4] == ("V", 1.0) and reads["under_voltage_l1_trip"][11] == 0.1
    assert reads["over_frequency_l1_trip"][11] == 0.01 and reads["over_frequency_l1_trip"][6] == 2
    assert reads["volt_var_q1"][12] == "s16" and reads["volt_var_v1"][12] == "u16"
    assert reads["grid_standard"][10] == "grid_standard"
    assert dict(register_meta.ENUMS["grid_standard"])[13] == "AS/NZS 4777.2:2020 Australia A"
    # meta.gen.ts: META carries /api/signals sigids only; READS go to PROTECTION_READS by address
    import gen_web_meta as w  # noqa: E402
    fam, info, enums, rd = w._load_meta()
    ts = w.gen(fam, info, enums)
    assert '"protection"' not in ts, "a protection READ leaked into the sigid-keyed META"
    ts = w.gen_reads(rd, enums)
    assert "address: 32322, key: \"under_voltage_l1_trip\"" in ts and "scale: 0.1" in ts
    assert w.gen_reads(rd, enums) == ts

    # fail-loudly cases: a modified copy of data/families + data/modbus_tcp
    inv = open(os.path.join(ROOT, "data", "families", "inverter.yaml"), encoding="utf-8").read()
    met = open(os.path.join(ROOT, "data", "families", "meter.yaml"), encoding="utf-8").read()
    cases = {
        "nomatch": ("inverter.yaml", inv.replace("signals:\n", "signals:\n- id: 0xEEEE\n  key: bogus\n"
                                                "  name: x\n  category: extra\n  confidence: guess\n"
                                                "  evidence: x\n", 1)),
        "dupkey": ("inverter.yaml", inv.replace("key: phase_b_voltage", "key: phase_a_voltage", 1)),
        "badfix": ("meter.yaml", met.replace("stock_address: 4161, address: 4163",
                                            "stock_address: 4161, address: 4999", 1)),
        "badcategory": ("inverter.yaml", inv.replace("category: extra", "category: bogus", 1)),
        "baddtype": ("inverter.yaml", inv.replace("dtype: s16\n", "dtype: f32\n", 1)),
    }
    for name, (fname, text) in cases.items():
        assert text != (inv if fname == "inverter.yaml" else met), name
        with tempfile.TemporaryDirectory() as d:
            shutil.copytree(os.path.join(ROOT, "data", "families"), os.path.join(d, "families"))
            shutil.copytree(os.path.join(ROOT, "data", "modbus_tcp"), os.path.join(d, "modbus_tcp"))
            with open(os.path.join(d, "families", fname), "w", encoding="utf-8") as f:
                f.write(text)
            r = run_gen(d)
            assert r.returncode != 0, "%s: generator accepted a bad data file" % name
            if name == "nomatch":
                assert "matches no table row" in r.stderr, r.stderr
    print("PASS gen_tables signal definitions (%d signals, %d core entities, %d allowed shared)"
          % (len(ov["names"]), ncore, len(ov["shared"])))


if __name__ == "__main__":
    main()
