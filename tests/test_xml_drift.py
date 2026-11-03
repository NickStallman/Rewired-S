#!/usr/bin/env python3
"""The committed grid profiles equal the iSolarCloud au/ and base/ downloads structurally.

data/grid_profiles/<family>.yaml is what the generators read; data/fetched/grid_profiles/{au,base}/
are tools/fetch_grid_profiles.py downloads (not tracked). Each download is converted with
tools/grid_profiles_xml.py and compared with the committed data, labels ignored (a download has no
label text of its own). A difference means the committed data drifted from the cloud copy. Without
the downloads the test is skipped; `tools/fetch_grid_profiles.py --check` compares against the live
gateway.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import datalib  # noqa: E402
import gen_installer_rules  # noqa: E402
import grid_profiles_xml as gpx  # noqa: E402

CLOUD = os.path.join(ROOT, "data", "fetched", "grid_profiles")


def main():
    index, docs = datalib.load_grid_profiles()
    assert sorted(docs) == ["res_es", "res_new_pv", "str_pv", "unit"], sorted(docs)
    if not os.path.isdir(CLOUD):
        print("SKIP xml drift (no %s; run tools/fetch_grid_profiles.py --region au,base)"
              % os.path.relpath(CLOUD, ROOT))
        return
    label = gpx.labels_from_committed(docs)
    dtypes = gen_installer_rules.load()["dtypes"]
    for region in ("au", "base"):
        got = gpx.convert(os.path.join(CLOUD, region), index, label, dtypes)
        for fam in sorted(docs):
            assert fam in got, "%s/web_%s_config.xml missing" % (region, fam)
            assert gpx.strip_labels(got[fam]) == gpx.strip_labels(docs[fam]), \
                "%s differs from data/grid_profiles/%s.yaml" % (region, fam)
    print("PASS xml drift (%d families == au == base)" % len(docs))


if __name__ == "__main__":
    main()
