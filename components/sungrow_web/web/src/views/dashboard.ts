import { api, type DeviceInfo, type SignalValue } from "../api.js";
import { el } from "../dom.js";
import { controlProfile, deviceFamily, formatSignal, lookupMetaStrict } from "../meta.js";
import type { View } from "./types.js";

const DEVICES_POLL_MS = 5000;
// The fast poll tier (power flow) every second, the whole set every 10 s; the two responses merge
// into one signal map so the cards always render from the latest value of each signal.
const FAST_POLL_MS = 1000;
const FULL_POLL_MS = 10000;
const STALE_MS = 30000;

// How a stat card renders its value. The dashboard targets the inverter family, so sigids and
// units below are the inverter-family meta; absent/invalid signals hide their card.
type Kind = "power" | "energy" | "percent" | "freq" | "temp" | "duration";

interface CardSpec {
  id: string;
  title: string;
  candidates: string[]; // sigid keys; first present+valid wins
  kind: Kind;
  hero?: boolean;
}

// Primary inverter headline signals. Candidate lists absorb the SG/SH/public map variants so one
// card works across proto1-5 string + hybrid inverters; whichever variant the device reports wins.
const CARDS: CardSpec[] = [
  { id: "ac", title: "Active power", candidates: ["0x2080"], kind: "power", hero: true },
  { id: "pv", title: "PV / DC power", candidates: ["0x2032", "0xF010"], kind: "power" },
  { id: "today", title: "Today's yield", candidates: ["0x2020"], kind: "energy" },
  { id: "total", title: "Total yield", candidates: ["0x2021", "0x2022", "0x2023"], kind: "energy" },
  // 0x2519 is the hybrid's PV yield; a battery-less string inverter doesn't populate it, so fall
  // back to daily yield 0x2020 (which on a PV-only inverter IS the PV yield). Deduped below so it
  // never shows the same value as "Today's yield".
  { id: "pvtoday", title: "PV yield today", candidates: ["0x2519", "0x2020"], kind: "energy" },
  { id: "load", title: "Load power", candidates: ["0x2502", "0x2544"], kind: "power" },
  { id: "soc", title: "Battery SOC", candidates: ["0x2514"], kind: "percent" },
  { id: "batt", title: "Battery power", candidates: ["0x252C", "0x252D"], kind: "power" },
  { id: "freq", title: "Grid frequency", candidates: ["0x208C", "0x2084", "0x2085"], kind: "freq" },
  { id: "temp", title: "Internal temperature", candidates: ["0x2031"], kind: "temp" },
  { id: "runtime", title: "Running today", candidates: ["0x208A"], kind: "duration" },
];

// Grid card (import/export) is handled specially so it can show a direction.
const GRID_IMPORT = "0xF011"; // kW (derived)
const GRID_EXPORT = "0xF012"; // kW (derived)
const GRID_METER = ["0x2540", "0x2182"]; // W, signed meter active power (fallback)

// Active-power limit card: the setpoint register when the inverter serves it, else (string
// inverters) the read-backs of the power-limitation switch and percentage; caption from the
// limitation reason, or the dispatched-running state.
const LIMIT_SETPOINT = "0x217E"; // W
const LIMIT_SWITCH = "0xF140"; // 0xAA = limitation on
const LIMIT_PCT = "0xF141"; // 0.1 %
const LIMIT_REASON = "0x2090"; // enum
const DEVICE_STATE = "0x2086";
const STATE_DISPATCHED = 0x8200;
const SWITCH_ON = 0xaa;

function toWatts(v: number, unit: string): number {
  return unit === "kW" || unit === "kVA" || unit === "kvar" ? v * 1000 : v;
}

function fmtPower(watts: number): { value: string; unit: string } {
  const a = Math.abs(watts);
  if (a >= 1000) return { value: (watts / 1000).toFixed(a >= 100000 ? 0 : 2), unit: "kW" };
  return { value: String(Math.round(watts)), unit: "W" };
}

function fmtDuration(minutes: number): { value: string; unit: string } {
  const m = Math.max(0, Math.round(minutes));
  if (m >= 60) return { value: `${Math.floor(m / 60)}h ${m % 60}m`, unit: "" };
  return { value: String(m), unit: "min" };
}

// A single stat card: value + unit + title, shown/hidden as its signal appears/vanishes.
class Card {
  readonly root: HTMLElement;
  private valueEl: HTMLElement;
  private unitEl: HTMLElement;
  private subEl: HTMLElement;
  private shown = false;

  constructor(spec: CardSpec, extraClass = "") {
    this.valueEl = el("span", { class: "metric-value" }, ["—"]);
    this.unitEl = el("span", { class: "metric-unit" }, []);
    this.subEl = el("span", { class: "metric-sub" }, []);
    this.root = el("div", { class: `metric ${spec.hero ? "metric-hero" : ""} ${extraClass}`, hidden: true }, [
      el("span", { class: "metric-title" }, [spec.title]),
      el("span", { class: "metric-body" }, [this.valueEl, this.unitEl]),
      this.subEl,
    ]);
  }

  set(value: string, unit: string, sub: string, stale: boolean): void {
    if (this.valueEl.textContent !== value) this.valueEl.textContent = value;
    if (this.unitEl.textContent !== unit) this.unitEl.textContent = unit;
    if (this.subEl.textContent !== sub) this.subEl.textContent = sub;
    this.root.classList.toggle("stale", stale);
    if (!this.shown) {
      this.root.hidden = false;
      this.shown = true;
    }
  }

  hide(): void {
    if (this.shown) {
      this.root.hidden = true;
      this.shown = false;
    }
  }
}

export class DashboardView implements View {
  readonly id = "dashboard";
  readonly label = "Dashboard";

  private abort = new AbortController();
  private devTimer = 0;
  private fastTimer = 0;
  private fullTimer = 0;
  private device: DeviceInfo | null = null;
  private sigMap = new Map<string, SignalValue>();

  private statusPill!: HTMLElement;
  private nameEl!: HTMLElement;
  private subtitle!: HTMLElement;
  private updatedEl!: HTMLElement;
  private grid!: HTMLElement;
  private emptyEl!: HTMLElement;
  private cards = new Map<string, Card>();
  private gridCard!: Card;
  private limitCard!: Card;

  render(root: HTMLElement): void {
    // Re-init per-mount state: the view is destroyed on tab leave (abort fired, timers cleared) and
    // render() runs again on every re-activation, so a fresh AbortController + cleared device/cards
    // are required or fetches would use the already-aborted signal and nothing would load.
    this.abort = new AbortController();
    this.device = null;
    this.fastTimer = 0;
    this.fullTimer = 0;
    this.sigMap = new Map<string, SignalValue>();
    this.cards = new Map<string, Card>();

    root.append(el("h1", { class: "view-title" }, ["Dashboard"]));

    this.statusPill = el("span", { class: "pill" }, ["…"]);
    this.nameEl = el("span", { class: "dash-name" }, ["—"]);
    this.subtitle = el("span", { class: "dash-sub muted" }, ["Looking for inverter…"]);
    this.updatedEl = el("span", { class: "dash-updated muted" }, []);
    root.append(
      el("div", { class: "dash-head" }, [
        el("div", {}, [this.nameEl, el("br", {}, []), this.subtitle]),
        el("span", { class: "spacer" }, []),
        this.statusPill,
      ]),
    );

    this.grid = el("div", { class: "dash-grid" }, []);
    for (const spec of CARDS) {
      const card = new Card(spec);
      this.cards.set(spec.id, card);
      this.grid.append(card.root);
    }
    this.gridCard = new Card({ id: "grid", title: "Grid power", candidates: [], kind: "power" });
    // Place the grid card right after the hero/PV so it reads well, before the rest.
    this.grid.insertBefore(this.gridCard.root, this.cards.get("today")!.root);
    this.limitCard = new Card({ id: "limit", title: "Active-power limit", candidates: [], kind: "power" });
    this.grid.insertBefore(this.limitCard.root, this.cards.get("today")!.root);

    this.emptyEl = el("div", { class: "placeholder" }, [
      el("span", { class: "spin" }, []),
      " Waiting for live data…",
    ]);
    root.append(this.emptyEl, this.grid, this.updatedEl);

    void this.loadDevices();
    this.devTimer = window.setInterval(() => void this.loadDevices(), DEVICES_POLL_MS);
  }

  destroy(): void {
    window.clearInterval(this.devTimer);
    window.clearInterval(this.fastTimer);
    window.clearInterval(this.fullTimer);
    this.abort.abort();
  }

  private async loadDevices(): Promise<void> {
    try {
      const devices = await api.devices(this.abort.signal);
      const inverter =
        devices.find((d) => d.discovered && deviceFamily(d) === "inverter") ??
        devices.find((d) => d.discovered);
      this.setDevice(inverter ?? null);
    } catch (err) {
      if (this.abort.signal.aborted) return;
      this.setStatus("offline", "bad");
      this.subtitle.textContent = `Could not load devices: ${(err as Error).message}`;
    }
  }

  private setDevice(d: DeviceInfo | null): void {
    if (!d) {
      this.device = null;
      this.nameEl.textContent = "No inverter";
      this.subtitle.textContent = "No discovered inverter on the bus.";
      this.setStatus("—", "pending");
      this.emptyEl.hidden = false;
      return;
    }
    const changed = this.device?.index !== d.index;
    this.device = d;
    this.nameEl.textContent = d.model || `Device ${d.index}`;
    this.subtitle.textContent = `addr ${d.address} · ${d.state}`;
    this.setStatus(d.online ? "online" : "offline", d.online ? "ok" : "bad");
    if (changed) {
      window.clearInterval(this.fastTimer);
      window.clearInterval(this.fullTimer);
      this.sigMap = new Map<string, SignalValue>();
      void this.loadSignals(true);
      this.fastTimer = window.setInterval(() => void this.loadSignals(false), FAST_POLL_MS);
      this.fullTimer = window.setInterval(() => void this.loadSignals(true), FULL_POLL_MS);
    }
  }

  private setStatus(text: string, tone: "ok" | "bad" | "pending"): void {
    this.statusPill.textContent = text;
    this.statusPill.style.color =
      tone === "ok" ? "var(--ok)" : tone === "bad" ? "var(--bad)" : "var(--warn)";
  }

  // full: the whole set replaces the map (signals that went invalid disappear); otherwise the fast
  // tier is merged over it. A reply for a device other than the selected one is dropped.
  private async loadSignals(full: boolean): Promise<void> {
    const dev = this.device;
    if (!dev) return;
    try {
      const res = await api.signals(dev.index, full ? {} : { cls: "fast" }, this.abort.signal);
      if (this.device?.index !== dev.index) return;
      if (full) this.sigMap = new Map<string, SignalValue>();
      for (const s of res.signals) this.sigMap.set(s.key, s);
      this.renderSignals(dev, this.sigMap, res.now_ms);
    } catch (err) {
      if (this.abort.signal.aborted) return;
      // keep last values; status pill already reflects reachability from the devices poll
      void err;
    }
  }

  private renderSignals(dev: DeviceInfo, map: Map<string, SignalValue>, nowMs: number): void {
    const fam = deviceFamily(dev);
    let anyShown = false;
    const resolvedKey: Record<string, string> = {}; // card id -> the sigid it rendered

    for (const spec of CARDS) {
      const card = this.cards.get(spec.id)!;
      try {
        // STRICT: only a valid, numeric signal that is a known member of THIS device's family.
        // Battery/grid/load sigids that are absent or invalid on a string inverter -> card hides.
        const sig = this.pick(map, spec.candidates, fam);
        if (!sig) {
          card.hide();
          continue;
        }
        // Dedup: don't show "PV yield today" when it fell back to the same sigid as "Today's yield".
        if (spec.id === "pvtoday" && resolvedKey["today"] === sig.key) {
          card.hide();
          continue;
        }
        resolvedKey[spec.id] = sig.key;
        const meta = lookupMetaStrict(fam, sig.key)!; // non-null: pick() guarantees it
        const raw = (sig.value as number) * meta.mult;
        const stale = nowMs - sig.updated_ms > STALE_MS;
        const r = this.format(spec.kind, raw, meta.unit, meta.precision);
        card.set(r.value, r.unit, r.sub, stale);
        anyShown = true;
      } catch {
        card.hide(); // one bad lookup must never stall the dashboard
      }
    }

    anyShown = this.renderGrid(map, nowMs, fam) || anyShown;
    anyShown = this.renderLimit(map, nowMs, dev) || anyShown;
    this.emptyEl.hidden = anyShown;
    const freshest = this.freshestUpdate(map);
    if (freshest > 0) {
      const secs = Math.max(0, Math.round((nowMs - freshest) / 1000));
      this.updatedEl.textContent = `Updated ${secs <= 1 ? "just now" : `${secs}s ago`}`;
    } else {
      this.updatedEl.textContent = "";
    }
  }

  private renderGrid(map: Map<string, SignalValue>, nowMs: number, fam: string | undefined): boolean {
    try {
      // Only valid, in-family grid signals (absent on a meter-less string inverter -> hide).
      const imp = this.inFamily(map, GRID_IMPORT, fam);
      const exp = this.inFamily(map, GRID_EXPORT, fam);
      const impW = imp ? (imp.value as number) * 1000 : null;
      const expW = exp ? (exp.value as number) * 1000 : null;

      let watts: number | null = null;
      let dir = "";
      let stale = false;
      if (expW !== null && expW > 1) {
        watts = expW;
        dir = "Exporting to grid";
        stale = nowMs - exp!.updated_ms > STALE_MS;
      } else if (impW !== null && impW > 1) {
        watts = impW;
        dir = "Importing from grid";
        stale = nowMs - imp!.updated_ms > STALE_MS;
      } else if (impW !== null || expW !== null) {
        watts = 0;
        dir = "At grid parity";
      } else {
        // Fallback: signed meter active power (W). Sign convention not firmly established, so the
        // caption stays neutral.
        const m = this.pick(map, GRID_METER, fam);
        if (m) {
          watts = m.value as number;
          dir = "Meter active power";
          stale = nowMs - m.updated_ms > STALE_MS;
        }
      }
      if (watts === null) {
        this.gridCard.hide();
        return false;
      }
      const p = fmtPower(Math.abs(watts));
      this.gridCard.set(p.value, p.unit, dir, stale);
      return true;
    } catch {
      this.gridCard.hide();
      return false;
    }
  }

  // Active-power limit: the setpoint (W) when the inverter serves it; else, on a string inverter,
  // the limitation switch / percentage read-backs as "Limit 50 %" or "No limit". The caption is
  // the limitation reason, or "Limited by the active-power setting" while the state is
  // dispatched running (0x8200).
  private renderLimit(map: Map<string, SignalValue>, nowMs: number, dev: DeviceInfo): boolean {
    try {
      const fam = deviceFamily(dev);
      let caption = "";
      const state = map.get(DEVICE_STATE);
      const reason = map.get(LIMIT_REASON);
      if (state && state.valid && state.value === STATE_DISPATCHED) {
        caption = "Limited by the active-power setting";
      } else if (reason && reason.valid && typeof reason.value === "number") {
        caption = formatSignal(fam, LIMIT_REASON, reason.value);
      }
      const sp = this.inFamily(map, LIMIT_SETPOINT, fam);
      if (sp) {
        const meta = lookupMetaStrict(fam, LIMIT_SETPOINT)!;
        const p = fmtPower(toWatts((sp.value as number) * meta.mult, meta.unit));
        this.limitCard.set(p.value, p.unit, caption, nowMs - sp.updated_ms > STALE_MS);
        return true;
      }
      if (controlProfile(dev) === "string") {
        const sw = map.get(LIMIT_SWITCH);
        const pct = map.get(LIMIT_PCT);
        if (sw && sw.valid && typeof sw.value === "number") {
          if (sw.value === SWITCH_ON && pct && pct.valid && typeof pct.value === "number") {
            const stale = nowMs - Math.max(sw.updated_ms, pct.updated_ms) > STALE_MS;
            this.limitCard.set(`Limit ${(pct.value / 10).toFixed(pct.value % 10 === 0 ? 0 : 1)}`, "%", caption, stale);
          } else {
            this.limitCard.set("No limit", "", caption, nowMs - sw.updated_ms > STALE_MS);
          }
          return true;
        }
      }
      this.limitCard.hide();
      return false;
    } catch {
      this.limitCard.hide();
      return false;
    }
  }

  // First candidate that is present, valid, numeric AND a known signal of this device's family.
  private pick(map: Map<string, SignalValue>, candidates: string[], fam: string | undefined): SignalValue | null {
    for (const key of candidates) {
      const s = this.inFamily(map, key, fam);
      if (s) return s;
    }
    return null;
  }

  private inFamily(map: Map<string, SignalValue>, key: string, fam: string | undefined): SignalValue | null {
    const s = map.get(key);
    const m = lookupMetaStrict(fam, key);
    // Present, valid, numeric, a known signal of THIS family, and not marked hidden by the data
    // layer (e.g. total_yield_sg/fine 0x2022/0x2023 never back a dashboard card).
    if (s && s.valid && typeof s.value === "number" && m !== null && m.group !== "hidden") return s;
    return null;
  }

  private freshestUpdate(map: Map<string, SignalValue>): number {
    let max = 0;
    for (const s of map.values()) if (s.valid && s.updated_ms > max) max = s.updated_ms;
    return max;
  }

  private format(kind: Kind, raw: number, unit: string, precision: number): {
    value: string;
    unit: string;
    sub: string;
  } {
    switch (kind) {
      case "power": {
        const p = fmtPower(toWatts(raw, unit));
        return { value: p.value, unit: p.unit, sub: "" };
      }
      case "energy":
        return { value: raw.toFixed(precision >= 0 ? precision : 1), unit: unit || "kWh", sub: "" };
      case "percent":
        return { value: raw.toFixed(precision >= 0 ? precision : 0), unit: "%", sub: "" };
      case "freq":
        return { value: raw.toFixed(2), unit: "Hz", sub: "" };
      case "temp":
        return { value: raw.toFixed(1), unit: "°C", sub: "" };
      case "duration": {
        const d = fmtDuration(raw);
        return { value: d.value, unit: d.unit, sub: "" };
      }
    }
  }
}
