#!/usr/bin/env python3
"""Generate the sungrow_control write allow-list from data/write_registers.yaml.

Outputs (deterministic, DO NOT EDIT):
  components/sungrow_control/write_registers.h / .cpp   rules (with per-protocol addresses),
                                                        read-back blocks + RegisterTable per
                                                        profile and protocol (fn 3 rows,
                                                        synthetic sigids 0xF100+)
  components/sungrow_control/write_meta.py              entity metadata for the select/number
                                                        platforms

Needs PyYAML (tools/requirements.txt); the file format is described in docs/data_format.md.

Usage: python3 tools/gen_write_registers.py [--check]
  --check   exit 1 if the generated files are not up to date (used by the tests).
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import datalib  # noqa: E402

SRC = datalib.WRITE_REGISTERS
OUT_DIR = os.path.join(ROOT, "components", "sungrow_control")

PROFILES = {"hybrid": (1, 0xF100), "string": (2, 0xF140)}
TYPES = {"u16": (4, 0, 0xFFFF), "s16": (3, -0x8000, 0x7FFF)}
FIELDS_REQ = {"profile", "key", "name", "address", "type", "min", "max", "interval", "confidence",
              "evidence", "mkaiser"}
FIELDS_OPT = {"values", "options", "scale", "unit", "step", "number_min", "number_max",
              "address_by_protocol", "requires"}
REQUIRES = {"battery"}
CONFIDENCE = {"confirmed", "likely", "guess"}


def fail(e, msg):
    raise SystemExit("%s: registers[%d] (%s): %s" % (datalib.rel(e.get("_path", SRC)),
                                                     e.get("_index", -1), e.get("key", "?"), msg))


def _int(e, f):
    v = e.get(f)
    if isinstance(v, bool) or not isinstance(v, int):
        fail(e, "%s=%r is not an integer" % (f, v))
    return v


def _float(e, f, default):
    if f not in e or e[f] in ("", None):
        return default
    v = e[f]
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        fail(e, "%s=%r is not a number" % (f, v))
    return float(v)


def load(path=SRC):
    entries = datalib.load_write_registers(path)
    out, keys, addrs = [], set(), set()
    for i, e in enumerate(entries):
        if not isinstance(e, dict):
            raise SystemExit("%s: registers[%d] is not a mapping" % (datalib.rel(path), i))
        e = dict(e, _index=i, _path=path)
        fields = set(e) - {"_index", "_path"}
        if FIELDS_REQ - fields:
            fail(e, "missing %s" % sorted(FIELDS_REQ - fields))
        if fields - FIELDS_REQ - FIELDS_OPT:
            fail(e, "unknown %s" % sorted(fields - FIELDS_REQ - FIELDS_OPT))
        if e["profile"] not in PROFILES:
            fail(e, "profile must be one of %s" % sorted(PROFILES))
        if e["type"] not in TYPES:
            fail(e, "type must be one of %s" % sorted(TYPES))
        if e["confidence"] not in CONFIDENCE:
            fail(e, "confidence must be one of %s" % sorted(CONFIDENCE))
        if "requires" in e and e["requires"] not in REQUIRES:
            fail(e, "requires must be one of %s" % sorted(REQUIRES))
        if e["key"] in keys:
            fail(e, "duplicate key %s" % e["key"])
        keys.add(e["key"])
        r = {
            "line": i, "profile": e["profile"], "key": e["key"], "name": str(e["name"]),
            "address": _int(e, "address"), "type": e["type"], "min": _int(e, "min"),
            "max": _int(e, "max"), "interval": _int(e, "interval"),
            "confidence": e["confidence"], "evidence": str(e["evidence"]),
            "mkaiser": str(e["mkaiser"]), "scale": _float(e, "scale", 1.0),
            "unit": str(e.get("unit") or ""), "requires": str(e.get("requires") or ""),
        }
        if (r["profile"], r["address"]) in addrs:
            fail(e, "duplicate address %d in profile %s" % (r["address"], r["profile"]))
        addrs.add((r["profile"], r["address"]))
        if not 1 <= r["address"] <= 0xFFFF:
            fail(e, "address out of range")
        _, tmin, tmax = TYPES[r["type"]]
        if not tmin <= r["min"] <= r["max"] <= tmax:
            fail(e, "need %d <= min <= max <= %d" % (tmin, tmax))
        vals = e.get("values") or []
        if not isinstance(vals, list) or any(isinstance(v, bool) or not isinstance(v, int) for v in vals):
            fail(e, "values must be a list of integers")
        r["values"] = list(vals)
        for v in r["values"]:
            if not r["min"] <= v <= r["max"]:
                fail(e, "value %d outside min/max" % v)
        opts = []
        if e.get("options"):
            for o in e["options"]:
                if not isinstance(o, dict) or set(o) != {"value", "label"} or not str(o["label"]):
                    fail(e, "options must be a list of {value, label}")
                opts.append((int(o["value"]), str(o["label"])))
            if not r["values"]:
                fail(e, "a select (options) needs an explicit values list")
            for v in r["values"]:
                if v not in [o[0] for o in opts]:
                    fail(e, "value %d has no option text" % v)
        r["options"] = opts
        alt = {}
        abp = e.get("address_by_protocol") or {}
        if not isinstance(abp, dict):
            fail(e, "address_by_protocol must be a mapping {protocol: address}")
        for proto, addr in abp.items():
            if (isinstance(proto, bool) or not isinstance(proto, int) or isinstance(addr, bool)
                    or not isinstance(addr, int) or not 1 <= proto <= 5 or not 1 <= addr <= 0xFFFF):
                fail(e, "address_by_protocol: bad entry %r: %r" % (proto, addr))
            alt[proto] = addr
        r["address_by_protocol"] = alt
        r["step"] = _float(e, "step", r["scale"])
        r["number_min"] = _float(e, "number_min", r["min"] * r["scale"])
        r["number_max"] = _float(e, "number_max", r["max"] * r["scale"])
        if not opts and not (r["min"] * r["scale"] <= r["number_min"] + 1e-9 and
                             r["number_max"] <= r["max"] * r["scale"] + 1e-9):
            fail(e, "number_min/number_max must lie inside min/max x scale")
        out.append(r)
    for r in out:
        for a in r["address_by_protocol"].values():
            if (r["profile"], a) in addrs:
                raise SystemExit("%s: registers[%d] (%s): address_by_protocol %d is another %s row"
                                 % (datalib.rel(path), r["line"], r["key"], a, r["profile"]))
    # Synthetic sigids: per profile, in file order.
    for p, (_, base) in PROFILES.items():
        for i, r in enumerate(x for x in out if x["profile"] == p):
            r["sigid"] = base + i
    return out


def blocks(rows):
    """Contiguous addresses -> one fn 3 read-back block (period = smallest interval)."""
    rows = sorted(rows, key=lambda r: r["address"])
    out = []
    for r in rows:
        if out and out[-1]["start"] + out[-1]["count"] == r["address"] and out[-1]["count"] < 16:
            out[-1]["count"] += 1
            out[-1]["interval"] = min(out[-1]["interval"], r["interval"])
        else:
            out.append({"start": r["address"], "count": 1, "interval": r["interval"]})
    return out


def c_str(s):
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def c_float(v):
    if v == 1.0:
        return "0.0f"  # RegisterRow scale 0 = raw
    s = repr(float(v))
    return s + "f" if ("." in s or "e" in s) else s + ".0f"


def gen_cpp(rows):
    h = [
        "// GENERATED by tools/gen_write_registers.py from data/write_registers.yaml - DO NOT EDIT.",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        '#include "../sungrow_datamodel/dm_types.h"',
        "namespace esphome {",
        "namespace sungrow_control {",
        "",
        "enum class Profile : uint8_t { NONE = 0, HYBRID = 1, STRING = 2 };",
        "",
        "/// The register a rule uses on devices of one protocol type (stock descriptor table).",
        "struct RuleAlt {",
        "  uint8_t protocol;",
        "  uint16_t address;",
        "};",
        "",
        "/// One allow-listed holding register (1-based table address = document number).",
        "struct RuleDef {",
        "  uint16_t address;  ///< default address (protocols without an `alt` entry)",
        "  uint8_t type;      ///< sungrow_datamodel TypeCode (TYPE_U16 / TYPE_S16)",
        "  int32_t min;       ///< raw, inclusive",
        "  int32_t max;",
        "  const uint16_t *values;  ///< nullptr = any value in [min, max]",
        "  uint8_t nvalues;",
        "  uint16_t sigid;    ///< read-back signal in the SignalStore",
        "  const char *key;",
        "  const RuleAlt *alt;  ///< per-protocol addresses; nullptr = `address` everywhere",
        "  uint8_t nalt;",
        "};",
        "",
        "/// fn 3 read-back block.",
        "struct BlockDef {",
        "  uint16_t start;",
        "  uint8_t count;",
        "  uint32_t interval_ms;",
        "};",
        "",
        "/// Read-back blocks + rows of a profile for one protocol type.",
        "struct ReadbackDef {",
        "  uint8_t protocol;  ///< 0 = the default addresses",
        "  const BlockDef *blocks;",
        "  uint8_t nblocks;",
        "  /// Read-back rows (fn 3, synthetic sigids) for SignalStore::ingest_block().",
        "  const sungrow_datamodel::RegisterTable *table;",
        "};",
        "",
        "struct ProfileDef {",
        "  Profile profile;",
        "  const char *name;",
        "  const RuleDef *rules;",
        "  uint8_t nrules;",
        "  const ReadbackDef *readback;  ///< [0] = default addresses, then one per alt protocol",
        "  uint8_t nreadback;",
        "};",
        "",
        "const ProfileDef *find_profile(Profile p);",
        "const ProfileDef *find_profile(const char *name);",
        "/// Address of `r` on a device of protocol type `protocol` (0 = unknown: the default).",
        "uint16_t rule_address(const RuleDef &r, uint8_t protocol);",
        "/// Read-back blocks + rows of `p` for `protocol` (the default set when it has no alt).",
        "const ReadbackDef &profile_readback(const ProfileDef &p, uint8_t protocol);",
        "",
    ]
    for r in rows:
        h.append("static constexpr uint16_t SIG_%s = 0x%04X;  // %s @%d" %
                 (r["key"].upper(), r["sigid"], r["profile"], r["address"]))
    h += ["", "}  // namespace sungrow_control", "}  // namespace esphome", ""]

    c = [
        "// GENERATED by tools/gen_write_registers.py from data/write_registers.yaml - DO NOT EDIT.",
        '#include "write_registers.h"',
        "#include <cstring>",
        "namespace esphome {",
        "namespace sungrow_control {",
        "using sungrow_datamodel::Family;",
        "using sungrow_datamodel::NO_KEY;",
        "using sungrow_datamodel::RegisterRow;",
        "using sungrow_datamodel::RegisterTable;",
        "",
    ]
    defs = []
    for p, (pid, _) in PROFILES.items():
        pr = [r for r in rows if r["profile"] == p]
        for r in pr:
            if r["values"]:
                c.append("static const uint16_t k_vals_%s[] = {%s};" %
                         (r["key"], ", ".join("0x%04X" % v for v in r["values"])))
            if r["address_by_protocol"]:
                c.append("static const RuleAlt k_alt_%s[] = {%s};" %
                         (r["key"], ", ".join("{%d, %d}" % kv for kv in
                                              sorted(r["address_by_protocol"].items()))))
        c.append("static const RuleDef k_rules_%s[] = {" % p)
        for r in pr:
            vals = ("k_vals_%s, %d" % (r["key"], len(r["values"]))) if r["values"] else "nullptr, 0"
            alt = (("k_alt_%s, %d" % (r["key"], len(r["address_by_protocol"])))
                   if r["address_by_protocol"] else "nullptr, 0")
            c.append("    {%d, %d, %d, %d, %s, 0x%04X, %s, %s},  // %s, %s" %
                     (r["address"], TYPES[r["type"]][0], r["min"], r["max"], vals, r["sigid"],
                      c_str(r["key"]), alt, r["name"], r["confidence"]))
        c.append("};")
        protos = sorted({k for r in pr for k in r["address_by_protocol"]})
        rb = []
        for proto in [0] + protos:
            sfx = p if proto == 0 else "%s_p%d" % (p, proto)
            prow = [dict(r, address=r["address_by_protocol"].get(proto, r["address"])) for r in pr]
            bl = blocks(prow)
            c.append("static const BlockDef k_blocks_%s[] = {" % sfx)
            for b in bl:
                c.append("    {%d, %d, %d}," % (b["start"], b["count"], b["interval"] * 1000))
            c.append("};")
            c.append("static const RegisterRow k_rows_%s[] = {" % sfx)
            for r in sorted(prow, key=lambda r: r["address"]):
                # sigid, regaddr, group (100 ms), key, nreg, fn, type, tag, nbits, dep_word, dep_bit, unit, scale
                c.append("    {0x%04X, %d, %d, NO_KEY, 1, 3, %d, 0, 0, 0, 0, 0, %s}," %
                         (r["sigid"], r["address"], r["interval"] * 10, TYPES[r["type"]][0],
                          c_float(r["scale"])))
            c.append("};")
            c.append("static const RegisterTable k_table_%s = {0xF0, Family::INVERTER, \"control_%s\", "
                     "k_rows_%s, %d};" % (sfx, sfx, sfx, len(prow)))
            rb.append("    {%d, k_blocks_%s, %d, &k_table_%s}," % (proto, sfx, len(bl), sfx))
        c.append("static const ReadbackDef k_readback_%s[] = {" % p)
        c += rb
        c.append("};")
        c.append("")
        defs.append("    {Profile::%s, %s, k_rules_%s, %d, k_readback_%s, %d}," %
                    (p.upper(), c_str(p), p, len(pr), p, len(rb)))
    c.append("static const ProfileDef k_profiles[] = {")
    c += defs
    c.append("};")
    c += [
        "",
        "const ProfileDef *find_profile(Profile p) {",
        "  for (const auto &d : k_profiles)",
        "    if (d.profile == p)",
        "      return &d;",
        "  return nullptr;",
        "}",
        "",
        "const ProfileDef *find_profile(const char *name) {",
        "  for (const auto &d : k_profiles)",
        "    if (name != nullptr && std::strcmp(d.name, name) == 0)",
        "      return &d;",
        "  return nullptr;",
        "}",
        "",
        "uint16_t rule_address(const RuleDef &r, uint8_t protocol) {",
        "  for (uint8_t i = 0; i < r.nalt; i++)",
        "    if (r.alt[i].protocol == protocol)",
        "      return r.alt[i].address;",
        "  return r.address;",
        "}",
        "",
        "const ReadbackDef &profile_readback(const ProfileDef &p, uint8_t protocol) {",
        "  for (uint8_t i = 1; i < p.nreadback; i++)",
        "    if (p.readback[i].protocol == protocol)",
        "      return p.readback[i];",
        "  return p.readback[0];",
        "}",
        "",
        "}  // namespace sungrow_control",
        "}  // namespace esphome",
        "",
    ]
    return "\n".join(h), "\n".join(c)


def gen_meta(rows):
    out = [
        "# GENERATED by tools/gen_write_registers.py from data/write_registers.yaml - DO NOT EDIT.",
        '"""Entity metadata of the allow-listed registers (key -> dict)."""',
        "",
        "PROFILES = %r" % sorted(PROFILES),
        "",
        "KEYS = {",
    ]
    for r in rows:
        d = {k: r[k] for k in ("profile", "name", "address", "type", "min", "max", "values",
                               "scale", "unit", "step", "number_min", "number_max", "sigid",
                               "interval", "confidence", "address_by_protocol", "requires")}
        d["options"] = r["options"]
        out.append("    %r: %r," % (r["key"], d))
    out += ["}", ""]
    return "\n".join(out)


def outputs():
    rows = load()
    h, c = gen_cpp(rows)
    return {
        os.path.join(OUT_DIR, "write_registers.h"): h,
        os.path.join(OUT_DIR, "write_registers.cpp"): c,
        os.path.join(OUT_DIR, "write_meta.py"): gen_meta(rows),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    stale = []
    for path, text in outputs().items():
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old != text:
            stale.append(path)
            if not a.check:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
    if a.check and stale:
        print("stale: %s (run tools/gen_write_registers.py)" % ", ".join(stale))
        return 1
    if not a.check:
        print("wrote %d file(s)" % len(stale))
    return 0


if __name__ == "__main__":
    sys.exit(main())
