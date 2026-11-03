#!/usr/bin/env python3
"""Shared loader for the project's data files under data/ (see docs/data_format.md).

Every generator in tools/ reads its inputs through this module:

  load_yaml(path)              PyYAML safe_load with a clear error when PyYAML is missing
  dump_yaml(doc)               deterministic, commented YAML text (the converters and the
                               --to-project-format options of the extraction tools use it)
  load_register_data()         data/families/*.yaml + data/modbus_tcp/*.yaml -> the register
                               tables, the signal definitions, address fixes, shared-address
                               allowances and the non-polled protection reads
  load_cloud_points()          data/cloud/point_tables.yaml
  load_installer_rules()       data/installer_rules.yaml
  load_grid_profiles()         data/grid_profiles/index.yaml + <family>.yaml
  load_write_registers()       data/write_registers.yaml
  load_installer_help()        data/installer_help.yaml

Requires PyYAML (tools/requirements.txt). Deterministic: every loader returns data in file
order or sorted order, never in hash order.
"""

import json
import os
import re

try:
    import yaml
except ImportError:  # pragma: no cover - reported once, at the first load
    yaml = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA_DIR = os.path.join(ROOT, "data")
FAMILIES_DIR = os.path.join(DATA_DIR, "families")
MODBUS_TCP_DIR = os.path.join(DATA_DIR, "modbus_tcp")
CLOUD_POINTS = os.path.join(DATA_DIR, "cloud", "point_tables.yaml")
GRID_PROFILES_DIR = os.path.join(DATA_DIR, "grid_profiles")
INSTALLER_RULES = os.path.join(DATA_DIR, "installer_rules.yaml")
WRITE_REGISTERS = os.path.join(DATA_DIR, "write_registers.yaml")
INSTALLER_HELP = os.path.join(DATA_DIR, "installer_help.yaml")


class DataError(SystemExit):
    """A data file is malformed; the message names the file and the entry."""


def load_yaml(path):
    if yaml is None:
        raise SystemExit("PyYAML is required to read %s: pip install -r tools/requirements.txt"
                         % os.path.relpath(path, ROOT))
    with open(path, encoding="utf-8") as f:
        try:
            return yaml.safe_load(f)
        except yaml.YAMLError as e:
            raise DataError("%s: %s" % (os.path.relpath(path, ROOT), e))


def rel(path):
    return os.path.relpath(path, ROOT)


# ---------------------------------------------------------------------------------------
# Deterministic YAML writer. PyYAML's dumper cannot write comments or hex integers, so the
# converters emit through this small writer instead: insertion order is kept, mappings marked
# Flow are written inline, Hex integers as 0x%04X, and comments attached to keys / rows.

class Hex(int):
    """An integer written as 0x%0*X (width 4 by default)."""
    width = 4

    def __new__(cls, v, width=4):
        o = int.__new__(cls, v)
        o.width = width
        return o


class Flow(dict):
    """A mapping written inline: {a: 1, b: 2}. `comment` is appended after the line."""

    def __init__(self, *a, comment=None, **kw):
        super().__init__(*a, **kw)
        self.comment = comment


class Doc(dict):
    """A block mapping; `comments[key]` is written as comment lines before the key, and
    `blank_before` lists keys preceded by an empty line."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.comments = {}
        self.blank_before = set()


class Lit(str):
    """A multi-line string written as a literal block (|)."""


_PLAIN = re.compile(r"^[A-Za-z_][A-Za-z0-9_\-]*$")
_RESERVED = {"true", "false", "yes", "no", "on", "off", "null", "y", "n", "~"}


def _scalar(v):
    if isinstance(v, Hex):
        return "0x%0*X" % (v.width, int(v))
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        if v.is_integer() and abs(v) < 1e15:
            return "%d.0" % v
        return repr(v)
    if v is None:
        return "null"
    if isinstance(v, str):
        if _PLAIN.match(v) and v.lower() not in _RESERVED:
            return v
        return json.dumps(v, ensure_ascii=False)
    raise TypeError("cannot write %r" % (v,))


def _flow(v):
    if isinstance(v, dict):
        return "{%s}" % ", ".join("%s: %s" % (_scalar(k), _flow(x)) for k, x in v.items())
    if isinstance(v, (list, tuple)):
        return "[%s]" % ", ".join(_flow(x) for x in v)
    return _scalar(v)


def _is_flow_list(v):
    """A list of plain scalars is written inline: [1, 2, 3]."""
    return not any(isinstance(x, (dict, list, tuple)) for x in v)


def _emit(lines, v, indent, key=None):
    pad = " " * indent
    if isinstance(v, Lit):
        lines.append("%s%s: |" % (pad, key))
        for ln in v.rstrip("\n").split("\n"):
            lines.append(("%s  %s" % (pad, ln)).rstrip())
        return
    if isinstance(v, Flow):
        text = "%s%s: %s" % (pad, key, _flow(v)) if key is not None else "%s- %s" % (pad, _flow(v))
        if v.comment:
            text += "  # " + v.comment
        lines.append(text)
        return
    if isinstance(v, dict):
        if key is not None:
            lines.append("%s%s:" % (pad, key))
            indent += 2
            pad = " " * indent
        if not v:
            lines[-1] += " {}"
            return
        comments = getattr(v, "comments", {})
        blank = getattr(v, "blank_before", set())
        for k, x in v.items():
            if k in blank:
                lines.append("")
            for c in (comments.get(k) or "").split("\n") if comments.get(k) else []:
                lines.append(("%s# %s" % (pad, c)).rstrip())
            _emit(lines, x, indent, _scalar(k))
        return
    if isinstance(v, (list, tuple)):
        if key is not None and (not v or _is_flow_list(v)):
            lines.append("%s%s: %s" % (pad, key, _flow(v)))
            return
        if key is not None:
            lines.append("%s%s:" % (pad, key))
        for x in v:
            if isinstance(x, Flow):
                _emit(lines, x, indent)
            elif isinstance(x, dict):
                sub = []
                _emit(sub, x, indent + 2)
                sub[0] = pad + "- " + sub[0][indent + 2:]
                lines.extend(sub)
            elif isinstance(x, (list, tuple)):
                lines.append("%s- %s" % (pad, _flow(x)))
            else:
                lines.append("%s- %s" % (pad, _scalar(x)))
        return
    lines.append("%s%s: %s" % (pad, key, _scalar(v)))


def dump_yaml(doc, header=None):
    """-> YAML text. `header`: comment lines (without '# ') written first."""
    lines = []
    for ln in header or []:
        lines.append(("# " + ln).rstrip())
    if header:
        lines.append("")
    _emit(lines, doc, 0)
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------
# Register data: data/families/<family>.yaml and data/modbus_tcp/<map>.yaml

FAMILIES = ["inverter", "battery", "charger", "energy_bridge", "meter", "logger"]
# Row value types: name -> sungrow_datamodel TypeCode. f32 = IEEE float over 2 registers.
TYPE_CODES = {"s16": 3, "u16": 4, "s32": 5, "u32": 6, "s64": 7, "u64": 8, "f32": 9, "str": 12}
TYPE_NAMES = {v: k for k, v in TYPE_CODES.items()}
CATEGORIES = ("core", "core_hybrid", "core_string", "extra", "hidden")
CONFIDENCES = ("confirmed", "likely", "guess")
GROUPS = ("realtime", "static", "protection")
READ_DTYPES = ("u16", "s16")
KEY_RE = re.compile(r"^[a-z][a-z0-9_]*$")
# Row flags (RegisterRow.tag bits, dm_types.h TAG_*).
ROW_FLAGS = {"end": 0x01, "clear_offline": 0x02, "priority": 0x04, "no_bulk_decode": 0x08,
             "keep_sentinel": 0x10}

ROW_FIELDS = {"id", "address", "fn", "count", "type", "cycle", "flags", "bits", "capability",
              "unit", "scale"}
SIGNAL_REQUIRED = {"id", "key", "name", "confidence", "evidence"}
SIGNAL_OPTIONAL = {"unit", "scale", "device_class", "state_class", "category", "scope",
                   "priority", "group", "accuracy", "enum", "absent", "bits"}
READ_REQUIRED = {"key", "name", "fn", "address", "group", "confidence", "evidence"}
READ_OPTIONAL = {"unit", "scale", "device_class", "state_class", "priority", "enum", "dtype",
                 "accuracy"}
FIX_FIELDS = {"table", "id", "stock_address", "address", "confidence", "evidence"}
SHARED_FIELDS = {"id", "address", "fn", "evidence"}


def _check_fields(d, required, optional, where):
    if not isinstance(d, dict):
        raise DataError("%s: expected a mapping" % where)
    have = set(d)
    if required - have or have - required - optional:
        raise DataError("%s: missing %s / unknown %s" % (
            where, sorted(required - have), sorted(have - required - optional)))


def _int(d, f, where, default=None):
    v = d.get(f, default)
    if isinstance(v, bool) or not isinstance(v, int):
        raise DataError("%s: %s=%r is not an integer" % (where, f, v))
    return v


def _str(d, f, where, default=""):
    v = d.get(f, default)
    if v is None:
        return ""
    if not isinstance(v, str):
        raise DataError("%s: %s=%r is not a string" % (where, f, v))
    return v


def _scale(d, where):
    v = d.get("scale", 0)
    if v in ("", None):
        return 0.0
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise DataError("%s: scale=%r is not a number" % (where, v))
    return float(v)


def parse_capability(v, where):
    """'word.bit' -> (word, bit); None -> (0, 0)."""
    if v in (None, ""):
        return 0, 0
    m = re.match(r"^(\d+)\.(\d+)$", str(v))
    if not m:
        raise DataError("%s: capability %r is not word.bit" % (where, v))
    return int(m.group(1)), int(m.group(2))


def parse_flags(v, where):
    if v in (None, 0):
        return 0
    if isinstance(v, int) and not isinstance(v, bool):
        return v
    out = 0
    for tok in str(v).split():
        if tok not in ROW_FLAGS:
            raise DataError("%s: flag %r is not one of %s" % (where, tok, sorted(ROW_FLAGS)))
        out |= ROW_FLAGS[tok]
    return out


def flag_names(tag):
    """Inverse of parse_flags: 'end priority' (bits without a name stay numeric)."""
    names = [n for n, b in sorted(ROW_FLAGS.items(), key=lambda kv: kv[1]) if tag & b]
    rest = tag & ~sum(ROW_FLAGS.values())
    if rest:
        names.append("0x%x" % rest)
    return " ".join(names)


def _load_rows(rows, table, where):
    out = []
    for i, r in enumerate(rows or []):
        w = "%s rows[%d]" % (where, i)
        _check_fields(r, {"id", "address", "fn", "count", "type"}, ROW_FIELDS, w)
        t = r["type"]
        if t not in TYPE_CODES:
            raise DataError("%s: unknown type %r" % (w, t))
        nreg = _int(r, "count", w)
        if TYPE_CODES[t] == 9 and nreg != 2:
            raise DataError("%s: an f32 row needs count 2" % w)
        word, bit = parse_capability(r.get("capability"), w)
        nbits, bit_base = 0, 0
        if "bits" in r:
            _check_fields(r["bits"], {"first", "count"}, set(), w + " bits")
            nbits, bit_base = _int(r["bits"], "count", w), _int(r["bits"], "first", w)
            if not 1 <= nbits <= 32 or TYPE_CODES[t] not in (4, 6):
                raise DataError("%s: bits needs a u16/u32 row and 1..32 bits" % w)
        out.append({
            "sigid": _int(r, "id", w), "regaddr": _int(r, "address", w), "nreg": nreg,
            "fn": _int(r, "fn", w), "type": TYPE_CODES[t], "group": _int(r, "cycle", w, 0),
            "tag": parse_flags(r.get("flags"), w), "nbits": nbits, "bit_base": bit_base,
            "dep_word": word, "dep_bit": bit, "unit": _str(r, "unit", w),
            "scale": _scale(r, w), "table": table, "source": "table",
        })
    return out


def _load_signal(s, family, where):
    _check_fields(s, SIGNAL_REQUIRED, SIGNAL_OPTIONAL, where)
    e = {"family": family, "sigid": _int(s, "id", where), "key": _str(s, "key", where),
         "name": _str(s, "name", where), "unit": _str(s, "unit", where),
         "scale": _scale(s, where), "device_class": _str(s, "device_class", where),
         "state_class": _str(s, "state_class", where),
         "category": _str(s, "category", where), "confidence": s["confidence"],
         "evidence": _str(s, "evidence", where), "_where": where}
    if not KEY_RE.match(e["key"]):
        raise DataError("%s: key %r must be lower-case snake_case" % (where, e["key"]))
    if e["category"] and e["category"] not in CATEGORIES:
        raise DataError("%s: category must be one of %s" % (where, CATEGORIES))
    if e["confidence"] not in CONFIDENCES:
        raise DataError("%s: confidence must be one of %s" % (where, CONFIDENCES))
    for f in ("scope", "enum", "absent", "group"):
        if f in s:
            e[f] = _str(s, f, where)
    if e.get("scope", "hybrid") != "hybrid":
        raise DataError("%s: scope must be 'hybrid'" % where)
    if e.get("group", "realtime") not in GROUPS:
        raise DataError("%s: group must be one of %s" % (where, GROUPS))
    for f in ("priority", "accuracy"):
        if f in s:
            e[f] = _int(s, f, where)
    if "bits" in s:
        b = s["bits"]
        _check_fields(b, {"first", "count"}, set(), where + " bits")
        e["bits"] = (_int(b, "first", where), _int(b, "count", where))
    return e


def _load_read(s, family, where):
    _check_fields(s, READ_REQUIRED, READ_OPTIONAL, where)
    e = {"kind": "read", "family": family, "key": _str(s, "key", where),
         "name": _str(s, "name", where), "unit": _str(s, "unit", where),
         "scale": _scale(s, where), "device_class": _str(s, "device_class", where),
         "state_class": _str(s, "state_class", where), "fn": _int(s, "fn", where),
         "regaddr": _int(s, "address", where), "group": _str(s, "group", where),
         "confidence": s["confidence"], "evidence": _str(s, "evidence", where),
         "dtype": _str(s, "dtype", where, "u16"), "_where": where}
    if not KEY_RE.match(e["key"]):
        raise DataError("%s: key %r must be lower-case snake_case" % (where, e["key"]))
    if e["group"] not in GROUPS:
        raise DataError("%s: group must be one of %s" % (where, GROUPS))
    if e["confidence"] not in CONFIDENCES:
        raise DataError("%s: confidence must be one of %s" % (where, CONFIDENCES))
    if e["dtype"] not in READ_DTYPES:
        raise DataError("%s: dtype must be one of %s" % (where, READ_DTYPES))
    for f in ("priority", "accuracy"):
        if f in s:
            e[f] = _int(s, f, where)
    if "enum" in s:
        e["enum"] = _str(s, "enum", where)
    return e


def load_register_data(families_dir=FAMILIES_DIR, modbus_tcp_dir=MODBUS_TCP_DIR):
    """-> dict(tables=[{name, family, rows, title, ...}] sorted by table name,
    signals={family: [entry]} in file order, fixes=[...], shared={(table, fn, addr): entry},
    reads=[entry], families={family: header dict})."""
    tables, signals, fixes, shared, reads, headers = [], {}, [], {}, [], {}
    for fn in sorted(os.listdir(families_dir)):
        if not fn.endswith(".yaml"):
            continue
        path = os.path.join(families_dir, fn)
        doc = load_yaml(path)
        where = rel(path)
        _check_fields(doc, {"family", "tables", "signals"},
                      {"title", "device_types", "reference", "notes", "fixes", "reads"}, where)
        family = doc["family"]
        if family not in FAMILIES or family != fn[:-5]:
            raise DataError("%s: family %r must be the file name and one of %s"
                            % (where, family, FAMILIES))
        headers[family] = {k: doc.get(k) for k in ("title", "device_types", "reference", "notes")}
        if not isinstance(doc["tables"], dict):
            raise DataError("%s: tables must be a mapping" % where)
        for tname, t in doc["tables"].items():
            tw = "%s tables.%s" % (where, tname)
            _check_fields(t, {"rows"}, {"title", "protocols", "subtype", "notes", "shared"}, tw)
            tables.append({"name": tname, "family": family, "rows": _load_rows(t["rows"], tname, tw),
                           "title": t.get("title", ""), "protocols": t.get("protocols"),
                           "subtype": t.get("subtype")})
            for i, s in enumerate(t.get("shared") or []):
                sw = "%s shared[%d]" % (tw, i)
                _check_fields(s, SHARED_FIELDS, set(), sw)
                shared[(tname, _int(s, "fn", sw), _int(s, "address", sw))] = {
                    "table": tname, "fn": s["fn"], "regaddr": s["address"], "sigid": s["id"],
                    "evidence": s["evidence"], "_where": sw}
        seen_id, seen_key = {}, {}
        for i, s in enumerate(doc["signals"] or []):
            sw = "%s signals[%d]" % (where, i)
            e = _load_signal(s, family, sw)
            e["_order"] = i
            if e["sigid"] in seen_id:
                raise DataError("%s: duplicate signal 0x%04x (first at signals[%d])"
                                % (sw, e["sigid"], seen_id[e["sigid"]]))
            if e["key"] in seen_key:
                raise DataError("%s: key %r already used by signals[%d]"
                                % (sw, e["key"], seen_key[e["key"]]))
            seen_id[e["sigid"]], seen_key[e["key"]] = i, i
            signals.setdefault(family, []).append(e)
        for i, f in enumerate(doc.get("fixes") or []):
            fw = "%s fixes[%d]" % (where, i)
            _check_fields(f, FIX_FIELDS, set(), fw)
            if f["confidence"] not in CONFIDENCES:
                raise DataError("%s: confidence must be one of %s" % (fw, CONFIDENCES))
            fixes.append({"table": f["table"], "sigid": _int(f, "id", fw),
                          "regaddr": _int(f, "stock_address", fw),
                          "new_regaddr": _int(f, "address", fw), "confidence": f["confidence"],
                          "evidence": f["evidence"], "_where": fw})
        for i, r in enumerate(doc.get("reads") or []):
            reads.append(_load_read(r, family, "%s reads[%d]" % (where, i)))
    for fn in sorted(os.listdir(modbus_tcp_dir)) if os.path.isdir(modbus_tcp_dir) else []:
        if not fn.endswith(".yaml"):
            continue
        path = os.path.join(modbus_tcp_dir, fn)
        doc = load_yaml(path)
        where = rel(path)
        _check_fields(doc, {"table", "family", "device_types", "rows"},
                      {"title", "notes", "shared", "reference"}, where)
        if doc["family"] not in FAMILIES:
            raise DataError("%s: unknown family %r" % (where, doc["family"]))
        tname = doc["table"]
        tables.append({"name": tname, "family": doc["family"], "rows": _load_rows(doc["rows"], tname, where),
                       "title": doc.get("title", ""), "protocols": None, "subtype": None,
                       "device_types": doc["device_types"], "export": True})
        for i, s in enumerate(doc.get("shared") or []):
            sw = "%s shared[%d]" % (where, i)
            _check_fields(s, SHARED_FIELDS, set(), sw)
            shared[(tname, _int(s, "fn", sw), _int(s, "address", sw))] = {
                "table": tname, "fn": s["fn"], "regaddr": s["address"], "sigid": s["id"],
                "evidence": s["evidence"], "_where": sw}
    names = [t["name"] for t in tables]
    if len(set(names)) != len(names):
        raise DataError("duplicate table name in data/families + data/modbus_tcp: %s"
                        % sorted(n for n in names if names.count(n) > 1))
    tables.sort(key=lambda t: t["name"])
    return {"tables": tables, "signals": signals, "fixes": fixes, "shared": shared,
            "reads": reads, "families": headers}


# ---------------------------------------------------------------------------------------
# The other data files: thin loaders with structural checks. Field semantics are documented
# in docs/data_format.md; the generators validate the values they use.

def load_cloud_points(path=CLOUD_POINTS):
    doc = load_yaml(path)
    _check_fields(doc, {"tables"}, {"notes"}, rel(path))
    for i, t in enumerate(doc["tables"]):
        w = "%s tables[%d]" % (rel(path), i)
        _check_fields(t, {"table_type", "cloud_device_type", "points"}, {"title", "notes"}, w)
        for j, p in enumerate(t["points"]):
            _check_fields(p, {"signal", "point", "attrs", "format", "scale", "size", "type"},
                          set(), "%s points[%d]" % (w, j))
    return doc


def load_installer_rules(path=INSTALLER_RULES):
    if not os.path.exists(path):
        return {"families": {}}
    doc = load_yaml(path)
    _check_fields(doc, {"families"}, {"notes"}, rel(path))
    for fam, rules in doc["families"].items():
        for i, r in enumerate(rules or []):
            _check_fields(r, {"address", "type"}, {"label", "scale", "unit", "range", "regulation"},
                          "%s families.%s[%d]" % (rel(path), fam, i))
    return doc


def load_grid_profiles(dir_=GRID_PROFILES_DIR):
    """-> (index doc, {family: doc}) for every family the index names with a file."""
    index = load_yaml(os.path.join(dir_, "index.yaml"))
    _check_fields(index, {"families", "descriptors", "setup_countries", "unit_company_register",
                          "unit_grid_type_register"}, {"notes"}, rel(os.path.join(dir_, "index.yaml")))
    docs = {}
    for fam in index["families"]:
        p = os.path.join(dir_, fam + ".yaml")
        if not os.path.exists(p):
            continue
        d = load_yaml(p)
        _check_fields(d, {"family", "registers", "profiles", "grid_type_profiles", "catalogue"},
                      {"title", "notes"}, rel(p))
        if d["family"] != fam:
            raise DataError("%s: family %r does not match the file name" % (rel(p), d["family"]))
        docs[fam] = d
    return index, docs


def load_write_registers(path=WRITE_REGISTERS):
    doc = load_yaml(path)
    _check_fields(doc, {"registers"}, {"notes"}, rel(path))
    return doc["registers"]


def load_installer_help(path=INSTALLER_HELP):
    """{family: {address: text}}; {} when the file is absent."""
    if not os.path.exists(path):
        return {}
    doc = load_yaml(path) or {}
    out = {}
    for fam, m in doc.items():
        if fam == "notes":
            continue
        out[fam] = {int(k): str(v) for k, v in (m or {}).items()}
    return out
