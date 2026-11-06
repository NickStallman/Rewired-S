#!/usr/bin/env python3
"""Generate the power-stage acceptance ranges per inverter model.

BUILD-TIME ONLY (needs PyYAML, deterministic).

Inputs: data/dsp_params/models.yaml, one row per power-stage firmware line whose ranges are known
(key, name, family, firmware, csv, models, dev_codes), and the CSV each row names
(doc_reg,count,min,max,label,unit; count 2 = a U32 value with its low word at doc_reg). Rows of
one CSV that repeat a register are merged into the union of their ranges. docs/data_format.md
describes both.

Output: components/sungrow_control/dsp_ranges.gen.h: one address-sorted DspRange array per line,
the DspModel table sorted by key, dsp_model(dev_code), dsp_model_by_name(model),
dsp_model_for(dev_code, model) and dsp_range(model, addr). ControlCore::setup_apply uses them to
refuse (Installer) or skip (Setup profile) a value the power stage would ignore; a device whose
code and model string are both unknown to the manifest gets no range check.

Usage: python3 tools/gen_dsp_ranges.py [--check]   (--check: exit 1 if the header is stale).
"""

import argparse
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import datalib  # noqa: E402

ROOT = os.path.dirname(HERE)
DATA_DIR = os.path.join(ROOT, "data", "dsp_params")
MANIFEST = os.path.join(DATA_DIR, "models.yaml")
OUT_H = os.path.join(ROOT, "components", "sungrow_control", "dsp_ranges.gen.h")

FIELDS = ("key", "name", "family", "firmware", "csv", "models", "dev_codes")
REQUIRED = ("key", "name", "family", "firmware", "csv", "models")
MODEL_RE = r"^[A-Z0-9][A-Z0-9._-]{0,15}$"  # fits DevInfo::model (16 chars)


def load_manifest(path=MANIFEST):
    """[{key, name, family, firmware, csv, models: [str], dev_codes: {model: int}}] sorted by key."""
    doc = datalib.load_yaml(path)
    name = os.path.basename(path)
    if not isinstance(doc, dict) or not isinstance(doc.get("models"), list):
        raise SystemExit("%s: expected a 'models:' list" % name)
    rows = []
    for i, r in enumerate(doc["models"]):
        if not isinstance(r, dict):
            raise SystemExit("%s: models[%d] is not a mapping" % (name, i))
        unknown = sorted(set(r) - set(FIELDS))
        if unknown:
            raise SystemExit("%s: models[%d]: unknown field(s) %s" % (name, i, unknown))
        missing = [k for k in REQUIRED if k not in r or r[k] in ("", None, [])]
        if missing:
            raise SystemExit("%s: line %s lacks %s" % (name, r.get("key", "?"), missing))
        row = {k: str(r[k]) for k in ("key", "name", "family", "firmware", "csv")}
        if not row["key"].replace("_", "").isalnum() or row["key"][0].isdigit():
            raise SystemExit("%s: key %r is not an identifier" % (name, row["key"]))
        if row["family"] not in ("hybrid", "string"):
            raise SystemExit("%s: %s: family must be hybrid or string" % (name, row["key"]))
        models = r["models"]
        if not isinstance(models, list) or not all(isinstance(m, str) for m in models):
            raise SystemExit("%s: %s: models must be a list of strings" % (name, row["key"]))
        for m in models:
            if not re.match(MODEL_RE, m):
                raise SystemExit("%s: %s: model %r is not a devinfo model string" % (name, row["key"], m))
        if len(set(models)) != len(models):
            raise SystemExit("%s: %s: duplicate model" % (name, row["key"]))
        row["models"] = list(models)
        codes = r.get("dev_codes") or {}
        if not isinstance(codes, dict):
            raise SystemExit("%s: %s: dev_codes must be a mapping model -> code" % (name, row["key"]))
        row["dev_codes"] = {}
        for m, c in codes.items():
            if m not in models:
                raise SystemExit("%s: %s: dev_codes names %r, not in models" % (name, row["key"], m))
            if isinstance(c, bool) or not isinstance(c, int) or not 1 <= c <= 0xFFFF:
                raise SystemExit("%s: %s: dev_code %r for %s out of range" % (name, row["key"], c, m))
            row["dev_codes"][m] = c
        rows.append(row)
    keys = [r["key"] for r in rows]
    if len(set(keys)) != len(keys):
        raise SystemExit("%s: duplicate key" % name)
    seen = {}
    for r in rows:
        for m in r["models"]:
            if m.upper() in seen:
                raise SystemExit("%s: model %s listed under %s and %s" % (name, m, seen[m.upper()], r["key"]))
            seen[m.upper()] = r["key"]
    codes = [c for r in rows for c in r["dev_codes"].values()]
    if len(set(codes)) != len(codes):
        raise SystemExit("%s: duplicate dev_code" % name)
    rows.sort(key=lambda r: r["key"])
    return rows


def load_ranges(path):
    """{doc_reg: (count, min, max)} of one CSV, repeated registers merged (union)."""
    out = {}
    with open(path, encoding="utf-8", newline="") as f:
        rd = csv.DictReader(f)
        for n, row in enumerate(rd, 2):
            where = "%s:%d" % (os.path.basename(path), n)
            try:
                addr, count, lo, hi = (int(row[k], 0) for k in ("doc_reg", "count", "min", "max"))
            except (KeyError, TypeError, ValueError):
                raise SystemExit("%s: need integer doc_reg,count,min,max" % where)
            if not 1 <= addr <= 0xFFFF or count not in (1, 2):
                raise SystemExit("%s: doc_reg %d / count %d out of range" % (where, addr, count))
            limit = 0xFFFF if count == 1 else 0xFFFFFFFF
            if not 0 <= lo <= hi <= limit:
                raise SystemExit("%s: range %d..%d invalid for count %d" % (where, lo, hi, count))
            if addr in out:
                c0, lo0, hi0 = out[addr]
                if c0 != count:
                    raise SystemExit("%s: register %d listed with counts %d and %d" % (where, addr, c0, count))
                out[addr] = (count, min(lo0, lo), max(hi0, hi))
            else:
                out[addr] = (count, lo, hi)
    return out


def load(manifest=MANIFEST):
    """[(line row, {addr: (count, min, max)})] sorted by key."""
    models = load_manifest(manifest)
    data_dir = os.path.dirname(manifest)
    out = []
    for m in models:
        out.append((m, load_ranges(os.path.join(data_dir, m["csv"]))))
    out.sort(key=lambda e: e[0]["key"])
    return out


def gen_h(models):
    h = [
        "// GENERATED by tools/gen_dsp_ranges.py from data/dsp_params/models.yaml - DO NOT EDIT.",
        "#pragma once",
        "// Power-stage acceptance ranges per inverter firmware line (data/dsp_params/<line>.csv). The",
        "// power stage ignores a parameter write outside its range while the control board",
        "// acknowledges it, so ControlCore refuses (Installer) or skips (Setup profile) such a value",
        "// on a listed model. A device is matched on the device code of its devinfo block first, then",
        "// on its model string; a model known to neither has no table and no range check. count 2: a",
        "// U32 value, low word at addr, compared as one 32-bit number. Header-only constexpr data.",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace esphome {",
        "namespace sungrow_control {",
        "",
        "struct DspRange {",
        "  uint16_t addr;   ///< 1-based document holding register (first word of a U32)",
        "  uint8_t count;   ///< 1 (U16) or 2 (U32)",
        "  uint32_t min;",
        "  uint32_t max;",
        "};",
        "",
        "struct DspModel {",
        "  const char *key;",
        "  const char *name;      ///< the firmware line (SH-RS, SG-RS, SH-T, ...)",
        "  const char *firmware;  ///< power-stage firmware the ranges apply to",
        "  const char *const *models;  ///< devinfo model strings of the line",
        "  size_t model_count;",
        "  const uint16_t *dev_codes;  ///< devinfo device codes seen on hardware",
        "  size_t dev_code_count;",
        "  const DspRange *ranges;  ///< sorted by addr",
        "  size_t count;",
        "};",
        "",
    ]
    for m, ranges in models:
        h.append("// %s (%s): %d models, %d registers" % (m["name"], m["firmware"], len(m["models"]), len(ranges)))
        h.append("inline constexpr const char *const DSP_MODELS_%s[] = {%s};"
                 % (m["key"], ", ".join('"%s"' % x for x in m["models"])))
        codes = sorted(m["dev_codes"].values())
        h.append("inline constexpr uint16_t DSP_CODES_%s[] = {%s};"
                 % (m["key"], ", ".join(str(c) for c in codes) if codes else "0"))
        h.append("inline constexpr DspRange DSP_RANGES_%s[] = {" % m["key"])
        for addr in sorted(ranges):
            count, lo, hi = ranges[addr]
            h.append("    {%d, %d, %d, %d}," % (addr, count, lo, hi))
        h.append("};")
        h.append("")
    h.append("inline constexpr DspModel DSP_MODELS[] = {")
    for m, _ in models:
        n_codes = len(m["dev_codes"])
        h.append('    {"%s", "%s", "%s", DSP_MODELS_%s, %d, DSP_CODES_%s, %d, DSP_RANGES_%s, sizeof(DSP_RANGES_%s) / sizeof(DspRange)},'
                 % (m["key"], m["name"], m["firmware"], m["key"], len(m["models"]), m["key"], n_codes, m["key"], m["key"]))
    h += [
        "};",
        "",
        "/// The line with a model whose devinfo device code is `dev_code`, nullptr when unknown.",
        "inline const DspModel *dsp_model(uint16_t dev_code) {",
        "  if (dev_code == 0)",
        "    return nullptr;",
        "  for (const DspModel &m : DSP_MODELS)",
        "    for (size_t i = 0; i < m.dev_code_count; i++)",
        "      if (m.dev_codes[i] == dev_code)",
        "        return &m;",
        "  return nullptr;",
        "}",
        "",
        "/// ASCII case-insensitive equality of two model strings (trailing spaces ignored).",
        "inline bool dsp_model_name_eq(const char *a, const char *b) {",
        "  auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };",
        "  while (*a != '\\0' && *b != '\\0') {",
        "    if (up(*a) != up(*b))",
        "      return false;",
        "    a++;",
        "    b++;",
        "  }",
        "  while (*a == ' ')",
        "    a++;",
        "  while (*b == ' ')",
        "    b++;",
        "  return *a == '\\0' && *b == '\\0';",
        "}",
        "",
        "/// The line listing the devinfo model string `model`, nullptr when unknown or empty.",
        "inline const DspModel *dsp_model_by_name(const char *model) {",
        "  if (model == nullptr || *model == '\\0')",
        "    return nullptr;",
        "  for (const DspModel &m : DSP_MODELS)",
        "    for (size_t i = 0; i < m.model_count; i++)",
        "      if (dsp_model_name_eq(m.models[i], model))",
        "        return &m;",
        "  return nullptr;",
        "}",
        "",
        "/// The line for a device: by its device code, else by its model string.",
        "inline const DspModel *dsp_model_for(uint16_t dev_code, const char *model) {",
        "  const DspModel *m = dsp_model(dev_code);",
        "  return m != nullptr ? m : dsp_model_by_name(model);",
        "}",
        "",
        "/// Range of `addr` (first word of a U32) in `m`, nullptr when unlisted or `m` is nullptr.",
        "inline const DspRange *dsp_range(const DspModel *m, uint16_t addr) {",
        "  if (m == nullptr)",
        "    return nullptr;",
        "  size_t lo = 0, hi = m->count;",
        "  while (lo < hi) {",
        "    const size_t mid = (lo + hi) / 2;",
        "    if (m->ranges[mid].addr < addr)",
        "      lo = mid + 1;",
        "    else",
        "      hi = mid;",
        "  }",
        "  return lo < m->count && m->ranges[lo].addr == addr ? &m->ranges[lo] : nullptr;",
        "}",
        "",
        "}  // namespace sungrow_control",
        "}  // namespace esphome",
        "",
    ]
    return "\n".join(h)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    models = load()
    text = gen_h(models)
    old = open(OUT_H, encoding="utf-8").read() if os.path.exists(OUT_H) else None
    if a.check:
        if old != text:
            print("stale: %s (run tools/gen_dsp_ranges.py)" % OUT_H)
            return 1
        return 0
    if old != text:
        with open(OUT_H, "w", encoding="utf-8") as f:
            f.write(text)
    print("wrote %s: %s" % (os.path.relpath(OUT_H, ROOT),
                            ", ".join("%s %d" % (m["name"], len(r)) for m, r in models)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
