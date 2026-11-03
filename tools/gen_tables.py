#!/usr/bin/env python3
"""Generate sungrow_datamodel register tables + ESPHome device packages.

Inputs : data/families/<family>.yaml   register tables (poll order, cycle, flags, capability
                                       gates, bitfield words) and the signal definitions
                                       (keys, names, units, HA classes, categories, evidence),
                                       address fixes and the non-polled protection reads
         data/modbus_tcp/<map>.yaml    the Modbus-TCP (:502) export maps
         (docs/data_format.md documents both; tools/datalib.py loads them)
Outputs: components/sungrow_datamodel/register_tables.h    C++ enums / constants / lookups
         components/sungrow_datamodel/register_tables.cpp  constexpr row arrays (flash/.rodata)
         components/sungrow_datamodel/register_meta.py     codegen-time key -> sigid metadata
         devices/*.yaml                                    ESPHome entity packages

The generated C++ files live at the top level of the component directory on purpose:
ESPHome only copies top-level source files of an external component.

Needs PyYAML (tools/requirements.txt). Output is fully deterministic (sorted inputs, no
timestamps), which tests/test_gen_deterministic.py checks.

Row fields (see data/families/*.yaml): id (signal id, family-scoped), address (1-based, wire =
address - 1), count, fn, type (s16/u16/s32/u32/s64/u64/f32/str), cycle (poll cycle, 100 ms
units; the request-node key), flags (end / clear_offline / priority / ...), bits (status word
expansion), capability (word.bit gate into signal 0x2181); unit / scale / name come from the
signal definition of the id.
"""

import argparse
import math
import os
import re
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import datalib  # noqa: E402
from datalib import (CATEGORIES, CONFIDENCES, FAMILIES, GROUPS, TYPE_CODES,  # noqa: E402,F401
                     TYPE_NAMES, DataError)

FW_ROOT = os.path.dirname(HERE)
DEFAULT_DATA_DIR = os.path.join(FW_ROOT, "data")

# Priority order for picking the canonical metadata of a (family, sigid): poll tables first,
# the Modbus-TCP export maps last.
TABLE_PRIORITY = ["inv_proto2", "inv_proto3", "inv_proto1_4", "inv_proto5", "battery",
                  "charger", "energy_bridge", "other_dev18_st6100", "other_dev18_st6200",
                  "other_dev18_st6300", "other_dev34_st1819", "other_dev34_st1820",
                  "tcpslave_dev35", "tcpslave_dev1_21", "tcpslave_dev44", "tcpslave_dev18"]

# ---------------------------------------------------------------------------------------
# Signal definitions (data/families/*.yaml `signals`): key/name/unit/scale/HA classes per
# (family, sigid), display grouping and the not-served rules. Validation of the fields the
# loader does not type-check:
# Display grouping for the SPA / MQTT (register_meta.py INFO + meta.gen.ts MetaTuple):
#   realtime   live measurements (default)
#   static     device facts (serial/model/firmware/ratings) - rarely change
#   protection grid-code / protection holding registers (read-only here; see `reads`)
DEFAULT_PRIORITY = 9999  # sort-rank when a signal has no curated `priority` (sorts last)
# `absent`: space-separated conditions under which a polled value is "not served" and ingest stores
# the signal as absent (on top of the per-type sentinels). Tokens:
#   <integer>     the raw register value (masked to the row width, e.g. 0xFFFF on an s16 row)
#   single_phase  the device's output type (sig 0x2003) reads 0 = single-phase (phase B/C rows)
#   no_meter      no meter active power is valid (sig 0x2182 / 0x2540): the meter_* rows
ABSENT_KINDS = {"raw": 0, "single_phase": 1, "no_meter": 2}
# `reads` (inverter.yaml): a holding register that is NOT polled (so it has no table row),
#   surfaced as metadata only (register_meta.py READS + meta.gen.ts PROTECTION_READS, keyed by
#   its register address; the SPA reads the value through /api/grid_read). It never enters the
#   C++ register tables / request builder, so the poll set is unchanged. `dtype`: u16 | s16.



def _check_group_priority_enum(e, where):
    """Validate the optional display scalars (`group`, `priority`, `enum`)."""
    if e.get("group", "realtime") not in GROUPS:
        raise SystemExit("%s: group must be one of %s" % (where, GROUPS))
    if "enum" in e and e["enum"] not in ENUMS:
        raise SystemExit("%s: enum %r is not a defined ENUM table %s"
                         % (where, e["enum"], tuple(sorted(ENUMS))))


def _parse_absent(e, where):
    """`absent` scalar -> [(kind, raw)] (see ABSENT_KINDS); [] when the field is not given."""
    out = []
    for tok in e.get("absent", "").split():
        if tok in ABSENT_KINDS:
            out.append((ABSENT_KINDS[tok], 0))
            continue
        try:
            raw = int(tok, 0)
        except ValueError:
            raise SystemExit("%s: absent token %r is not an integer or one of %s"
                             % (where, tok, tuple(k for k in ABSENT_KINDS if k != "raw")))
        if not 0 <= raw <= 0xFFFFFFFF:
            raise SystemExit("%s: absent raw value %r out of range" % (where, tok))
        out.append((ABSENT_KINDS["raw"], raw))
    if "absent" in e and not out:
        raise SystemExit("%s: absent is empty" % where)
    return out


def load_overrides(data):
    """From datalib.load_register_data() -> dict(names={(family, sigid): entry}, fixes=[...],
    shared={(table, fn, regaddr): e}, reads=[entry]). `reads` are non-polled holding registers
    surfaced as metadata only."""
    names = {}
    for family in sorted(data["signals"]):
        for e in data["signals"][family]:
            where = e["_where"]
            _check_group_priority_enum(e, where)
            e["absent_rules"] = _parse_absent(e, where)
            names[(family, e["sigid"])] = e
    for e in data["reads"]:
        _check_group_priority_enum(e, e["_where"])
    return {"names": names, "fixes": data["fixes"], "shared": data["shared"],
            "reads": data["reads"]}


def apply_overrides(tables, ov, issues):
    """Check the address fixes, the shared-address allowances and attach the signal
    definitions to the rows. Unmatched entries are fatal."""
    tmap = {t["name"]: t for t in tables}
    for e in ov["fixes"]:
        t = tmap.get(e["table"])
        hits = [r for r in (t["rows"] if t else [])
                if r["sigid"] == e["sigid"] and r["regaddr"] == e["new_regaddr"]]
        stale = [r for r in (t["rows"] if t else [])
                 if r["sigid"] == e["sigid"] and r["regaddr"] == e["regaddr"]]
        if len(hits) != 1 or stale:
            raise SystemExit("%s: fix %s 0x%04x %d -> %d: the table needs exactly one row with "
                             "the fixed address and none at the stock address (found %d / %d)"
                             % (e["_where"], e["table"], e["sigid"], e["regaddr"],
                                e["new_regaddr"], len(hits), len(stale)))
        hits[0]["regaddr_stock"] = e["regaddr"]
        issues.append("%s: 0x%04x moved %d -> %d (fix, %s)" % (
            e["table"], e["sigid"], e["regaddr"], e["new_regaddr"], e["confidence"]))
    for (tn, fn, addr), e in sorted(ov["shared"].items()):
        t = tmap.get(tn)
        hits = [r for r in (t["rows"] if t else [])
                if r["fn"] == fn and r["regaddr"] == addr and r["sigid"] == e["sigid"]]
        if len(hits) < 2:
            raise SystemExit("%s: shared %s fn%d@%d 0x%04x does not match a shared address"
                             % (e["_where"], tn, fn, addr, e["sigid"]))
    seen = set()
    for t in tables:
        for r in t["rows"]:
            e = ov["names"].get((t["family"], r["sigid"])) if r["sigid"] else None
            if e is None:
                continue
            seen.add((t["family"], r["sigid"]))
            for kind, raw in e["absent_rules"]:
                if r["type"] in (9, 12) or (kind == ABSENT_KINDS["raw"]
                                            and raw >> (16 * min(r["nreg"], 2))):
                    raise SystemExit("%s: absent %r does not fit %s row %d (%s x%d)"
                                     % (e["_where"], e["absent"], t["name"], r["regaddr"],
                                        TYPE_NAMES[r["type"]], r["nreg"]))
            r.update(unit=e["unit"], scale=e["scale"], name=e["name"], ov=e, source="signal")
    for fk, e in sorted(ov["names"].items()):
        if fk not in seen:
            raise SystemExit("%s: %s 0x%04x matches no table row" % (e["_where"], fk[0], fk[1]))


def is_named(r):
    return bool(r["sigid"]) and bool(r.get("ov"))

# ---------------------------------------------------------------------------------------
# Derived / synthetic signals. Stored in the SignalStore like any other sigid, in a range
# (0xF000+) no register table uses (max real sigid is 0x5858).
DERIVED = [
    # (sigid, key, name, unit, decimals, family[, type, nreg, scale])
    # 0x2021 is the cloud's total-yield signal (u64, 0.1 kWh); no inverter poll table carries
    # it, so it is derived from 0x2023 / 0x2022 / 0x2024 like the stock dongle does (derived.cpp).
    (0x2021, "total_yield", "Total yield", "kWh", 1, "inverter", 8, 4, 0.1),
    (0xF011, "grid_import_power", "Grid import power", "kW", 3, "inverter"),
    (0xF012, "grid_export_power", "Grid export power", "kW", 3, "inverter"),
    (0xF001, "cell_voltage_max_module", "Cell voltage max module", "", 0, "battery"),
    (0xF002, "cell_voltage_max_cell", "Cell voltage max cell", "", 0, "battery"),
    (0xF003, "cell_voltage_min_module", "Cell voltage min module", "", 0, "battery"),
    (0xF004, "cell_voltage_min_cell", "Cell voltage min cell", "", 0, "battery"),
    (0xF005, "module_temperature_max_module", "Module temperature max module", "", 0, "battery"),
    (0xF006, "module_temperature_min_module", "Module temperature min module", "", 0, "battery"),
    (0xF007, "cell_voltage_spread", "Cell voltage spread", "V", 4, "battery"),
    (0xF010, "mppt_total_power", "MPPT total power", "W", 0, "inverter"),
    # Fault list block 5448..5472 (sig 0x2156, stock types it str25 and never shows it):
    # count at 5452, then 10 (code, class) pairs; class 25 = fault, 24 = alarm (derived.cpp).
    (0xF060, "active_fault_count", "Active faults", "", 0, "battery", 4, 1, 0.0),
    (0xF061, "active_fault_codes", "Active fault codes", "", 0, "battery", 12, 32, 0.0),
    (0xF060, "active_fault_count", "Active faults", "", 0, "inverter", 4, 1, 0.0),
    (0xF061, "active_fault_codes", "Active fault codes", "", 0, "inverter", 12, 32, 0.0),
    # First entry of the fault list as "<code> <name>" ("No active faults" when empty), the
    # headline fault for dashboards (derived.cpp decode_fault_list).
    (0xF065, "first_fault", "First active fault", "", 0, "battery", 12, 32, 0.0),
    (0xF065, "first_fault", "First active fault", "", 0, "inverter", 12, 32, 0.0),
    # 1 while the running state (sig 0x2086) is one of TRANSIENT_STATES for the inverter's model
    # family (I-V scan, rapid shutdown, restart, fault recovery, ...), else 0 (derived.cpp
    # update_state_flags).
    (0xF066, "state_transient", "Transient state", "", 0, "inverter", 4, 1, 0.0),
    # Power-flow flags 6099 (sig 0x2556) as text, e.g. "PV generating, Battery charging"
    # (derived.cpp update_power_flow; bit meanings: mkaiser 13001 "Power Flow Status").
    (0xF080, "power_flow", "Power flow", "", 0, "inverter", 12, 64, 0.0),
    # SBR BMS request flags 10666 (sig 0x2071) bits 0..2
    # (https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sbr192/registers-via-inverter.md).
    (0xF062, "charge_prohibited", "Charging not allowed", "", 0, "battery", 4, 1, 0.0),
    (0xF063, "discharge_prohibited", "Discharging not allowed", "", 0, "battery", 4, 1, 0.0),
    (0xF064, "forced_charge_request", "Forced charge request", "", 0, "battery", 4, 1, 0.0),
] + [
    # SBR module manufacturing dates "YYYY-MM-DD", decoded from the module date codes 10791+
    # (sigids 0x205E + m, YY*10^6 + M*10^4 + DD*100 + S/N character; derived.cpp).
    (0xF070 + m, "module_%d_manufacturing_date" % (m + 1), "Module %d manufacturing date" % (m + 1),
     "", 0, "battery", 12, 5, 0.0) for m in range(8)
]
# Per-MPPT power V x I: SIG_MPPT_POWER_BASE + mppt (one per kMpptPairs entry, added in build()).
MPPT_POWER_BASE = 0xF020
DERIVED_SIG_NAMES = {0x2021: "TOTAL_YIELD"}  # SIG_ constant names that differ from the key

# Curated display priority/group for derived signals (the `priority`/`group` fields of
# data/families/*.yaml only reach table-backed sigids). Keyed by derived sigid; absent =
# DEFAULT_PRIORITY / realtime. 0xF021..0xF024 = SIG_MPPT_POWER_BASE + 1..4 (MPPT1..4 power).
DERIVED_META = {
    0x2021: {"priority": 18},             # total_yield (headline energy)
    MPPT_POWER_BASE + 1: {"priority": 42},  # mppt1_power
    MPPT_POWER_BASE + 2: {"priority": 45},  # mppt2_power
    MPPT_POWER_BASE + 3: {"priority": 48},  # mppt3_power (4-MPPT hybrids, e.g. SH10RS)
    MPPT_POWER_BASE + 4: {"priority": 51},  # mppt4_power
    0xF010: {"priority": 22},             # mppt_total_power (= total DC power on string)
    0xF007: {"priority": 80},             # cell_voltage_spread (SBR: max - min cell voltage)
    0xF060: {"priority": 76},             # active_fault_count
    0xF061: {"priority": 77},             # active_fault_codes
    0xF080: {"priority": 15},             # power_flow (hybrid power-flow flags as text)
    0xF062: {"priority": 78, "enum": "no_yes"},  # charge_prohibited
    0xF063: {"priority": 79, "enum": "no_yes"},  # discharge_prohibited
    0xF064: {"enum": "no_yes"},           # forced_charge_request
    0xF065: {"priority": 75},             # first_fault (headline fault, before the count/list)
    0xF066: {"enum": "no_yes"},           # state_transient
}
DERIVED_META.update({0xF070 + m: {"group": "static"} for m in range(8)})  # module dates

# SBR cell block (battery table rows 10757..10780): its signals are defined in
# data/families/battery.yaml without a category and placed in the sbr_battery package by
# DEVICES["sbr_battery"]["derived"]. The C++ side exposes the block as kSbrCellSigids.
SBR_BASE = 10757

# ---------------------------------------------------------------------------------------
# Home Assistant conventions (winet-extractor src/types/HaTypes.ts + homeassistant.ts)
UNIT_NORMALISE = {"℃": ("°C", 1.0), "kvar": ("var", 1000.0), "kVA": ("VA", 1000.0),
                  "kWp": ("kW", 1.0)}
VALID_DEVICE_CLASSES = {"", "power", "voltage", "current", "energy", "temperature",
                        "reactive_power", "apparent_power", "frequency", "duration",
                        "reactive_energy", "signal_strength", "battery", "power_factor",
                        "energy_storage"}
VALID_STATE_CLASSES = {"", "measurement", "total", "total_increasing"}
DEVICE_CLASSES = {"W": "power", "kW": "power", "V": "voltage", "A": "current",
                  "kWh": "energy", "Wh": "energy", "°C": "temperature",
                  "var": "reactive_power", "VA": "apparent_power", "Hz": "frequency",
                  "h": "duration",
                  # 'reactive_energy' (varh/kvarh) needs an HA release that has the device
                  # class; ESPHome has it from 2026.9.
                  "kvarh": "reactive_energy",
                  "dBm": "signal_strength"}
STATE_CLASSES = {"kWh": "total_increasing", "Wh": "total_increasing",
                 "kvarh": "total_increasing", "h": "total_increasing"}
SOC_RE = re.compile(r"(soc|batt_power$|batt_remain_power|battery level)", re.I)


def ha_meta(key, name, unit, scale, is_string, ov=None):
    """-> dict(unit, multiplier, device_class, state_class, accuracy). With an override
    entry, its device_class/state_class (and accuracy if given) win."""
    if is_string:
        return {"unit": "", "multiplier": 1.0, "device_class": "", "state_class": "",
                "accuracy": 0}
    u, mult = UNIT_NORMALISE.get(unit, (unit, 1.0))
    if ov is not None:
        dc, sc = ov["device_class"], ov["state_class"]
        if dc not in VALID_DEVICE_CLASSES or sc not in VALID_STATE_CLASSES:
            raise SystemExit("%s: unknown device/state class %r/%r" % (ov["_where"], dc, sc))
    else:
        dc = DEVICE_CLASSES.get(u, "")
        if u == "%":
            dc = "battery" if SOC_RE.search(key) or SOC_RE.search(name) else ""
        if u == "" and ("factor" in key.lower() or "power factor" in name.lower()):
            dc = "power_factor"
        sc = STATE_CLASSES.get(u, "measurement" if (u or dc) else "")
    eff = (scale if scale else 1.0) * mult
    if ov is not None and "accuracy" in ov:
        return {"unit": u, "multiplier": mult, "device_class": dc, "state_class": sc,
                "accuracy": ov["accuracy"]}
    acc = max(0, int(round(-math.log10(eff)))) if eff < 1 else 0
    return {"unit": u, "multiplier": mult, "device_class": dc, "state_class": sc,
            "accuracy": acc}


def slug(s):
    s = re.sub(r"[^0-9A-Za-z]+", "_", s).strip("_").lower()
    return s or "unnamed"


def fnum(x):
    """Deterministic float literal."""
    if not x:
        return "0.0f"
    return repr(float(x)) + "f"


# ---------------------------------------------------------------------------------------
def load_tables(data_dir=DEFAULT_DATA_DIR):
    """-> (tables, data): the register tables of data/families + data/modbus_tcp (sorted by
    table name, rows in poll order) and the raw datalib document."""
    data = datalib.load_register_data(os.path.join(data_dir, "families"),
                                      os.path.join(data_dir, "modbus_tcp"))
    for t in data["tables"]:
        for r in t["rows"]:
            r["name"] = ""
    return data["tables"], data


def assign_keys(tables, issues):
    """Canonical entity key per (family, sigid), unique within the family: the `key` of the
    signal definition; type / count / unit / scale come from the highest-priority table
    (TABLE_PRIORITY) that carries the id."""
    order = {n: i for i, n in enumerate(TABLE_PRIORITY)}
    fam = {}
    for t in sorted(tables, key=lambda t: (order.get(t["name"], 99), t["name"])):
        for r in t["rows"]:
            if not is_named(r):
                continue
            fam.setdefault(t["family"], {}).setdefault(r["sigid"], []).append((t["name"], r))
    meta = {}
    for family in sorted(fam):
        used = {}
        out = {}
        for sigid in sorted(fam[family]):
            tname, r = fam[family][sigid][0]
            e = r["ov"]
            key, name = e["key"], e["name"]
            info = (e["confidence"], e["category"], e.get("scope", ""),
                    e.get("priority", DEFAULT_PRIORITY), e.get("group", "realtime"),
                    e.get("enum", ""))
            if key in used:
                raise SystemExit("%s key %r used by 0x%04x and 0x%04x"
                                 % (family, key, used[key], sigid))
            used[key] = sigid
            out[sigid] = {"key": key, "name": name, "unit": r["unit"], "scale": r["scale"],
                          "type": r["type"], "nreg": r["nreg"], "table": tname,
                          "source": r["source"], "string": r["type"] == 12,
                          "ov": e, "info": info}
        meta[family] = out
    for d in derived_list(meta):
        sigid, key, name, unit, dec, family = d[:6]
        typ, nreg, scale = d[6:] if len(d) > 6 else (0x80, 2, 0.0)
        if any(m["key"] == key for m in meta.get(family, {}).values()):
            raise SystemExit("derived key %r collides in %s" % (key, family))
        if sigid in meta.get(family, {}):
            raise SystemExit("derived sigid 0x%04x collides in %s" % (sigid, family))
        dm = DERIVED_META.get(sigid, {})
        meta.setdefault(family, {})[sigid] = {
            "key": key, "name": name, "unit": unit, "scale": scale, "type": typ, "nreg": nreg,
            "table": "", "source": "derived", "string": typ == 12, "decimals": dec, "ov": None,
            "info": ("derived", "", "", dm.get("priority", DEFAULT_PRIORITY),
                     dm.get("group", "realtime"), dm.get("enum", ""))}
    add_bitfield_children(meta, tables)
    for family in meta:
        for sigid, m in meta[family].items():
            m.update(ha_meta(m["key"], m["name"], m["unit"], m["scale"], m["string"], m["ov"]))
            if "decimals" in m:
                m["accuracy"] = m.pop("decimals")
            if m["source"] == "derived" and "device_class" in DERIVED_META.get(sigid, {}):
                m["device_class"] = DERIVED_META[sigid]["device_class"]
    return meta


def absent_rules(meta):
    """[(family, kind, sigid, raw, key)] from the override `absent` fields, deterministic order."""
    out = []
    for family in sorted(meta):
        for sigid in sorted(meta[family]):
            m = meta[family][sigid]
            for kind, raw in (m["ov"] or {}).get("absent_rules", []):
                out.append((family, kind, sigid, raw, m["key"]))
    return out


def mppt_pairs(meta):
    """[(mppt, voltage_sigid, current_sigid)] from the inverter keys mpptN_voltage/current."""
    pairs = {}
    for sigid, m in meta.get("inverter", {}).items():
        mm = re.match(r"^mppt(\d+)_(voltage|current)$", m["key"])
        if mm:
            pairs.setdefault(int(mm.group(1)), {})[mm.group(2)] = sigid
    return [(n, p["voltage"], p["current"]) for n, p in sorted(pairs.items())
            if "voltage" in p and "current" in p]


def derived_list(meta):
    """DERIVED plus one mpptN_power per MPPT pair."""
    out = list(DERIVED)
    for n, _v, _i in mppt_pairs(meta):
        out.append((MPPT_POWER_BASE + n, "mppt%d_power" % n, "MPPT%d Power" % n, "W", 0,
                    "inverter"))
    return out


def load_bitfields(tables):
    """Rows with a `bits` word -> [(table, parent_sigid, child_base, nbits)] sorted by
    (table, parent). Children are child_base + bit for bit < nbits."""
    out = []
    for t in tables:
        for r in t["rows"]:
            if r["nbits"]:
                out.append((t["name"], r["sigid"], r["bit_base"], r["nbits"]))
    return sorted(out)


def add_bitfield_children(meta, tables):
    """Binary-sensor keys for bitfield children (`<parent key>_bit<n>`, parent key
    `sig_<hex>` when the parent is unnamed). Category hidden (INFO)."""
    tmap = {t["name"]: t for t in tables}
    for tn, parent, base, nbits in load_bitfields(tables):
        t = tmap[tn]
        fam = meta.setdefault(t["family"], {})
        pkey = fam[parent]["key"] if parent in fam else "sig_%04x" % parent
        pname = fam[parent]["name"] if parent in fam else "Status 0x%04X" % parent
        for bit in range(nbits):
            child = base + bit
            if child in fam:
                raise SystemExit("bitfield child 0x%04x collides with %s" % (child,
                                                                            fam[child]["key"]))
            key = "%s_bit%d" % (pkey, bit)
            if any(m["key"] == key for m in fam.values()):
                raise SystemExit("bitfield child key %r collides" % key)
            fam[child] = {"key": key, "name": "%s bit %d" % (pname, bit), "unit": "",
                          "scale": 0.0, "type": 4, "nreg": 1, "table": tn, "source": "bitfield",
                          "string": False, "decimals": 0, "ov": None,
                          "info": ("confirmed", "hidden", "", DEFAULT_PRIORITY, "realtime", ""),
                          "bit_of": (parent, bit)}


def address_collisions(tables, shared=None):
    """Rows of one table that overlap on (fn, register range). -> list of (table, a, b, ok)
    where ok = explicitly allowed by an allow_shared override (same sigid, same address)."""
    shared = shared or {}
    out = []
    for t in tables:
        srt = sorted(t["rows"], key=lambda r: (r["fn"], r["regaddr"], r["sigid"]))
        for a, b in zip(srt, srt[1:]):
            if a["fn"] == b["fn"] and a["regaddr"] + a["nreg"] > b["regaddr"]:
                ok = (a["regaddr"] == b["regaddr"] and a["sigid"] == b["sigid"]
                      and (t["name"], a["fn"], a["regaddr"]) in shared)
                out.append((t["name"], a, b, ok))
    return out


def check_quality(tables, issues, shared=None):
    widths = {3: 1, 4: 1, 5: 2, 6: 2, 7: 4, 8: 4}
    for t in tables:
        rows = t["rows"]
        n9 = sum(1 for r in rows if r["type"] == 9)
        if n9:
            issues.append("%s: %d rows with type code 9 (float32)" % (t["name"], n9))
        arr = [r for r in rows if r["type"] in widths and r["nreg"] != widths[r["type"]]]
        if arr:
            issues.append("%s: %d numeric rows whose nreg != type width (arrays / odd widths),"
                          " e.g. 0x%04x@%d %s x%d" % (t["name"], len(arr), arr[0]["sigid"],
                                                      arr[0]["regaddr"],
                                                      TYPE_NAMES[arr[0]["type"]],
                                                      arr[0]["nreg"]))
        nm = sum(1 for a, b in zip(rows, rows[1:]) if b["regaddr"] < a["regaddr"])
        if nm:
            issues.append("%s: %d places where table order goes backwards in address "
                          "(the u16 span wraps: new request)" % (t["name"], nm))
        for r in rows:
            if r["nbits"] and r["type"] not in (4, 6):
                issues.append("%s: nbits on non-u16/u32 row 0x%04x" % (t["name"], r["sigid"]))
        nb = sum(1 for r in rows if r["nbits"])
        if nb:
            issues.append("%s: %d bitfield rows (bits)" % (t["name"], nb))
    allowed = 0
    for tn, a, b, ok in address_collisions(tables, shared):
        if ok:
            allowed += 1
            continue
        issues.append("%s: overlapping rows 0x%04x@%d+%d / 0x%04x@%d (not allowed by overrides)"
                      % (tn, a["sigid"], a["regaddr"], a["nreg"], b["sigid"], b["regaddr"]))
    if allowed:
        issues.append("%d duplicate (same sigid, same address) rows allowed by overrides"
                      % allowed)


def build_requests_py(rows, max_span=120, max_req=59):
    """Python mirror of request_builder.cpp (the stock request-merging rules; stats only)."""
    reqs = []
    for fn in (3, 4):
        cur = None
        for i, r in enumerate(rows):
            if r["fn"] != fn or not 0 < r["nreg"] <= max_span:
                continue
            end_node = r["tag"] & 1
            if cur and not end_node and r["group"] != cur["group"] and r["regaddr"] == 0x396d:
                continue
            span = (r["regaddr"] + r["nreg"] - cur["start"]) & 0xFFFF if cur else None
            if cur and span <= max_span:
                cur["count"] = max(cur["count"], span)
            else:
                cur = {"fn": fn, "group": r["group"], "start": r["regaddr"], "count": r["nreg"]}
                reqs.append(cur)
            if end_node and i + 1 < len(rows) and rows[i + 1]["fn"] == fn:
                cur = None
    return reqs


# ---------------------------------------------------------------------------------------
HDR = "// GENERATED by tools/gen_tables.py from data/families/*.yaml - DO NOT EDIT.\n"


def cname(n):
    return n.upper()


def emit_cpp(tables, meta, out_dir):
    keys = []          # string pool for row keys
    key_idx = {}
    units = [""]
    for t in tables:
        for r in t["rows"]:
            for u in [r["unit"]]:
                if u not in units:
                    units.append(u)
    units = [""] + sorted(units[1:])
    if len(units) > 255:
        raise SystemExit("too many units")

    def kidx(k):
        if not k:
            return 0xFFFF
        if k not in key_idx:
            key_idx[k] = len(keys)
            keys.append(k)
        return key_idx[k]

    # deterministic key pool: assign in family/sigid order first
    for family in sorted(meta):
        for sigid in sorted(meta[family]):
            kidx(meta[family][sigid]["key"])

    h = [HDR, "#pragma once\n", "#include <cstddef>\n#include <cstdint>\n",
         '#include "dm_types.h"\n', "namespace esphome {\nnamespace sungrow_datamodel {\n"]
    h.append("enum class TableId : uint8_t {\n")
    for i, t in enumerate(tables):
        h.append("  %s = %d,  // %s, %d rows\n" % (cname(t["name"]), i, t["family"],
                                                  len(t["rows"])))
    h.append("};\nstatic constexpr size_t kTableCount = %d;\n\n" % len(tables))
    h.append("// Sigids of derived values: 0xF0xx are synthetic (no register table uses them);\n"
             "// 0x2021 is the cloud total-yield sig, derived (no inverter poll row).\n")
    for sigid, key in dict((d[0], d[1]) for d in DERIVED).items():
        h.append("static constexpr uint16_t SIG_%s = 0x%04X;\n"
                 % (DERIVED_SIG_NAMES.get(sigid, key.upper()), sigid))
    h.append("/// Per-MPPT power V x I [W]: SIG_MPPT_POWER_BASE + MpptPair::mppt.\n")
    h.append("static constexpr uint16_t SIG_MPPT_POWER_BASE = 0x%04X;\n" % MPPT_POWER_BASE)
    h.append("/// Running-state code (device_state), input of state_class() / state_transient().\n")
    h.append("static constexpr uint16_t SIG_DEVICE_STATE = 0x%04X;\n" % DEV_STATE_SIGID)
    h.append("""
/// Inverter model family by stock dev_type (hybrid-only hooks, string-only transient codes).
enum class ModelFamily : uint8_t { UNKNOWN, %s };
ModelFamily inverter_model_family(uint8_t dev_type);

/// Behaviour class of a running-state code (SIG_DEVICE_STATE), UNKNOWN for unlisted codes.
enum class StateClass : uint8_t { %s };
StateClass state_class(uint16_t code);
/// Lower-case class name ("running", "stopped", ...).
const char *state_class_name(StateClass cls);
/// True while `code` is a state the inverter leaves by itself (I-V scan, rapid shutdown,
/// restart, fault recovery, ...); the string model family has extra codes.
bool state_transient(uint16_t code, bool string_family);

/// Fault / alarm code name of a table family ("inverter"), nullptr when the code has none.
const char *fault_code_text(uint16_t code, const char *family = "inverter");
""" % (", ".join(f.upper() for f in INVERTER_MODEL_FAMILIES), ", ".join(DEV_STATE_CLASSES)))
    h.append("\n// SBR cell block (battery table)\n")
    h.append("static constexpr uint16_t SBR_CELL_BLOCK_START = %d;\n" % SBR_BASE)
    h.append("static constexpr uint16_t SBR_CELL_BLOCK_COUNT = 24;\n")
    batt = next(t for t in tables if t["name"] == "battery")
    ba = {r["regaddr"]: r["sigid"] for r in batt["rows"] if r["fn"] == 4}
    h.append("static constexpr uint16_t kSbrCellSigids[24] = {")
    h.append(", ".join("0x%04X" % ba[SBR_BASE + i] for i in range(24)))
    h.append("};\n\n")
    h.append("""// MPPT voltage/current sigid pairs of the inverter family (for MPPT total power).
struct MpptPair {
  uint8_t mppt;
  uint16_t voltage_sigid;
  uint16_t current_sigid;
};
""")
    pairs = mppt_pairs(meta)
    h.append("static constexpr size_t kMpptPairCount = %d;\n" % len(pairs))
    h.append("extern const MpptPair kMpptPairs[kMpptPairCount];\n\n")
    tid = {t["name"]: i for i, t in enumerate(tables)}
    bfs = [(tid[tn], parent, base, nbits) for tn, parent, base, nbits in load_bitfields(tables)]
    h.append("// Bitfield children (rows with `bits` in data/families/*.yaml).\n")
    h.append("static constexpr size_t kBitfieldCount = %d;\n" % len(bfs))
    h.append("extern const BitfieldDesc kBitfields[kBitfieldCount];\n\n")
    rules = absent_rules(meta)
    h.append("// Not-served rules per signal (`absent` in data/families/*.yaml), applied at ingest.\n")
    h.append("static constexpr size_t kAbsentRuleCount = %d;\n" % len(rules))
    h.append("extern const AbsentRule kAbsentRules[kAbsentRuleCount];\n\n")
    h.append("""extern const RegisterTable kTables[kTableCount];
extern const char *const kRowKeys[];
extern const char *const kUnits[];

/// Table by id (always valid for ids < kTableCount).
const RegisterTable &get_table(TableId id);
/// Table by name ("inv_proto2", "battery", ...), nullptr if unknown.
const RegisterTable *find_table(const char *name);
/// Inverter poll table for a protocol type reported by the device (1/4, 2, 3, 5), else nullptr.
const RegisterTable *inverter_table_for_protocol(int protocol_type);
/// Modbus-TCP slave map for a dev_type (1/21, 35, 18, 44), else nullptr.
const RegisterTable *tcpslave_table_for_dev_type(int dev_type);

}  // namespace sungrow_datamodel
}  // namespace esphome
""")

    c = [HDR, '#include "register_tables.h"\n\n#include <cstring>\n\n',
         "namespace esphome {\nnamespace sungrow_datamodel {\n\n"]
    c.append("const char *const kUnits[] = {%s};\n" % ", ".join(
        '"%s"' % u for u in units))
    c.append("// Canonical entity keys (unique per family, see register_meta.py).\n")
    c.append("const char *const kRowKeys[] = {\n")
    for k in keys:
        c.append('    "%s",\n' % k)
    c.append("};\n\n")
    c.append("// sigid regaddr group key nreg fn type tag nbits dep_word dep_bit unit scale\n")
    total_rows = 0
    for t in tables:
        fam = meta.get(t["family"], {})
        c.append("static constexpr RegisterRow k_%s[] = {\n" % t["name"])
        for r in t["rows"]:
            m = fam.get(r["sigid"]) if is_named(r) else None
            k = kidx(m["key"]) if m else 0xFFFF
            c.append("    {0x%04X, %d, %d, 0x%04X, %d, %d, %d, %d, %d, %d, %d, %d, %s},\n" % (
                r["sigid"], r["regaddr"], r["group"], k, r["nreg"], r["fn"], r["type"],
                r["tag"], r["nbits"], r["dep_word"], r["dep_bit"], units.index(r["unit"]),
                fnum(r["scale"])))
            total_rows += 1
        c.append("};\n")
    c.append("\nconst RegisterTable kTables[kTableCount] = {\n")
    for i, t in enumerate(tables):
        c.append('    {%d, Family::%s, "%s", k_%s, %d},\n' % (
            i, t["family"].upper(), t["name"], t["name"], len(t["rows"])))
    c.append("};\n\n")
    c.append("const MpptPair kMpptPairs[kMpptPairCount] = {\n")
    for n, v, i in pairs:
        c.append("    {%d, 0x%04X, 0x%04X},\n" % (n, v, i))
    c.append("};\n\n")
    c.append("// table nbits parent_sigid child_base\n")
    c.append("const BitfieldDesc kBitfields[kBitfieldCount] = {\n")
    for t_id, parent, base, nbits in bfs:
        c.append("    {%d, %d, 0x%04X, 0x%04X},\n" % (t_id, nbits, parent, base))
    c.append("};\n\n")
    kind_names = {v: "ABSENT_" + k.upper() for k, v in ABSENT_KINDS.items()}
    c.append("// family kind sigid raw  (key)\n")
    c.append("const AbsentRule kAbsentRules[kAbsentRuleCount] = {\n")
    for family, kind, sigid, raw, key in rules:
        c.append("    {Family::%s, %s, 0x%04X, 0x%X},  // %s\n"
                 % (family.upper(), kind_names[kind], sigid, raw, key))
    c.append("};\n\n")
    c.append("// dev_type -> model family\n")
    c.append("ModelFamily inverter_model_family(uint8_t dev_type) {\n  switch (dev_type) {\n")
    for mf in INVERTER_MODEL_FAMILIES:
        for dt in sorted(INVERTER_MODEL_DEV_TYPES[mf]):
            c.append("    case %d:\n" % dt)
        c.append("      return ModelFamily::%s;\n" % mf.upper())
    c.append("    default:\n      return ModelFamily::UNKNOWN;\n  }\n}\n\n")
    c.append("// code class  (label)  -- sorted by code\n")
    c.append("struct StateClassEntry {\n  uint16_t code;\n  StateClass cls;\n};\n")
    c.append("static constexpr StateClassEntry kStateClasses[] = {\n")
    labels = dict(DEV_STATE_MAP)
    for code in sorted(DEV_STATE_CLASS):
        c.append("    {0x%04X, StateClass::%s},  // %s\n" % (code, DEV_STATE_CLASS[code], labels[code]))
    c.append("};\n")
    c.append("static const char *const kStateClassNames[] = {%s};\n\n"
             % ", ".join('"%s"' % s.lower() for s in DEV_STATE_CLASSES))
    c.append("StateClass state_class(uint16_t code) {\n"
             "  size_t lo = 0, hi = sizeof(kStateClasses) / sizeof(kStateClasses[0]);\n"
             "  while (lo < hi) {\n    const size_t mid = (lo + hi) / 2;\n"
             "    if (kStateClasses[mid].code < code)\n      lo = mid + 1;\n    else\n      hi = mid;\n  }\n"
             "  if (lo < sizeof(kStateClasses) / sizeof(kStateClasses[0]) && kStateClasses[lo].code == code)\n"
             "    return kStateClasses[lo].cls;\n  return StateClass::UNKNOWN;\n}\n\n")
    c.append("const char *state_class_name(StateClass cls) {\n"
             "  const size_t i = static_cast<size_t>(cls);\n"
             "  return i < sizeof(kStateClassNames) / sizeof(kStateClassNames[0]) ? kStateClassNames[i]\n"
             "                                                                    : kStateClassNames[0];\n}\n\n")
    c.append("// Transient running-state codes, sorted: per table family, plus the string model family's extras.\n")
    for fam in sorted(TRANSIENT_STATES):
        c.append("static constexpr uint16_t kTransientStates_%s[] = {%s};\n"
                 % (fam, ", ".join("0x%04X" % x for x in sorted(TRANSIENT_STATES[fam]))))
    for mf in sorted(TRANSIENT_STATE_EXTRAS):
        c.append("static constexpr uint16_t kTransientStateExtras_%s[] = {%s};\n"
                 % (mf, ", ".join("0x%04X" % x for x in sorted(TRANSIENT_STATE_EXTRAS[mf]))))
    c.append("""
namespace {
bool sorted_contains(const uint16_t *v, size_t n, uint16_t code) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (v[mid] < code)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < n && v[lo] == code;
}
}  // namespace

bool state_transient(uint16_t code, bool string_family) {
  if (sorted_contains(kTransientStates_inverter,
                      sizeof(kTransientStates_inverter) / sizeof(kTransientStates_inverter[0]), code))
    return true;
  return string_family &&
         sorted_contains(kTransientStateExtras_string,
                         sizeof(kTransientStateExtras_string) / sizeof(kTransientStateExtras_string[0]), code);
}

""")
    c.append("// Fault / alarm code names per table family, sorted by code.\n")
    c.append("struct FaultCodeName {\n  uint16_t code;\n  const char *name;\n};\n")
    for fam in sorted(FAULT_CODES):
        c.append("static constexpr FaultCodeName kFaultCodes_%s[] = {\n" % fam)
        for code, name in sorted(fault_code_table(fam).items()):
            c.append('    {%d, "%s"},\n' % (code, name.replace("\\", "\\\\").replace('"', '\\"')))
        c.append("};\n")
    c.append("struct FaultCodeFamily {\n  const char *family;\n  const FaultCodeName *codes;\n  size_t count;\n};\n")
    c.append("static constexpr FaultCodeFamily kFaultCodeFamilies[] = {\n")
    for fam in sorted(FAULT_CODES):
        c.append('    {"%s", kFaultCodes_%s, sizeof(kFaultCodes_%s) / sizeof(kFaultCodes_%s[0])},\n'
                 % (fam, fam, fam, fam))
    c.append("};\n\n")
    c.append("""const char *fault_code_text(uint16_t code, const char *family) {
  if (family == nullptr)
    return nullptr;
  for (const auto &f : kFaultCodeFamilies) {
    if (std::strcmp(f.family, family) != 0)
      continue;
    size_t lo = 0, hi = f.count;
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (f.codes[mid].code < code)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo < f.count && f.codes[lo].code == code ? f.codes[lo].name : nullptr;
  }
  return nullptr;
}

""")
    c.append("""const RegisterTable &get_table(TableId id) { return kTables[static_cast<size_t>(id)]; }

const RegisterTable *find_table(const char *name) {
  if (name == nullptr)
    return nullptr;
  for (const auto &t : kTables) {
    if (std::strcmp(t.name, name) == 0)
      return &t;
  }
  return nullptr;
}

const RegisterTable *inverter_table_for_protocol(int protocol_type) {
  switch (protocol_type) {
    case 1:
    case 4:
      return &get_table(TableId::INV_PROTO1_4);
    case 2:
      return &get_table(TableId::INV_PROTO2);
    case 3:
      return &get_table(TableId::INV_PROTO3);
    case 5:
      return &get_table(TableId::INV_PROTO5);
    default:
      return nullptr;
  }
}

const RegisterTable *tcpslave_table_for_dev_type(int dev_type) {
  switch (dev_type) {
    case 1:
    case 21:
      return &get_table(TableId::TCPSLAVE_DEV1_21);
    case 35:
      return &get_table(TableId::TCPSLAVE_DEV35);
    case 18:
      return &get_table(TableId::TCPSLAVE_DEV18);
    case 44:
      return &get_table(TableId::TCPSLAVE_DEV44);
    default:
      return nullptr;
  }
}

}  // namespace sungrow_datamodel
}  // namespace esphome
""")
    write(os.path.join(out_dir, "register_tables.h"), "".join(h))
    write(os.path.join(out_dir, "register_tables.cpp"), "".join(c))
    key_bytes = sum(len(k) + 1 for k in keys) + 4 * len(keys)
    return {"rows": total_rows, "row_bytes": total_rows * 20, "key_bytes": key_bytes,
            "keys": len(keys), "units": units, "mppt_pairs": len(pairs)}


def emit_meta_py(tables, meta, ov, out_dir):
    lines = ['"""GENERATED by tools/gen_tables.py - DO NOT EDIT.\n\n'
             "Codegen-time metadata for sungrow_datamodel entity platforms:\n"
             "FAMILIES[family][key] = (sigid, name, unit, multiplier, device_class,\n"
             "                         state_class, accuracy_decimals, is_string, type, nreg,\n"
             "                         bit_parent, bit)\n"
             "bit_parent/bit: bitfield child = bit `bit` of sigid bit_parent (else 0, -1).\n"
             "`unit`/`multiplier` are already HA-normalised (kvar->var x1000, ...).\n"
             "TABLES[name] = (table_id, family, rows)\n"
             "INFO[family][key] = (confidence, category, scope, priority, group, enum) from\n"
             "data/families/*.yaml (confidence: confirmed|likely|guess|derived; category:\n"
             "core|core_hybrid|core_string|extra|hidden, '' for cell-block/derived keys; priority: int\n"
             "sort-rank, lower = more important, 9999 = unranked; group: realtime|static|protection;\n"
             "enum: name of an ENUMS table or '').\n"
             "GROUPS[family][key] = 'static'|'protection'|'hidden' (non-'realtime' only; a key\n"
             "absent from GROUPS[family] is 'realtime'). category:hidden signals are reported here\n"
             "as group 'hidden' so the SPA/MQTT suppress them (category itself is not in MetaTuple).\n"
             "ENUMS[name] = [(raw_code, text), ...] read-side decode tables (see `enum` above).\n"
             "DEV_STATE_CLASSES = class names; DEV_STATE_CLASS[code] = class of each device_state\n"
             "(sig 0x2086) code; TRANSIENT_STATES[family] / TRANSIENT_STATE_EXTRAS[model_family]\n"
             "= codes the inverter leaves by itself; INVERTER_MODEL_DEV_TYPES[model_family] =\n"
             "stock dev_type codes.\n"
             "FAULT_CODES[family][code] = fault / alarm name; FAULT_CODE_OVERRIDES[model][code]\n"
             "= per-model additions merged over the family table.\n"
             "READS[family][key] = (regaddr, name, unit, multiplier, device_class, state_class,\n"
             "                      accuracy, fn, group, priority, enum, scale, dtype): holding\n"
             "registers that are not polled (no sigid / no C++ row), surfaced for display keyed by\n"
             "register address. The value is the raw register (dtype u16|s16): displayed value =\n"
             "raw * scale * multiplier.\n"
             '"""\n\n']
    lines.append("TABLES = {\n")
    for i, t in enumerate(tables):
        lines.append("    %r: (%d, %r, %d),\n" % (t["name"], i, t["family"], len(t["rows"])))
    lines.append("}\n\nFAMILIES = {\n")
    for family in sorted(meta):
        lines.append("    %r: {\n" % family)
        for sigid in sorted(meta[family], key=lambda s: meta[family][s]["key"].lower()):
            m = meta[family][sigid]
            bp, bit = m.get("bit_of", (0, -1))
            lines.append("        %r: (0x%04X, %r, %r, %r, %r, %r, %d, %r, %d, %d, 0x%04X, %d),\n" % (
                m["key"], sigid, m["name"], m["unit"], m["multiplier"], m["device_class"],
                m["state_class"], m["accuracy"], m["string"], m["type"], m["nreg"], bp, bit))
        lines.append("    },\n")
    lines.append("}\n\nINFO = {\n")
    for family in sorted(meta):
        lines.append("    %r: {\n" % family)
        for sigid in sorted(meta[family], key=lambda s: meta[family][s]["key"].lower()):
            m = meta[family][sigid]
            lines.append("        %r: %r,\n" % (m["key"], m["info"]))
        lines.append("    },\n")
    lines.append("}\n\nENUMS = {\n")
    for name in sorted(ENUMS):
        lines.append("    %r: [\n" % name)
        for code, txt in ENUMS[name]:
            lines.append("        (0x%04X, %r),\n" % (code, txt))
        lines.append("    ],\n")
    lines.append("}\n")
    lines.append("\nDEV_STATE_CLASSES = %r\n" % (DEV_STATE_CLASSES,))
    lines.append("\nDEV_STATE_CLASS = {\n")
    for code in sorted(DEV_STATE_CLASS):
        lines.append("    0x%04X: %r,\n" % (code, DEV_STATE_CLASS[code]))
    lines.append("}\n")
    lines.append("\nTRANSIENT_STATES = {\n")
    for fam in sorted(TRANSIENT_STATES):
        lines.append("    %r: [%s],\n" % (fam, ", ".join("0x%04X" % x for x in sorted(TRANSIENT_STATES[fam]))))
    lines.append("}\n")
    lines.append("\nTRANSIENT_STATE_EXTRAS = {\n")
    for mf in sorted(TRANSIENT_STATE_EXTRAS):
        lines.append("    %r: [%s],\n" % (mf, ", ".join("0x%04X" % x for x in sorted(TRANSIENT_STATE_EXTRAS[mf]))))
    lines.append("}\n")
    lines.append("\nINVERTER_MODEL_DEV_TYPES = {\n")
    for mf in INVERTER_MODEL_FAMILIES:
        lines.append("    %r: %r,\n" % (mf, tuple(sorted(INVERTER_MODEL_DEV_TYPES[mf]))))
    lines.append("}\n")
    lines.append("\nFAULT_CODES = {\n")
    for fam in sorted(FAULT_CODES):
        lines.append("    %r: {\n" % fam)
        for code, name in sorted(FAULT_CODES[fam].items()):
            lines.append("        %d: %r,\n" % (code, name))
        lines.append("    },\n")
    lines.append("}\n")
    lines.append("\nFAULT_CODE_OVERRIDES = {\n")
    for model in sorted(FAULT_CODE_OVERRIDES):
        lines.append("    %r: {\n" % model)
        for code, name in sorted(FAULT_CODE_OVERRIDES[model].items()):
            lines.append("        %d: %r,\n" % (code, name))
        lines.append("    },\n")
    lines.append("}\n")

    # ENUM_SIGNALS[family][key] = enum_name, and GROUPS[family][key] = group (non-realtime only;
    # a key absent from GROUPS[family] is 'realtime'). Both span polled signals (meta, info idx
    # 4=group 5=enum) and the non-polled protection READS below, so the MQTT _gen_header and the
    # SPA share one source. The MQTT generator reads them by these exact names.
    reads_by_family = {}
    for e in ov.get("reads", []):
        reads_by_family.setdefault(e["family"], []).append(e)
    enum_sig, grp = {}, {}
    for family in sorted(meta):
        for sigid in sorted(meta[family], key=lambda s: meta[family][s]["key"].lower()):
            m = meta[family][sigid]
            cat, g_, en = m["info"][1], m["info"][4], m["info"][5]
            if cat == "hidden":  # category:hidden overrides group so the UI/MQTT suppress it
                g_ = "hidden"
            if en:
                enum_sig.setdefault(family, {})[m["key"]] = en
            if g_ != "realtime":
                grp.setdefault(family, {})[m["key"]] = g_
    for family in sorted(reads_by_family):
        for e in sorted(reads_by_family[family], key=lambda e: e["regaddr"]):
            if e.get("enum"):
                enum_sig.setdefault(family, {})[e["key"]] = e["enum"]
            if e["group"] != "realtime":
                grp.setdefault(family, {})[e["key"]] = e["group"]

    def _emit_nested(varname, data):
        lines.append("\n%s = {\n" % varname)
        for family in sorted(data):
            lines.append("    %r: {\n" % family)
            for key in sorted(data[family]):
                lines.append("        %r: %r,\n" % (key, data[family][key]))
            lines.append("    },\n")
        lines.append("}\n")

    _emit_nested("ENUM_SIGNALS", enum_sig)
    _emit_nested("GROUPS", grp)

    lines.append("\nREADS = {\n")
    for family in sorted(reads_by_family):
        lines.append("    %r: {\n" % family)
        for e in sorted(reads_by_family[family], key=lambda e: e["regaddr"]):
            h = ha_meta(e["key"], e["name"], e["unit"], e["scale"], False, e)
            lines.append("        %r: (%d, %r, %r, %r, %r, %r, %d, %d, %r, %d, %r, %r, %r),\n" % (
                e["key"], e["regaddr"], e["name"], h["unit"], h["multiplier"],
                h["device_class"], h["state_class"], h["accuracy"], e["fn"], e["group"],
                e.get("priority", DEFAULT_PRIORITY), e.get("enum", ""), e["scale"] or 1.0,
                e["dtype"]))
        lines.append("    },\n")
    lines.append("}\n")
    write(os.path.join(out_dir, "register_meta.py"), "".join(lines))


# ---------------------------------------------------------------------------------------
# Device packages
# Running-state codes for sig 0x2086 (device_state). Three maps carry it: the private 5221
# (inv_proto2/3, served by both the SH10RS and the SG5.0RS), the SG public 5038 ("work state") and the SH
# public 13000 ("system state", one bit per state). 5221 and 5038 share one code set; the
# 13000 codes do not collide with it, so one merged map serves all three. Sources:
#   5221: the "Operating status" option list of the private map (24 codes incl. 0x0801,
#         0x1202, 0x6600); an SG5.0RS reads 0x0000 and an SH10RS 0x8200 (hardware readings).
#   5038: mkaiser modbus_sungrow.yaml template "Sungrow inverter state", SunGather work_state_1.
#   13000: SH public map "System status" (SunGather system_state, mkaiser).
# Labels keep the established wording (the HA/battery-controller contract decodes 0x0000 as
# "Running"); codes only one source has are labelled from that source.
DEV_STATE_MAP = [
    (0x0000, "Running"), (0x0001, "Stop"), (0x0002, "Awaiting startup"), (0x0004, "Emergency stop"),
    (0x0008, "Standby"), (0x0010, "Initial standby"), (0x0014, "Microgrid operation"),
    (0x0020, "Starting"), (0x0040, "Running"), (0x0041, "Off-grid DC charging"),
    (0x0080, "Derating running"), (0x0100, "Fault"), (0x0200, "Update failed"),
    (0x0400, "Running (maintain mode)"), (0x0800, "Running (forced mode)"),
    (0x0801, "Running (backup mode)"),
    (0x1000, "Running (off-grid)"), (0x1111, "Uninitialized"), (0x1200, "Initial standby"),
    (0x1202, "Running"),
    (0x1300, "Key stop"), (0x1400, "Standby"), (0x1500, "Emergency stop"),
    (0x1600, "Starting"), (0x1700, "AFCI self-test shutdown"),
    (0x1800, "Intelligent station building"), (0x1900, "Safe mode"), (0x2000, "Open loop"),
    (0x2500, "Communication fault"), (0x2501, "Restarting"), (0x4000, "Running (external EMS)"),
    (0x4001, "Emergency charging"), (0x5500, "Fault"), (0x6600, "Recovering from fault"),
    (0x8000, "Stop"), (0x8100, "Derating running"), (0x8200, "Dispatched running"),
    (0x9100, "Running with alarm"), (0xA100, "Standby (low PV)"), (0xFFFE, "Offline")]
DEV_STATE_SIGID = 0x2086  # device_state (SIG_DEVICE_STATE)

# Behaviour class of every DEV_STATE_MAP code (C++ enum StateClass, register_meta.py
# DEV_STATE_CLASS, meta.gen.ts STATE_CLASS). RUNNING covers every producing state including
# derated / dispatched / alarm-free special modes; TRANSIENT is a state the inverter leaves by
# itself (sweep, restart, recovery, rapid shutdown); OFFLINE is a communication state of the
# logger, not of the power stage.
DEV_STATE_CLASSES = ("UNKNOWN", "RUNNING", "STOPPED", "STANDBY", "STARTING", "TRANSIENT", "FAULT",
                 "OFFLINE")
DEV_STATE_CLASS = {
    0x0000: "RUNNING", 0x0001: "STOPPED", 0x0002: "STOPPED", 0x0004: "STOPPED",
    0x0008: "STANDBY", 0x0010: "STANDBY", 0x0014: "RUNNING", 0x0020: "STARTING",
    0x0040: "RUNNING", 0x0041: "RUNNING", 0x0080: "RUNNING", 0x0100: "FAULT",
    0x0200: "FAULT", 0x0400: "RUNNING", 0x0800: "RUNNING", 0x0801: "RUNNING",
    0x1000: "RUNNING", 0x1111: "STANDBY", 0x1200: "STANDBY", 0x1202: "RUNNING",
    0x1300: "STOPPED", 0x1400: "STANDBY", 0x1500: "STOPPED", 0x1600: "STARTING",
    0x1700: "STOPPED", 0x1800: "TRANSIENT", 0x1900: "TRANSIENT", 0x2000: "RUNNING",
    0x2500: "OFFLINE", 0x2501: "TRANSIENT", 0x4000: "RUNNING", 0x4001: "RUNNING",
    0x5500: "FAULT", 0x6600: "TRANSIENT", 0x8000: "STOPPED", 0x8100: "RUNNING",
    0x8200: "RUNNING", 0x9100: "FAULT", 0xA100: "STANDBY", 0xFFFE: "OFFLINE",
}
# Running-state codes that pass by themselves, per table family: 0x1200 initial standby,
# 0x1600 starting / self-check, 0x1800 PV string sweep (I-V scan), 0x1900 rapid shutdown
# engaged, 0x2501 restarting, 0x6600 recovering from a fault, 0xA100 low-PV standby. The
# state_transient derived signal (0xF066) is 1 while the inverter reports one of them.
TRANSIENT_STATES = {"inverter": {0x1200, 0x1600, 0x1800, 0x1900, 0x2501, 0x6600, 0xA100}}
# Extra transient codes per inverter model family: a string inverter reports rapid shutdown
# as 0x1300 (the hybrid's key stop), so 0x1300 is transient for the string family only.
TRANSIENT_STATE_EXTRAS = {"string": {0x1300}}
# Stock dev_type codes of the inverter model families (C++ ModelFamily /
# inverter_model_family()): 35 = hybrid (SH), 1 / 21 = string (SG). The hybrid-only derived
# hooks and the string-only transient codes are selected by this.
INVERTER_MODEL_FAMILIES = ("hybrid", "string")
INVERTER_MODEL_DEV_TYPES = {"hybrid": (35,), "string": (1, 21)}


def check_state_tables():
    codes = {c for c, _ in DEV_STATE_MAP}
    if set(DEV_STATE_CLASS) != codes:
        raise SystemExit("DEV_STATE_CLASS must cover exactly the DEV_STATE_MAP codes: %s"
                         % sorted(set(DEV_STATE_CLASS) ^ codes))
    bad = {c: k for c, k in DEV_STATE_CLASS.items() if k not in DEV_STATE_CLASSES}
    if bad:
        raise SystemExit("DEV_STATE_CLASS unknown class: %r" % bad)
    for fam, s in list(TRANSIENT_STATES.items()) + list(TRANSIENT_STATE_EXTRAS.items()):
        if not s <= codes:
            raise SystemExit("transient codes of %r not in DEV_STATE_MAP: %s" % (fam, sorted(s - codes)))
    for mf in TRANSIENT_STATE_EXTRAS:
        if mf not in INVERTER_MODEL_FAMILIES:
            raise SystemExit("TRANSIENT_STATE_EXTRAS: unknown model family %r" % mf)


# Fault / alarm code -> name, per table family (C++ fault_code_text(), register_meta.py
# FAULT_CODES, meta.gen.ts FAULT_CODES). The inverter reports faults as numeric codes in the
# fault list block 5448..5472 (derived.cpp decode_fault_list); the names are the inverter's
# protection conditions and match the stock dongle. Codes without a name are shown numeric.
FAULT_CODES = {
    "inverter": {
        2: "Grid overvoltage",
        3: "Grid transient overvoltage",
        4: "Grid undervoltage",
        8: "Grid overfrequency",
        9: "Grid underfrequency",
        10: "Grid power outage",
        11: "DC injection too high",
        12: "Excess leakage current",
        13: "Grid abnormal",
        14: "10-minute grid overvoltage",
        19: "DC bus instantaneous overvoltage",
        20: "DC bus overvoltage",
        21: "PV1 overcurrent",
        22: "PV2 overcurrent",
        24: "Bus unbalance (average)",
        25: "Bus unbalance (instantaneous)",
        28: "PV1 reverse polarity",
        29: "PV2 reverse polarity",
        36: "Inverter bridge overtemperature",
        37: "Internal overtemperature",
        38: "Relay fault",
        39: "Low insulation resistance",
        41: "Leakage current sensor fault",
        43: "Internal undertemperature",
        44: "Inverter self-check fault",
        47: "PV input configuration abnormal",
        48: "Current sensor fault",
        71: "AC surge protector alarm",
        72: "DC surge protector alarm",
        88: "Arc fault (AFCI)",
        100: "Hardware inverter overcurrent",
        105: "Grid self-check failed",
        106: "Ground wire fault",
        200: "Hardware DC bus overvoltage",
        201: "DC bus unbalance",
        202: "Hardware PV overcurrent",
        204: "Boost 1 short circuit",
        205: "Boost 2 short circuit",
        207: "PV3 overcurrent",
        208: "PV3 reverse polarity",
        209: "Boost 3 short circuit",
        212: "PV4 reverse polarity",
        306: "Input/output power mismatch",
        309: "Voltage sensor channel fault",
        312: "DC current sensor channel fault",
        315: "PV1 current sensor fault",
        316: "PV2 current sensor fault",
        324: "PV3 current sensor fault",
        600: "PV input overvoltage",
        605: "Battery relay check failed",
        997: "Frequency derating active",
    },
}
# Per-model additions / replacements: {model_key: {code: name}}, merged over the family table
# by fault_code_table().
FAULT_CODE_OVERRIDES = {}


def fault_code_table(family, model=None):
    """{code: name} of `family`, with FAULT_CODE_OVERRIDES[model] merged on top."""
    out = dict(FAULT_CODES.get(family, {}))
    out.update(FAULT_CODE_OVERRIDES.get(model, {}))
    return out


# ---------------------------------------------------------------------------------------
# Read-side ENUM tables (raw code -> text). A signal references one by name via a flat scalar
# `enum: <name>` in data/families/*.yaml; baked into register_meta.py ENUMS and emitted as
# [[raw, label]] into meta.gen.ts (the SPA / MQTT publisher decode a raw code to text). Sources:
# the labels the stock web UI shows, mkaiser modbus_sungrow.yaml and SunGather
# registers-sungrow.yaml. `device_state` reuses DEV_STATE_MAP
# (the operating-status map already proven for sig 0x2086). Wrong text only mislabels a read
# value (no write impact); maps marked "authored" below come from public sources and are
# unverified against device codes.
OUTPUT_TYPE_MAP = [  # sig 0x2003 feed_mode / SunGather output_type (Sungrow string protocol)
    (0, "Single-phase"), (1, "Three-phase (3P4L)"), (2, "Three-phase (3P3L)")]
# SBR 0x2019 battery operation status: the 9 codes the stock web UI labels (it shows 0 as
# "Unplugged"; the label key says idle). An SBR192 in operation reads 2 = Running.
BATTERY_STATUS_MAP = [
    (0, "Idle"), (1, "Standby"), (2, "Running"), (3, "Fault"), (4, "Shutdown"), (5, "Update"),
    (6, "Correction"), (7, "Test"), (32767, "Offline")]
# Charger 0x2017 charging status: the codes the stock web UI labels for the EV charger.
CHARGING_STATUS_MAP = [
    (0, "Unplugged"), (1, "Unplugged"), (2, "Standby"), (3, "Charging"),
    (4, "Charging suspended (charger)"), (5, "Charging suspended (EV)"), (6, "Charging complete"),
    (8, "Disabled"), (9, "Fault")]
# SBR 10683 balancing status (iSolarCloud point 58649), see docs/battery_sbr.md:
# 1 = capacity-mode, 2 = voltage-mode balancing.
BATTERY_BALANCING_MAP = [(0, "Idle"), (1, "Balancing (capacity)"), (2, "Balancing (voltage)")]
# SBR 10789 DC contactor status.
BATTERY_CONTACTOR_MAP = [(0, "Open"), (1, "Fault"), (2, "Closed"), (3, "Fault")]
# SBR 10684 low-SOC rest calibration (docs/battery_sbr.md): 5 = every module OCV-calibrated at
# the low end after a rest, 1 = some modules, 0 = none since current last exceeded 0.5 A; 2 is
# defined but never produced.
BATTERY_SOC_CALIBRATION_MAP = [(0, "Inactive"), (1, "Some modules calibrated"), (2, "Reserved"),
                               (5, "All modules calibrated")]
# SBR 10781+ module cell-type code = ASCII character 5 of the module S/N (1 = EEPROM read failed).
BATTERY_MODULE_CELL_TYPE_MAP = ([(0, "No module"), (1, "Unreadable")]
                                + [(c, chr(c)) for c in range(ord("A"), ord("Z") + 1)])
NO_YES_MAP = [(0, "No"), (1, "Yes")]
# Other EV charger enums (labels of the stock web UI).
CHARGER_FULL_MAP = [(0, "No"), (1, "Yes")]                                  # 0x2015 charging full
CHARGER_PHASE_MAP = [(1, "Single-phase"), (3, "Three-phase")]              # 0x2001 phase
CHARGER_POWER_FLAG_MAP = [(0, "Close"), (1, "Open")]                       # 0x2016 power flag
CHARGER_START_MODE_MAP = [(0, "Unplugged"), (1, "EMS start"), (2, "RFID start")]  # 0x2014 start mode
# Device/model code -> model name (sig 0x0004). Empty: no public code -> name mapping.
DEVICE_TYPE_CODE_MAP = []
# Holding 30036 (unit family): the grid-standard picker options of data/grid_profiles/unit.yaml
# (profile codes 12..15).
GRID_STANDARD_MAP = [
    (12, "AS/NZS 4777.2:2015"), (13, "AS/NZS 4777.2:2020 Australia A"),
    (14, "AS/NZS 4777.2:2020 Australia B"), (15, "AS/NZS 4777.2:2020 Australia C")]
# Grid-code enable switches (off = 85, on = 170; e.g. 31230 volt-watt).
ENABLE_SWITCH_MAP = [(85, "Off"), (170, "On")]
# Inverter enums below: option lists of the private 5xxx map that inv_proto2/3 poll (labels as
# the stock web UI / iSolarCloud show them; mkaiser and SunGather where they carry the code).
# 5506 (sig 0x2090) "Power limiting mode": why output is currently limited. The power stage writes
# code 1 for both the temperature derating and the microgrid-mode output cap.
POWER_LIMIT_REASON_MAP = [
    (0, "No power limitation"), (1, "Temperature or microgrid derating"), (2, "Active power limitation"),
    (4, "Grid undervoltage"), (5, "Power factor below 1"), (8, "Grid overfrequency"),
    (11, "Optimizer overtemperature"), (16, "Feed-in limitation (without meter)"),
    (32, "Soft start"), (64, "Feed-in limitation"), (128, "PV input power limit"),
    (256, "Grid overvoltage"), (257, "Ripple control"), (512, "DRM5-DRM8"), (1024, "DRM1-DRM4"),
    (2048, "Grid underfrequency"), (4096, "Charging derating at low grid voltage"),
    (8192, "Reactive power regulation"), (16384, "High PV voltage"), (32768, "High altitude"),
    (32770, "Undertemperature"), (32771, "Alarm"), (32772, "Module temperature abnormal"),
    (36864, "Generation limit control")]
# 5503 / 13043 (sig 0x217B) "DRM state": AS/NZS 4777 demand-response mode (SunGather drm_state).
DRM_STATE_MAP = [
    (1, "DRM0"), (2, "DRM1"), (3, "DRM2"), (4, "DRM3"), (5, "DRM4"), (6, "DRM5"), (7, "DRM6"),
    (8, "DRM7"), (9, "DRM8"), (255, "No DRM dispatching")]
# 5507 (sig 0x217C) "Reactive power mode": the active reactive-power regulation mode.
REACTIVE_POWER_MODE_MAP = [
    (0, "Off"), (1, "LVRT"), (2, "Fixed Q(T)"), (3, "Fixed power factor"), (4, "Q(P)"),
    (5, "Q(U) (volt-var)"), (6, "HVRT"), (7, "SVG mode")]
# 5629 / 13055 (sig 0x2510) "Current battery": the battery type the hybrid is configured for.
BATTERY_TYPE_MAP = [
    (0, "Narada lead-acid"), (1, "Samsung Li-ion"), (2, "No battery"), (3, "Other lead-acid"),
    (4, "ZTE Pylontech US2000A Li-ion"), (5, "LG Li-ion"), (6, "ZTE Pylontech US2000B Li-ion"),
    (7, "GCL Li-ion"), (8, "BSG Li-ion"), (9, "Sungrow Li-ion"), (10, "BYD Li-ion"),
    (11, "TAWAKI Li-ion"), (12, "Dyness Li-ion"), (14, "Pylontech Li-ion"), (15, "SUNGROW-LV"),
    (16, "SUNGROW-YG"), (49, "Other battery"), (50, "WECO Li-ion"),
    (101, "Pylontech high-voltage Li-ion"), (102, "BYD high-voltage Li-ion"),
    (103, "LG high-voltage Li-ion"), (104, "QCELL high-voltage Li-ion"),
    (105, "Sungrow high-voltage Li-ion"), (106, "SMA high-voltage Li-ion"),
    (107, "WTS CORE series"), (108, "LGES enblock E"), (109, "Sungrow LFP"),
    (110, "Sungrow modular battery"), (111, "Dyness high-voltage Li-ion")]
# Holding 32310 (unit family) / 30500 (str_pv family) country (region) code: the Sungrow
# country-code list (150 codes; 6 = Australia on the test inverters).
COUNTRY_MAP = [
    (0, "United Kingdom"), (1, "Germany"), (2, "France (FR)"), (3, "Italy"), (4, "Spain"),
    (5, "Austria"), (6, "Australia"), (7, "Czech Republic"), (8, "Belgium"), (9, "Denmark"),
    (10, "Greece (mainland)"), (11, "Greece (islands)"), (12, "Netherlands"), (13, "Portugal"),
    (14, "China"), (15, "Sweden"), (16, "Others (50 Hz)"), (17, "Romania"), (18, "Thailand-PEA"),
    (19, "Türkiye"), (21, "US IEEE 2800"), (24, "Brazil-PORTARIA 140"), (25, "Vorarlberg State"),
    (26, "India"), (27, "United Arab Emirates"), (28, "Israel"), (29, "Hungary"),
    (30, "Malaysia"), (31, "Philippines"), (32, "Poland"), (34, "Colombia"), (35, "Thailand-MEA"),
    (36, "Vietnam"), (38, "Oman"), (39, "Saudi Arabia"), (40, "Finland"), (41, "Ireland"),
    (42, "Morocco"), (43, "China Southern Power Grid"), (44, "Iraq"), (46, "China TaiWan (480V)"),
    (47, "Others (50 Hz 480 V)"), (48, "Others (60 Hz 480 V)"), (52, "Tunisia"),
    (59, "US – Hawaii"), (60, "Canada"), (61, "United States"), (62, "Others (60 Hz)"),
    (63, "South Korea"), (64, "South Africa"), (65, "Chile"), (66, "Brazil"),
    (67, "China Taipei"), (68, "Brazil-ONS"), (69, "Japan"), (70, "Japan (JP-50Hz)"),
    (71, "Japan (JP-60Hz)"), (72, "Luxembourg (Luxembourg)"), (73, "Egypt"), (74, "Kazakhstan"),
    (75, "Chile (CHL-NTSyCS)"), (76, "EN50549-1"), (77, "EN50549-2"), (80, "China Qinghai"),
    (81, "China Tibet"), (82, "Thailand-EGAT"), (86, "China Shaanxi"), (89, "Argentina"),
    (90, "Singapore"), (96, "UK-transmission grid"), (97, "US – New England region"),
    (98, "US-SA"), (99, "New Zealand"), (106, "Mexico"), (107, "Denmark (DK2)"),
    (108, "Denmark (DK1)"), (110, "United Kingdom_G98"), (112, "United Arab Emirates-MV"),
    (113, "Qatar"), (117, "AS/NZS 4777.2:2020 New Zealand"), (118, "Jordan_g98"),
    (119, "Jordan_g99"), (120, "Spain(ES) NTS 631"), (122, "Ukraine"), (123, "Jordan"),
    (128, "US_240"), (129, "US_240 (no neutral point)"), (130, "US_208"),
    (131, "US_208 (no neutral point)"), (132, "US-SA_240"), (133, "US-SA_240 (no neutral point)"),
    (134, "US-SA_208"), (135, "US-SA_208 (no neutral point)"), (136, "US-HWE_240"),
    (137, "US-HWE_240(no neutral)"), (138, "US-HWE_208"), (139, "US-HWE_208(no neutral)"),
    (140, "Colombia - Single-phase - 240 V"), (141, "Colombia - Single-phase - 220 V"),
    (142, "Colombia - Three-phase - 208 V"), (143, "Colombia - Three-phase - 220 V"),
    (144, "Colombia - Three-phase - 440 V"), (145, "Colombia - Three-phase - 380 V"),
    (146, "Colombia - Three-phase - 480 V"), (148, "Bosnia and Herzegovina"), (166, "Brazil_230"),
    (167, "Brazil_240"), (168, "France-50Hz"), (169, "France-60Hz"), (170, "Mexico – 220"),
    (171, "Mexico – 230"), (172, "Mexico – 240"), (173, "United States(US) IEEE 1547-2018"),
    (174, "Saudi Arabia (SA)"), (175, "Switzerland (CH) NA/EEA-NE7"),
    (176, "Greece mainland (GR) NC RfG GR"), (177, "Greece islands (GR) NC RfG GR"),
    (178, "Hungary (HU) HMKE"), (180, "United Kingdom(UK) G99"), (181, "Germany(DE) VDE"),
    (182, "France(FR) VFR2019"), (183, "Italy (IT) CEI 0-21"), (184, "Italy (IT) CEI 0-16"),
    (185, "Spain (ES) RD 1699"), (186, "Austria(AT) TOR"), (187, "Czech(CZ) EN50549-1 CZ"),
    (188, "Belgium (BE) C10/11 2021"), (189, "Netherlands(NL) NCRfG NL"),
    (190, "Portugal(PT) EN50549-1 PT"), (191, "Portugal(PT) EN50549-2 PT"),
    (192, "China(CHN) GB/T 37408"), (193, "China(CHN) NB/T 32004"),
    (195, "Sweden (SE) EIFS 2018:2-EnergiForetagen"), (196, "Romania(RO) ANRE Ordin"),
    (197, "Türkiye (TR) EPDK"), (198, "Poland (PL) NC RfG PSE:2018"), (199, "Finland (FI) VJV"),
    (200, "Ireland(IRL) ESB"), (201, "Ireland (IRL) ESB EirGrid"),
    (202, "Denmark(DK2) NC RfG DK:2019"), (203, "Denmark(DK1) NC RfG DK:2019"),
    (204, "Cyprus(CY)"), (205, "Norway (NO) RENBLAD 342"), (206, "Norway (NO) NVF"),
    (207, "Slovenia(SI)"), (209, "Lithuania"), (211, "Denmark West (DK1) MV/HV NC RfG DK"),
    (212, "Denmark East (DK2) MV/HV NC RfG DK"), (213, "United States (US) PRC-024-3"),
    (214, "Uzbekistan"),
]
ENUMS = {
    "device_state": DEV_STATE_MAP,
    "output_type": OUTPUT_TYPE_MAP,
    "battery_status": BATTERY_STATUS_MAP,
    "charging_status": CHARGING_STATUS_MAP,
    "battery_balancing": BATTERY_BALANCING_MAP,
    "battery_contactor": BATTERY_CONTACTOR_MAP,
    "battery_soc_calibration": BATTERY_SOC_CALIBRATION_MAP,
    "battery_module_cell_type": BATTERY_MODULE_CELL_TYPE_MAP,
    "no_yes": NO_YES_MAP,
    "charger_full": CHARGER_FULL_MAP,
    "charger_phase": CHARGER_PHASE_MAP,
    "charger_power_flag": CHARGER_POWER_FLAG_MAP,
    "charger_start_mode": CHARGER_START_MODE_MAP,
    "device_type_code": DEVICE_TYPE_CODE_MAP,
    "grid_standard": GRID_STANDARD_MAP,
    "enable_switch": ENABLE_SWITCH_MAP,
    "power_limit_reason": POWER_LIMIT_REASON_MAP,
    "drm_state": DRM_STATE_MAP,
    "reactive_power_mode": REACTIVE_POWER_MODE_MAP,
    "battery_type": BATTERY_TYPE_MAP,
    "country": COUNTRY_MAP,
}

# Core packages are driven by `category` in data/families/<family>.yaml (signal order =
# entity order). Per device: which tables feed it, which core categories apply, whether hybrid-scope
# entries are dropped, the running-state key, and extra non-override keys (SBR overlay /
# derived) inserted after a given key ("" = append).
DEVICES = {
    "hybrid_inverter": {
        "title": "Sungrow hybrid inverter (dev_type 35; poll table by protocol type: "
                 "inv_proto2/3 private map, inv_proto5 SH public map). Where the firmware "
                 "uses different sigids per map (key suffix _sh / _pub), core entities list "
                 "them as one key fallback list; the first valid one is published.",
        "family": "inverter", "device": 0, "prefix": "Inverter ",
        "tables": ["inv_proto2", "inv_proto3", "inv_proto5"],
        "core": ("core", "core_hybrid"), "drop_hybrid": False,
        "state": ["device_state"],
        "derived": [("total_dc_power", "mppt_total_power"), ("daily_yield", "total_yield"),
                    ("export_power", "grid_import_power"),
                    ("grid_import_power", "grid_export_power")],
    },
    "string_inverter": {
        "title": "Sungrow string (HOME) inverter (dev_type 21). Poll table by protocol type "
                 "(inv_proto1_4 SG public map, inv_proto2/3/5); where the firmware uses "
                 "different sigids per map (key suffix _sg / _sh / _pub) core entities list "
                 "them as one key fallback list; the first valid one is published.",
        "family": "inverter", "device": 0, "prefix": "Inverter ",
        "tables": ["inv_proto1_4", "inv_proto2", "inv_proto3", "inv_proto5"],
        "core": ("core", "core_string"), "drop_hybrid": True,
        "state": ["device_state"],
        "derived": [("total_dc_power", "mppt_total_power"), ("daily_yield", "total_yield"),
                    ("export_power", "grid_import_power"),
                    ("grid_import_power", "grid_export_power")],
    },
    "auto_inverter": {
        "title": "Sungrow inverter, auto-detecting union of string (dev_type 21) and hybrid "
                 "(dev_type 35) entities. The RS485 master already picks the poll table at "
                 "runtime from the devinfo block (table: auto); this package compiles in every "
                 "inverter entity of both device types so no build-time choice is needed. "
                 "Entities whose signal the connected inverter does not report stay "
                 "unavailable (sensors publish NaN). Protocol-suffixed duplicates (_sg / _sh / "
                 "_pub) collapse into one entity with a key fallback list; the first valid "
                 "signal is published.",
        "family": "inverter", "device": 0, "prefix": "Inverter ",
        "tables": ["inv_proto1_4", "inv_proto2", "inv_proto3", "inv_proto5"],
        "core": ("core", "core_string", "core_hybrid"), "drop_hybrid": False,
        "state": ["device_state"],
        "derived": [("total_dc_power", "mppt_total_power"), ("daily_yield", "total_yield"),
                    ("export_power", "grid_import_power"),
                    ("grid_import_power", "grid_export_power")],
    },
    "sbr_battery": {
        "title": "Sungrow SBR battery stack (dev_type 44, battery table + SBR cell block "
                 "10757..10780)",
        "family": "battery", "device": 1, "prefix": "SBR ",
        "tables": ["battery"],
        "core": ("core",), "drop_hybrid": False, "state": [],
        "derived": [("", k) for k in (
            "cell_voltage_max", "cell_voltage_min", "cell_voltage_spread",
            "cell_voltage_max_module", "cell_voltage_max_cell", "cell_voltage_min_module",
            "cell_voltage_min_cell", "module_temperature_max", "module_temperature_min",
            "module_temperature_max_module", "module_temperature_min_module",
            "active_fault_count", "active_fault_codes", "charge_prohibited",
            "discharge_prohibited")],
        "exclude_re": r"_position$",
    },
    "meter": {
        "title": "Sungrow meter (dev_type 18; tables: st6100 = Acrel "
                 "DTSD1352, st6200 = DTSU666, st6300 = S100 single-phase)",
        "family": "meter", "device": 2, "prefix": "Meter ",
        "tables": ["other_dev18_st6100", "other_dev18_st6200", "other_dev18_st6300"],
        "core": ("core",), "drop_hybrid": False, "state": [], "derived": [],
    },
}


def yaml_str(s):
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def entity_yaml(platform, family, key, name, extra=None, keys=None):
    out = ["  - platform: sungrow_datamodel\n",
           "    sungrow_datamodel_id: ${dm_id}\n",
           "    device: ${device}\n",
           "    family: %s\n" % family,
           "    key: %s\n" % (("[%s]" % ", ".join(keys)) if keys else key),
           "    name: %s\n" % yaml_str("${prefix}" + name)]
    for line in extra or []:
        out.append("    " + line + "\n")
    return "".join(out)


KEY_SUFFIXES = ("_sh", "_pub", "_sg")
KEY_SUFFIX_RE = re.compile(r"_(sh|pub|sg)$")


def device_plan(tables, meta, ov, dname):
    """-> dict(core=[keys], text=[keys], state=[keys], extra_num=[keys], extra_str=[keys])
    for one device package (deterministic: override file order, then sigid order)."""
    d = DEVICES[dname]
    tmap = {t["name"]: t for t in tables}
    fam = meta[d["family"]]
    bykey = {m["key"]: (s, m) for s, m in fam.items()}
    present = set()
    for tn in d["tables"]:
        for r in tmap[tn]["rows"]:
            if is_named(r) and r["sigid"] in fam:
                present.add(fam[r["sigid"]]["key"])
    present |= {m["key"] for m in fam.values() if m["source"] == "derived"}

    def dropped(k):
        m = bykey[k][1]
        if m["ov"] and m["ov"].get("scope") == "hybrid" and d["drop_hybrid"]:
            return True
        return bool(d.get("exclude_re") and re.search(d["exclude_re"], k))

    order = sorted((e for (f, _s), e in ov["names"].items() if f == d["family"]),
                   key=lambda e: e["_order"])
    core = [e["key"] for e in order if e["category"] in d["core"] and e["key"] in present
            and not dropped(e["key"])]
    for after, k in d["derived"]:
        if k not in present:
            raise SystemExit("%s: derived/overlay key %r not present" % (dname, k))
        if after:
            if after not in core:
                raise SystemExit("%s: %r not in core" % (dname, after))
            core.insert(core.index(after) + 1, k)
        else:
            core.append(k)
    # Per-MPPT power right after each core MPPT current.
    for k in list(core):
        mm = re.match(r"^mppt(\d+)_current$", k)
        if mm and "mppt%s_power" % mm.group(1) in present:
            core.insert(core.index(k) + 1, "mppt%s_power" % mm.group(1))
    for k in d["state"]:
        if k not in present:
            raise SystemExit("%s: state key %r not present" % (dname, k))
    state = list(d["state"])

    def hidden(k):
        return bool(bykey[k][1]["ov"] and bykey[k][1]["ov"]["category"] == "hidden")

    # Collapse protocol-suffixed duplicates (_sh/_pub/_sg, see data/families/inverter.yaml) into one
    # entity with a key fallback list: only the sigid of the inverter's protocol is populated.
    groups, merged, used = {}, [], set()
    for k in core:
        if k in used:
            continue
        b = KEY_SUFFIX_RE.sub("", k)
        cand = [b] + [b + suf for suf in KEY_SUFFIXES]
        members = [x for x in cand if x in present and not dropped(x) and not hidden(x)
                   and bykey[x][1]["string"] == bykey[k][1]["string"]]
        if k not in members or len(members) == 1:
            members = [k]
        merged.append(members[0])
        used |= set(members)
        if len(members) > 1:
            groups[members[0]] = members
    core = merged
    text = [k for k in core if bykey[k][1]["string"]]
    core = [k for k in core if k not in text and k not in state]
    done = set(core) | set(text) | set(state) | used
    rest = sorted((k for k in present if k not in done and not dropped(k)
                   and not (bykey[k][1]["ov"] and bykey[k][1]["ov"]["category"] == "hidden")),
                  key=lambda k: bykey[k][0])
    return {"core": core, "text": text, "state": state, "bykey": bykey, "groups": groups,
            "extra_num": [k for k in rest if not bykey[k][1]["string"]],
            "extra_str": [k for k in rest if bykey[k][1]["string"]]}


def emit_devices(tables, meta, ov, dev_dir, issues):
    for dname in sorted(DEVICES):
        d = DEVICES[dname]
        p = device_plan(tables, meta, ov, dname)
        bykey = p["bykey"]
        head = ("# GENERATED by tools/gen_tables.py - DO NOT EDIT (curate in "
                "data/families/<family>.yaml).\n"
                "# %s\n"
                "# Usage:\n"
                "#   packages:\n"
                "#     %s: !include { file: devices/%s.yaml, vars: { device: %d } }\n"
                "# Unit / device_class / state_class / accuracy are filled in from the register\n"
                "# tables at codegen time and can be overridden per entity.\n"
                % ("\n# ".join(textwrap.wrap(d["title"], 86)), dname, dname, d["device"]))
        defaults = ("defaults:\n  dm_id: sungrow_dm\n  device: %d\n  prefix: \"%s\"\n\n"
                    % (d["device"], d["prefix"]))
        names_used = set()

        def uname(m):
            n = m["name"]
            if slug(n) in names_used:
                n = "%s (%s)" % (n, m["key"])
            names_used.add(slug(n))
            return n

        main = [head, defaults, "sensor:\n"]
        for k in p["core"]:
            main.append(entity_yaml("sensor", d["family"], k, uname(bykey[k][1]),
                                    keys=p["groups"].get(k)))
        if p["text"] or p["state"]:
            main.append("\ntext_sensor:\n")
            for k in p["text"]:
                main.append(entity_yaml("text_sensor", d["family"], k, uname(bykey[k][1]),
                                        ["entity_category: diagnostic"], keys=p["groups"].get(k)))
            for k in p["state"]:
                extra = ["# Merged SG (5038) + SH (13000) state codes; unknown codes show as hex.",
                         "map:"]
                extra += ["  0x%04X: %s" % (code, yaml_str(txt)) for code, txt in DEV_STATE_MAP]
                main.append(entity_yaml("text_sensor", d["family"], k, uname(bykey[k][1]),
                                        extra))
        write(os.path.join(dev_dir, dname + ".yaml"), "".join(main))

        ex = [head.replace("devices/%s.yaml" % dname, "devices/%s_extra.yaml" % dname)
              .replace("# Usage:", "# Optional extra entities (everything named that is not in "
                                   "the core set).\n# Usage:"), defaults]
        if p["extra_num"]:
            ex.append("sensor:\n")
            for k in p["extra_num"]:
                ex.append(entity_yaml("sensor", d["family"], k, uname(bykey[k][1]),
                                      ["disabled_by_default: true"]))
        if p["extra_str"]:
            ex.append("\ntext_sensor:\n")
            for k in p["extra_str"]:
                ex.append(entity_yaml("text_sensor", d["family"], k, uname(bykey[k][1]),
                                      ["disabled_by_default: true",
                                       "entity_category: diagnostic"]))
        write(os.path.join(dev_dir, dname + "_extra.yaml"), "".join(ex))


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def build(data_dir=DEFAULT_DATA_DIR, issues=None):
    """Load + check the tables. -> (tables, overrides, meta). Used by main() and tests."""
    issues = [] if issues is None else issues
    check_state_tables()
    tables, data = load_tables(data_dir)
    ov = load_overrides(data)
    apply_overrides(tables, ov, issues)
    check_quality(tables, issues, ov["shared"])
    meta = assign_keys(tables, issues)
    return tables, ov, meta


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--data-dir", default=DEFAULT_DATA_DIR,
                    help="directory holding families/ and modbus_tcp/ (default data/)")
    ap.add_argument("--out-root", default=FW_ROOT,
                    help="firmware root (components/ and devices/ are written below it)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print data-quality notes")
    a = ap.parse_args(argv)
    issues = []
    tables, ov, meta = build(a.data_dir, issues)
    comp_dir = os.path.join(a.out_root, "components", "sungrow_datamodel")
    stats = emit_cpp(tables, meta, comp_dir)
    emit_meta_py(tables, meta, ov, comp_dir)
    emit_devices(tables, meta, ov, os.path.join(a.out_root, "devices"), issues)
    print("tables=%d rows=%d (~%d B rows + ~%d B keys) keys=%d units=%d mppt_pairs=%d" % (
        len(tables), stats["rows"], stats["row_bytes"], stats["key_bytes"], stats["keys"],
        len(stats["units"]), stats["mppt_pairs"]))
    if a.verbose:
        for t in tables:
            reqs = build_requests_py(t["rows"])
            print("  %-20s %-13s rows=%3d named=%3d requests=%3d%s" % (
                t["name"], t["family"], len(t["rows"]),
                sum(1 for r in t["rows"] if is_named(r)), len(reqs),
                "  (>59 cap!)" if len(reqs) > 59 else ""))
        print("data-quality notes:")
        for i in issues:
            print("  - " + i)
    return 0


if __name__ == "__main__":
    sys.exit(main())
