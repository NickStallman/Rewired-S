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


def by_code(models, code):
    for m, ranges in models:
        if m["dev_code"] == code:
            return m, ranges
    raise AssertionError("no model with dev_code %d" % code)


def main():
    for _ in range(2):  # the committed header is what the generator produces, every run
        r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "gen_dsp_ranges.py"), "--check"],
                           stdout=subprocess.PIPE)
        assert r.returncode == 0, "gen_dsp_ranges.py --check: header stale, rerun it (%s)" % r.stdout.decode()

    models = gd.load()
    assert gd.gen_h(models) == gd.gen_h(gd.load()), "gen_dsp_ranges output is not deterministic"
    assert gd.gen_h(models) == open(gd.OUT_H, encoding="utf-8").read()

    codes = [m["dev_code"] for m, _ in models]
    assert codes == sorted(codes) and len(set(codes)) == len(codes), codes
    assert {3355, 9734} <= set(codes), codes
    for m, ranges in models:
        assert m["family"] in ("hybrid", "string"), m
        assert m["firmware"], m
        assert len(ranges) > 100, (m["model"], len(ranges))
        for addr, (count, lo, hi) in ranges.items():
            assert 1 <= addr <= 0xFFFF and count in (1, 2), (m["model"], addr)
            assert 0 <= lo <= hi <= (0xFFFF if count == 1 else 0xFFFFFFFF), (m["model"], addr, lo, hi)

    sh, sh_ranges = by_code(models, 3355)
    sg, sg_ranges = by_code(models, 9734)
    assert sh["model"] == "SH10RS" and sh["key"] == "SH_RS" and sh["family"] == "hybrid", sh
    assert sg["model"] == "SG5.0RS" and sg["key"] == "SG_RS" and sg["family"] == "string", sg
    assert sh_ranges[32550] == (1, 1000, 1200), sh_ranges[32550]
    assert sh_ranges[30091] == (1, 10, 900), sh_ranges[30091]
    assert sg_ranges[31205] == (1, 0, 1500), sg_ranges[31205]
    assert sg_ranges[30813][0] == 2, sg_ranges[30813]

    h = open(gd.OUT_H, encoding="utf-8").read()
    for needle in ("DSP_RANGES_SH_RS[]", "DSP_RANGES_SG_RS[]", '{3355, "SH10RS", "%s"' % sh["firmware"],
                   '{9734, "SG5.0RS", "%s"' % sg["firmware"], "{32550, 1, 1000, 1200},", "{31205, 1, 0, 1500},",
                   "inline const DspModel *dsp_model(uint16_t dev_code)",
                   "inline const DspRange *dsp_range(const DspModel *m, uint16_t addr)"):
        assert needle in h, needle
    print("PASS gen_dsp_ranges (%s)" % ", ".join("%s %d" % (m["model"], len(r)) for m, r in models))


if __name__ == "__main__":
    main()
