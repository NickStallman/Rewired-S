#!/usr/bin/env python3
"""Installer catalogue, per-family allow-lists and value rules: fresh, deterministic, sane."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)

import gen_grid_profiles as gp  # noqa: E402
import gen_installer_settings as gi  # noqa: E402
import gen_installer_rules as ir  # noqa: E402


def check(gen):
    for _ in range(2):  # twice: the committed output is stable across runs
        r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, gen), "--check"], stdout=subprocess.PIPE)
        assert r.returncode == 0, "%s --check: committed output stale, rerun it (%s)" % (gen, r.stdout.decode())


def row(rows, fam, addr):
    for r in rows:
        if r["family"] == fam and r["address"] == addr:
            return r
    raise AssertionError("no %s row %d" % (fam, addr))


def main():
    check("gen_installer_settings.py")
    a, b = gi.outputs()[1], gi.outputs()[1]
    assert a == b, "gen_installer_settings output is not deterministic"

    rows = gi.load()
    assert len(rows) > 50, len(rows)
    fams = {r["family"] for r in rows}
    assert fams == {"str_pv", "res_es", "res_new_pv", "unit"}, fams
    for r in rows:
        assert r["control"] in ("number", "enum", "switch"), r
        assert r["dtype"] in ("U16", "S16", "U32"), r
        assert 1 <= r["address"] <= 0xFFFF
        if r["control"] in ("enum", "switch"):
            assert r["options"], r
        assert r["category"], r
        # labels are English text: no numeric keys, no unfilled placeholders
        for text in [r["label"]] + r["category"]:
            assert not re.fullmatch(r"[\d\s]+", text), ("all-digit label", r["family"], r["address"], text)
        for text in [r["label"]] + r["category"] + [o["label"] for o in r["options"]]:
            assert "{0}" not in text and "%@" not in text, (r["family"], r["address"], text)
        assert r["range_source"] in ("stock", "envelope", "dtype"), r
        assert r["raw_min"] <= r["raw_max"], r

    # U32 rows are low word first in the TS catalogue.
    ts = gi.outputs()[1][os.path.join(gi.WEB_DIR, "installer_settings.gen.ts")]
    for line in ts.splitlines():
        if 'dtype: "U32"' in line:
            assert 'wordOrder: "lo_first"' in line, line
    assert "hi word" not in ts

    # unit 30037 carries every grid-type variant's options.
    gt = {o["value"] for o in row(rows, "unit", 30037)["options"]}
    assert set(range(1, 19)) <= gt, sorted(gt)
    # unit country register 32310 is in the catalogue with Australia.
    assert 6 in {o["value"] for o in row(rows, "unit", 32310)["options"]}

    # per-family allow-lists.
    allow = {f: set(gi.allow_addrs(rows, f)) for f in gp.PROFILE_FAMILIES}
    assert 32322 in allow["str_pv"]          # str_pv UV-L1
    assert 30646 not in allow["str_pv"]      # res-family block
    assert 30646 in allow["res_es"]
    assert {30500, 30502} <= allow["str_pv"]
    assert 30643 in allow["res_es"] and 30643 in allow["res_new_pv"]
    assert {32310, 32311, 30036, 30037} <= allow["unit"]
    for f in ("str_pv", "res_es", "res_new_pv"):
        assert 30036 not in allow[f], f
        assert 32310 not in allow[f], f
    assert gi.allow_addrs(rows, "none") == [] and gi.allow_addrs(rows, "unit_nodesc") == []
    for f in gp.PROFILE_FAMILIES:
        for r in rows:
            if r["family"] == f:
                assert r["address"] in allow[f]
                if r["dtype"] == "U32":
                    assert r["address"] + 1 in allow[f]
        for a_, ws in gp.all_sets(f):
            assert set(range(a_, a_ + len(ws))) <= allow[f], (f, a_)

    # Ranges: every profile set value fits its row's range; typos do not.
    for f in gp.PROFILE_FAMILIES:
        vals = gi.set_values(f, {r["address"]: r["dtype"] for r in rows if r["family"] == f})
        for r in rows:
            if r["family"] != f:
                continue
            for v in vals.get(r["address"], ()):
                assert r["raw_min"] <= v <= r["raw_max"], (f, r["address"], v, r["raw_min"], r["raw_max"])
    assert row(rows, "str_pv", 32323)["raw_max"] < 26500
    assert row(rows, "unit", 32323)["raw_max"] < 26500

    # The generators run without the rules file (envelope / dtype only).
    bare = ir.load(path=os.path.join(ROOT, "data", "no_such_installer_rules.yaml"))
    assert not bare["ranges"] and not bare["rels"]
    rows2 = [dict(r, dtype=None if r["address"] == 31272 else r["dtype"]) for r in gi.load()]
    gi.apply_ranges(rows2, bare)
    assert {r["range_source"] for r in rows2} <= {"envelope", "dtype"}
    assert "INSTALLER_RELS" not in gi.gen_rules_h(rows2, bare)

    # Relations (data/installer_rules.yaml): UV2 <= UV1 < UV recovery, OV2 >= OV1 >
    # OV recovery for 32318-32329 in both str_pv and unit.
    if os.path.exists(ir.RULES_PATH):
        tabs = gi.rules_tables(rows, gi.stock_rules())
        for f in ("str_pv", "unit"):
            rels = set(tabs[f][1])
            for rel in [(32326, 32322, "<="), (32322, 32319, "<"), (32323, 32318, ">"),
                        (32327, 32323, ">="), (32328, 32324, "<="), (32329, 32325, ">=")]:
                assert rel in rels, (f, rel)
                flipped = (rel[1], rel[0], gi.OP_FLIP[rel[2]])
                assert flipped in rels, (f, flipped)
        assert row(rows, "unit", 32322)["regulation"] == "@32322<@32319;@32322>=@32326"
        assert row(rows, "str_pv", 32322)["range_source"] == "stock"

    print("PASS gen_installer_settings (%d settings; allow %s)" % (
        len(rows), ", ".join("%s %d" % (f, len(allow[f])) for f in gp.PROFILE_FAMILIES)))


if __name__ == "__main__":
    main()
