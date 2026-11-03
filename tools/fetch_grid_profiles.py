#!/usr/bin/env python3
"""Fetch the grid-code configuration XML set from iSolarCloud and compare it with the project data.

The dongle refreshes its grid-code / parameter XML files (web_<family>_config.xml, template line
v1.2) at runtime from iSolarCloud with the `getTemplateResConfig` call; this tool makes the same
call so the current per-region set can be pulled offline. The XML files are not part of this
repository: the generators read data/grid_profiles/<family>.yaml. `--to-project-format` converts a
download to that format (tools/grid_profiles_xml.py; labels are reused from the committed data by
register unless --labels names a key=text file), `--check` converts and compares it with the
committed data (labels ignored), and tests/test_xml_drift.py does the same offline for a download
under data/fetched/grid_profiles/<region>/.

Request:
  POST https://<gateway>/v1/commonService/getTemplateResConfig
  Content-Type: application/json;charset=UTF-8
  sys_code: <code>                         <-- HTTP header, not a body field
  body: {"appkey": <APPKEY>, "product_type": <pt>, "version": <ver>}
  -> result_data.template_config_info.config[] = [{file_name, file_url, file_version}, ...]
  -> download each file_url -> web_*_config.xml / settings.xml

The `appkey` is the public commonService appkey the dongle sends (not a user credential). The
gateway host + `sys_code` header select the region.

  1. `sys_code` is an HTTP header ("sys_code: 917"). Sent inside the JSON body, the server answers
     result_code "1" with an empty config[].
  2. `product_type` is a small enum selecting the resource class, not the dongle model: only 2
     returns config[] (the web-resources set).
  3. `version` is the template line the device advertises; the device sends "1.2". The server
     matches on major.minor and returns the latest patch in that line (v1.2 -> 1.2.4). A different
     minor ("1.0") or a higher patch ("1.2.5") returns nothing.
  The AU and base gateways return 5 entries: web_str_pv / web_res_new_pv / web_res_es / web_unit
  _config.xml (v1.2.4) + settings.xml (v1.2).
This tool prints + saves the raw response, and makes an outbound request to Sungrow only when you run it.

Usage:
  python3 tools/fetch_grid_profiles.py --list                 # show known region gateways
  python3 tools/fetch_grid_profiles.py --region au            # fetch one region
  python3 tools/fetch_grid_profiles.py --region au,eu,auto    # several
  python3 tools/fetch_grid_profiles.py --region au --dry-run  # print the request, send nothing
  python3 tools/fetch_grid_profiles.py --region au --to-project-format [--labels FILE]
                                                              # also write <region>/grid_profiles/*.yaml
  python3 tools/fetch_grid_profiles.py --check                # fetch au into a temp dir, convert and
                                                              # diff against data/grid_profiles/ (exit 1 on drift)
Downloaded files are written under data/fetched/grid_profiles/<region>/ (git-ignored; the committed
data/grid_profiles/ is left untouched). Downloaded data is untrusted: parse, don't run.
"""

import argparse
import json
import os
import sys
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_ROOT = os.path.join(ROOT, "data", "fetched", "grid_profiles")
COMMITTED = os.path.join(ROOT, "data", "grid_profiles")

# commonService appkey: the public key the dongle sends with this call; not a user credential.
DEFAULT_APPKEY = "5FD913AA74124F88B98E3705E9D29AEF"
# product_type is a small enum selecting the resource class, not the dongle model: only 2 returns
# config[] (the web-resources set). See the module docstring.
DEFAULT_PRODUCT_TYPE = 2
# Template-line version the device advertises; the server matches major.minor and serves the latest
# patch in that line (v1.2 -> 1.2.4). A mismatched minor ("1.0") returns nothing. The device sends "1.2".
DEFAULT_VERSION = "1.2"

# Region -> (gateway host[:port], sys_code).
REGIONS = {
    "au": ("augateway.isolarcloud.com", "917"),
    "base": ("base.isolarcloud.com:8770", "901"),
    "eu": ("gateway.isolarcloud.eu", "901"),
    "cn": ("gateway.isolarcloud.com.cn", "901"),
    "in": ("gateway.isolarcloud.in", "901"),
    "auto": ("auto.isolarcloud.com", "901"),
}

TIMEOUT = 20


def request(region, appkey, product_type, version, dry_run=False):
    host, sys_code = REGIONS[region]
    url = "https://%s/v1/commonService/getTemplateResConfig" % host
    # sys_code is an HTTP header, not a body field (see the module docstring).
    body = {"appkey": appkey, "product_type": product_type, "version": version}
    data = json.dumps(body).encode("utf-8")
    headers = {"Content-Type": "application/json;charset=UTF-8", "sys_code": str(sys_code)}
    print("[%s] POST %s" % (region, url))
    print("      hdr sys_code: %s" % sys_code)
    print("      body: %s" % json.dumps({**body, "appkey": appkey[:6] + "…"}))
    if dry_run:
        return None
    req = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
        raw = resp.read().decode("utf-8", "replace")
    return raw


def parse_config(raw):
    """Pull the [{file_name, file_url, file_version}] list out of a getTemplateResConfig response."""
    doc = json.loads(raw)
    rd = doc.get("result_data") or {}
    tci = rd.get("template_config_info") or {}
    cfg = tci.get("config") or tci.get("configs") or []
    if not cfg and isinstance(rd.get("config"), list):
        cfg = rd["config"]
    return cfg, doc


def download(url, dest):
    with urllib.request.urlopen(url, timeout=TIMEOUT) as resp:
        blob = resp.read()
    with open(dest, "wb") as f:
        f.write(blob)
    return len(blob)


def fetch_region(region, appkey, product_type, version, dry_run, out_root=OUT_ROOT):
    raw = request(region, appkey, product_type, version, dry_run)
    if dry_run or raw is None:
        return
    out_dir = os.path.join(out_root, region)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "_getTemplateResConfig.json"), "w", encoding="utf-8") as f:
        f.write(raw)
    try:
        cfg, doc = parse_config(raw)
    except json.JSONDecodeError:
        print("  !! response is not JSON (first 200 chars): %r" % raw[:200])
        return
    if not cfg:
        code = doc.get("result_code"); msg = doc.get("result_msg")
        print("  !! no config[] in response (result_code=%r result_msg=%r). Saved raw JSON for "
              "inspection — the request may need a different version or a sign." % (code, msg))
        return
    print("  %d config entr(y/ies):" % len(cfg))
    for c in cfg:
        name = c.get("file_name") or "unknown.xml"
        furl = c.get("file_url")
        ver = c.get("file_version")
        if not furl:
            print("     - %s (version %s): no file_url" % (name, ver))
            continue
        dest = os.path.join(out_dir, os.path.basename(name))
        try:
            n = download(furl, dest)
            print("     - %s (version %s) <- %s  [%d bytes]" % (name, ver, furl, n))
        except Exception as e:  # noqa: BLE001 - report + continue
            print("     - %s: download failed: %s" % (name, e))


def compare_with_committed(xml_dir, labels_path=None):
    """Convert the XML set in xml_dir and compare it with data/grid_profiles/ (labels ignored).
    Returns a list of difference descriptions (empty = same)."""
    sys.path.insert(0, HERE)
    import datalib
    import gen_installer_rules
    import grid_profiles_xml as gpx
    index, docs = datalib.load_grid_profiles()
    label = gpx.labels_from_properties(labels_path) if labels_path else gpx.labels_from_committed(docs)
    got = gpx.convert(xml_dir, index, label, gen_installer_rules.load()["dtypes"])
    diffs = []
    for fam in sorted(set(docs) | set(got)):
        if fam not in got:
            diffs.append("%s: not in the download" % fam)
        elif fam not in docs:
            diffs.append("%s: not in data/grid_profiles/" % fam)
        elif gpx.strip_labels(got[fam]) != gpx.strip_labels(docs[fam]):
            diffs.append("%s: differs from data/grid_profiles/%s.yaml" % (fam, fam))
    return diffs


def check_drift(regions, appkey, product_type, version, labels_path=None):
    """Fetch `regions` into a temp dir and compare each set with the committed data.
    Returns the number of differing or missing families."""
    drift = 0
    with tempfile.TemporaryDirectory() as tmp:
        for r in regions:
            fetch_region(r, appkey, product_type, version, False, out_root=tmp)
            diffs = compare_with_committed(os.path.join(tmp, r), labels_path)
            for d in diffs:
                print("DRIFT [%s] %s" % (r, d))
            if not diffs:
                print("same  [%s] every family equals data/grid_profiles/" % r)
            drift += len(diffs)
    return drift


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--region", default="au", help="comma-separated region keys (see --list); default au")
    ap.add_argument("--appkey", default=DEFAULT_APPKEY)
    ap.add_argument("--product-type", type=int, default=DEFAULT_PRODUCT_TYPE)
    ap.add_argument("--version", default=DEFAULT_VERSION)
    ap.add_argument("--dry-run", action="store_true", help="print the request(s); send nothing")
    ap.add_argument("--list", action="store_true", help="list known region gateways and exit")
    ap.add_argument("--check", action="store_true",
                    help="fetch into a temp dir, convert and diff against data/grid_profiles/, exit 1 on drift")
    ap.add_argument("--to-project-format", action="store_true",
                    help="also convert each download to data/grid_profiles-format YAML (<region>/grid_profiles/)")
    ap.add_argument("--labels", default=None, help="key=text label file for the conversion")
    a = ap.parse_args()
    if a.list:
        print("Known regions (key: host, sys_code):")
        for k, (h, sc) in REGIONS.items():
            print("  %-6s %s  sys_code=%s" % (k, h, sc))
        return 0
    regions = [r.strip() for r in a.region.split(",") if r.strip()]
    bad = [r for r in regions if r not in REGIONS]
    if bad:
        print("unknown region(s): %s (see --list)" % ", ".join(bad))
        return 2
    if a.check:
        if a.dry_run:
            ap.error("--check fetches; it cannot be combined with --dry-run")
        try:
            drift = check_drift(regions, a.appkey, a.product_type, a.version, a.labels)
        except Exception as e:  # noqa: BLE001
            print("check failed: %s" % e)
            return 2
        print("%d drifted file(s)" % drift)
        return 1 if drift else 0
    for r in regions:
        try:
            fetch_region(r, a.appkey, a.product_type, a.version, a.dry_run)
        except Exception as e:  # noqa: BLE001
            print("[%s] request failed: %s" % (r, e))
            continue
        if a.to_project_format and not a.dry_run:
            sys.path.insert(0, HERE)
            import extract_init_param_xml as ex
            paths = ex.convert_to_project_format(os.path.join(OUT_ROOT, r),
                                                 os.path.join(OUT_ROOT, r, "grid_profiles"), a.labels)
            print("  project format: %s" % ", ".join(os.path.relpath(p, ROOT) for p in paths))
    if not a.dry_run:
        print("\nDownloaded under %s/<region>/. The generators read only data/grid_profiles/; "
              "compare with --check (or tests/test_xml_drift.py) and copy a converted set there to "
              "adopt it." % os.path.relpath(OUT_ROOT, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
