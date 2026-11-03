#!/usr/bin/env python3
"""Generate the Installer catalogue, the per-family write allow-lists and the value rules.

BUILD-TIME ONLY (needs PyYAML, deterministic). Companion to tools/gen_grid_profiles.py (families,
profile sets, descriptor table) and tools/gen_installer_rules.py (ranges / relations).

Source: the `catalogue` of data/grid_profiles/<family>.yaml (docs/data_format.md): every
editable, register-backed setting of the family's writable pages, one item per holding
register with its page breadcrumb, control type, width, scale / accuracy / unit and options.

Outputs:
  components/sungrow_web/web/src/installer_settings.gen.ts   INSTALLER_SETTINGS for the SPA
      Installer page: address, label, breadcrumb, control, dtype (U32 rows carry
      wordOrder "lo_first": low word at `address`), scale / accuracy / unit, risk, options,
      min / max in display units with rangeSource, and the regulation string.
  components/sungrow_control/installer_allow.gen.h   installer_addr_allowed(family, addr): per
      settings family the catalogue addresses (+ U32 second words), every register of the
      family's profile sets and the family's descriptor commit registers (country, grid code,
      and 30036/30037 for unit). NONE / UNIT_NODESC allow nothing.
  components/sungrow_control/installer_rules.gen.h   installer_range(family, addr) and
      installer_rels(family, addr, ...): the raw value range and the register relations the
      server enforces on Installer writes.

Item model (catalogue items):
  - control: number   scale = display multiplier, accuracy = decimals, unit.
  - control: enum     options [{value, label}].
  - control: switch   two options (off / on values).
  - type: U16 | S16 | U32 (U32 spans address and address + 1, low word first).
Risk tier from the group's write_priority: developer -> "expert", senior -> "advanced", else
"basic".

Ranges, per (family, address), first match wins (rangeSource):
  "stock"    the range of data/installer_rules.yaml (when that file exists),
  "envelope" min / max over every profile set value of that register in the family, together
             with the option values of an enum / switch row,
  "dtype"    the full U16 / S16 / U32 range.
The server (ControlCore) enforces the range and the simple relations on the Installer path; the SPA
evaluates the regulation string for inline feedback.

Usage: python3 tools/gen_installer_settings.py [--check]   (--check: exit 1 if outputs are stale).
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import datalib  # noqa: E402
import gen_grid_profiles as gp  # noqa: E402
import gen_installer_rules as ir  # noqa: E402

ROOT = os.path.dirname(HERE)
CTL_DIR = os.path.join(ROOT, "components", "sungrow_control")
WEB_DIR = os.path.join(ROOT, "components", "sungrow_web", "web", "src")

CONTROLS = {"number", "enum", "switch"}
DTYPE_RANGE = {"U16": (0, 0xFFFF), "S16": (-0x8000, 0x7FFF), "U32": (0, 0xFFFFFFFF)}
ITEM_FIELDS = {"address", "label", "control", "type", "scale", "accuracy", "unit", "options",
               "refresh"}


def reg_types(fam):
    """addr -> 'U16'|'S16'|'U32' from the family's register table."""
    return {a: t for a, (t, _) in gp.reg_table(fam).items()}


def catalogue_rows(fam):
    """The family's catalogue items as flat rows (one per address, sorted by address)."""
    rows = {}
    where = datalib.rel(gp.profile_path(fam))
    for gi_, g in enumerate(gp.DOCS[fam]["catalogue"] or []):
        gw = "%s catalogue[%d]" % (where, gi_)
        if not isinstance(g, dict) or "path" not in g or "items" not in g:
            raise SystemExit("%s: need path, items" % gw)
        path = [str(x) for x in g["path"]]
        write_pri = str(g.get("write_priority") or "client")
        for ii, it in enumerate(g["items"] or []):
            iw = "%s items[%d]" % (gw, ii)
            if not isinstance(it, dict) or set(it) - ITEM_FIELDS or not {"address", "label", "control", "type"} <= set(it):
                raise SystemExit("%s: need address, label, control, type (+ scale, accuracy, unit, options, refresh)" % iw)
            addr = it["address"]
            if isinstance(addr, bool) or not isinstance(addr, int) or not 1 <= addr <= 0xFFFF:
                raise SystemExit("%s: bad address %r" % (iw, addr))
            if addr in rows:
                raise SystemExit("%s: register %d listed twice in the catalogue" % (iw, addr))
            if it["control"] not in CONTROLS or it["type"] not in DTYPE_RANGE:
                raise SystemExit("%s: control must be number / enum / switch and type U16 / S16 / U32" % iw)
            opts = []
            for o in it.get("options") or []:
                if not isinstance(o, dict) or set(o) != {"value", "label"}:
                    raise SystemExit("%s: options must be {value, label}" % iw)
                opts.append({"label": str(o["label"]), "value": int(o["value"])})
            if it["control"] != "number" and not opts:
                raise SystemExit("%s: an enum / switch needs options" % iw)
            rows[addr] = dict(family=fam, address=addr, label=str(it["label"]), category=path,
                              dtype=it["type"], write_pri=write_pri, control=it["control"],
                              scale=float(it.get("scale", 1.0)), accuracy=int(it.get("accuracy", 0)),
                              unit=str(it.get("unit") or ""), options=opts)
    return [rows[a] for a in sorted(rows)]


# ---- values -----------------------------------------------------------------------------------

def set_values(fam, dtypes):
    """{addr: set of raw values} over every profile set of the family, decoded per register width
    (`dtypes` {addr: dtype} first, then the register table; U32 = low word first, S16 signed)."""
    rtypes = reg_types(fam)
    rtypes.update(dtypes)
    out = {}
    for a, ws in gp.all_sets(fam):
        i = 0
        while i < len(ws):
            addr = a + i
            t = rtypes.get(addr, "U16")
            if t == "U32" and i + 1 < len(ws):
                v, i = ws[i] | (ws[i + 1] << 16), i + 2
            else:
                v, i = (ws[i] - 0x10000 if t == "S16" and ws[i] & 0x8000 else ws[i]), i + 1
            out.setdefault(addr, set()).add(v)
    return out


_RULES = None


def stock_rules():
    """The data/installer_rules.yaml tables (cached)."""
    global _RULES
    if _RULES is None:
        _RULES = ir.load()
    return _RULES


def apply_ranges(rows, stock):
    """Attach dtype (the item's type, else the rules file's, else U16), raw_min / raw_max /
    range_source and regulation to every row. A rules-file range is widened to cover the
    family's own profile set values and enum options: the profiles write those themselves, and
    the static range of a row with a live range callback (e.g. the unit OV thresholds,
    [466.0~659.0] V for 400 V models) can exclude the values of a 230 V model."""
    for r in rows:
        if r["dtype"] is None:
            r["dtype"] = stock["dtypes"].get((r["family"], r["address"]), "U16")
    env = {fam: set_values(fam, {r["address"]: r["dtype"] for r in rows if r["family"] == fam})
           for fam in gp.PROFILE_FAMILIES}
    for r in rows:
        key = (r["family"], r["address"])
        vals = set(env[r["family"]].get(r["address"], ()))
        vals |= {o["value"] for o in r["options"]}
        if key in stock["ranges"]:
            lo, hi = stock["ranges"][key]
            r["raw_min"], r["raw_max"] = min([lo] + list(vals)), max([hi] + list(vals))
            r["range_source"] = "stock"
        elif vals:
            r["raw_min"], r["raw_max"] = min(vals), max(vals)
            r["range_source"] = "envelope"
        else:
            r["raw_min"], r["raw_max"] = DTYPE_RANGE[r["dtype"]]
            r["range_source"] = "dtype"
        if key in stock["regulation"]:
            r["regulation"] = stock["regulation"][key]


def load():
    rows = []
    help_map = datalib.load_installer_help()
    for fam in gp.PROFILE_FAMILIES:
        rows.extend(catalogue_rows(fam))
    rows.sort(key=lambda e: (e["family"], e["address"]))
    for e in rows:
        h = help_map.get(e["family"], {}).get(e["address"])
        if h:
            e["help"] = h
    apply_ranges(rows, stock_rules())
    return rows


def allow_addrs(rows, fam):
    """Per-family write allow-list (sorted)."""
    if fam not in gp.PROFILE_FAMILIES:
        return []
    addrs = set()
    for e in rows:
        if e["family"] != fam:
            continue
        addrs.add(e["address"])
        if e["dtype"] == "U32":
            addrs.add(e["address"] + 1)
    for a, ws in gp.all_sets(fam):
        addrs.update(range(a, a + len(ws)))
    addrs.update(gp.commit_regs(fam))
    return sorted(addrs)


# ---- emitters ---------------------------------------------------------------------------------

GEN = "GENERATED by tools/gen_installer_settings.py from data/grid_profiles/*.yaml - DO NOT EDIT."
FAM_ENUM = {"str_pv": "STR_PV", "res_es": "RES_ES", "res_new_pv": "RES_NEW_PV", "unit": "UNIT"}


def ts_str(s):
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def num_s(x):
    x = round(x, 6)
    return ("%d" % x) if float(x).is_integer() else repr(x)


def risk_of(write_pri):
    return {"developer": "expert", "senior": "advanced"}.get(write_pri, "basic")


def gen_ts(rows):
    t = [
        "// " + GEN,
        "// Typed Installer catalogue for the SPA Installer page (view + edit).",
        "// `scale`/`accuracy`/`unit` format a value (display = raw * scale). A U32 row spans `address`",
        "// and `address + 1` with wordOrder \"lo_first\": the low word is at `address`. min / max are in",
        "// display units; rangeSource says where they come from (stock getParam range, envelope of",
        "// the stock XML values, or the dtype). `regulation` is the stock rule string with every row",
        "// reference rewritten to @<address>. `risk`: basic | advanced | expert.",
        "export type SettingControl = \"number\" | \"enum\" | \"switch\";",
        "export type SettingRisk = \"basic\" | \"advanced\" | \"expert\";",
        "export type RangeSource = \"stock\" | \"envelope\" | \"dtype\";",
        "export interface InstallerSetting {",
        "  family: string;",
        "  address: number;         // 1-based document holding register",
        "  label: string;",
        "  category: string[];      // breadcrumb from the stock page tree",
        "  control: SettingControl;",
        "  dtype: \"U16\" | \"S16\" | \"U32\";",
        "  wordOrder?: \"lo_first\"; // U32 rows only",
        "  scale: number;           // display = raw * scale",
        "  accuracy: number;        // decimal places",
        "  unit: string;",
        "  risk: SettingRisk;",
        "  min?: number;            // display units",
        "  max?: number;",
        "  rangeSource: RangeSource;",
        "  regulation?: string;     // e.g. \"@32322<=@32323\"",
        "  help?: string;           // plain-English tooltip (from data/installer_help.yaml)",
        "  options?: { label: string; value: number }[];",
        "}",
        "",
        "export const INSTALLER_SETTINGS: InstallerSetting[] = [",
    ]
    for e in rows:
        cats = "[%s]" % ", ".join(ts_str(c) for c in e["category"])
        word = ', wordOrder: "lo_first"' if e["dtype"] == "U32" else ""
        extra = ", min: %s, max: %s, rangeSource: %s" % (
            num_s(e["raw_min"] * e["scale"]), num_s(e["raw_max"] * e["scale"]), ts_str(e["range_source"]))
        if e.get("regulation"):
            extra += ", regulation: %s" % ts_str(e["regulation"])
        if e.get("help"):
            extra += ", help: %s" % ts_str(e["help"])
        if e["options"]:
            extra += ", options: [%s]" % ", ".join(
                "{ label: %s, value: %d }" % (ts_str(o["label"]), o["value"]) for o in e["options"])
        t.append(
            "  { family: %s, address: %d, label: %s, category: %s, control: %s, dtype: %s%s, "
            "scale: %s, accuracy: %d, unit: %s, risk: %s%s }," % (
                ts_str(e["family"]), e["address"], ts_str(e["label"]), cats, ts_str(e["control"]),
                ts_str(e["dtype"]), word, num_s(e["scale"]), e["accuracy"], ts_str(e["unit"]),
                ts_str(risk_of(e["write_pri"])), extra))
    t += ["];", ""]
    return "\n".join(t)


def c_rows(vals, per=14):
    return ["    " + ", ".join(str(v) for v in vals[i:i + per]) + ","
            for i in range(0, len(vals), per)]


def gen_allow_h(rows):
    h = [
        "// " + GEN,
        "#pragma once",
        "// Installer write allow-list per XML family: the catalogue addresses (U32 -> both registers),",
        "// every <set> register of the family's PROTECT_PARAM_INIT and the family's descriptor commit",
        "// registers (country, grid code; 30036 / 30037 for unit). NONE and UNIT_NODESC allow nothing.",
        "// Header-only constexpr data.",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        '#include "../sungrow_modbus/discovery.h"',
        "",
        "namespace esphome {",
        "namespace sungrow_control {",
        "",
    ]
    for fam in gp.PROFILE_FAMILIES:
        a = allow_addrs(rows, fam)
        h.append("inline constexpr uint16_t INSTALLER_ALLOW_%s[] = {" % FAM_ENUM[fam])
        h += c_rows(a)
        h.append("};")
    h += [
        "",
        "/// Sorted allow-list of `fam` (nullptr / 0 for NONE and UNIT_NODESC).",
        "inline const uint16_t *installer_allow_list(sungrow_modbus::XmlFamily fam, size_t *n) {",
        "  switch (fam) {",
    ]
    for fam in gp.PROFILE_FAMILIES:
        h += ["    case sungrow_modbus::XmlFamily::%s:" % FAM_ENUM[fam],
              "      *n = sizeof(INSTALLER_ALLOW_%s) / sizeof(uint16_t);" % FAM_ENUM[fam],
              "      return INSTALLER_ALLOW_%s;" % FAM_ENUM[fam]]
    h += [
        "    default:",
        "      *n = 0;",
        "      return nullptr;",
        "  }",
        "}",
        "",
        "/// True if `addr` may be written to a device of XML family `fam` (binary search).",
        "inline bool installer_addr_allowed(sungrow_modbus::XmlFamily fam, uint16_t addr) {",
        "  size_t n = 0;",
        "  const uint16_t *a = installer_allow_list(fam, &n);",
        "  size_t lo = 0, hi = n;",
        "  while (lo < hi) {",
        "    size_t mid = (lo + hi) / 2;",
        "    if (a[mid] < addr)",
        "      lo = mid + 1;",
        "    else",
        "      hi = mid;",
        "  }",
        "  return lo < n && a[lo] == addr;",
        "}",
        "",
        "}  // namespace sungrow_control",
        "}  // namespace esphome",
        "",
    ]
    return "\n".join(h)


OP_ENUM = {"<": "LT", "<=": "LE", ">": "GT", ">=": "GE", "=": "EQ"}
OP_FLIP = {"<": ">", "<=": ">=", ">": "<", ">=": "<=", "=": "="}


def rules_tables(rows, stock):
    """-> {fam: (ranges [(addr, dtype, min, max)], rels [(lhs, rhs, op)])}, both sorted."""
    out = {}
    for fam in gp.PROFILE_FAMILIES:
        rng = {r["address"]: (r["address"], r["dtype"], r["raw_min"], r["raw_max"])
               for r in rows if r["family"] == fam}
        allowed = set(allow_addrs(rows, fam))
        for (f, a), (lo, hi) in stock["ranges"].items():
            if f == fam and a in allowed and a not in rng:
                rng[a] = (a, stock["dtypes"].get((f, a), "U16"), lo, hi)
        rels = set()
        for lhs, rhs, op in stock["rels"].get(fam, []):
            if lhs not in allowed or rhs not in allowed:
                continue  # the stock SPA skips a rule whose row is not on the page
            rels.add((lhs, rhs, op))
            rels.add((rhs, lhs, OP_FLIP[op]))
        out[fam] = ([rng[a] for a in sorted(rng)], sorted(rels))
    return out


def gen_rules_h(rows, stock):
    tabs = rules_tables(rows, stock)
    h = [
        "// " + GEN,
        "#pragma once",
        "// Installer value rules per XML family. InstallerRange: the raw range of a register (first",
        "// word of a U32, whose value is lo | hi << 16; S16 compared signed), from the stock getParam",
        "// table, else the envelope of the stock XML values (and enum options), else the dtype.",
        "// InstallerRel: `lhs op rhs` between two registers' values (same dtype and scale on both",
        "// sides). Every relation is listed under both registers, normalised so lhs is the register",
        "// looked up; relations with a register outside the family's allow-list are left out (the",
        "// stock SPA skips rules whose row it does not show). Header-only constexpr data.",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        '#include "../sungrow_modbus/discovery.h"',
        "",
        "namespace esphome {",
        "namespace sungrow_control {",
        "",
        "enum class InstallerDtype : uint8_t { U16, S16, U32 };",
        "enum class RelOp : uint8_t { LT, LE, GT, GE, EQ };",
        "",
        "struct InstallerRange {",
        "  uint16_t addr;",
        "  InstallerDtype dtype;",
        "  int64_t raw_min;",
        "  int64_t raw_max;",
        "};",
        "",
        "struct InstallerRel {",
        "  uint16_t lhs;",
        "  uint16_t rhs;",
        "  RelOp op;",
        "};",
        "",
    ]
    for fam in gp.PROFILE_FAMILIES:
        rng, rels = tabs[fam]
        h.append("inline constexpr InstallerRange INSTALLER_RANGES_%s[] = {" % FAM_ENUM[fam])
        for a, dt, lo, hi in rng:
            h.append("    {%d, InstallerDtype::%s, %d, %d}," % (a, dt, lo, hi))
        h.append("};")
        if rels:
            h.append("inline constexpr InstallerRel INSTALLER_RELS_%s[] = {" % FAM_ENUM[fam])
            for lhs, rhs, op in rels:
                h.append("    {%d, %d, RelOp::%s}," % (lhs, rhs, OP_ENUM[op]))
            h.append("};")
    h += [
        "",
        "/// Range of `addr` for family `fam`; nullptr when the register has no generated range.",
        "inline const InstallerRange *installer_range(sungrow_modbus::XmlFamily fam, uint16_t addr) {",
        "  const InstallerRange *a = nullptr;",
        "  size_t n = 0;",
        "  switch (fam) {",
    ]
    for fam in gp.PROFILE_FAMILIES:
        h += ["    case sungrow_modbus::XmlFamily::%s:" % FAM_ENUM[fam],
              "      a = INSTALLER_RANGES_%s;" % FAM_ENUM[fam],
              "      n = sizeof(INSTALLER_RANGES_%s) / sizeof(InstallerRange);" % FAM_ENUM[fam],
              "      break;"]
    h += [
        "    default:",
        "      return nullptr;",
        "  }",
        "  size_t lo = 0, hi = n;",
        "  while (lo < hi) {",
        "    size_t mid = (lo + hi) / 2;",
        "    if (a[mid].addr < addr)",
        "      lo = mid + 1;",
        "    else",
        "      hi = mid;",
        "  }",
        "  return lo < n && a[lo].addr == addr ? &a[lo] : nullptr;",
        "}",
        "",
        "/// Relations of `addr` (lhs == addr) for family `fam`: *out / *n, false when there are none.",
        "inline bool installer_rels(sungrow_modbus::XmlFamily fam, uint16_t addr, const InstallerRel **out,",
        "                           size_t *n) {",
        "  const InstallerRel *a = nullptr;",
        "  size_t cnt = 0;",
        "  switch (fam) {",
    ]
    for fam in gp.PROFILE_FAMILIES:
        if tabs[fam][1]:
            h += ["    case sungrow_modbus::XmlFamily::%s:" % FAM_ENUM[fam],
                  "      a = INSTALLER_RELS_%s;" % FAM_ENUM[fam],
                  "      cnt = sizeof(INSTALLER_RELS_%s) / sizeof(InstallerRel);" % FAM_ENUM[fam],
                  "      break;"]
    h += [
        "    default:",
        "      break;",
        "  }",
        "  size_t lo = 0, hi = cnt;",
        "  while (lo < hi) {",
        "    size_t mid = (lo + hi) / 2;",
        "    if (a[mid].lhs < addr)",
        "      lo = mid + 1;",
        "    else",
        "      hi = mid;",
        "  }",
        "  size_t end = lo;",
        "  while (end < cnt && a[end].lhs == addr)",
        "    end++;",
        "  *out = end > lo ? &a[lo] : nullptr;",
        "  *n = end - lo;",
        "  return end > lo;",
        "}",
        "",
        "}  // namespace sungrow_control",
        "}  // namespace esphome",
        "",
    ]
    return "\n".join(h)


def outputs():
    rows = load()
    stock = stock_rules()
    return rows, {
        os.path.join(WEB_DIR, "installer_settings.gen.ts"): gen_ts(rows),
        os.path.join(CTL_DIR, "installer_allow.gen.h"): gen_allow_h(rows),
        os.path.join(CTL_DIR, "installer_rules.gen.h"): gen_rules_h(rows, stock),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    rows, files = outputs()
    stale = []
    for path, text in files.items():
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old != text:
            stale.append(path)
            if not a.check:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
    if a.check and stale:
        print("stale: %s (run tools/gen_installer_settings.py)" % ", ".join(stale))
        return 1
    if not a.check:
        src = {}
        for r in rows:
            src[r["range_source"]] = src.get(r["range_source"], 0) + 1
        print("wrote %d file(s); %d settings (ranges: %s); allow-list %s" % (
            len(stale), len(rows), ", ".join("%s %d" % kv for kv in sorted(src.items())),
            ", ".join("%s %d" % (f, len(allow_addrs(rows, f))) for f in gp.PROFILE_FAMILIES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
