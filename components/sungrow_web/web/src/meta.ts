// Presentation metadata for signals, baked from register_meta.py into meta.gen.ts.
//
// /api/signals keys are bare sigid hex ("0x2080"); this maps them to human label + unit +
// device_class + display precision + multiplier. The same sigid can mean different things in
// different device families, so lookups are family-scoped (the device's family comes from
// /api/devices). The Installer catalogue helpers at the end label and format holding registers
// per grid-code XML family.

import { INSTALLER_SETTINGS, type InstallerSetting } from "./installer_settings.gen.js";
import { META, type MetaTuple, WRITE_META, type WriteDef } from "./meta.gen.js";
import { rawToDisplay as settingDisplay } from "./setup_logic.js";
import { SET_LABELS } from "./setup_profiles.gen.js";

export type { WriteDef } from "./meta.gen.js";

// Unranked sentinel — matches the data layer's default priority (9999 = "no curated rank").
// Signals at this value sort last and are excluded from the key-metric summary row.
export const DEFAULT_PRIORITY = 9999;

export interface SignalMeta {
  label: string;
  unit: string;
  deviceClass: string;
  precision: number; // -1 = unspecified
  mult: number; // displayed value = raw signal value * mult
  priority: number; // sort rank, lower = more important (DEFAULT_PRIORITY when uncurated)
  group: string; // "static" | "realtime" | "protection" (default "realtime")
  enumMap: [number, string][] | null; // raw -> text, for enum-decoded reads
}

// MetaTuple (generated): [label, unit, deviceClass, precision, mult, priority, group, enum].
function fromTuple(t: MetaTuple): SignalMeta {
  return {
    label: t[0],
    unit: t[1],
    deviceClass: t[2],
    precision: t[3],
    mult: t[4],
    priority: t[5],
    group: t[6],
    enumMap: t[7],
  };
}

/**
 * Strict, family-scoped lookup: ONLY the given family's table, never a cross-family fallback.
 * Per-device views MUST use this — a sigid can mean different things in different families (e.g.
 * 0x201D is "Battery SOC" in the battery family but not a signal of the inverter family), so an
 * inverter must never borrow a battery label. Returns null when the sigid is not a known signal
 * of this family (callers hide those).
 */
export function lookupMetaStrict(family: string | undefined, key: string): SignalMeta | null {
  const fam = family ? META[family] : undefined;
  const hit = fam ? fam[key] : undefined;
  return hit ? fromTuple(hit) : null;
}

/**
 * Resolve a device's family: trust the firmware-reported `family` (from /api/devices), else a
 * conservative client fallback from dev_type when the entry carries no `family`. Inverter
 * dev_types are 21 (string), 35 (hybrid), 1 (other).
 */
export function deviceFamily(d: { family?: string; dev_type: number }): string | undefined {
  if (d.family) return d.family;
  if (d.dev_type === 21 || d.dev_type === 35 || d.dev_type === 1) return "inverter";
  if (d.dev_type === 44) return "battery";
  if (d.dev_type === 46) return "charger";
  if (d.dev_type === 92) return "energy_bridge";
  if (d.dev_type === 18) return "meter";
  return undefined;
}

/** Format a numeric/string signal value using the meta multiplier + precision, with unit. */
function formatValue(value: number | string | null, meta: SignalMeta | null): string {
  if (value === null) return "—";
  if (typeof value === "string") return value;
  let v = value;
  if (meta) v *= meta.mult;
  const text = formatNumber(v, meta?.precision ?? -1);
  const unit = meta?.unit ?? "";
  return unit ? `${text} ${unit}` : text;
}

/** Format a number to a given precision (-1 = trim trailing zeros, cap at 3 dp). */
export function formatNumber(v: number, precision: number): string {
  if (!Number.isFinite(v)) return "—";
  if (precision >= 0) return v.toFixed(precision);
  if (Number.isInteger(v)) return String(v);
  return v.toFixed(3).replace(/\.?0+$/, "");
}

/**
 * Format a signal value for display, decoding enums (operating status, output type, ...) to text
 * via the family's enum map before falling back to numeric/multiplier/unit formatting. Used by the
 * per-device views so e.g. operating status shows "Grid-connected operation", not "0".
 */
export function formatSignal(family: string | undefined, key: string, value: number | string | null): string {
  const meta = lookupMetaStrict(family, key);
  if (value !== null && typeof value === "number" && meta?.enumMap) {
    const hit = meta.enumMap.find(([raw]) => raw === value);
    if (hit) return hit[1];
  }
  return formatValue(value, meta);
}

// ---- writable registers ----------------------------------------------------------------------

/** Map a device dev_type to its control profile. 21 string, 35 hybrid, 1 other -> string. */
export function profileForDevType(devType: number): string {
  return devType === 35 ? "hybrid" : "string";
}

/**
 * Control profile of a device: the one the firmware attached (`control.profile`; null = none, so
 * no controls), or by device type when the server does not report one.
 */
export function controlProfile(dev: { dev_type: number; control?: { profile?: string | null } }): string | null {
  if (dev.control && dev.control.profile !== undefined) return dev.control.profile;
  return profileForDevType(dev.dev_type);
}

/** Writable defs for a profile, in the catalogue's stable key order. */
export function writeDefsForProfile(profile: string): WriteDef[] {
  return Object.keys(WRITE_META)
    .map((k) => WRITE_META[k])
    .filter((d) => d.profile === profile);
}

/** Display value (unit-space) -> raw register value, clamped to the raw allow-list range. */
export function displayToRaw(def: WriteDef, display: number): number {
  const raw = Math.round(display / (def.scale || 1));
  return Math.max(def.rawMin, Math.min(def.rawMax, raw));
}

/** Raw register value -> display value (unit-space). */
export function rawToDisplay(def: WriteDef, raw: number): number {
  return raw * (def.scale || 1);
}

// ---- Installer catalogue (grid-code XML family) -----------------------------------------------

/** The Installer catalogue row of `family` at `address` (the low word for U32 rows). */
export function catalogueRow(family: string, address: number): InstallerSetting | undefined {
  return INSTALLER_SETTINGS.find((s) => s.family === family && s.address === address);
}

/** Human label of a holding register in a family: catalogue row, profile label, else the address. */
export function registerLabel(family: string, address: number): string {
  return catalogueRow(family, address)?.label ?? SET_LABELS[`${family}:${address}`] ?? `Register ${address}`;
}

/** Format a setting's raw value: option label for enums, else scaled number with unit. */
export function formatSettingRaw(s: InstallerSetting, raw: number): string {
  if (s.options) {
    const o = s.options.find((x) => x.value === raw);
    if (o) return o.label;
  }
  const t = formatNumber(settingDisplay(s, raw), s.accuracy);
  return s.unit ? `${t} ${s.unit}` : t;
}
