#!/usr/bin/env python3
"""Setup profiles from data/grid_profiles/: fresh, deterministic, and faithful to the profile model."""
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


def check():
    for _ in range(2):
        r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "gen_grid_profiles.py"), "--check"],
                           stdout=subprocess.PIPE)
        assert r.returncode == 0, "gen_grid_profiles.py --check: stale (%s)" % r.stdout.decode()


def option(opts, fam, code):
    for g in opts:
        if g["family"] == fam and g["code"] == code:
            return g
    raise AssertionError("no %s option %d" % (fam, code))


def u32_set_check():
    """Every profile set that touches a U32 register carries both of its words (low word first),
    and every set register is in the family's register width table."""
    checked = 0
    for fam in gp.PROFILE_FAMILIES:
        types = {a: t for a, (t, _) in gp.reg_table(fam).items()}
        for a, words in gp.all_sets(fam):
            i = 0
            while i < len(words):
                if types.get(a + i) == "U32":
                    assert i + 1 < len(words), (fam, a + i, words)
                    checked += 1
                    i += 2
                else:
                    i += 1
    assert checked >= 30, checked
    return checked


def main():
    check()
    a, b = gp.outputs()[1], gp.outputs()[1]
    assert a == b, "gen_grid_profiles output is not deterministic"

    opts = gp.load()
    # Families and the stock option values.
    assert [(g["family"], g["code"]) for g in opts] == [
        ("str_pv", 1), ("res_es", 1), ("res_new_pv", 1),
        ("unit", 12), ("unit", 13), ("unit", 14), ("unit", 15)], [(g["family"], g["code"]) for g in opts]
    s1 = option(opts, "str_pv", 1)
    assert len(s1["variants"]) == 1 and len(s1["variants"][0]["sets"]) == 20
    for code in (13, 14, 15):
        g = option(opts, "unit", code)
        assert (g["needparam"], g["needreadtype"]) == (1, 1)
        pv, es = g["variants"]
        assert pv["ref5479"] == [1, 2] and es["ref5479"] == [3, 4, 5, 6]
        assert len(pv["sets"]) == 39 and sum(len(w) for _, w in pv["sets"]) == 111
        assert len(es["sets"]) == 41 and sum(len(w) for _, w in es["sets"]) == 121
    # The unit grid-type register 30037 is a commit register, never a profile set.
    assert all(a_ != 30037 for g in opts for v in g["variants"] for a_, _ in v["sets"])

    # Descriptor commit registers (index.yaml), not the catalogue's country item.
    assert gp.commit_regs("str_pv") == [30500, 30502]
    assert gp.commit_regs("res_es") == [30643] and gp.commit_regs("res_new_pv") == [30643]
    assert gp.commit_regs("unit") == [30036, 30037, 32310, 32311]
    assert gp.commit_regs("none") == [] and gp.commit_regs("unit_nodesc") == []

    t = gp.build_tables(opts)
    size = gp.data_bytes(t)
    assert size < 6000, size
    for g in opts:
        assert not re.fullmatch(r"[\d\s]+", g["label"])
    labels = gp.set_labels()
    assert labels["unit:32363"].endswith("(high word)"), labels.get("unit:32363")
    rows = {(r["family"], r["address"]) for r in gi.load()}
    for k in labels:
        fam, addr = k.split(":")
        assert (fam, int(addr)) not in rows, k

    n = u32_set_check()
    print("PASS gen_grid_profiles (%d options, %d runs, %d bytes, %d U32 set values checked)" % (
        len(opts), len(t["runs"]), size, n))


if __name__ == "__main__":
    main()
