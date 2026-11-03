import {
  api,
  descriptorRegs,
  isIdentified,
  isRealFamily,
  readRegisters,
  splitValues,
  verdictText,
  type DeviceInfo,
} from "../api.js";
import { el } from "../dom.js";
import { INSTALLER_SETTINGS, type InstallerSetting } from "../installer_settings.gen.js";
import { deviceFamily, formatNumber, formatSettingRaw } from "../meta.js";
import {
  currentRaw,
  displayToRaw,
  evalRegulation,
  frameGroups,
  orderCommitFirst,
  pairsForPending,
  rawToNumber,
  regulationAddrs,
  validateRange,
  type ClauseResult,
} from "../setup_logic.js";
import { FAMILY_DESCRIPTORS } from "../setup_profiles.gen.js";
import { progressView, waitForBatch } from "./setup.js";
import type { View } from "./types.js";

// Installer settings page: the current value of every installer/expert register the stock firmware
// exposes for this inverter's grid-code XML family, with per-value edits. Edits collect in a pending
// tray (unchanged values are dropped), are checked against the stock range and regulation rules,
// and are written as one confirmed batch with the commit registers (country / standard) first.

const RANGE_SOURCE_TEXT: Record<string, string> = {
  stock: "stock range",
  envelope: "range of the stock profiles",
  dtype: "register range",
};

const OP_WORDS: Record<string, string> = { "<": "<", "<=": "≤", ">": ">", ">=": "≥", "=": "=" };

interface RowEls {
  val: HTMLElement;
  err: HTMLElement;
}

export class InstallerView implements View {
  readonly id = "installer";
  readonly label = "Installer";
  readonly navHidden = true; // reached from the Devices tab, not a top-level tab
  readonly navParent = "devices";

  private abort = new AbortController();
  private device: DeviceInfo | null = null;
  private wantIndex?: number; // device index from the route (#/installer/<index>)
  private family = "";
  private settings: InstallerSetting[] = [];
  private byAddr = new Map<number, InstallerSetting>();
  private commitAddrs: number[] = [];
  private current = new Map<number, number>(); // register address -> raw 16-bit value
  private reasons = new Map<number, string>(); // register address -> why it has no value
  private pending = new Map<number, number>(); // setting.address -> new combined raw value
  private rowEls = new Map<number, RowEls>();
  private bodyEl!: HTMLElement;
  private trayEl!: HTMLElement;
  private statusEl!: HTMLElement;
  private bannerEl!: HTMLElement;
  private busy = false;

  render(root: HTMLElement, param?: string): void {
    this.abort = new AbortController();
    this.device = null;
    this.wantIndex = param != null && param !== "" ? Number(param) : undefined;
    this.current = new Map();
    this.reasons = new Map();
    this.pending = new Map();
    this.rowEls = new Map();
    this.busy = false;

    root.append(
      el("div", { class: "view-head" }, [
        el("a", { class: "backlink", href: "#/devices" }, ["← Devices"]),
        el("h1", { class: "view-title" }, ["Installer settings"]),
      ]),
    );
    this.statusEl = el("p", { class: "muted" }, ["Looking for an inverter…"]);
    this.bannerEl = el("div", { class: "banner", hidden: true }, []);
    this.trayEl = el("div", { class: "installer-tray" }, []);
    this.bodyEl = el("div", {}, []);
    root.append(this.statusEl, this.bannerEl, this.trayEl, this.bodyEl);

    void this.load();
  }

  destroy(): void {
    this.abort.abort();
  }

  private async load(): Promise<void> {
    try {
      const devices = (await api.devices(this.abort.signal)).filter(isIdentified);
      this.device =
        this.wantIndex != null
          ? (devices.find((d) => d.index === this.wantIndex) ?? null)
          : (devices.find((d) => deviceFamily(d) === "inverter" && isRealFamily(d.xml_family)) ?? null);
    } catch {
      /* reported below */
    }
    const dev = this.device;
    if (!dev) {
      this.statusEl.textContent =
        this.wantIndex != null ? `Device ${this.wantIndex} is not an identified device.` : "No inverter found.";
      return;
    }
    const name = dev.model || `device ${dev.index}`;
    if (!isRealFamily(dev.xml_family)) {
      this.statusEl.replaceChildren(
        el("div", { class: "banner err" }, [
          `${name}: installer settings are not supported for this inverter type (${dev.xml_family ?? "unknown"}).`,
        ]),
      );
      return;
    }
    this.family = dev.xml_family;
    this.settings = INSTALLER_SETTINGS.filter((s) => s.family === this.family);
    this.byAddr = new Map(this.settings.map((s) => [s.address, s]));
    const desc = descriptorRegs(dev.descriptor) ?? FAMILY_DESCRIPTORS[this.family];
    this.commitAddrs = [desc?.country, desc?.grid, 30036, 30037].filter((a): a is number => !!a);
    this.statusEl.replaceChildren(
      `${name} `,
      el("span", { class: "muted" }, [
        `(${this.family} · ${this.settings.length} settings${dev.online ? "" : " · offline"})`,
      ]),
    );
    this.bannerEl.replaceChildren(
      "Expert settings. Changing grid-protection / power-regulation values on a live grid-tied inverter is a ",
      "compliance and safety matter — only change what you understand. To apply a whole country/standard profile use ",
      el("a", { href: `#/setup/${dev.index}` }, ["Setup"]),
      ".",
    );
    this.bannerEl.hidden = false;
    this.renderGroups();
    void this.readValues();
  }

  private allAddrs(): number[] {
    const a = new Set<number>(this.commitAddrs);
    for (const s of this.settings) {
      a.add(s.address);
      if (s.dtype === "U32") a.add(s.address + 1);
      if (s.regulation) for (const r of regulationAddrs(s.regulation)) a.add(r);
    }
    return [...a].sort((x, y) => x - y);
  }

  private async readValues(): Promise<void> {
    const dev = this.device;
    if (!dev) return;
    try {
      await readRegisters(dev.index, this.allAddrs(), this.abort.signal, (r) => {
        ({ values: this.current, reasons: this.reasons } = splitValues(r));
        this.refreshValueCells();
      });
    } catch {
      /* keep whatever we have */
    }
    this.renderTray();
  }

  private curRaw(s: InstallerSetting): number | null {
    return currentRaw(s, this.current);
  }

  private reasonFor(s: InstallerSetting): string | undefined {
    return this.reasons.get(s.address) ?? (s.dtype === "U32" ? this.reasons.get(s.address + 1) : undefined);
  }

  private refreshValueCells(): void {
    for (const s of this.settings) {
      const cell = this.rowEls.get(s.address)?.val;
      if (!cell) continue;
      const cur = this.curRaw(s);
      const reason = this.reasonFor(s);
      const text = cur !== null ? formatSettingRaw(s, cur) : reason ? `— (${reason})` : "…";
      if (cell.textContent !== text) cell.textContent = text;
      cell.classList.toggle("muted", cur === null);
    }
  }

  // ---- grouping + rows --------------------------------------------------------------------------

  private renderGroups(): void {
    // Group by category breadcrumb, preserving first-seen order.
    const groups = new Map<string, InstallerSetting[]>();
    for (const s of this.settings) {
      const key = s.category.join(" › ");
      (groups.get(key) ?? groups.set(key, []).get(key)!).push(s);
    }
    this.rowEls = new Map();
    this.bodyEl.replaceChildren();
    for (const [name, rows] of groups) {
      const hasExpert = rows.some((r) => r.risk === "expert");
      const allBasic = rows.every((r) => r.risk === "basic");
      const details = el("details", allBasic ? { open: "" } : {}, [
        el("summary", {}, [name, el("small", { class: "muted" }, [` (${rows.length})`]), hasExpert ? el("span", { class: "pill err" }, [" expert"]) : ""]),
        hasExpert
          ? el("p", { class: "banner err", style: "font-size:13px" }, ["Developer/expert parameters — incorrect values can damage the inverter."])
          : "",
        el("table", { class: "installer-table" }, [el("tbody", {}, rows.map((s) => this.row(s)))]),
      ]);
      this.bodyEl.append(details);
    }
    this.refreshValueCells();
  }

  private rangeHint(s: InstallerSetting): string {
    if (s.options || (s.min === undefined && s.max === undefined)) return "";
    const lo = s.min !== undefined ? formatNumber(s.min, s.accuracy) : "…";
    const hi = s.max !== undefined ? formatNumber(s.max, s.accuracy) : "…";
    return `${lo} – ${hi}${s.unit ? " " + s.unit : ""} (${RANGE_SOURCE_TEXT[s.rangeSource] ?? s.rangeSource})`;
  }

  private row(s: InstallerSetting): HTMLElement {
    const val = el("td", { class: "installer-val" }, ["…"]);
    const err = el("div", { class: "field-err", hidden: true }, []);
    this.rowEls.set(s.address, { val, err });
    const hint = this.rangeHint(s);
    const nameCell = el("td", { title: s.help ?? s.label }, [
      s.label,
      el("small", { class: "muted" }, [` @${s.address}${s.dtype === "U32" ? "/" + (s.address + 1) : ""}`]),
      ...(s.help ? [el("div", { class: "settings-desc muted" }, [s.help])] : []),
      ...(hint ? [el("div", { class: "settings-desc muted" }, [`Range ${hint}`])] : []),
      ...(s.regulation ? [el("div", { class: "settings-desc muted" }, [`Rule: ${this.ruleWords(s.regulation)}`])] : []),
    ]);
    return el("tr", {}, [nameCell, val, el("td", { class: "installer-edit" }, [this.editor(s), err])]);
  }

  private editor(s: InstallerSetting): HTMLElement {
    if (s.options) {
      const sel = el("select", { class: "ctrl-input" }, [
        el("option", { value: "" }, ["(keep)"]),
        ...s.options.map((o) => el("option", { value: String(o.value) }, [o.label])),
      ]) as HTMLSelectElement;
      sel.addEventListener("change", () => {
        if (sel.value === "") this.clearPending(s);
        else this.setPending(s, Number(sel.value));
      });
      return sel;
    }
    const input = el("input", {
      class: "ctrl-input",
      type: "number",
      step: s.accuracy > 0 ? String(1 / Math.pow(10, s.accuracy)) : "1",
      min: s.min,
      max: s.max,
      placeholder: s.unit || "value",
    }) as HTMLInputElement;
    input.addEventListener("change", () => {
      if (input.value === "") {
        this.clearPending(s);
        return;
      }
      const disp = Number(input.value);
      if (Number.isFinite(disp)) this.setPending(s, displayToRaw(s, disp));
    });
    return input;
  }

  private setPending(s: InstallerSetting, raw: number): void {
    if (this.curRaw(s) === raw) this.pending.delete(s.address); // unchanged: nothing to write
    else this.pending.set(s.address, raw);
    this.renderTray();
  }
  private clearPending(s: InstallerSetting): void {
    this.pending.delete(s.address);
    this.renderTray();
  }

  // ---- validation -------------------------------------------------------------------------------

  /**
   * A register's value for rule evaluation: pending or current raw (signed for S16), as the server
   * compares them. Rule peers share their unit, so raw values compare like display values.
   */
  private ruleValue(addr: number): number | undefined {
    const s = this.byAddr.get(addr);
    if (!s) return this.current.get(addr);
    const raw = this.pending.get(addr) ?? this.curRaw(s);
    return raw === null ? undefined : rawToNumber(s, raw);
  }

  private ruleWords(rule: string): string {
    return rule
      .split(";")
      .map((c) => c.trim())
      .filter((c) => c !== "")
      .map((c) => this.clauseWords(c))
      .join("; ");
  }

  private clauseWords(clause: string): string {
    return clause
      .replace(/(<=|>=|<|>|=)/, (op: string) => ` ${OP_WORDS[op]} `)
      .replace(/\s*([+-])\s*@/g, " $1 @")
      .replace(/@(\d+)/g, (_, a: string) => this.byAddr.get(Number(a))?.label ?? `register ${a}`);
  }

  /** The two sides of a clause as values: formatted with the catalogue when a side is one register. */
  private clauseValues(c: ClauseResult): string {
    const sides = c.clause.split(/<=|>=|<|>|=/);
    const show = (expr: string, v: number | null): string => {
      if (v === null) return "not read";
      const m = /^\s*@(\d+)\s*$/.exec(expr);
      const s = m ? this.byAddr.get(Number(m[1])) : undefined;
      return s ? formatSettingRaw(s, s.dtype === "S16" ? v & 0xffff : v) : formatNumber(v, -1);
    };
    return `${show(sides[0] ?? "", c.lhs)} vs ${show(sides[1] ?? "", c.rhs)}`;
  }

  /** Range errors per pending setting, and the regulation clauses that fail (or cannot be checked). */
  private validate(): { range: Map<number, string>; rules: ClauseResult[] } {
    const range = new Map<number, string>();
    for (const [addr, raw] of this.pending) {
      const s = this.byAddr.get(addr);
      const msg = s ? validateRange(s, raw) : null;
      if (msg) range.set(addr, msg);
    }
    const rules: ClauseResult[] = [];
    const seen = new Set<string>();
    for (const s of this.settings) {
      if (!s.regulation) continue;
      for (const c of evalRegulation(s.regulation, (a) => this.ruleValue(a))) {
        if (c.ok === true || seen.has(c.clause)) continue;
        if (!this.pending.has(s.address) && !c.addrs.some((a) => this.pending.has(a))) continue;
        seen.add(c.clause);
        rules.push(c);
      }
    }
    return { range, rules };
  }

  // ---- tray + apply -----------------------------------------------------------------------------

  private renderTray(): void {
    if (this.busy) return;
    const { range, rules } = this.validate();
    for (const [addr, els] of this.rowEls) {
      const msg = range.get(addr) ?? "";
      if (els.err.textContent !== msg) els.err.textContent = msg;
      els.err.hidden = msg === "";
    }
    if (this.pending.size === 0) {
      this.trayEl.replaceChildren();
      return;
    }
    const items: (Node | string)[] = [el("strong", {}, [`${this.pending.size} pending change(s)`])];
    for (const [addr, to] of this.pending) {
      const s = this.byAddr.get(addr);
      if (!s) continue;
      const cur = this.curRaw(s);
      const bad = range.get(addr);
      items.push(
        el("div", { class: bad ? "tray-row err" : "tray-row" }, [
          `${s.label}: `,
          el("span", { class: "muted" }, [cur === null ? "—" : formatSettingRaw(s, cur)]),
          " → ",
          el("strong", {}, [formatSettingRaw(s, to)]),
          ...(bad ? [` — ${bad}`] : []),
        ]),
      );
    }
    for (const c of rules) {
      items.push(
        el("div", { class: c.ok === false ? "tray-row err" : "tray-row warn" }, [
          c.ok === false ? "Rule broken: " : "Rule not checked here (a value is not read; the dongle checks it): ",
          this.clauseWords(c.clause),
          el("span", { class: "muted" }, [` (${this.clauseValues(c)})`]),
        ]),
      );
    }
    const pairs = this.orderedPairs();
    const blocked = range.size > 0 || rules.some((c) => c.ok === false) || pairs.length === 0;
    items.push(
      el("div", { class: "muted", style: "font-size:12px" }, [
        `Writes ${pairs.length} register(s) in ${frameGroups(pairs).length} frame(s), country/standard registers first.`,
      ]),
    );
    const apply = el(
      "button",
      { class: "btn primary", type: "button", disabled: blocked, onClick: () => void this.apply() },
      [`Apply ${this.pending.size} change(s)`],
    );
    const discard = el("button", { class: "btn", type: "button", onClick: () => this.discard() }, ["Discard"]);
    items.push(el("div", { class: "unlock-bar" }, [apply, discard]));
    this.trayEl.replaceChildren(el("div", { class: "banner" }, items));
  }

  private discard(): void {
    this.pending.clear();
    this.renderGroups();
    this.renderTray();
  }

  private orderedPairs(): [number, number][] {
    return orderCommitFirst(pairsForPending(this.settings, this.pending, this.current), this.commitAddrs);
  }

  private async apply(): Promise<void> {
    const dev = this.device;
    const pairs = this.orderedPairs();
    if (!dev || pairs.length === 0 || this.busy) return;
    this.busy = true;
    const status = el("p", {}, [`Applying ${pairs.length} register(s)…`]);
    const progress = el("div", {}, []);
    const box = el("div", { class: "banner" }, [status, progress]);
    this.trayEl.replaceChildren(box);
    try {
      const c = await api.confirm(dev.index, api.SETUP_WRITE_CONFIRM_ADDR);
      const r = await api.setupWrite(dev.index, pairs, c.token);
      if (!r.ok) {
        this.busy = false;
        this.trayEl.replaceChildren(el("div", { class: "banner err" }, [`Rejected: ${verdictText(r)}`]));
        return;
      }
      const p = await waitForBatch(dev.index, this.abort.signal, (sp) => progress.replaceChildren(progressView(sp)));
      status.textContent = !p
        ? "No progress reported by the dongle."
        : p.busy
          ? "Timed out waiting for the apply."
          : p.failed
            ? `Applied with ${p.failed} failed register(s).`
            : `Applied ${p.ok} register(s) ✓`;
      box.className = !p || p.busy || p.failed > 0 ? "banner err" : "banner ok";
    } catch (err) {
      if (this.abort.signal.aborted) return;
      this.busy = false;
      this.trayEl.replaceChildren(el("div", { class: "banner err" }, [(err as Error).message]));
      return;
    }
    this.busy = false;
    this.pending.clear();
    this.renderGroups();
    await this.readValues();
    this.trayEl.replaceChildren(box); // keep the per-frame result visible after the re-read
  }
}
