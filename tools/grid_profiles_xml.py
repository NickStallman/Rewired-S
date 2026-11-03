#!/usr/bin/env python3
"""Convert a set of grid-code configuration XML files into data/grid_profiles/<family>.yaml.

The WiNet dongle keeps one XML file per settings family (web_<family>_config.xml, recoverable
with tools/extract_init_param_xml.py or downloadable with tools/fetch_grid_profiles.py). Those
files are not part of this repository; the generators read data/grid_profiles/ only. This
module turns a downloaded / extracted set into the project format so it can be compared with
the committed data (tests/test_xml_drift.py) or adopted.

What is taken from each file:
  - the country / standard picker of the protection-parameter page: one setup profile per
    option (code, label, whether it writes its register sets and whether the variant is chosen
    by a live read of input register 5479), each variant being an ordered list of
    (address, words) register sets;
  - the grid-type options' register sets (allow-list / value-envelope input only);
  - the holding-register width table (U16 / S16 / U32);
  - the installer catalogue: every editable register-backed item (number / list / switch) of
    the writable pages, flattened to one item per register with its page breadcrumb.

Labels: the XML names items and categories with translation keys. `label` is a callable
key -> English text; without a label file the committed data's labels are reused where the
register matches and the key is turned into readable words otherwise (key_fallback()).

Usage (library): convert(xml_dir, index, label, dtypes) -> {family: document}
"""

import os
import re
import xml.etree.ElementTree as ET

import datalib
from datalib import Doc, Flow, Hex

CJK = re.compile(r"[　-鿿＀-￯]")

# The XML names its categories and items with translation keys; they are matched on the
# key's suffix.
PROTECT_SUFFIX = "_PROTECT_PARAM_INIT"
COMPANY_SUFFIX = "_POWER_COMPANY"
GRIDTYPE_SUFFIX = "_POWER_GRID_TYPE"
COUNTRY_SUFFIX = "_SELECT_COUNTRIE"
SKIP_SUFFIXES = ("_RUN_INFO", "_SELF_TEST_RESULT")
EDITABLE_CONTROLS = {"number", "list", "switch"}
DTYPES = ("U16", "S16", "U32")


def xml_family(path):
    """web_str_pv_config.xml -> str_pv."""
    base = os.path.basename(path)
    if base.startswith("web_"):
        base = base[4:]
    for suf in ("_config.xml", ".xml"):
        if base.endswith(suf):
            base = base[: -len(suf)]
    return base


def key_fallback(key):
    """Readable words from a translation key: '<prefix>_SELECT_COUNTRIE' -> 'Select Countrie'."""
    parts = (key or "").split("_")
    while parts and parts[0] in ("I18N", "COMMON", "CONFIG", "KEY"):
        parts.pop(0)
    s = " ".join(parts).strip()
    if s.isupper():
        s = " ".join(w.capitalize() for w in s.split())
    return s or key


def labels_from_properties(path):
    """A label function from a Java .properties file (key=text, \\uXXXX unescaped)."""
    table = {}
    for line in open(path, encoding="utf-8"):
        s = line.strip()
        if not s or s[0] in "#!" or "=" not in s:
            continue
        k, v = s.split("=", 1)
        table[k.strip()] = re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)),
                                  v.strip())

    def label(key):
        key = (key or "").strip()
        return table.get(key) or (key_fallback(key) if key.startswith("I18N") else key)
    return label


def labels_from_committed(docs):
    """A label function that reuses the committed data: item labels by (family, address),
    option labels by (family, address, value), profile labels by (family, code). Keys that
    cannot be matched fall back to key_fallback(). Returns label(key, family=None, address=None,
    value=None, code=None)."""
    items, options, profiles = {}, {}, {}
    for fam, d in docs.items():
        for g in d["catalogue"]:
            for it in g["items"]:
                items[(fam, it["address"])] = it["label"]
                for o in it.get("options") or []:
                    options[(fam, it["address"], o["value"])] = o["label"]
        for p in d["profiles"]:
            profiles[(fam, p["code"])] = p["label"]

    def label(key, family=None, address=None, value=None, code=None):
        if value is not None and (family, address, value) in options:
            return options[(family, address, value)]
        if code is not None and (family, code) in profiles:
            return profiles[(family, code)]
        if address is not None and value is None and (family, address) in items:
            return items[(family, address)]
        key = (key or "").strip()
        return key_fallback(key) if key.startswith("I18N") else key
    return label


def _label(label, key, **ctx):
    try:
        return label(key, **ctx)
    except TypeError:
        return label(key)


# ---- XML parsing (the structure the dongle itself reads) -------------------------------------

def parse_register(ref):
    """'4x_30500' -> 30500 (1-based holding register), None if not a 4x reference."""
    ref = (ref or "").strip()
    if ref.lower().startswith("4x_"):
        try:
            return int(ref[3:], 10)
        except ValueError:
            return None
    return None


def decode_write_data(hexstr):
    h = hexstr.strip()
    if len(h) % 4 != 0:
        raise ValueError("writeData %r is not a whole number of 16-bit registers" % hexstr)
    return [int(h[i:i + 4], 16) for i in range(0, len(h), 4)]


def is_placeholder(write_data):
    wd = (write_data or "").strip().upper()
    return not wd or set(wd) == {"F"}


def option_sets(option):
    """[(addr, (w0, w1, ...))] of an <option>'s real register sets, document order."""
    out = []
    for s in option.findall("sets/set"):
        addr, wd = s.get("address"), s.get("writeData")
        if addr is None or is_placeholder(wd):
            continue
        out.append((int(addr, 10), tuple(decode_write_data(wd))))
    return out


def protect_category(root):
    for c in root.findall("category"):
        if (c.get("name") or "").endswith(PROTECT_SUFFIX):
            return c
    raise SystemExit("protection-parameter category missing")


def parse_option(op):
    np_, nrt = op.get("needparam"), op.get("needreadtype")
    if np_ not in ("0", "1") or nrt not in ("0", "1"):
        raise SystemExit("option %r: needparam/needreadtype missing or not 0/1" % op.get("name"))
    refs = [int(r.get("value"), 10) for r in op.findall("refs/ref")
            if r.get("register") == "3x_5479" and r.get("value") is not None]
    return dict(code=int(op.get("value"), 10), name=op.get("name") or "", needparam=int(np_),
                needreadtype=int(nrt), ref5479=refs, sets=option_sets(op))


def company_item(cat, fam, unit_company_reg):
    """The picker item the dongle applies: unit -> the item bound to the real company
    register (its virtual-point twin must list the same options); other families have one."""
    items = [it for it in cat.findall("item") if (it.get("name") or "").endswith(COMPANY_SUFFIX)]
    if fam != "unit":
        if len(items) != 1:
            raise SystemExit("%s: expected one picker item, found %d" % (fam, len(items)))
        return items[0]
    real = [it for it in items if it.get("register") == "4x_%d" % unit_company_reg]
    twin = [it for it in items if it.get("virtualPoint") == "vp_000"]
    if len(real) != 1 or len(twin) != 1:
        raise SystemExit("unit: expected one register-bound and one virtual picker item")
    a = [parse_option(o) for o in real[0].findall("list/option")]
    b = [parse_option(o) for o in twin[0].findall("list/option")]
    if a != b:
        raise SystemExit("unit: the register-bound and virtual picker items differ")
    return real[0]


def assert_no_grid_type_path(root, fam):
    for op in root.iter("option"):
        if op.get("needparam") == "0":
            raise SystemExit(
                "%s: option %r has needparam=\"0\". The dongle then routes through the grid-type "
                "items; that path is not modelled (ResolveStatus::GRID_TYPE_UNSUPPORTED)."
                % (fam, op.get("name")))


def group_options(fam, options):
    """Options sharing a code become one profile with one variant per option."""
    groups, by_code = [], {}
    for o in options:
        g = by_code.get(o["code"])
        if g is None:
            g = dict(family=fam, code=o["code"], name=o["name"], needparam=o["needparam"],
                     needreadtype=o["needreadtype"], variants=[])
            by_code[o["code"]] = g
            groups.append(g)
        if (g["needparam"], g["needreadtype"]) != (o["needparam"], o["needreadtype"]):
            raise SystemExit("%s option %d: variants disagree on needparam/needreadtype" % (fam, o["code"]))
        if o["needreadtype"] == 1 and not o["ref5479"]:
            raise SystemExit("%s option %d: needreadtype=1 without a 3x_5479 ref" % (fam, o["code"]))
        g["variants"].append(dict(ref5479=o["ref5479"], sets=o["sets"]))
    for g in groups:
        if g["needreadtype"] == 0 and len(g["variants"]) != 1:
            raise SystemExit("%s option %d: several needreadtype=0 variants" % (fam, g["code"]))
    return groups


def grid_type_options(cat):
    """Distinct (code, name, sets) of every grid-type item option in the protection page."""
    out, seen = [], set()
    for it in cat.findall("item"):
        if not (it.get("name") or "").endswith(GRIDTYPE_SUFFIX):
            continue
        for op in it.findall("list/option"):
            sets = option_sets(op)
            k = (op.get("value"), op.get("name"), tuple(sets))
            if k in seen:
                continue
            seen.add(k)
            out.append(dict(code=int(op.get("value"), 10), name=op.get("name") or "", sets=sets))
    return out


def reg_table(root):
    """{addr: (type, description)} of the <register registerId="4x_N"> table."""
    out = {}
    for r in root.iter("register"):
        a = parse_register(r.get("registerId"))
        if a is not None:
            out[a] = ((r.get("type") or "U16").strip().upper(), r.get("description") or "")
    return out


# ---- installer catalogue --------------------------------------------------------------------

def _num(v, default=None):
    try:
        return int(v, 10)
    except (TypeError, ValueError):
        try:
            return float(v)
        except (TypeError, ValueError):
            return default


def list_options(item, label, fam, addr):
    opts, seen = [], set()
    for op in item.findall("list/option"):
        val = _num(op.get("value"))
        if val is None or int(val) in seen:
            continue
        seen.add(int(val))
        opts.append({"label": _label(label, op.get("name") or str(val), family=fam, address=addr,
                                     value=int(val)), "value": int(val)})
    return opts


def item_entry(item, fam, path, write_pri, read_pri, rtypes, label):
    control = item.get("control")
    if control not in EDITABLE_CONTROLS or item.get("virtualPoint"):
        return None
    addr = parse_register(item.get("register"))
    if addr is None:
        return None
    text = _label(label, item.get("name"), family=fam, address=addr)
    if item.get("placeholder"):
        text = text.replace("{0}", item.get("placeholder")).replace("%@", item.get("placeholder"))
    entry = dict(family=fam, address=addr, label=text, category=list(path),
                 dtype=rtypes.get(addr), write_pri=write_pri or "client", read_pri=read_pri,
                 control="", scale=1.0, accuracy=0, unit="", options=[],
                 refresh=item.get("refresh") == "true")
    if control == "number":
        n = item.find("number")
        entry["control"] = "number"
        if n is not None:
            entry["scale"] = _num(n.get("base"), 1.0)
            entry["accuracy"] = int(_num(n.get("accuracy"), 0))
            entry["unit"] = (n.get("suffix") or "").strip()
    elif control == "switch":
        sw = item.find("switch")
        entry["control"] = "switch"
        off = int(_num(sw.get("off"), 85)) if sw is not None else 85
        on = int(_num(sw.get("on"), 170)) if sw is not None else 170
        entry["options"] = [{"label": "Disabled", "value": off}, {"label": "Enabled", "value": on}]
    else:
        entry["control"] = "enum"
        entry["options"] = list_options(item, label, fam, addr)
        if not entry["options"]:
            return None
    return entry


def walk(node, fam, path, write_pri, read_pri, rtypes, out, label):
    for child in list(node):
        if child.tag == "category":
            name = child.get("name") or ""
            if name.endswith(SKIP_SUFFIXES) or child.get("readOnly") == "true":
                continue
            wp = child.get("writePri") or write_pri
            rp = child.get("readPri") or read_pri
            walk(child, fam, path + [_label(label, name)], wp, rp, rtypes, out, label)
        elif child.tag == "item":
            e = item_entry(child, fam, path, write_pri, read_pri, rtypes, label)
            if e is not None:
                out.append(e)
        else:
            walk(child, fam, path, write_pri, read_pri, rtypes, out, label)


def country_options(roots, label):
    """Union of every file's country-picker options (file order, first label per value)."""
    opts, seen = [], set()
    for fam, root in roots:
        for it in root.iter("item"):
            if not (it.get("name") or "").endswith(COUNTRY_SUFFIX):
                continue
            addr = parse_register(it.get("register"))
            for o in list_options(it, label, fam, addr):
                if o["value"] not in seen:
                    seen.add(o["value"])
                    opts.append(o)
    return opts


def catalogue_rows(root, fam, label, country_reg, protect_write_pri_default, all_roots):
    rtypes = {a: t for a, (t, _) in reg_table(root).items() if t in DTYPES}
    out = []
    for cat in root.findall("category"):
        name = cat.get("name") or ""
        if name.endswith(SKIP_SUFFIXES) or cat.get("readOnly") == "true":
            continue
        walk(cat, fam, [_label(label, name)], cat.get("writePri") or "client", cat.get("readPri"),
             rtypes, out, label)
    rows = {}
    for e in out:  # first occurrence per address wins; enum options are merged
        prev = rows.get(e["address"])
        if prev is None:
            rows[e["address"]] = e
        elif prev["control"] == "enum" and e["control"] == "enum":
            have = {o["value"] for o in prev["options"]}
            prev["options"] += [o for o in e["options"] if o["value"] not in have]
    if country_reg and country_reg not in rows:
        pc = protect_category(root)
        country_key = next((it.get("name") for _, r in all_roots for it in r.iter("item")
                            if (it.get("name") or "").endswith(COUNTRY_SUFFIX)), "Country")
        rows[country_reg] = dict(
            family=fam, address=country_reg, label=_label(label, country_key),
            category=[_label(label, pc.get("name"))], dtype=rtypes.get(country_reg),
            write_pri=pc.get("writePri") or protect_write_pri_default, read_pri=pc.get("readPri"),
            control="enum", scale=1.0, accuracy=0, unit="", options=country_options(all_roots, label),
            refresh=False)
    return [rows[a] for a in sorted(rows)]


# ---- document assembly ----------------------------------------------------------------------

def _sets_yaml(sets):
    return [Flow({"address": a, "words": [Hex(w) for w in ws]}) for a, ws in sets]


def family_doc(fam, root, index, label, dtypes, all_roots):
    desc = next((d for d in index["descriptors"] if d["family"] == fam), None)
    country_reg = desc["country_register"] if desc else 0
    assert_no_grid_type_path(root, fam)
    pc = protect_category(root)
    item = company_item(pc, fam, index["unit_company_register"])
    profiles = group_options(fam, [parse_option(o) for o in item.findall("list/option")])
    regs = reg_table(root)
    rows = catalogue_rows(root, fam, label, country_reg, "client", all_roots)

    doc = Doc()
    doc["family"] = fam
    doc["title"] = index["families"][fam].get("title", "")
    reg_map = Doc()
    for a in sorted(regs):
        t, d = regs[a]
        entry = {"type": t if t in DTYPES else "U16"}
        if d and not CJK.search(d):
            entry["label"] = d
        reg_map[a] = Flow(entry)
    doc["registers"] = reg_map
    plist = []
    for g in profiles:
        p = Doc()
        p["code"] = g["code"]
        p["label"] = _label(label, g["name"], family=fam, code=g["code"])
        p["writes_sets"] = bool(g["needparam"])
        p["variant_by_input_5479"] = bool(g["needreadtype"])
        p["variants"] = [Doc(input_5479=v["ref5479"], sets=_sets_yaml(v["sets"]))
                         for v in g["variants"]]
        plist.append(p)
    doc["profiles"] = plist
    doc["grid_type_profiles"] = [Doc(code=o["code"], label=_label(label, o["name"]),
                                     sets=_sets_yaml(o["sets"])) for o in grid_type_options(pc)]
    groups, order = {}, []
    for r in rows:
        k = (tuple(r["category"]), r["write_pri"], r["read_pri"])
        if k not in groups:
            groups[k] = Doc(path=list(r["category"]), write_priority=r["write_pri"])
            if r["read_pri"]:
                groups[k]["read_priority"] = r["read_pri"]
            groups[k]["items"] = []
            order.append(k)
        it = {"address": r["address"], "label": r["label"], "control": r["control"],
              "type": r["dtype"] or dtypes.get((fam, r["address"]), "U16")}
        if r["control"] == "number":
            it["scale"] = r["scale"]
            it["accuracy"] = r["accuracy"]
            it["unit"] = r["unit"]
        if r["refresh"]:
            it["refresh"] = True
        if r["options"]:
            it["options"] = [Flow(value=o["value"], label=o["label"]) for o in r["options"]]
        groups[k]["items"].append(Flow(it) if not r["options"] else Doc(it))
    doc["catalogue"] = [groups[k] for k in order]
    return doc


def convert(xml_dir, index, label, dtypes=None):
    """{family: document} for every index family whose web_<family>_config.xml is in xml_dir."""
    dtypes = dtypes or {}
    roots = []
    for fam in index["families"]:
        p = os.path.join(xml_dir, "web_%s_config.xml" % fam)
        if os.path.exists(p):
            roots.append((fam, ET.parse(p).getroot()))
    return {fam: family_doc(fam, root, index, label, dtypes, roots) for fam, root in roots}


HEADER = [
    "data/grid_profiles/{fam}.yaml - grid-code setup profiles and the installer catalogue of the",
    "{fam} settings family (docs/data_format.md). Read by tools/gen_grid_profiles.py and",
    "tools/gen_installer_settings.py. Addresses are 1-based holding registers (wire = n - 1);",
    "U32 values are written low word first.",
]


def write_profiles(out_dir, docs):
    """Write {family: doc} as <out_dir>/<family>.yaml; returns the paths written."""
    os.makedirs(out_dir, exist_ok=True)
    paths = []
    for fam in sorted(docs):
        p = os.path.join(out_dir, fam + ".yaml")
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(datalib.dump_yaml(docs[fam], [h.format(fam=fam) for h in HEADER]))
        paths.append(p)
    return paths


def plain(doc):
    """A copy with plain dict / list / int / str values (no writer wrappers)."""
    if isinstance(doc, dict):
        return {k: plain(v) for k, v in doc.items()}
    if isinstance(doc, (list, tuple)):
        return [plain(v) for v in doc]
    if isinstance(doc, bool):
        return doc
    if isinstance(doc, int):
        return int(doc)
    if isinstance(doc, str):
        return str(doc)
    return doc


def strip_labels(doc):
    """A plain copy with every label removed, for structural comparison (tests/test_xml_drift.py,
    tools/fetch_grid_profiles.py --check)."""
    def walk_(v):
        if isinstance(v, dict):
            return {k: walk_(x) for k, x in v.items() if k not in ("label", "title", "path")}
        if isinstance(v, list):
            return [walk_(x) for x in v]
        return v
    return walk_(plain(doc))
