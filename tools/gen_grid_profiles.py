#!/usr/bin/env python3
"""Generate the country / grid-code setup profiles from data/grid_profiles/.

BUILD-TIME ONLY (needs PyYAML, deterministic). Inputs: data/grid_profiles/index.yaml (the
settings families, the descriptor table that selects one from (inverter type, protocol type),
the Setup countries) and data/grid_profiles/<family>.yaml (the profiles with their register
sets, the register width table and the installer catalogue; docs/data_format.md).
tools/gen_installer_settings.py imports this module for the per-family set addresses, the set
values (range envelopes) and the descriptor table.

Profile model (what the dongle does when a profile is applied):
  - The settings family is chosen from (inverter type = holding 30644, protocol type) with the
    descriptor table; the country / grid-code / power-switch registers come from the same table.
  - A profile is one country / standard code. writes_sets: its register sets are applied.
    variant_by_input_5479: the variant is chosen by reading input register 5479 and taking the
    first variant whose input_5479 list contains the value.
  - Register sets are written verbatim in document order. `words` is the word sequence as it
    goes on the wire, so a U32 register is low word first.

Outputs:
  components/sungrow_control/grid_profiles.gen.h / .cpp   run dictionary for setup_profile.cpp:
      GP_WORDS     every distinct run's words (a run that is a sub-sequence of the pool reuses it)
      GP_RUNS      distinct (addr, words) runs, deduplicated across options and families
      GP_RUN_INDEX per-variant run ids in document order
      GP_REF5479   per-variant 3x_5479 values
      GP_VARIANTS  one per profile variant; variants sharing a code are one GpOption
      GP_OPTIONS   per family (XmlFamily numeric order), file order inside
  components/sungrow_web/web/src/setup_profiles.gen.ts    SETUP_OPTIONS, SETUP_COUNTRIES,
      FAMILY_DESCRIPTORS, SET_LABELS for the SPA Setup view.

Usage: python3 tools/gen_grid_profiles.py [--check]   (--check: exit 1 if outputs are stale).
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import datalib  # noqa: E402

ROOT = os.path.dirname(HERE)
PROFILES_DIR = datalib.GRID_PROFILES_DIR
CTL_DIR = os.path.join(ROOT, "components", "sungrow_control")
WEB_DIR = os.path.join(ROOT, "components", "sungrow_web", "web", "src")

INDEX, DOCS = datalib.load_grid_profiles(PROFILES_DIR)

# XmlFamily (components/sungrow_modbus/discovery.h) numeric values, in enum order.
FAMILY_NAMES = list(INDEX["families"])
FAMILY_NUM = {n: i for i, n in enumerate(FAMILY_NAMES)}
# Families that have a profile file (profiles + catalogue).
PROFILE_FAMILIES = [n for n in FAMILY_NAMES if INDEX["families"][n].get("file")]
for _fam in PROFILE_FAMILIES:
    if _fam not in DOCS:
        raise SystemExit("data/grid_profiles/%s.yaml missing (index.yaml lists it with a file)" % _fam)

# Descriptor table: (inv_type, protocol, family, country register, grid-code register (0 = none),
# power-switch register), lookup order.
DESCRIPTORS = [(d["inverter_type"], d["protocol"], d["family"], d["country_register"],
                d["grid_code_register"], d["power_switch_register"]) for d in INDEX["descriptors"]]
for _d in DESCRIPTORS:
    if _d[2] not in FAMILY_NUM:
        raise SystemExit("index.yaml: descriptor names unknown family %r" % _d[2])
# Unit family with W32B5 (virtualPointFlag 0): company and grid type go to these real registers.
UNIT_COMPANY_REG = int(INDEX["unit_company_register"])
UNIT_GRIDTYPE_REG = int(INDEX["unit_grid_type_register"])
SETUP_COUNTRIES = [(int(c["code"]), str(c["label"])) for c in INDEX["setup_countries"]]


def family_descriptor(fam):
    """family name -> (country_reg, grid_reg, power_switch_reg) of its first descriptor, or None."""
    for _, _, f, c, g, p in DESCRIPTORS:
        if f == fam:
            return c, g, p
    return None


def commit_regs(fam):
    """Registers written as the setup commit for a family: country (+ grid code), and for unit
    the real company / grid-type registers. Empty for families without setup."""
    d = family_descriptor(fam)
    if d is None or fam == "none":
        return []
    out = [d[0]] + ([d[1]] if d[1] else [])
    if fam == "unit":
        out += [UNIT_COMPANY_REG, UNIT_GRIDTYPE_REG]
    return sorted(out)


def profile_path(fam):
    return os.path.join(PROFILES_DIR, "%s.yaml" % fam)


# ---- profile data -----------------------------------------------------------------------------

def _sets(entries, where):
    out = []
    for i, s in enumerate(entries or []):
        if not isinstance(s, dict) or set(s) != {"address", "words"} or not s["words"]:
            raise SystemExit("%s sets[%d]: need {address, words}" % (where, i))
        words = tuple(int(w) for w in s["words"])
        if any(not 0 <= w <= 0xFFFF for w in words) or not 1 <= int(s["address"]) <= 0xFFFF:
            raise SystemExit("%s sets[%d]: words / address out of range" % (where, i))
        out.append((int(s["address"]), words))
    return out


def parse_family(fam):
    """-> list of profile dicts (file order): family, code, label, needparam, needreadtype,
    variants [{ref5479, sets [(addr, words)]}]."""
    doc = DOCS[fam]
    out, codes = [], set()
    for i, p in enumerate(doc["profiles"]):
        where = "%s profiles[%d]" % (datalib.rel(profile_path(fam)), i)
        for f in ("code", "label", "writes_sets", "variant_by_input_5479", "variants"):
            if f not in p:
                raise SystemExit("%s: missing %s" % (where, f))
        if p["code"] in codes:
            raise SystemExit("%s: code %d listed twice" % (where, p["code"]))
        codes.add(p["code"])
        g = dict(family=fam, code=int(p["code"]), label=str(p["label"]),
                 needparam=int(bool(p["writes_sets"])),
                 needreadtype=int(bool(p["variant_by_input_5479"])), variants=[])
        for j, v in enumerate(p["variants"]):
            refs = [int(x) for x in (v.get("input_5479") or [])]
            if g["needreadtype"] and not refs:
                raise SystemExit("%s variants[%d]: variant_by_input_5479 needs input_5479 values"
                                 % (where, j))
            g["variants"].append(dict(ref5479=refs, sets=_sets(v.get("sets"), "%s variants[%d]" % (where, j))))
        if not g["needreadtype"] and len(g["variants"]) != 1:
            raise SystemExit("%s: a profile without variant_by_input_5479 has exactly one variant" % where)
        if not g["needparam"]:
            raise SystemExit("%s: writes_sets false is not supported (the dongle would route "
                             "through the grid-type items; setup_profile.cpp "
                             "ResolveStatus::GRID_TYPE_UNSUPPORTED)" % where)
        out.append(g)
    return out


def all_sets(fam):
    """Every register set of the family's profiles and grid-type profiles: [(addr, words)].
    Used for allow-lists and range envelopes."""
    out = []
    for g in parse_family(fam):
        for v in g["variants"]:
            out.extend(v["sets"])
    for i, gt in enumerate(DOCS[fam]["grid_type_profiles"] or []):
        out.extend(_sets(gt.get("sets"), "%s grid_type_profiles[%d]" % (datalib.rel(profile_path(fam)), i)))
    return out


def reg_table(fam):
    """{addr: (type, label)} from the family's register width table."""
    out = {}
    for a, r in (DOCS[fam]["registers"] or {}).items():
        t = str(r.get("type") or "U16").upper()
        if t not in ("U16", "S16", "U32"):
            raise SystemExit("%s registers[%s]: type must be U16 / S16 / U32" % (datalib.rel(profile_path(fam)), a))
        out[int(a)] = (t, str(r.get("label") or ""))
    return out


def load():
    """All setup profiles of every family with a file, families in XmlFamily order."""
    out = []
    for fam in PROFILE_FAMILIES:
        out.extend(parse_family(fam))
    return out


# ---- run dictionary ---------------------------------------------------------------------------

def build_tables(options):
    words_pool, runs, run_ids, run_index, refs, variants, opts = [], [], {}, [], [], [], []

    def word_off(ws):
        n = len(ws)
        for i in range(len(words_pool) - n + 1):
            if tuple(words_pool[i:i + n]) == ws:
                return i
        words_pool.extend(ws)
        return len(words_pool) - n

    for g in sorted(options, key=lambda g: FAMILY_NUM[g["family"]]):  # stable: file order kept
        voff = len(variants)
        for v in g["variants"]:
            roff = len(run_index)
            for a, ws in v["sets"]:
                key = (a, ws)
                if key not in run_ids:
                    run_ids[key] = len(runs)
                    runs.append((a, len(ws), word_off(ws)))
                run_index.append(run_ids[key])
            refoff = len(refs)
            refs.extend(v["ref5479"])
            variants.append((len(v["ref5479"]), refoff, roff, len(v["sets"])))
        opts.append((FAMILY_NUM[g["family"]], g["code"], g["label"], g["needparam"],
                     g["needreadtype"], voff, len(g["variants"])))
    return dict(words=words_pool, runs=runs, run_index=run_index, refs=refs, variants=variants,
                options=opts)


def data_bytes(t):
    """Flash bytes of the .cpp tables on the ESP32 (32-bit pointers, natural alignment)."""
    n = 2 * len(t["words"]) + 6 * len(t["runs"]) + 2 * len(t["run_index"]) + 2 * len(t["refs"])
    n += 8 * len(t["variants"]) + 16 * len(t["options"])
    n += sum(len(s.encode("utf-8")) + 1 for s in {o[2] for o in t["options"]})
    return n


GEN = "GENERATED by tools/gen_grid_profiles.py from data/grid_profiles/*.yaml - DO NOT EDIT."


def c_str(s):
    out = []
    for ch in s:
        if ch in '\\"':
            out.append("\\" + ch)
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ch)
        else:
            out.extend("\\x%02x\"\"" % b for b in ch.encode("utf-8"))
    return '"%s"' % "".join(out)


def wrap(vals, per=16, fmt="%d"):
    return ["    " + ", ".join(fmt % v for v in vals[i:i + per]) + ","
            for i in range(0, len(vals), per)]


def gen_h(t):
    return "\n".join([
        "// " + GEN,
        "#pragma once",
        "// Stock POWER_COMPANY setup profiles as a run dictionary. A GpOption is one company /",
        "// standard value of one family; each XML <option> with that value is a GpVariant",
        "// (needreadtype=1 variants are told apart by their 3x_5479 refs). A variant is an ordered",
        "// list of runs; a run is `nwords` consecutive holding registers from `addr` with the words",
        "// exactly as the XML writeData (U32 = low word first).",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace esphome {",
        "namespace sungrow_control {",
        "",
        "struct GpRun {",
        "  uint16_t addr;      ///< first 1-based holding register",
        "  uint8_t nwords;",
        "  uint16_t word_off;  ///< into GP_WORDS",
        "};",
        "",
        "struct GpVariant {",
        "  uint8_t nref;       ///< 3x_5479 values that select this variant (0 = unconditional)",
        "  uint16_t ref_off;   ///< into GP_REF5479",
        "  uint16_t run_off;   ///< into GP_RUN_INDEX",
        "  uint8_t nruns;",
        "};",
        "",
        "struct GpOption {",
        "  uint8_t family;     ///< sungrow_modbus::XmlFamily numeric value",
        "  uint16_t code;      ///< option value = company / standard code (30036 for unit)",
        "  const char *label;",
        "  uint8_t needparam;",
        "  uint8_t needreadtype;",
        "  uint16_t variant_off;  ///< into GP_VARIANTS",
        "  uint8_t nvariants;",
        "};",
        "",
        "extern const uint16_t GP_WORDS[];",
        "extern const GpRun GP_RUNS[];",
        "extern const uint16_t GP_RUN_INDEX[];",
        "extern const uint16_t GP_REF5479[];",
        "extern const GpVariant GP_VARIANTS[];",
        "extern const GpOption GP_OPTIONS[];  ///< grouped by family (XmlFamily order), XML order inside",
        "inline constexpr size_t GP_N_WORDS = %d;" % len(t["words"]),
        "inline constexpr size_t GP_N_RUNS = %d;" % len(t["runs"]),
        "inline constexpr size_t GP_N_RUN_INDEX = %d;" % len(t["run_index"]),
        "inline constexpr size_t GP_N_REF5479 = %d;" % len(t["refs"]),
        "inline constexpr size_t GP_N_VARIANTS = %d;" % len(t["variants"]),
        "inline constexpr size_t GP_N_OPTIONS = %d;" % len(t["options"]),
        "/// Unit family with W32B5: the real company / grid-type registers.",
        "inline constexpr uint16_t GP_UNIT_COMPANY_REG = %d;" % UNIT_COMPANY_REG,
        "inline constexpr uint16_t GP_UNIT_GRIDTYPE_REG = %d;" % UNIT_GRIDTYPE_REG,
        "",
        "}  // namespace sungrow_control",
        "}  // namespace esphome",
        "",
    ])


def gen_cpp(t):
    c = ["// " + GEN, '#include "grid_profiles.gen.h"', "", "namespace esphome {",
         "namespace sungrow_control {", ""]
    c.append("const uint16_t GP_WORDS[] = {")
    c += wrap(t["words"], 12, "0x%04X")
    c += ["};", "", "const GpRun GP_RUNS[] = {"]
    for i, (a, n, off) in enumerate(t["runs"]):
        c.append("    {%d, %d, %d},  // %d" % (a, n, off, i))
    c += ["};", "", "const uint16_t GP_RUN_INDEX[] = {"]
    c += wrap(t["run_index"], 20)
    c += ["};", "", "const uint16_t GP_REF5479[] = {"]
    c += wrap(t["refs"], 20) if t["refs"] else ["    0,  // unused"]
    c += ["};", "", "const GpVariant GP_VARIANTS[] = {"]
    for i, v in enumerate(t["variants"]):
        c.append("    {%d, %d, %d, %d},  // %d" % (v + (i,)))
    c += ["};", "", "const GpOption GP_OPTIONS[] = {"]
    for fam, code, lab, np_, nrt, voff, nv in t["options"]:
        c.append("    {%d, %d, %s, %d, %d, %d, %d},  // %s" % (
            fam, code, c_str(lab), np_, nrt, voff, nv, FAMILY_NAMES[fam]))
    c += ["};", "", "}  // namespace sungrow_control", "}  // namespace esphome", ""]
    return "\n".join(c)


def ts_str(s):
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def set_labels():
    """'<family>:<addr>' -> label for every set register that is not the `address` of an
    Installer-catalogue row of that family: the high word of a U32 row, else an English catalogue
    label of another family, else the register table's label, else the address."""
    import gen_installer_settings as gi  # imports this module; resolved at call time
    rows = gi.load()
    by_fam_addr = {(r["family"], r["address"]): r for r in rows}
    any_addr = {}
    for r in rows:
        any_addr.setdefault(r["address"], r["label"])
    out = {}
    for fam in PROFILE_FAMILIES:
        regs = reg_table(fam)
        for a, ws in all_sets(fam):
            for i in range(len(ws)):
                addr = a + i
                if (fam, addr) in by_fam_addr:
                    continue
                prev = by_fam_addr.get((fam, addr - 1))
                if prev is not None and prev["dtype"] == "U32":
                    text = "%s (high word)" % prev["label"]
                elif addr in any_addr:
                    text = any_addr[addr]
                elif regs.get(addr, ("", ""))[1]:
                    text = regs[addr][1]
                else:
                    text = str(addr)
                out.setdefault("%s:%d" % (fam, addr), text)
    return dict(sorted(out.items(), key=lambda kv: (kv[0].split(":")[0], int(kv[0].split(":")[1]))))


def gen_ts(options):
    t = [
        "// " + GEN,
        "// Setup view data: the stock POWER_COMPANY options per XML family (variants by 3x_5479),",
        "// the countries Setup offers, the stock descriptor registers per family, and labels for",
        "// profile registers that are not the address of an Installer-catalogue row. The server",
        "// resolves and applies profiles (setup_profile.cpp); this file drives the picker and preview.",
        "export type XmlFamilyName = %s;" % " | ".join(ts_str(n) for n in FAMILY_NAMES),
        "export interface SetupVariant {",
        "  ref5479: number[];  // input 5479 values selecting this variant ([] = unconditional)",
        "  nsets: number;      // <set> rows",
        "  nregs: number;      // registers written by the sets",
        "}",
        "export interface SetupOption {",
        "  family: XmlFamilyName;",
        "  code: number;           // company / standard code (written to 30036 on unit + W32B5)",
        "  label: string;",
        "  needparam: number;      // stock attribute (0|1)",
        "  needreadtype: number;   // 1 = variant chosen by a live read of input register 5479",
        "  variants: SetupVariant[];",
        "}",
        "export const SETUP_OPTIONS: SetupOption[] = [",
    ]
    for g in options:
        vs = ", ".join("{ ref5479: [%s], nsets: %d, nregs: %d }" % (
            ", ".join(str(r) for r in v["ref5479"]), len(v["sets"]), sum(len(w) for _, w in v["sets"]))
            for v in g["variants"])
        t.append("  { family: %s, code: %d, label: %s, needparam: %d, needreadtype: %d, variants: [%s] }," % (
            ts_str(g["family"]), g["code"], ts_str(g["label"]), g["needparam"], g["needreadtype"], vs))
    t += ["];", "",
          "export const SETUP_COUNTRIES: { code: number; label: string }[] = [%s];" % ", ".join(
              "{ code: %d, label: %s }" % (c, ts_str(k)) for c, k in SETUP_COUNTRIES),
          "",
          "// Stock descriptor registers per family (first (inverter type, protocol) entry of the family).",
          "// grid: null = the family has no grid-code register.",
          "export const FAMILY_DESCRIPTORS: Record<string, { country: number; grid: number | null; "
          "powerSwitch: number }> = {"]
    seen = set()
    for _, _, fam, c, g, p in DESCRIPTORS:
        if fam in seen:
            continue
        seen.add(fam)
        t.append("  %s: { country: %d, grid: %s, powerSwitch: %d }," % (fam, c, str(g) if g else "null", p))
    t += ["};", "", "export const SET_LABELS: Record<string, string> = {"]
    for k, v in set_labels().items():
        t.append("  %s: %s," % (ts_str(k), ts_str(v)))
    t += ["};", ""]
    return "\n".join(t)


def outputs():
    options = load()
    tables = build_tables(options)
    return tables, {
        os.path.join(CTL_DIR, "grid_profiles.gen.h"): gen_h(tables),
        os.path.join(CTL_DIR, "grid_profiles.gen.cpp"): gen_cpp(tables),
        os.path.join(WEB_DIR, "setup_profiles.gen.ts"): gen_ts(options),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    tables, files = outputs()
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
        print("stale: %s (run tools/gen_grid_profiles.py)" % ", ".join(stale))
        return 1
    if not a.check:
        print("wrote %d file(s); %d options, %d variants, %d runs, %d words; .cpp data %d bytes" % (
            len(stale), len(tables["options"]), len(tables["variants"]), len(tables["runs"]),
            len(tables["words"]), data_bytes(tables)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
