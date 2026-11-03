#!/usr/bin/env python3
"""The P-number of a release package from a semantic version.

Every release package must carry a version above the stock one (the updater refuses a downgrade)
and above the previous release, so the number only goes up: 0.9.0 -> P990, 0.9.1 -> P991,
0.10.0 -> P992 ... (minor versions after 0.9 continue from 992), 1.0.0 -> P995 and so on, capped
at P999. Change the scheme before it runs out.

    python3 -I tools/sgu_build_number.py 0.9.1   -> 991
"""
import re
import sys

ORDER = [(0, 9, 0), (0, 9, 1), (0, 9, 2), (0, 10, 0), (0, 11, 0), (1, 0, 0), (1, 0, 1), (1, 1, 0),
         (1, 2, 0), (2, 0, 0)]


def build_number(version: str) -> int:
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)", version)
    if not m:
        raise SystemExit(f"not a version: {version}")
    v = tuple(int(x) for x in m.groups())
    if v in ORDER:
        return 990 + ORDER.index(v)
    raise SystemExit(f"{version} is not in the release order table of tools/sgu_build_number.py; add it")


if __name__ == "__main__":
    print(build_number(sys.argv[1]))
