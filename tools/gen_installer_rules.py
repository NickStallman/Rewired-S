#!/usr/bin/env python3
"""Read data/installer_rules.yaml: raw value ranges and register relations per settings family.

BUILD-TIME LIBRARY (needs PyYAML, deterministic), imported by tools/gen_installer_settings.py.

Input: data/installer_rules.yaml (docs/data_format.md). Per settings family (str_pv /
res_new_pv / res_es / unit) one rule per holding register:
  address      1-based holding register;  type  U16 | S16 | U32 | S32 (32-bit = low word first)
  scale        display = raw x scale (default 1)
  range        [[lo, hi], ...] intervals in display units; a value is valid in any interval.
               Enforced here as the overall [min, max].
  regulation   ';'-separated rules that must all hold; @N is the value of register N.
Rows with a live range callback in the inverter keep their static range (the live one can only
be narrower).

The file is optional: without it load() returns empty tables and the catalogue falls back to the
profile <set> envelope and the dtype range (tools/gen_installer_settings.py).

Conversion:
  raw = round(display / scale).
  A rule "@a op @b" (op in <, <=, >, >=, =) becomes a relation (a, b, op). Other rules
  (arithmetic, constants) are kept only in the regulation string.

Usage: python3 tools/gen_installer_rules.py   (prints what the file converts to)
"""

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import datalib  # noqa: E402
import re  # noqa: E402

RULES_PATH = datalib.INSTALLER_RULES
SIMPLE_RULE = re.compile(r"^@(\d+)\s*(<=|>=|==|<|>|=)\s*@(\d+)$")
DTYPES = ("U16", "S16", "U32", "S32")


def to_raw(display, scale):
    q = display / scale
    return int(math.floor(q + 0.5)) if q >= 0 else -int(math.floor(-q + 0.5))


def load(path=RULES_PATH):
    """-> dict(ranges={(fam, addr): (raw_min, raw_max)}, dtypes={(fam, addr): dtype},
                regulation={(fam, addr): str}, rels={fam: sorted [(lhs, rhs, op)]})."""
    doc = datalib.load_installer_rules(path)
    ranges, dtypes, regulation, rels = {}, {}, {}, {}
    for fam, rules in doc["families"].items():
        seen = set()
        for i, r in enumerate(rules or []):
            where = "%s: families.%s[%d]" % (datalib.rel(path), fam, i)
            addr = r["address"]
            if isinstance(addr, bool) or not isinstance(addr, int) or not 1 <= addr <= 0xFFFF:
                raise SystemExit("%s: bad address %r" % (where, addr))
            if addr in seen:
                raise SystemExit("%s: register %d listed twice" % (where, addr))
            seen.add(addr)
            if r["type"] not in DTYPES:
                raise SystemExit("%s: type must be one of %s" % (where, DTYPES))
            key = (fam, addr)
            dtypes[key] = r["type"]
            scale = float(r.get("scale", 1) or 1)
            iv = r.get("range") or []
            if iv:
                try:
                    lo = min(to_raw(float(a), scale) for a, _ in iv)
                    hi = max(to_raw(float(b), scale) for _, b in iv)
                except (TypeError, ValueError):
                    raise SystemExit("%s: range must be [[lo, hi], ...]" % where)
                if lo > hi:
                    raise SystemExit("%s: range minimum above maximum" % where)
                ranges[key] = (lo, hi)
            reg = str(r.get("regulation") or "").strip()
            if not reg:
                continue
            regulation[key] = reg
            for rule in reg.split(";"):
                m = SIMPLE_RULE.match(rule.strip())
                if not m:
                    continue
                op = "=" if m.group(2) == "==" else m.group(2)
                rels.setdefault(fam, set()).add((int(m.group(1)), int(m.group(3)), op))
    return dict(ranges=ranges, dtypes=dtypes, regulation=regulation,
                rels={f: sorted(v) for f, v in sorted(rels.items())})


def main():
    if not os.path.exists(RULES_PATH):
        print("%s absent: no ranges or relations" % datalib.rel(RULES_PATH))
        return 0
    d = load()
    print("%d ranges, %d regulation strings, %d relations" % (
        len(d["ranges"]), len(d["regulation"]), sum(len(v) for v in d["rels"].values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
