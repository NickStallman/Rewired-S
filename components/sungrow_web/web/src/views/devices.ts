import {
  api,
  descriptorRegs,
  isIdentified,
  isRealFamily,
  readRegisters,
  splitValues,
  VERDICT_TEXT,
  type DeviceInfo,
  type ExpectInfo,
  type SignalValue,
} from "../api.js";
import { clear, el } from "../dom.js";
import {
  catalogueRow,
  controlProfile,
  DEFAULT_PRIORITY,
  deviceFamily,
  displayToRaw,
  formatNumber,
  formatSettingRaw,
  formatSignal,
  lookupMetaStrict,
  rawToDisplay,
  writeDefsForProfile,
  type WriteDef,
} from "../meta.js";
import { PROTECTION_READS, type ProtectionRead } from "../meta.gen.js";
import { FAMILY_DESCRIPTORS } from "../setup_profiles.gen.js";
import type { View } from "./types.js";

const DEVICES_POLL_MS = 5000;
const SIGNALS_POLL_MS = 2000;
const STALE_MS = 30000;
// A decided start/stop outcome stays on its control row this long.
const EXPECT_FINAL_SHOW_MS = 60000;

// Routine (always-visible) writable keys per profile; the rest go under a collapsed "Advanced".
const ROUTINE_KEYS: Record<string, Set<string>> = {
  string: new Set(["start_stop", "power_limit_switch", "power_limit_pct", "power_limit_kw"]),
  hybrid: new Set([
    "ems_mode",
    "min_soc",
    "max_soc",
    "export_power_limit",
    "forced_cmd",
    "forced_power",
    "battery_max_charge_power",
    "battery_max_discharge_power",
  ]),
};

// Stock device-type names; stock has none for 44 / 92 and shows the
// i18n "Battery" / "EnergyBridge" texts instead.
function devTypeName(t: number): string {
  switch (t) {
    case 1:
      return "String inverter";
    case 21:
      return "Residential PV inverter";
    case 35:
      return "Residential hybrid inverter";
    case 18:
      return "Meter";
    case 44:
      return "Battery";
    case 34:
      return "Sungrow logger";
    case 46:
      return "Charger";
    case 92:
      return "Energy Bridge";
    default:
      return `Type ${t}`;
  }
}

// Protocol type from the devinfo probe (stock names).
const PROTOCOL_NAMES: Record<number, string> = {
  0: "unknown",
  1: "residential/string",
  2: "AA10+",
  3: "ZS incremental",
  4: "LCD",
  5: "storage",
};

function protocolText(p: number): string {
  return `${p} (${PROTOCOL_NAMES[p] ?? "unknown"})`;
}

// Fallback names of the grid identity registers when the family catalogue has no row for them.
const GRID_REG_NAMES: Record<number, string> = { 30036: "Grid standard", 30037: "Grid type" };

/** Format a grid identity register: the family's catalogue row, else the unit catalogue / enum reads. */
function formatGridReg(xmlFamily: string, addr: number, raw: number): string {
  const s = catalogueRow(xmlFamily, addr) ?? catalogueRow("unit", addr);
  if (s) return formatSettingRaw(s, raw);
  const r = PROTECTION_READS.inverter?.find((x) => x.address === addr);
  return r?.enum?.find(([v]) => v === raw)?.[1] ?? String(raw);
}

function gridRegLabel(xmlFamily: string, addr: number, desc: { country: number | null; grid: number | null } | null): string {
  const row = catalogueRow(xmlFamily, addr) ?? catalogueRow("unit", addr);
  const name = desc?.country === addr ? "Country" : desc?.grid === addr ? "Grid code" : (GRID_REG_NAMES[addr] ?? row?.label ?? "Register");
  return `${name} (${addr})`;
}

// Only identified devices are shown: answered discovery, or typed by YAML / table inference. The bus
// scanner's empty slots (addr 1-8) never answer and carry no dev_type, so they stay hidden.

// In-place signal table. Rows are created once (keyed by sigid) and patched on each poll: the
// <tbody>, <tr> and <td> nodes are STABLE across polls — only a value cell's textContent and the
// stale class ever change, and DOM order is touched only when the row set actually changes. So an
// expanded card never flickers or reorders. Only signals that are BOTH valid AND a known signal of
// the device's OWN family are shown (no cross-family labels, no raw unmapped sigids) — mirroring
// the sensor set the MQTT publisher exposes.
class SignalTable {
  private table: HTMLTableElement;
  private tbody: HTMLTableSectionElement;
  private empty: HTMLElement;
  private rows = new Map<string, { tr: HTMLTableRowElement; val: HTMLElement; stale: boolean }>();
  private order: string[] = []; // current sorted keys, mirrors DOM row order

  constructor(
    root: HTMLElement,
    private family: string | undefined,
  ) {
    this.empty = el("p", { class: "muted" }, ["No live signals yet."]);
    this.tbody = el("tbody", {}, []) as HTMLTableSectionElement;
    this.table = el("table", { class: "signals" }, [
      el("thead", {}, [
        el("tr", {}, [el("th", {}, ["Signal"]), el("th", { class: "val" }, ["Value"])]),
      ]),
      this.tbody,
    ]) as HTMLTableElement;
    clear(root);
    root.append(this.empty, this.table);
    this.table.hidden = true;
  }

  update(signals: SignalValue[], nowMs: number): void {
    // Keep only valid, in-family, mapped REALTIME signals with a real value. Static facts and
    // protection params are rendered by their own panels (left column / Protection section).
    const shown = signals.filter((s) => {
      if (!s.valid || s.value === null) return false;
      const m = lookupMetaStrict(this.family, s.key);
      return m !== null && m.group !== "static" && m.group !== "protection" && m.group !== "hidden";
    });
    // Our own importance order: by priority (lower = first), tie-break by label; unranked last.
    shown.sort((a, b) => {
      const ma = lookupMetaStrict(this.family, a.key)!;
      const mb = lookupMetaStrict(this.family, b.key)!;
      return ma.priority - mb.priority || ma.label.localeCompare(mb.label) || a.key.localeCompare(b.key);
    });

    const seen = new Set<string>();
    for (const s of shown) {
      try {
        seen.add(s.key);
        const meta = lookupMetaStrict(this.family, s.key)!;
        const stale = nowMs - s.updated_ms > STALE_MS;
        const valueText = formatSignal(this.family, s.key, s.value); // enum-decoded where applicable
        let row = this.rows.get(s.key);
        if (!row) {
          const label = el("td", { class: "sig-label", title: s.key }, [meta.label]);
          const val = el("td", { class: "val" }, [valueText]);
          const tr = el("tr", {}, [label, val]) as HTMLTableRowElement;
          row = { tr, val, stale: !stale };
          this.rows.set(s.key, row);
        } else if (row.val.textContent !== valueText) {
          row.val.textContent = valueText; // patch ONLY the value cell in steady state
        }
        if (row.stale !== stale) {
          row.tr.classList.toggle("stale", stale);
          row.stale = stale;
        }
      } catch {
        /* skip this one signal; the rest of the table still renders */
      }
    }

    // Remove rows whose signal is absent from this reply (invalid or unmapped).
    for (const key of [...this.rows.keys()]) {
      if (!seen.has(key)) {
        this.rows.get(key)!.tr.remove();
        this.rows.delete(key);
      }
    }

    // Reconcile DOM order ONLY when the key list actually changed (steady state = no DOM ops).
    // Restrict to keys whose row actually exists (a build may have been skipped on error).
    const want = shown.map((s) => s.key).filter((k) => this.rows.has(k));
    let changed = want.length !== this.order.length;
    for (let i = 0; !changed && i < want.length; i++) changed = want[i] !== this.order[i];
    if (changed) {
      for (const key of want) this.tbody.appendChild(this.rows.get(key)!.tr);
      this.order = want;
    }

    const hasRows = this.rows.size > 0;
    if (!hasRows && this.empty.textContent !== "No live signals yet.") {
      this.empty.textContent = "No live signals yet."; // clear any prior error text
    }
    this.empty.hidden = hasRows;
    this.table.hidden = !hasRows;
  }

  error(message: string): void {
    this.empty.textContent = `signals unavailable: ${message}`;
    this.empty.hidden = false;
    this.table.hidden = this.rows.size === 0;
  }
}

// One writable register: a labelled control (select / number / enable-toggle) plus a Set button,
// its live read-back value, and the last write verdict. The input value is only seeded once (and
// re-seeded when not focused) so a live poll never clobbers what the user is typing.
class ControlRow {
  readonly root: HTMLElement;
  private currentEl: HTMLElement;
  private input: HTMLSelectElement | HTMLInputElement;
  private setBtn: HTMLButtonElement;
  private result: HTMLElement;
  private seeded = false;

  constructor(
    private def: WriteDef,
    private onSet: (def: WriteDef, raw: number) => Promise<void>,
  ) {
    this.currentEl = el("span", { class: "ctrl-current" }, ["—"]);
    if (def.options.length) {
      const sel = el("select", { class: "ctrl-input" }, []) as HTMLSelectElement;
      for (const [raw, label] of def.options) {
        sel.append(el("option", { value: String(raw) }, [label]));
      }
      this.input = sel;
    } else {
      this.input = el("input", {
        class: "ctrl-input",
        type: "number",
        min: def.numberMin,
        max: def.numberMax,
        step: def.step,
      }) as HTMLInputElement;
    }
    this.result = el("span", { class: "ctrl-result muted" }, []);
    this.setBtn = el("button", { class: "btn", type: "button", onClick: () => void this.fire() }, [
      "Set",
    ]) as HTMLButtonElement;

    const unit = def.unit && !def.options.length ? ` (${def.unit})` : "";
    this.root = el("div", { class: "ctrl-row" }, [
      el("div", { class: "ctrl-label" }, [
        el("span", { class: "ctrl-name" }, [def.name + unit]),
        this.currentEl,
      ]),
      el("div", { class: "ctrl-input-wrap" }, [this.input, this.setBtn]),
      this.result,
    ]);
  }

  private async fire(): Promise<void> {
    const raw = this.readRaw();
    if (raw === null) {
      this.setResult("enter a value", false);
      return;
    }
    this.setBtn.disabled = true;
    this.setResult("…", null);
    try {
      await this.onSet(this.def, raw);
    } finally {
      this.setBtn.disabled = false;
    }
  }

  private readRaw(): number | null {
    if (this.input instanceof HTMLSelectElement) {
      const v = Number(this.input.value);
      return Number.isFinite(v) ? v : null;
    }
    const n = Number(this.input.value);
    if (!Number.isFinite(n) || this.input.value.trim() === "") return null;
    return displayToRaw(this.def, n);
  }

  /** Update the live read-back display and seed the input once from the current value. */
  update(raw: number | null): void {
    if (raw === null) {
      setText(this.currentEl, "now: —");
      return;
    }
    if (this.input instanceof HTMLSelectElement) {
      const label = this.def.options.find(([r]) => r === raw)?.[1] ?? String(raw);
      setText(this.currentEl, `now: ${label}`);
      if (!this.seeded) this.input.value = String(raw);
    } else {
      const disp = rawToDisplay(this.def, raw);
      setText(this.currentEl, `now: ${trimNum(disp)}${this.def.unit ? " " + this.def.unit : ""}`);
      if (!this.seeded && document.activeElement !== this.input) this.input.value = trimNum(disp);
    }
    this.seeded = true;
  }

  setResult(text: string, ok: boolean | null): void {
    setText(this.result, text);
    this.result.classList.toggle("ok", ok === true);
    this.result.classList.toggle("err", ok === false);
  }
}

// The device's Controls section: routine rows + a collapsed Advanced block. Web-page writes are
// authorised by the login (the writes-enabled switch gates Modbus-TCP, MQTT and the HA action only).
// Values refresh in place from the same /api/signals poll as the table.
/** Battery-type signal (enum): 2 = "No battery". Battery controls are hidden while it reads so. */
const BATTERY_TYPE_SIGID = "0x2510";
const BATTERY_TYPE_NONE = 2;

export function hasBattery(map: Map<string, SignalValue>): boolean {
  const s = map.get(BATTERY_TYPE_SIGID);
  return !(s && s.valid && s.value === BATTERY_TYPE_NONE);
}

class ControlsPanel {
  readonly root: HTMLElement;
  private rows: { def: WriteDef; row: ControlRow }[] = [];
  private advancedSummary: HTMLElement | null = null;

  constructor(
    private device: number,
    private profile: string,
  ) {
    const defs = writeDefsForProfile(profile);
    const routine = ROUTINE_KEYS[profile] ?? new Set<string>();

    const routineBox = el("div", { class: "ctrl-group" }, []);
    const advancedBox = el("div", { class: "ctrl-group" }, []);
    let advancedCount = 0;
    for (const def of defs) {
      const row = new ControlRow(def, (d, raw) => this.write(d, raw));
      this.rows.push({ def, row });
      if (routine.has(def.key)) routineBox.append(row.root);
      else {
        advancedBox.append(row.root);
        advancedCount++;
      }
    }

    const children: (Node | string)[] = [
      el("h3", { class: "ctrl-heading" }, ["Controls"]),
      routineBox,
    ];
    if (advancedCount > 0) {
      this.advancedSummary = el("summary", {}, [`Advanced (${advancedCount})`]);
      const adv = el("details", { class: "ctrl-advanced" }, [this.advancedSummary, advancedBox]);
      children.push(adv);
    }
    this.root = el("div", { class: "controls" }, children);
  }

  update(map: Map<string, SignalValue>): void {
    const battery = hasBattery(map);
    const routine = ROUTINE_KEYS[this.profile] ?? new Set<string>();
    let advancedShown = 0;
    for (const { def, row } of this.rows) {
      const hidden = def.requiresBattery && !battery;
      row.root.hidden = hidden;
      if (!hidden && !routine.has(def.key)) advancedShown++;
      if (hidden) continue;
      const s = map.get(def.sigid);
      const raw = s && s.valid && typeof s.value === "number" ? s.value : null;
      row.update(raw);
    }
    if (this.advancedSummary) setText(this.advancedSummary, `Advanced (${advancedShown})`);
  }

  /** Show the start/stop follow-up on its control row while it is pending or freshly decided. */
  setExpect(expect: ExpectInfo | null | undefined): void {
    if (!expect || (expect.final && expect.age_ms >= EXPECT_FINAL_SHOW_MS)) return;
    const row = this.rows.find((r) => r.def.key === expect.key)?.row;
    row?.setResult(expect.text, expect.final ? expect.ok : null);
  }

  private async write(def: WriteDef, raw: number): Promise<void> {
    const row = this.rows.find((r) => r.def.key === def.key)?.row;
    try {
      const r = await api.write(this.device, def.key, raw);
      row?.setResult(r.ok ? "Queued ✓" : VERDICT_TEXT[r.verdict] ?? r.message, r.ok ? true : false);
    } catch (err) {
      row?.setResult(`error: ${(err as Error).message}`, false);
    }
  }
}

// Left-column static facts (group==="static": model, serial, firmware, output type, rated power).
// Rendered as a label/value list, enum-decoded, patched in place. Static values rarely change.
class StaticPanel {
  readonly root: HTMLElement;
  private dl: HTMLElement;
  private rows = new Map<string, HTMLElement>(); // sigid -> <dd>

  constructor(private family: string | undefined) {
    this.dl = el("dl", { class: "static-kv" }, []);
    this.root = el("div", { class: "static-panel", hidden: true }, [el("h3", {}, ["Device info"]), this.dl]);
  }

  update(signals: SignalValue[]): void {
    const shown = signals
      .filter((s) => {
        if (!s.valid || s.value === null) return false;
        const m = lookupMetaStrict(this.family, s.key);
        return m !== null && m.group === "static";
      })
      .sort(byPriority(this.family));
    for (const s of shown) {
      try {
        const meta = lookupMetaStrict(this.family, s.key)!;
        const text = formatSignal(this.family, s.key, s.value);
        let dd = this.rows.get(s.key);
        if (!dd) {
          dd = el("dd", {}, [text]);
          this.rows.set(s.key, dd);
          this.dl.append(el("dt", {}, [meta.label]), dd); // discovery order; static facts are stable
        } else setText(dd, text);
      } catch {
        /* skip one fact */
      }
    }
    this.root.hidden = this.rows.size === 0;
  }
}

// Compact key-metric summary row (top of the body): the device's top-priority signals as chips so
// each device type leads with its headline values. Curated (priority < DEFAULT) signals only.
const SUMMARY_MAX = 6;
class SummaryRow {
  readonly root: HTMLElement;
  private chips = new Map<string, { chip: HTMLElement; val: HTMLElement }>();
  private order: string[] = [];

  constructor(private family: string | undefined) {
    this.root = el("div", { class: "summary-row" }, []);
  }

  update(signals: SignalValue[]): void {
    const ranked = signals
      .filter((s) => {
        if (!s.valid || s.value === null) return false;
        const m = lookupMetaStrict(this.family, s.key);
        return (
          m !== null &&
          m.priority < DEFAULT_PRIORITY &&
          m.group !== "protection" &&
          m.group !== "static" &&
          m.group !== "hidden"
        );
      })
      .sort(byPriority(this.family))
      .slice(0, SUMMARY_MAX);

    const want = ranked.map((s) => s.key);
    const wantSet = new Set(want);
    for (const k of [...this.chips.keys()]) {
      if (!wantSet.has(k)) {
        this.chips.get(k)!.chip.remove();
        this.chips.delete(k);
      }
    }
    for (const s of ranked) {
      try {
        const meta = lookupMetaStrict(this.family, s.key)!;
        const text = formatSignal(this.family, s.key, s.value);
        let c = this.chips.get(s.key);
        if (!c) {
          const val = el("b", {}, [text]);
          c = { chip: el("div", { class: "summary-chip" }, [val, el("span", {}, [meta.label])]), val };
          this.chips.set(s.key, c);
        } else setText(c.val, text);
      } catch {
        /* skip one chip */
      }
    }
    let changed = want.length !== this.order.length;
    for (let i = 0; !changed && i < want.length; i++) changed = want[i] !== this.order[i];
    if (changed) {
      for (const k of want) if (this.chips.has(k)) this.root.appendChild(this.chips.get(k)!.chip);
      this.order = want.filter((k) => this.chips.has(k));
    }
    this.root.hidden = this.chips.size === 0;
  }
}

// Protection / grid-code params: read on demand through the device's read cache (/api/grid_read),
// read-only here. Editing lives on the Installer page. Rows the inverter does not support (invalid,
// or the 0xFFFF / 0x7FFF "not supported" raw) are hidden.
class ProtectionPanel {
  readonly root: HTMLElement;
  private statusEl: HTMLElement;
  private rows = new Map<number, { root: HTMLElement; valueEl: HTMLElement }>();

  constructor(
    private reads: ProtectionRead[],
    onRefresh: () => void,
  ) {
    const list = el("div", {}, []);
    for (const r of reads) {
      const valueEl = el("span", { class: "prot-value" }, ["—"]);
      const row = el("div", { class: "prot-row", hidden: true }, [
        el("span", { class: "prot-name", title: `@${r.address}` }, [r.label]),
        valueEl,
      ]);
      list.append(row);
      this.rows.set(r.address, { root: row, valueEl });
    }
    this.statusEl = el("span", { class: "muted" }, []);
    this.root = el("details", { class: "protection" }, [
      el("summary", {}, ["Protection / grid code"]),
      el("p", { class: "protection-note muted" }, [
        "Grid-code and protection limits (read-only here). Use ",
        el("strong", {}, ["Installer"]),
        " to review and change individual values. ",
        this.statusEl,
        " ",
        el("button", { class: "linkish", type: "button", onClick: onRefresh }, ["Refresh"]),
      ]),
      list,
    ]);
  }

  setStatus(text: string): void {
    setText(this.statusEl, text);
  }

  update(values: Map<number, number>): void {
    for (const r of this.reads) {
      const row = this.rows.get(r.address)!;
      const v = values.get(r.address);
      const unsupported = v === undefined || (r.dtype === "s16" ? v === 0x7fff : v === 0xffff);
      row.root.hidden = unsupported;
      if (unsupported) continue;
      setText(row.valueEl, formatProtection(r, v));
    }
  }
}

function formatProtection(r: ProtectionRead, raw: number): string {
  const hit = r.enum?.find(([v]) => v === raw);
  if (hit) return hit[1];
  const n = (r.dtype === "s16" && raw >= 0x8000 ? raw - 0x10000 : raw) * r.scale * r.mult;
  const t = formatNumber(n, r.precision);
  return r.unit ? `${t} ${r.unit}` : t;
}

// Shared comparator: importance order (priority asc), tie-break label then key.
function byPriority(family: string | undefined): (a: SignalValue, b: SignalValue) => number {
  return (a, b) => {
    const ma = lookupMetaStrict(family, a.key);
    const mb = lookupMetaStrict(family, b.key);
    const pa = ma?.priority ?? DEFAULT_PRIORITY;
    const pb = mb?.priority ?? DEFAULT_PRIORITY;
    return pa - pb || (ma?.label ?? a.key).localeCompare(mb?.label ?? b.key) || a.key.localeCompare(b.key);
  };
}

// One device card, created once and patched in place across /api/devices polls so the list is
// never re-rendered wholesale (which flickered and reset page scroll). Owns its own signal poller
// and grid-register read while expanded; expansion survives polls because the card node is stable.
class DeviceCard {
  readonly root: HTMLElement;
  readonly index: number;
  private readonly family: string | undefined;
  private last: DeviceInfo;
  private dot: HTMLElement;
  private titleEl: HTMLElement;
  private sub: HTMLElement;
  private stateDd: HTMLElement;
  private ident: Record<"protocol" | "proto_code" | "inv_type" | "xml_family", HTMLElement>;
  private gridKv: HTMLElement;
  private cells: Record<"sent" | "recv" | "timeouts" | "loss" | "age", HTMLElement>;
  private head: HTMLButtonElement;
  private body: HTMLElement;
  private tools: HTMLElement;
  private slot: HTMLElement;
  private leftCol: HTMLElement;
  private controls: ControlsPanel | null = null;
  private controlsProfile: string | null = null; // profile the controls panel was built for
  private signalTable: SignalTable;
  private staticPanel: StaticPanel;
  private summaryRow: SummaryRow;
  private protectionPanel: ProtectionPanel | null = null;
  private stopPoll: (() => void) | null = null;
  private gridAbort: AbortController | null = null;
  private open: boolean;

  constructor(d: DeviceInfo, open: boolean) {
    this.index = d.index;
    this.family = deviceFamily(d);
    this.last = d;
    this.open = open;

    this.dot = el("span", { class: "dot" }, []);
    this.titleEl = el("span", { class: "title" }, []);
    this.sub = el("span", { class: "sub" }, []);
    this.head = el(
      "button",
      { class: "card-head", "aria-expanded": open, onClick: () => this.toggle() },
      [
        this.dot,
        el("span", {}, [this.titleEl, el("br", {}, []), this.sub]),
        el("span", { class: "chev", "aria-hidden": "true" }, ["›"]),
      ],
    ) as HTMLButtonElement;

    this.stateDd = el("dd", {}, []);
    this.ident = {
      protocol: el("dd", {}, []),
      proto_code: el("dd", {}, []),
      inv_type: el("dd", {}, []),
      xml_family: el("dd", {}, []),
    };
    const sent = el("b", {}, []);
    const recv = el("b", {}, []);
    const timeouts = el("b", {}, []);
    const loss = el("b", {}, []);
    const age = el("b", {}, []);
    this.cells = { sent, recv, timeouts, loss, age };

    // Panels (created once, patched in place). The controls panel is built in sync() from the
    // profile the firmware reports for the device.
    this.staticPanel = new StaticPanel(this.family);
    this.summaryRow = new SummaryRow(this.family);
    this.slot = el("div", { class: "signals-slot" }, []);
    this.signalTable = new SignalTable(this.slot, this.family);
    const reads = this.family ? PROTECTION_READS[this.family] : undefined;
    if (reads?.length) {
      try {
        this.protectionPanel = new ProtectionPanel(reads, () => this.startGridRead());
      } catch {
        this.protectionPanel = null;
      }
    }

    const isInverter = this.family === "inverter";
    const factsKv = el("dl", { class: "kv" }, [
      el("dt", {}, ["Index"]),
      el("dd", {}, [String(d.index)]),
      el("dt", {}, ["Dev code"]),
      el("dd", {}, [`0x${d.dev_code.toString(16).padStart(4, "0")} (${d.dev_code})`]),
      el("dt", {}, ["Protocol"]),
      this.ident.protocol,
      el("dt", {}, ["Protocol code"]),
      this.ident.proto_code,
      ...(isInverter
        ? [el("dt", {}, ["Inverter type"]), this.ident.inv_type, el("dt", {}, ["XML family"]), this.ident.xml_family]
        : []),
      el("dt", {}, ["State"]),
      this.stateDd,
    ]);
    this.gridKv = el("dl", { class: "kv", hidden: true }, []);
    const statsRow = el("div", { class: "stats-row" }, [
      this.stat("sent", sent),
      this.stat("recv", recv),
      this.stat("timeouts", timeouts),
      this.stat("loss", loss),
      this.stat("data age", age),
    ]);
    this.leftCol = el("div", { class: "card-col" }, [factsKv, this.gridKv, statsRow, this.staticPanel.root]);
    const rightCol = el("div", { class: "card-col" }, [
      this.slot,
      ...(this.protectionPanel ? [this.protectionPanel.root] : []),
    ]);
    // Full-width summary row on top, then a two-column grid (collapses to one column narrow).
    this.body = el("div", { class: "card-body", hidden: !open }, [
      this.summaryRow.root,
      el("div", { class: "card-grid" }, [this.leftCol, rightCol]),
    ]);

    // Setup and Installer pages scoped to this device index, on the right of the card head; shown
    // only for an inverter with a grid-code XML family.
    this.tools = el("div", { class: "card-tools", hidden: true }, [
      el("a", { class: "btn", href: `#/setup/${this.index}` }, ["Setup"]),
      el("a", { class: "btn", href: `#/installer/${this.index}` }, ["Installer"]),
    ]);
    const headRow = el("div", { class: "card-head-row" }, [this.head, this.tools]);
    this.root = el("section", { class: "card" }, [headRow, this.body]);
    if (open) this.root.setAttribute("open", "");
    this.sync(d);
    if (open) this.startSignals();
  }

  private stat(label: string, value: HTMLElement): HTMLElement {
    return el("div", { class: "stat" }, [value, el("span", {}, [label])]);
  }

  /** Patch mutable fields from a fresh poll. Never rebuilds; leaves expansion/scroll untouched. */
  sync(d: DeviceInfo): void {
    this.last = d;
    const statusCls = !d.discovered ? "pending" : d.online ? "online" : "offline";
    const statusTxt = !d.discovered ? "discovering" : d.online ? "online" : "offline";
    const dotCls = `dot ${statusCls}`;
    if (this.dot.className !== dotCls) this.dot.className = dotCls;
    if (this.dot.title !== statusTxt) this.dot.title = statusTxt;
    // The S/N is part of the title so identical models (e.g. two SBR192 stacks) are told apart.
    const model = d.model || devTypeName(d.dev_type);
    const title = d.serial ? `${model} - ${d.serial}` : model;
    if (this.titleEl.textContent !== title) this.titleEl.textContent = title;
    const sub = `${devTypeName(d.dev_type)} · addr ${d.address} · ${statusTxt}`;
    if (this.sub.textContent !== sub) this.sub.textContent = sub;
    if (this.stateDd.textContent !== d.state) this.stateDd.textContent = d.state;
    setText(this.ident.protocol, protocolText(d.protocol));
    setText(this.ident.proto_code, d.proto_code || "—");
    setText(this.ident.inv_type, d.inv_type_known && d.inv_type !== undefined ? String(d.inv_type) : "unknown");
    setText(this.ident.xml_family, d.xml_family ?? "—");
    // Setup / Installer exist only for an inverter with a grid-code XML family (the family is
    // re-read here: a card created while the device was still discovering had none yet).
    this.tools.hidden = !(deviceFamily(d) === "inverter" && isRealFamily(d.xml_family));
    this.setCell(this.cells.sent, String(d.stats.sent));
    this.setCell(this.cells.recv, String(d.stats.received));
    this.setCell(this.cells.timeouts, String(d.stats.timeouts));
    this.setCell(this.cells.loss, d.stats.loss_pct < 0 ? "—" : `${d.stats.loss_pct.toFixed(1)}%`);
    // Age of the most frequently refreshed values when this list was fetched.
    const ageMs = d.poll?.fresh_age_ms ?? -1;
    this.setCell(this.cells.age, ageMs < 0 ? "—" : ageMs < 10000 ? `${(ageMs / 1000).toFixed(1)} s` : `${Math.round(ageMs / 1000)} s`);
    // Controls only for inverters, following the profile the firmware attached (it can arrive a
    // few polls after the card, once the device is discovered).
    if (this.family === "inverter") this.setControls(controlProfile(d));
    this.controls?.setExpect(d.control?.expect);
  }

  /** (Re)build the controls panel for `profile`; none for a device without a profile. Isolated so
   *  a build error can never prevent the card and signal list from rendering. */
  private setControls(profile: string | null): void {
    if (profile === this.controlsProfile) return;
    this.controlsProfile = profile;
    this.controls?.root.remove();
    this.controls = null;
    if (profile === null) return;
    try {
      this.controls = new ControlsPanel(this.index, profile);
      this.leftCol.append(this.controls.root);
    } catch {
      this.controls = null;
    }
  }

  private setCell(cell: HTMLElement, v: string): void {
    if (cell.textContent !== v) cell.textContent = v;
  }

  isOpen(): boolean {
    return this.open;
  }

  private toggle(): void {
    this.open = !this.open;
    if (this.open) {
      this.root.setAttribute("open", "");
      this.body.hidden = false;
      this.head.setAttribute("aria-expanded", "true");
      this.startSignals();
    } else {
      this.root.removeAttribute("open");
      this.body.hidden = true;
      this.head.setAttribute("aria-expanded", "false");
      this.stopSignals();
    }
  }

  /** The device's descriptor (from /api/devices, else the family's first stock descriptor). */
  private descriptor(): { country: number | null; grid: number | null } | null {
    const d = descriptorRegs(this.last.descriptor);
    if (d) return d;
    return this.last.xml_family ? (FAMILY_DESCRIPTORS[this.last.xml_family] ?? null) : null;
  }

  /** Grid identity registers of the device: descriptor country / grid, 30036, 30037. */
  private gridRegs(): number[] {
    const desc = this.descriptor();
    if (!desc) return [];
    return [...new Set([desc.country, desc.grid, 30036, 30037].filter((a): a is number => !!a))];
  }

  // One read per expansion (and on Refresh) of the grid identity registers plus the protection
  // reads. The server keeps one read cache (the latest start wins); the Installer page re-issues its own.
  private startGridRead(): void {
    if (this.family !== "inverter") return;
    this.gridAbort?.abort();
    const ctrl = new AbortController();
    this.gridAbort = ctrl;
    const ident = this.gridRegs();
    const prot = this.protectionPanel ? (PROTECTION_READS[this.family] ?? []).map((r) => r.address) : [];
    const addrs = [...new Set([...ident, ...prot])].sort((a, b) => a - b);
    if (addrs.length === 0) return;
    this.protectionPanel?.setStatus("Reading…");
    readRegisters(this.index, addrs, ctrl.signal, (r) => {
      const { values, reasons } = splitValues(r);
      this.renderGrid(ident, values, reasons);
      this.protectionPanel?.update(values);
    })
      .then((r) => this.protectionPanel?.setStatus(r?.complete ? "" : "Read incomplete."))
      .catch((err: Error) => {
        if (!ctrl.signal.aborted) this.protectionPanel?.setStatus(`Read failed: ${err.message}`);
      });
  }

  private renderGrid(addrs: number[], values: Map<number, number>, reasons: Map<number, string>): void {
    const fam = this.last.xml_family ?? "";
    const desc = this.descriptor();
    const items: HTMLElement[] = [];
    for (const a of addrs) {
      const v = values.get(a);
      const isDesc = a === desc?.country || a === desc?.grid;
      if (v === undefined && !isDesc) continue; // 30036/30037 only when this inverter has them
      const text = v !== undefined ? formatGridReg(fam, a, v) : reasons.has(a) ? `— (${reasons.get(a)})` : "…";
      items.push(el("dt", {}, [gridRegLabel(fam, a, desc)]), el("dd", {}, [text]));
    }
    this.gridKv.replaceChildren(...items);
    this.gridKv.hidden = items.length === 0;
  }

  private startSignals(): void {
    this.stopSignals();
    const ctrl = new AbortController();
    const tick = async (): Promise<void> => {
      try {
        const res = await api.signals(this.index, ctrl.signal);
        // Feed every panel from the one poll; each isolates its own render.
        this.signalTable.update(res.signals, res.now_ms); // realtime (right)
        this.staticPanel.update(res.signals); // static facts (left)
        this.summaryRow.update(res.signals); // key metrics (top)
        if (this.controls) {
          const map = new Map<string, SignalValue>();
          for (const s of res.signals) map.set(s.key, s);
          this.controls.update(map);
        }
      } catch (err) {
        if (ctrl.signal.aborted) return;
        this.signalTable.error((err as Error).message);
      }
    };
    void tick();
    const timer = window.setInterval(() => void tick(), SIGNALS_POLL_MS);
    this.stopPoll = () => {
      window.clearInterval(timer);
      ctrl.abort();
    };
    this.startGridRead();
  }

  private stopSignals(): void {
    this.stopPoll?.();
    this.stopPoll = null;
    this.gridAbort?.abort();
    this.gridAbort = null;
  }

  destroy(): void {
    this.stopSignals();
  }
}

export class DevicesView implements View {
  readonly id = "devices";
  readonly label = "Devices";

  private listEl!: HTMLElement;
  private devicesTimer = 0;
  private abort = new AbortController();
  private cards = new Map<number, DeviceCard>();
  private expanded = new Set<number>(); // remembered across view re-entry
  private loaded = false;

  render(root: HTMLElement): void {
    // Re-init per-mount state (destroyed on tab leave, re-rendered on return): fresh
    // AbortController + loaded=false so the list rebuilds and fetches don't use the aborted signal.
    // destroy() already tore down the old cards and remembered which were expanded.
    this.abort = new AbortController();
    this.loaded = false;
    this.cards.clear();

    root.append(el("div", { class: "view-head" }, [el("h1", { class: "view-title" }, ["Devices"])]));
    this.listEl = el("div", { class: "cards" }, [
      el("div", { class: "placeholder" }, [el("span", { class: "spin" }, []), " Loading devices…"]),
    ]);
    root.append(this.listEl);
    void this.loadDevices();
    this.devicesTimer = window.setInterval(() => void this.loadDevices(), DEVICES_POLL_MS);
  }

  destroy(): void {
    window.clearInterval(this.devicesTimer);
    this.abort.abort();
    for (const card of this.cards.values()) {
      if (card.isOpen()) this.expanded.add(card.index);
      card.destroy();
    }
    this.cards.clear();
  }

  private async loadDevices(): Promise<void> {
    let devices: DeviceInfo[];
    try {
      devices = (await api.devices(this.abort.signal)).filter(isIdentified);
    } catch (err) {
      if (this.abort.signal.aborted) return;
      if (!this.loaded) {
        clear(this.listEl); // replace "Loading…" with an error so it never hangs there
        this.listEl.append(
          el("div", { class: "banner err" }, [`Could not load devices: ${(err as Error).message}`]),
        );
      }
      return; // keep the last good list on a transient error after first load
    }
    try {
      this.reconcile(devices);
    } catch (err) {
      if (!this.loaded) {
        clear(this.listEl);
        this.listEl.append(el("div", { class: "banner err" }, [`Render error: ${(err as Error).message}`]));
      }
    }
  }

  // Patch existing cards in place; add new ones, remove gone ones. The list container is never
  // cleared once populated, so scroll position and expansion are preserved across polls. Each card
  // is built in isolation so one bad device/lookup can't abort the whole list.
  private reconcile(devices: DeviceInfo[]): void {
    // Drop the one-time loading placeholder as soon as any response arrives — do NOT clear the
    // whole list (that would wipe live cards); only remove a lingering placeholder node.
    if (!this.loaded) {
      this.listEl.querySelector(".placeholder")?.remove();
      this.loaded = true;
    }

    const present = new Set(devices.map((d) => d.index));
    for (const [index, card] of [...this.cards]) {
      if (!present.has(index)) {
        card.destroy();
        card.root.remove();
        this.cards.delete(index);
        this.expanded.delete(index);
      }
    }

    for (const d of devices) {
      try {
        const existing = this.cards.get(d.index);
        if (existing) {
          existing.sync(d); // patch only; do not touch the live signal table or expansion
        } else {
          const card = new DeviceCard(d, this.expanded.has(d.index));
          this.cards.set(d.index, card);
          this.listEl.append(card.root); // devices arrive append-only in index order from the API
        }
      } catch {
        /* skip this one device; the rest of the list still renders */
      }
    }

    const placeholder = this.listEl.querySelector(".placeholder");
    if (this.cards.size === 0 && !placeholder) {
      this.listEl.append(el("div", { class: "placeholder" }, ["No devices present on the bus."]));
    } else if (this.cards.size > 0 && placeholder) {
      placeholder.remove();
    }
  }
}

function setText(node: HTMLElement, text: string): void {
  if (node.textContent !== text) node.textContent = text;
}

/** Compact number for display: trim trailing zeros, cap at 2 dp. */
function trimNum(v: number): string {
  if (!Number.isFinite(v)) return "—";
  if (Number.isInteger(v)) return String(v);
  return v.toFixed(2).replace(/\.?0+$/, "");
}
