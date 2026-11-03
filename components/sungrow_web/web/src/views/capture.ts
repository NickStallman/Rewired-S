import { api, sleep, type CaptureDevice, type CaptureKind, type CaptureResult, type CaptureStatus } from "../api.js";
import { el } from "../dom.js";
import { confirmDialog } from "./setup.js";
import type { View } from "./types.js";

// Diagnostic captures: start an I-V scan or a grid-waveform recording on the inverter, or export
// its stored fault recordings, follow the progress, and download the raw file plus a metadata
// JSON (timeline, signal snapshots) for analysis. The files stay on the dongle until it reboots.

const POLL_MS = 1000;
/** Approximate I-V sweep time per PV input. */
const IV_S_PER_INPUT = 30;

const KIND_LABEL: Record<CaptureKind, string> = {
  iv: "I-V scan",
  grid: "Grid waveform",
  fault: "Fault recordings",
};

const FAULT_TEXT =
  "Exports the inverter's stored fault recordings (the newest 10, one per trip). Nothing new is recorded; " +
  "the inverter copies its store first, and its readings freeze while it does (about 20 s per record).";
const POLL_NOTE = "Polling pauses for the file transfer (a few seconds; longer for a large grid recording).";

function when(r: CaptureResult): string {
  if (!r.wall_start) return "time unknown";
  return new Date(r.wall_start * 1000).toLocaleString();
}

function seconds(ms: number): string {
  return `${Math.round(ms / 1000)} s`;
}

export class CaptureView implements View {
  readonly id = "capture";
  readonly label = "Captures";
  private abort = new AbortController();
  private statusEl!: HTMLElement;
  private actionsEl!: HTMLElement;
  private resultsEl!: HTMLElement;
  private last: CaptureStatus | null = null;
  private lastGen = -1;
  private busy = false;
  /** Device this page last started a capture on; used when the status carries no device index. */
  private startedOn: number | null = null;

  render(root: HTMLElement): void {
    this.abort = new AbortController();
    this.statusEl = el("div", {}, []);
    this.actionsEl = el("div", {}, []);
    this.resultsEl = el("div", {}, []);
    root.append(
      el("h1", { class: "view-title" }, ["Captures"]),
      el("p", { class: "muted" }, [
        "Ask the inverter for an I-V curve of its PV strings, a recording of the grid waveform or its stored " +
          "fault recordings, then view them here or download the raw file for analysis. Captures are kept until " +
          "the dongle restarts.",
      ]),
      this.statusEl,
      this.actionsEl,
      this.resultsEl,
    );
    void this.loop();
  }

  destroy(): void {
    this.abort.abort();
  }

  private async loop(): Promise<void> {
    const signal = this.abort.signal;
    for (;;) {
      try {
        const s = await api.captureStatus(signal);
        this.update(s);
      } catch (err) {
        if (signal.aborted) return;
        this.statusEl.replaceChildren(el("div", { class: "banner err" }, [(err as Error).message]));
      }
      try {
        await sleep(this.last?.running ? POLL_MS : 4000, signal);
      } catch {
        return;
      }
    }
  }

  private update(s: CaptureStatus): void {
    const wasRunning = this.last?.running ?? false;
    this.last = s;
    if (!s.available) {
      this.statusEl.replaceChildren(el("div", { class: "banner" }, ["Captures are not part of this firmware build."]));
      this.actionsEl.replaceChildren();
      this.resultsEl.replaceChildren();
      return;
    }
    if (s.running) {
      this.statusEl.replaceChildren(el("div", { class: "banner" }, this.runningLine(s)));
    } else {
      this.statusEl.replaceChildren();
    }
    if (wasRunning !== !!s.running || this.actionsEl.childElementCount === 0) this.renderActions(s);
    if (s.generation !== this.lastGen) {
      this.lastGen = s.generation ?? 0;
      this.renderResults(s.results ?? []);
    }
  }

  private runningLine(s: CaptureStatus): (Node | string)[] {
    const kind = s.kind ?? "grid";
    const cancel = el("button", { class: "btn", type: "button", onClick: () => void api.captureCancel() }, ["Cancel"]);
    const out: (Node | string)[] = [el("strong", {}, [KIND_LABEL[kind]])];
    if (kind === "iv" && s.step === "scanning") {
      // The inverter reports no per-input progress: estimate from its PV input count.
      const devs = s.devices ?? [];
      const running = s.device ?? this.startedOn;
      const dev = devs.find((d) => d.device === running) ?? devs.find((d) => d.iv) ?? devs[0];
      const total = (dev?.mppt || 2) * IV_S_PER_INPUT;
      const done = Math.round((s.scan_ms ?? s.elapsed_ms ?? 0) / 1000);
      out.push(
        `: scanning · ${done} s of about ${total} s `,
        el("progress", { class: "estimate", max: total, value: Math.min(done, total) }, []),
        " ",
        el("span", { class: "muted" }, ["(estimated from the number of PV inputs; the inverter does not report which input it is on)"]),
      );
    } else {
      const pct = s.progress !== undefined && s.progress >= 0 ? ` (${s.progress} %)` : "";
      out.push(`: ${s.step ?? ""}${pct}, ${seconds(s.elapsed_ms ?? 0)}`);
    }
    if (kind === "iv" && s.iv_fault) {
      out.push(` · inverter reports code ${s.iv_fault}${s.iv_fault_text ? `: ${s.iv_fault_text}` : ""}`);
    }
    out.push(" ", cancel);
    return out;
  }

  private renderActions(s: CaptureStatus): void {
    const devs = s.devices ?? [];
    if (devs.length === 0) {
      this.actionsEl.replaceChildren(el("div", { class: "banner" }, ["No inverter found yet."]));
      return;
    }
    this.actionsEl.replaceChildren(...devs.map((d) => this.deviceSection(d, !!s.running)));
  }

  private deviceSection(d: CaptureDevice, running: boolean): HTMLElement {
    const result = el("span", { class: "ctrl-result" }, []);
    const disabled = running || this.busy || !d.online;
    const ivBtn = el("button", { class: "btn primary", type: "button", disabled: disabled || !d.iv }, [
      "Run I-V scan…",
    ]) as HTMLButtonElement;
    ivBtn.addEventListener("click", () => void this.start(d, "iv", 1, result));
    const rows: HTMLElement[] = [
      el("div", { class: "settings-row" }, [
        el("div", { class: "settings-label" }, [
          el("span", { class: "settings-name" }, ["I-V scan"]),
          el("span", { class: "settings-desc muted" }, [
            d.iv
              ? "The inverter sweeps each PV input from the operating point down to 50 V, about 30 s per input. " +
                "PV output dips while it runs."
              : "This inverter does not report I-V scan support.",
          ]),
        ]),
        ivBtn,
      ]),
    ];
    if (d.grid) {
      const cycles = el("select", { "aria-label": "Cycles" }, [
        ...[1, 2, 3, 4, 5, 6, 7, 8].map((n) => el("option", { value: String(n), selected: n === 1 }, [String(n)])),
      ]) as HTMLSelectElement;
      const gridBtn = el("button", { class: "btn primary", type: "button", disabled }, [
        "Record grid waveform…",
      ]) as HTMLButtonElement;
      gridBtn.addEventListener("click", () => void this.start(d, "grid", Number(cycles.value), result));
      rows.push(
        el("div", { class: "settings-row" }, [
          el("div", { class: "settings-label" }, [
            el("span", { class: "settings-name" }, ["Grid waveform"]),
            el("span", { class: "settings-desc muted" }, [
              "The inverter records the grid waveform (the cycle count is passed to it as is). Values freeze for a " +
                "few seconds while it copies the recording.",
            ]),
          ]),
          el("label", { class: "muted" }, ["Cycles ", cycles]),
          gridBtn,
        ]),
      );
    }
    if (d.fault) {
      const faultBtn = el("button", { class: "btn primary", type: "button", disabled }, [
        "Export fault recordings…",
      ]) as HTMLButtonElement;
      faultBtn.addEventListener("click", () => void this.start(d, "fault", 1, result));
      rows.push(
        el("div", { class: "settings-row" }, [
          el("div", { class: "settings-label" }, [
            el("span", { class: "settings-name" }, ["Fault recordings"]),
            el("span", { class: "settings-desc muted" }, [FAULT_TEXT]),
          ]),
          faultBtn,
        ]),
      );
    }
    return el("section", { class: "settings-section" }, [
      el("h2", { class: "settings-title" }, [`${d.model || "Inverter"} (device ${d.device})${d.online ? "" : ", offline"}`]),
      el("div", { class: "settings-grid" }, rows),
      result,
    ]);
  }

  private async start(d: CaptureDevice, kind: CaptureKind, cycles: number, result: HTMLElement): Promise<void> {
    const ivTotal = (d.mppt || 2) * IV_S_PER_INPUT;
    const dialog: Record<CaptureKind, [string, string, string]> = {
      iv: [
        "Run an I-V scan",
        "The inverter interrupts normal MPPT and sweeps every PV input from the operating point down to 50 V. " +
          `Output drops while it scans (about ${ivTotal} s).`,
        "Start scan",
      ],
      grid: [
        "Record the grid waveform",
        `The inverter records ${cycles} cycle(s) of the grid waveform and copies it to its flash.`,
        "Start recording",
      ],
      fault: ["Export the fault recordings", FAULT_TEXT, "Start export"],
    };
    const [title, body, okLabel] = dialog[kind];
    const ok = await confirmDialog(title, [el("p", {}, [body]), el("p", { class: "muted" }, [POLL_NOTE])], okLabel);
    if (!ok) return;
    this.busy = true;
    this.startedOn = d.device;
    result.className = "ctrl-result muted";
    result.textContent = "Starting…";
    try {
      const c = await api.confirm(d.device, api.CAPTURE_CONFIRM_ADDR);
      const r = await api.captureStart(d.device, kind, cycles, c.token);
      result.className = r.ok ? "ctrl-result ok" : "ctrl-result err";
      result.textContent = r.ok ? "Started." : `Refused: ${r.message}`;
    } catch (err) {
      result.className = "ctrl-result err";
      result.textContent = (err as Error).message;
    } finally {
      this.busy = false;
    }
    // Pick the running state up at once.
    try {
      this.update(await api.captureStatus(this.abort.signal));
    } catch {
      /* the poll loop reports errors */
    }
  }

  private renderResults(results: CaptureResult[]): void {
    if (results.length === 0) {
      this.resultsEl.replaceChildren(el("p", { class: "muted" }, ["No captures yet."]));
      return;
    }
    const viewHref = (r: CaptureResult): string | null =>
      !r.ok || !r.size ? null : r.kind === "iv" ? `#/iv/${r.id}` : `#/wave/${r.id}`;
    const label = (r: CaptureResult): string =>
      r.kind === "iv"
        ? "I-V scan"
        : r.kind === "fault"
          ? `Fault recordings (${r.records} record${r.records === 1 ? "" : "s"})`
          : `Grid waveform (${r.cycles})`;
    const rows = results.map((r) =>
      el("tr", {}, [
        el("td", {}, [label(r)]),
        el("td", {}, [when(r)]),
        el("td", { class: r.ok ? "" : "muted" }, [r.result]),
        el("td", {}, [r.size ? `${r.size} B` : "–"]),
        el("td", {}, [seconds(r.duration_ms)]),
        el("td", {}, [
          r.size
            ? el("a", { href: api.captureFileUrl(r.id), download: "" }, ["file"])
            : el("span", { class: "muted" }, ["file"]),
          " · ",
          el("a", { href: api.captureMetaUrl(r.id), download: "" }, ["metadata"]),
          ...(viewHref(r) ? [" · ", el("a", { href: viewHref(r)! }, ["view"])] : []),
        ]),
      ]),
    );
    const firstIv = results.find((r) => r.kind === "iv" && viewHref(r));
    this.resultsEl.replaceChildren(
      el("section", { class: "settings-section" }, [
        el("div", { class: "results-head" }, [
          el("h2", { class: "settings-title" }, ["Results"]),
          el("span", { class: "results-links" }, [
            ...(results.some((r) => r.kind !== "iv" && viewHref(r))
              ? [el("a", { class: "btn", href: "#/wave" }, ["Open the waveform viewer"])]
              : []),
            ...(firstIv ? [el("a", { class: "btn", href: `#/iv/${firstIv.id}` }, ["Open the I-V viewer"])] : []),
          ]),
        ]),
        el("table", { class: "diff-table" }, [
          el("thead", {}, [
            el("tr", {}, ["Capture", "Started", "Result", "Size", "Took", "Download"].map((h) => el("th", {}, [h]))),
          ]),
          el("tbody", {}, rows),
        ]),
      ]),
    );
  }
}
