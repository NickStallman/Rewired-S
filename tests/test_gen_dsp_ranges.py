#!/usr/bin/env python3
"""Power-stage acceptance ranges: manifest + CSVs -> dsp_ranges.gen.h, fresh and deterministic."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)

import gen_dsp_ranges as gd  # noqa: E402


def by_key(models, key):
    for m, ranges in models:
        if m["key"] == key:
            return m, ranges
    raise AssertionError("no line with key %s" % key)


def main():
    for _ in range(2):  # the committed header is what the generator produces, every run
        r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "gen_dsp_ranges.py"), "--check"],
                           stdout=subprocess.PIPE)
        assert r.returncode == 0, "gen_dsp_ranges.py --check: header stale, rerun it (%s)" % r.stdout.decode()

    models = gd.load()
    assert gd.gen_h(models) == gd.gen_h(gd.load()), "gen_dsp_ranges output is not deterministic"
    assert gd.gen_h(models) == open(gd.OUT_H, encoding="utf-8").read()

    keys = [m["key"] for m, _ in models]
    assert keys == sorted(keys) and len(set(keys)) == len(keys), keys
    assert {"SH_RS", "SG_RS", "SH_T"} <= set(keys), keys
    names = [x for m, _ in models for x in m["models"]]
    assert len(set(n.upper() for n in names)) == len(names), "model listed twice"
    codes = [c for m, _ in models for c in m["dev_codes"].values()]
    assert len(set(codes)) == len(codes) and {3355, 9734, 3621} <= set(codes), codes
    for m, ranges in models:
        assert m["family"] in ("hybrid", "string"), m
        assert m["firmware"] and m["name"] and m["models"], m
        assert set(m["dev_codes"]) <= set(m["models"]), m
        assert len(ranges) > 100, (m["key"], len(ranges))
        for addr, (count, lo, hi) in ranges.items():
            assert 1 <= addr <= 0xFFFF and count in (1, 2), (m["key"], addr)
            assert 0 <= lo <= hi <= (0xFFFF if count == 1 else 0xFFFFFFFF), (m["key"], addr, lo, hi)

    sh, sh_ranges = by_key(models, "SH_RS")
    sg, sg_ranges = by_key(models, "SG_RS")
    st, st_ranges = by_key(models, "SH_T")
    assert sh["name"] == "SH-RS" and sh["family"] == "hybrid" and sh["dev_codes"] == {"SH10RS": 3355}, sh
    assert sg["name"] == "SG-RS" and sg["family"] == "string" and sg["dev_codes"] == {"SG5.0RS": 9734}, sg
    assert st["name"] == "SH-T" and st["family"] == "hybrid" and st["dev_codes"] == {"SH15T": 3621}, st
    assert st["firmware"].startswith("PEARL-H_03") and "SH20T" in st["models"] and "SH5T_V12" in st["models"], st
    assert "SH5.0RS" in sh["models"] and "SG10RS" in sg["models"]
    assert sh_ranges[32550] == (1, 1000, 1200), sh_ranges[32550]
    assert sh_ranges[30091] == (1, 10, 900), sh_ranges[30091]
    assert sg_ranges[31205] == (1, 0, 1500), sg_ranges[31205]
    assert sg_ranges[30813][0] == 2, sg_ranges[30813]
    # SH-T: three-phase windows differ from the SH-RS where the hardware does.
    assert st_ranges[30091] == (1, 10, 900), st_ranges[30091]
    assert st_ranges[32320] == (1, 4500, 6598), st_ranges[32320]
    assert st_ranges[32710] == (1, 85, 170) and sh_ranges[32710] == (1, 0, 1650), (st_ranges[32710], sh_ranges[32710])
    assert st_ranges[33501] == (1, 1, 10), st_ranges[33501]

    h = open(gd.OUT_H, encoding="utf-8").read()
    for needle in ("DSP_RANGES_SH_RS[]", "DSP_RANGES_SG_RS[]", "DSP_RANGES_SH_T[]",
                   'DSP_MODELS_SH_T[] = {"SH5T", "SH6T"', "DSP_CODES_SH_T[] = {3621}",
                   '{"SH_RS", "SH-RS", "%s"' % sh["firmware"], '{"SH_T", "SH-T", "%s"' % st["firmware"],
                   "{32550, 1, 1000, 1200},", "{31205, 1, 0, 1500},", "{32320, 1, 4500, 6598},",
                   "inline const DspModel *dsp_model(uint16_t dev_code)",
                   "inline const DspModel *dsp_model_by_name(const char *model)",
                   "inline const DspModel *dsp_model_for(uint16_t dev_code, const char *model)",
                   "inline const DspRange *dsp_range(const DspModel *m, uint16_t addr)"):
        assert needle in h, needle

    # The loader rejects a model listed under two lines and a code for an unlisted model.
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        for bad, why in (
            ("models:\n  - {key: A, name: a, family: hybrid, firmware: f, csv: x.csv, models: [M1]}\n"
             "  - {key: B, name: b, family: string, firmware: f, csv: x.csv, models: [m1]}\n", "twice"),
            ("models:\n  - {key: A, name: a, family: hybrid, firmware: f, csv: x.csv, models: [M1], dev_codes: {M2: 5}}\n", "not in models"),
            ("models:\n  - {key: A, name: a, family: other, firmware: f, csv: x.csv, models: [M1]}\n", "family"),
            ("models:\n  - {key: A, name: a, family: hybrid, firmware: f, csv: x.csv, models: [M1], dev_code: 5}\n", "unknown field"),
        ):
            path = os.path.join(d, "models.yaml")
            with open(path, "w", encoding="utf-8") as f:
                f.write(bad)
            try:
                gd.load_manifest(path)
                raise AssertionError("accepted a bad manifest (%s)" % why)
            except SystemExit as e:
                assert why.split()[0] in str(e) or "models" in str(e), (why, e)

    print("PASS gen_dsp_ranges (%s)" % ", ".join("%s %d" % (m["name"], len(r)) for m, r in models))


if __name__ == "__main__":
    main()
