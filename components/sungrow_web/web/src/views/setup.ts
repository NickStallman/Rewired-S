import {
  api,
  descriptorRegs,
  isIdentified,
  isRealFamily,
  sleep,
  VERDICT_TEXT,
  verdictText,
  type DeviceInfo,
  type SetupOptionInfo,
  type SetupOptions,
  type SetupPreview,
  type SetupProgress,
} from "../api.js";
import { el } from "../dom.js";
import { catalogueRow, deviceFamily, formatSettingRaw, registerLabel } from "../meta.js";
import { diffItems, u32Join, type DiffRow } from "../setup_logic.js";
import { SETUP_COUNTRIES, SETUP_OPTIONS } from "../setup_profiles.gen.js";
import type { View } from "./types.js";

// Setup: apply a stock country / standard profile (the XML POWER_COMPANY options) the way the stock
// dongle does. The server resolves the profile for this inverter (family, feature bits, the 5479
// machine-type variant), reads every target register for a preview, and writes all of them on a
// confirmed apply, country register first. This page picks, previews, confirms and shows progress.

const POLL_MS = 800;
const PREVIEW_TIMEOUT_MS = 30000;
const OPTIONS_TIMEOUT_MS = 30000;

function statusText(status: string, reg5479: number | null | undefined): string {
  switch (status.toUpperCase()) {
    case "NO_FAMILY":
      return "this inverter has no grid-code XML family";
    case "NO_DESCRIPTOR":
      return "there is no stock descriptor for this inverter type and protocol";
    case "UNKNOWN_OPTION":
      return "this standard is not offered for the inverter's family";
    case "NEED_CAPABILITY":
      return "the inverter's capability bitmap (register 5520) has not been polled yet; try again in a minute";
    case "NEED_5479":
      return "the inverter did not answer register 5479 (machine type), which selects the profile variant";
    case "NO_VARIANT_FOR_5479":
      return `no profile variant for machine type ${reg5479 ?? "?"} (register 5479)`;
    case "GRID_TYPE_UNSUPPORTED":
      return "this standard needs a grid-type parameter, which no shipped profile supports";
    default:
      return status;
  }
}

function isOk(status: string | undefined): boolean {
  return status !== undefined && status.toUpperCase() === "OK";
}

// ---- shared with the Installer page: batch progress -------------------------------------------

/**
 * Poll the dongle's batch progress (/api/grid_read `setup`) until the batch is drained or the
 * deadline passes. `expectedTotal` (from the apply reply) tells this batch from a previous one.
 */
export async function waitForBatch(
  device: number,
  signal: AbortSignal,
  onProgress: (p: SetupProgress) => void,
  expectedTotal?: number,
): Promise<SetupProgress | null> {
  let deadline = Date.now() + 75000;
  let last: SetupProgress | null = null;
  let sawBusy = false;
  for (;;) {
    await sleep(POLL_MS, signal);
    const p = (await api.gridRead(device, signal)).setup;
    if (p && (p.device === undefined || p.device === device)) {
      last = p;
      sawBusy ||= p.busy;
      onProgress(p);
      if (p.busy && p.deadline_ms !== undefined) deadline = Date.now() + p.deadline_ms + 15000;
      const ours = sawBusy || expectedTotal === undefined || p.total === expectedTotal;
      if (ours && !p.busy && p.done >= p.total) return p;
    }
    if (Date.now() > deadline) return last;
  }
}

/** Batch progress summary plus one row per frame (start, registers, status, exception). */
export function progressView(p: SetupProgress): HTMLElement {
  const parts = [`${p.done} of ${p.total} written`, `${p.ok} ok`, `${p.failed} failed`];
  if (p.busy && p.deadline_ms !== undefined) parts.push(`deadline in ${Math.ceil(p.deadline_ms / 1000)} s`);
  const box = el("div", { class: "progress" }, [
    el("div", {}, [p.busy ? el("span", { class: "spin" }, []) : "", p.busy ? " " : "", parts.join(" · ")]),
  ]);
  if (p.frames?.length) {
    box.append(
      el("table", { class: "diff-table" }, [
        el("thead", {}, [
          el("tr", {}, [el("th", {}, ["Frame"]), el("th", {}, ["Registers"]), el("th", {}, ["Status"]), el("th", {}, ["Exception"])]),
        ]),
        el(
          "tbody",
          {},
          p.frames.map((f) => {
            const st = f.status.toLowerCase();
            const cls = st === "ok" ? "ok" : st === "pending" || st === "queued" ? "muted" : "err";
            return el("tr", {}, [
              el("td", {}, [f.n > 1 ? `${f.start}–${f.start + f.n - 1}` : String(f.start)]),
              el("td", {}, [String(f.n)]),
              el("td", { class: cls }, [f.status]),
              el("td", {}, [f.ex ? `0x${f.ex.toString(16).padStart(2, "0")}` : ""]),
            ]);
          }),
        ),
      ]),
    );
  }
  return box;
}

/** Modal confirmation. Resolves true when the user presses the confirm button. */
export function confirmDialog(title: string, body: (Node | string)[], okLabel: string): Promise<boolean> {
  return new Promise((resolve) => {
    const dlg = el("dialog", { class: "confirm-dialog" }, []) as HTMLDialogElement;
    const done = (v: boolean) => {
      dlg.close();
      dlg.remove();
      resolve(v);
    };
    dlg.append(
      el("h2", {}, [title]),
      ...body,
      el("div", { class: "dialog-actions" }, [
        el("button", { class: "btn", type: "button", onClick: () => done(false) }, ["Cancel"]),
        el("button", { class: "btn primary", type: "button", onClick: () => done(true) }, [okLabel]),
      ]),
    );
    dlg.addEventListener("cancel", (e) => {
      e.preventDefault();
      done(false);
    });
    document.body.append(dlg);
    dlg.showModal();
  });
}

// ---- view -------------------------------------------------------------------------------------

export class SetupView implements View {
  readonly id = "setup";
  readonly label = "Setup";
  readonly navHidden = true;
  readonly navParent = "devices";

  private abort = new AbortController();
  private wantIndex?: number;
  private device: DeviceInfo | null = null;
  private opts: SetupOptions | null = null;
  private options: SetupOptionInfo[] = [];
  private option: SetupOptionInfo | null = null;
  private preview: SetupPreview | null = null;
  private previewSeq = 0;
  private busy = false;
  private onlyChanged = false;
  private country: number | null = null;

  private statusEl!: HTMLElement;
  private identityEl!: HTMLElement;
  private pickerEl!: HTMLElement;
  private hintEl!: HTMLElement;
  private previewEl!: HTMLElement;
  private applyEl!: HTMLElement;
  private startEl!: HTMLElement;

  render(root: HTMLElement, param?: string): void {
    this.abort = new AbortController();
    this.wantIndex = param != null && param !== "" ? Number(param) : undefined;
    this.device = null;
    this.opts = null;
    this.option = null;
    this.preview = null;
    this.busy = false;
    this.onlyChanged = false;

    root.append(
      el("div", { class: "view-head" }, [
        el("a", { class: "backlink", href: "#/devices" }, ["← Devices"]),
        el("h1", { class: "view-title" }, ["Grid-code setup"]),
      ]),
    );
    this.statusEl = el("p", { class: "muted" }, ["Looking for an inverter…"]);
    this.identityEl = el("div", { class: "settings-section", hidden: true }, []);
    this.pickerEl = el("div", { class: "settings-section", hidden: true }, []);
    this.hintEl = el("p", { class: "muted setup-hint" }, []);
    this.previewEl = el("div", {}, []);
    this.applyEl = el("div", {}, []);
    this.startEl = el("div", { class: "settings-section", hidden: true }, []);
    root.append(this.statusEl, this.identityEl, this.pickerEl, this.previewEl, this.applyEl, this.startEl);
    void this.load();
  }

  destroy(): void {
    this.abort.abort();
    this.previewSeq++;
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
    if (!isRealFamily(dev.xml_family)) {
      this.statusEl.replaceChildren(
        el("div", { class: "banner err" }, [
          `Grid-code setup is not supported for this inverter type (${dev.xml_family ?? "unknown"}).`,
        ]),
      );
      return;
    }
    this.statusEl.replaceChildren(el("span", { class: "spin" }, []), " Reading the inverter's country, standard and machine type…");
    try {
      this.opts = await this.fetchOptions(dev.index);
    } catch (err) {
      if (this.abort.signal.aborted) return;
      this.statusEl.replaceChildren(el("div", { class: "banner err" }, [`Setup options unavailable: ${(err as Error).message}`]));
      return;
    }
    const family = this.opts.family ?? dev.xml_family;
    if (!isRealFamily(family)) {
      this.statusEl.replaceChildren(
        el("div", { class: "banner err" }, [`Grid-code setup is not supported for this inverter type (${family}).`]),
      );
      return;
    }
    this.options = this.opts.options?.length ? this.opts.options : SETUP_OPTIONS.filter((o) => o.family === family);
    this.statusEl.replaceChildren(
      `${dev.model || "device " + dev.index} `,
      el("span", { class: "muted" }, [
        `(device ${dev.index}${dev.online ? "" : " · offline"}${this.opts.complete === false ? " · some reads did not complete" : ""})`,
      ]),
    );
    this.renderIdentity();
    this.renderPicker();
    this.renderStart();
  }

  /** Start-or-poll /api/setup_options until its reads are complete (or the timeout). */
  private async fetchOptions(device: number): Promise<SetupOptions> {
    const deadline = Date.now() + OPTIONS_TIMEOUT_MS;
    for (;;) {
      const o = await api.setupOptions(device, this.abort.signal);
      if (o.complete !== false || Date.now() > deadline) return o;
      await sleep(POLL_MS, this.abort.signal);
    }
  }

  private family(): string {
    return this.opts?.family ?? this.device?.xml_family ?? "";
  }

  private countries(): { code: number; label: string }[] {
    return this.opts?.countries?.length ? this.opts.countries : SETUP_COUNTRIES;
  }

  private countryLabel(code: number | null | undefined): string {
    if (code === null || code === undefined) return "—";
    const c = this.countries().find((x) => x.code === code);
    return c ? `${c.label} (${code})` : String(code);
  }

  private optionLabel(code: number | null | undefined): string {
    if (code === null || code === undefined) return "—";
    const o = this.options.find((x) => x.code === code);
    return o ? `${o.label} (${code})` : String(code);
  }

  private renderIdentity(): void {
    const o = this.opts!;
    const d = descriptorRegs(o.descriptor);
    const kv: [string, string][] = [
      ["XML family", this.family()],
      [
        "Descriptor registers",
        d
          ? [d.country ? `country ${d.country}` : "", d.grid ? `grid code ${d.grid}` : "", d.powerSwitch ? `power switch ${d.powerSwitch}` : ""]
              .filter((x) => x)
              .join(" · ")
          : "none",
      ],
      ["Current country", this.countryLabel(o.current?.country)],
      ...(d?.grid ? ([["Current grid code", o.current?.grid_code == null ? "—" : String(o.current.grid_code)]] as [string, string][]) : []),
      ["Current standard", this.optionLabel(o.current?.company)],
      ["Grid type", o.current?.grid_type === null || o.current?.grid_type === undefined ? "—" : String(o.current.grid_type)],
      ["Feature bits", `W32B5 ${o.feature?.w32b5 ? "set" : "clear"} · W29B12 ${o.feature?.w29b12 ? "set" : "clear"}`],
      ["Machine type (5479)", o.reg5479 === null || o.reg5479 === undefined ? "not read" : String(o.reg5479)],
    ];
    this.identityEl.replaceChildren(
      el("h2", { class: "settings-title" }, ["Inverter"]),
      el("dl", { class: "kv" }, kv.flatMap(([k, v]) => [el("dt", {}, [k]), el("dd", {}, [v])])),
    );
    this.identityEl.hidden = false;
  }

  private renderPicker(): void {
    const o = this.opts!;
    const countrySel = el(
      "select",
      { class: "ctrl-input" },
      this.countries().map((c) => el("option", { value: String(c.code) }, [c.label])),
    ) as HTMLSelectElement;
    if (o.current?.country !== null && o.current?.country !== undefined) countrySel.value = String(o.current.country);
    this.country = countrySel.value === "" ? null : Number(countrySel.value);
    countrySel.addEventListener("change", () => (this.country = Number(countrySel.value)));
    const optionSel = el("select", { class: "ctrl-input" }, [
      el("option", { value: "" }, ["Choose a standard…"]),
      ...this.options.map((x) =>
        el("option", { value: String(x.code) }, [`${x.label}${x.code === o.current?.company ? " (current)" : ""}`]),
      ),
    ]) as HTMLSelectElement;
    optionSel.addEventListener("change", () => {
      this.option = this.options.find((x) => String(x.code) === optionSel.value) ?? null;
      this.renderHint();
      void this.runPreview();
    });
    this.pickerEl.replaceChildren(
      el("h2", { class: "settings-title" }, ["Profile"]),
      el("div", { class: "setup-picker" }, [
        el("label", {}, [el("span", { class: "muted" }, ["Country"]), countrySel]),
        el("label", {}, [el("span", { class: "muted" }, ["Standard"]), optionSel]),
      ]),
      this.hintEl,
    );
    this.pickerEl.hidden = false;
    this.renderHint();
  }

  private renderHint(): void {
    const opt = this.option;
    if (!opt) {
      this.hintEl.textContent = "The preview reads every register the profile writes; nothing is written until you confirm.";
      return;
    }
    if (opt.needreadtype) {
      const r = this.opts?.reg5479;
      const v = r === null || r === undefined ? "not read yet (read during the preview)" : String(r);
      const refs = opt.variants.map((x, i) => `variant ${i + 1}: 5479 ∈ {${x.ref5479.join(", ")}}, ${x.nregs} registers`);
      this.hintEl.textContent = `PV/ES chosen from register 5479 = ${v}. ${refs.join("; ")}.`;
    } else {
      const n = opt.variants[0]?.nregs;
      this.hintEl.textContent = `One profile${n ? `, ${n} registers` : ""}.`;
    }
  }

  // ---- preview ----------------------------------------------------------------------------------

  private async fetchPreview(code: number, seq: number): Promise<SetupPreview | null> {
    const dev = this.device!;
    const deadline = Date.now() + PREVIEW_TIMEOUT_MS;
    for (;;) {
      const p = await api.setupPreview(dev.index, code, this.abort.signal);
      if (seq !== this.previewSeq) return null;
      if (p.complete) return p;
      if (Date.now() > deadline) return p;
      await sleep(POLL_MS, this.abort.signal);
      if (seq !== this.previewSeq) return null;
    }
  }

  private async runPreview(after?: string): Promise<void> {
    const seq = ++this.previewSeq;
    this.preview = null;
    this.applyEl.replaceChildren();
    const opt = this.option;
    if (!opt) {
      this.previewEl.replaceChildren();
      return;
    }
    this.previewEl.replaceChildren(
      el("p", { class: "muted" }, [el("span", { class: "spin" }, []), " Reading the inverter's current values…"]),
    );
    let p: SetupPreview | null;
    try {
      p = await this.fetchPreview(opt.code, seq);
    } catch (err) {
      if (this.abort.signal.aborted || seq !== this.previewSeq) return;
      this.previewEl.replaceChildren(el("div", { class: "banner err" }, [`Preview failed: ${(err as Error).message}`]));
      return;
    }
    if (!p) return;
    if (!p.complete) {
      this.previewEl.replaceChildren(
        el("div", { class: "banner err" }, ["The inverter did not answer the preview reads within 30 s. Choose the standard again to retry."]),
      );
      return;
    }
    if (p.status === "error") {
      this.previewEl.replaceChildren(
        el("div", { class: "banner err" }, [
          `The dongle could not read the inverter: ${VERDICT_TEXT[p.error ?? ""] ?? p.error ?? "error"}${p.message ? ` (${p.message})` : ""}.`,
        ]),
      );
      return;
    }
    if (!isOk(p.status)) {
      this.previewEl.replaceChildren(
        el("div", { class: "banner err" }, [`This profile cannot be applied: ${statusText(p.status ?? "unknown", p.reg5479)}.`]),
      );
      return;
    }
    this.preview = p;
    this.renderPreview(after);
    if (!after) this.renderApply();
  }

  private renderPreview(after?: string): void {
    const p = this.preview!;
    const items = p.items ?? [];
    const rows = diffItems(items);
    const nChanged = rows.filter((r) => r.changed).length;
    const byAddr = new Map(rows.map((r) => [r.a, r]));
    const summary = after
      ? `${after} ${nChanged === 0 ? "Every register now matches the profile." : `${nChanged} of ${rows.length} registers still differ.`}`
      : `${nChanged} of ${rows.length} registers change.`;
    const onlyChanged = el("input", { type: "checkbox" }) as HTMLInputElement;
    onlyChanged.checked = this.onlyChanged;
    onlyChanged.addEventListener("change", () => {
      this.onlyChanged = onlyChanged.checked;
      this.renderPreview(after);
    });
    const shown = this.onlyChanged ? rows.filter((r) => r.changed) : rows;
    this.previewEl.replaceChildren(
      el("div", { class: after && nChanged === 0 ? "banner ok" : "banner" }, [
        el("strong", {}, [summary]),
        p.variant !== null && p.variant !== undefined && this.option?.needreadtype
          ? ` Variant ${p.variant + 1} (register 5479 = ${p.reg5479 ?? "?"}).`
          : "",
        p.frames ? ` ${p.frames.length} write frame(s).` : "",
      ]),
      // Items the dongle will not write: the model's power stage would ignore these values.
      p.note ? el("div", { class: "banner warn" }, [`Skipped, outside the power-stage range: ${p.note}.`]) : "",
      el("label", { class: "toggle" }, [onlyChanged, "Only changed registers"]),
      el("table", { class: "diff-table" }, [
        el("thead", {}, [
          el("tr", {}, [el("th", {}, ["Register"]), el("th", {}, ["Setting"]), el("th", {}, ["Current"]), el("th", {}, ["New"])]),
        ]),
        el(
          "tbody",
          {},
          shown.map((r) =>
            el("tr", { class: r.changed ? "changed" : "" }, [
              el("td", { class: "num" }, [String(r.a)]),
              el("td", {}, [registerLabel(this.family(), r.a), r.kind && r.kind !== "set" ? el("span", { class: "pill" }, [r.kind]) : ""]),
              el("td", { class: "num" }, [this.cell(r, byAddr, "cur")]),
              el("td", { class: "num" }, [this.cell(r, byAddr, "new")]),
            ]),
          ),
        ),
      ]),
    );
  }

  /** A diff cell formatted with the catalogue (U32 rows combine their two words, lo first). */
  private cell(r: DiffRow, byAddr: Map<number, DiffRow>, which: "cur" | "new"): string {
    const raw = which === "cur" ? r.cur : r.new;
    if (raw === null) return "—";
    const fam = this.family();
    const s = catalogueRow(fam, r.a);
    if (!s) return catalogueRow(fam, r.a - 1)?.dtype === "U32" ? `0x${raw.toString(16).padStart(4, "0")}` : String(raw);
    if (s.dtype === "U32") {
      const hiRow = byAddr.get(r.a + 1);
      const hi = hiRow ? (which === "cur" ? hiRow.cur : hiRow.new) : null;
      return hi === null ? "—" : formatSettingRaw(s, u32Join(raw, hi));
    }
    return formatSettingRaw(s, raw);
  }

  // ---- apply ------------------------------------------------------------------------------------

  private renderApply(): void {
    const btn = el("button", { class: "btn primary", type: "button", onClick: () => void this.apply() }, ["Apply profile…"]);
    this.applyEl.replaceChildren(el("div", { class: "unlock-bar" }, [btn]));
  }

  private async apply(): Promise<void> {
    const dev = this.device;
    const opt = this.option;
    const p = this.preview;
    if (!dev || !opt || !p || this.busy) return;
    const rows = diffItems(p.items ?? []);
    const total = rows.length;
    const nChanged = rows.filter((r) => r.changed).length;
    const variant =
      opt.needreadtype && p.variant !== null && p.variant !== undefined
        ? `Register 5479 = ${p.reg5479 ?? "?"} → variant ${p.variant + 1} of ${opt.variants.length} (5479 ∈ {${opt.variants[p.variant]?.ref5479.join(", ") ?? "?"}}).`
        : "Single profile (no machine-type variant).";
    const ok = await confirmDialog(
      "Apply grid profile",
      [
        el("dl", { class: "kv" }, [
          el("dt", {}, ["Inverter"]),
          el("dd", {}, [`${dev.model || "device " + dev.index} (${this.family()})`]),
          el("dt", {}, ["Country"]),
          el("dd", {}, [this.countryLabel(this.country)]),
          el("dt", {}, ["Standard"]),
          el("dd", {}, [`${opt.label} (${opt.code})`]),
          el("dt", {}, ["Variant"]),
          el("dd", {}, [variant]),
        ]),
        el("p", {}, [
          el("strong", {}, [`Writes all ${total} registers, like the stock dongle; ${nChanged} of them change.`]),
          " The country register goes first, then the standard's protection and power-quality settings.",
        ]),
        el("p", { class: "muted" }, [
          "Grid-protection settings are a compliance matter: apply only the standard your network operator requires.",
        ]),
      ],
      `Write ${total} registers`,
    );
    if (!ok) return;
    this.busy = true;
    this.previewSeq++; // stop any preview poll
    const status = el("p", {}, ["Applying…"]);
    const progress = el("div", {}, []);
    const box = el("div", { class: "banner" }, [status, progress]);
    this.applyEl.replaceChildren(box);
    try {
      const c = await api.confirm(dev.index, api.SETUP_APPLY_CONFIRM_ADDR);
      const r = await api.setupApply(dev.index, opt.code, c.token);
      if (!r.ok) {
        box.className = "banner err";
        status.textContent = `Rejected: ${verdictText(r)}`;
        if (r.verdict === "PREVIEW_REQUIRED") this.applyEl.append(this.repreviewButton());
        return;
      }
      const res = await waitForBatch(dev.index, this.abort.signal, (sp) => progress.replaceChildren(progressView(sp)), r.total);
      const failed = !res || res.busy || res.failed > 0;
      status.textContent = !res
        ? "No progress reported by the dongle."
        : res.busy
          ? "Timed out waiting for the apply."
          : res.failed
            ? `Applied with ${res.failed} failed register(s).`
            : `Applied ${res.ok} register(s) ✓`;
      box.className = failed ? "banner err" : "banner ok";
    } catch (err) {
      if (this.abort.signal.aborted) return;
      box.className = "banner err";
      status.textContent = (err as Error).message;
      return;
    } finally {
      this.busy = false;
    }
    await this.runPreview("After apply:");
    this.applyEl.replaceChildren(box);
  }

  private repreviewButton(): HTMLElement {
    return el("button", { class: "btn", type: "button", onClick: () => void this.runPreview() }, ["Preview again"]);
  }

  // ---- start ------------------------------------------------------------------------------------

  private renderStart(): void {
    const reg = descriptorRegs(this.opts?.descriptor)?.powerSwitch;
    if (!reg) return;
    const result = el("span", { class: "ctrl-result muted" }, []);
    const btn = el("button", { class: "btn", type: "button" }, ["Start inverter…"]) as HTMLButtonElement;
    btn.addEventListener("click", () => void this.start(reg, btn, result));
    this.startEl.replaceChildren(
      el("h2", { class: "settings-title" }, ["Start"]),
      el("p", { class: "muted settings-desc" }, [
        `After a profile is applied the stock dongle starts the inverter by writing 0xCF to register ${reg}.`,
      ]),
      el("div", { class: "unlock-bar" }, [btn, result]),
    );
    this.startEl.hidden = false;
  }

  private async start(reg: number, btn: HTMLButtonElement, result: HTMLElement): Promise<void> {
    const dev = this.device;
    if (!dev) return;
    const ok = await confirmDialog(
      "Start inverter",
      [el("p", {}, [`Write 0xCF (start) to register ${reg} of ${dev.model || "device " + dev.index}?`])],
      "Start",
    );
    if (!ok) return;
    btn.disabled = true;
    result.textContent = "…";
    try {
      const c = await api.confirm(dev.index, api.SETUP_START_CONFIRM_ADDR);
      const r = await api.setupStart(dev.index, c.token);
      result.textContent = r.ok ? "Start queued ✓" : verdictText(r);
      result.className = `ctrl-result ${r.ok ? "ok" : "err"}`;
    } catch (err) {
      result.textContent = (err as Error).message;
      result.className = "ctrl-result err";
    } finally {
      btn.disabled = false;
    }
  }
}
