import { api, sleep, type CaptureResult } from "../api.js";
import { el, svg } from "../dom.js";
import {
  channelInfo,
  decodeRecordFile,
  LANES,
  niceTicks,
  stats,
  type ChannelStats,
  type GridWave,
  type Lane,
  type WaveChannel,
} from "../gridwave.js";
import type { View } from "./types.js";

// Grid-waveform viewer: the SH10RS records 30 channels (320 samples each) in one go. The rail on
// the left lists the requested grid recordings and every record of an exported fault-recording
// file, each with a thumbnail of its AC voltage at its true time span; the lanes share one time
// axis and one cursor. Drag across a trace to zoom, double-click to zoom out. Files are fetched
// raw from /api/capture_file and decoded here. Routes: #/wave/<id> and #/wave/<id>/<k>, where k
// is the record number within the file (1..N).

const VB_W = 1000; // plot viewBox width; height per lane below
const SLOTS = 3; // trace colours per lane (validated all-pairs)

/** One rail entry: record k (0-based) of the n in a capture's file. */
interface Entry {
  rec: CaptureResult;
  k: number;
  n: number;
}

const hex4 = (v: number) => v.toString(16).padStart(4, "0");

function fmt(v: number, unit: string): string {
  if (!Number.isFinite(v)) return "–";
  return unit ? `${v.toFixed(1)} ${unit}` : `${Math.round(v)}`;
}

function fmtMs(ms: number): string {
  return ms >= 100 ? `${ms.toFixed(0)} ms` : ms >= 10 ? `${ms.toFixed(1)} ms` : `${ms.toFixed(2)} ms`;
}

function rateText(fs: number): string {
  return fs >= 1000 ? `${(fs / 1000).toFixed(fs % 1000 ? 2 : 1)} kHz` : `${fs.toFixed(1)} Hz`;
}

/** "03:11:20" and "30 Oct 2026" from the record's local time. */
function splitTime(t: string): { clock: string; date: string } {
  const m = /^(\d+)-(\d+)-(\d+) (\d\d:\d\d:\d\d)$/.exec(t);
  if (!m) return { clock: t, date: "" };
  const d = new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]));
  return { clock: m[4], date: d.toLocaleDateString(undefined, { day: "numeric", month: "short", year: "numeric" }) };
}

/** SVG path of samples [a, b) mapped to x 0..w and y by `y`. */
function tracePath(s: Int16Array, a: number, b: number, w: number, y: (v: number) => number): string {
  const lo = Math.max(0, Math.floor(a));
  const hi = Math.min(s.length - 1, Math.ceil(b));
  const span = b - a || 1;
  let d = "";
  for (let i = lo; i <= hi; i++) d += `${i === lo ? "M" : "L"}${(((i - a) / span) * w).toFixed(2)},${y(s[i]).toFixed(2)}`;
  return d;
}

interface LaneUi {
  lane: Lane;
  wrap: HTMLElement;
  chans: WaveChannel[];
  plot: HTMLElement;
  svgEl: SVGSVGElement;
  grid: SVGGElement;
  traces: SVGPathElement[];
  ylabels: HTMLElement;
  dots: HTMLElement[];
  values: HTMLElement[];
  height: number;
  yr: [number, number];
}

export class WaveView implements View {
  readonly id = "wave";
  readonly label = "Waveforms";
  readonly navHidden = true;
  readonly navParent = "capture";

  private abort = new AbortController();
  private cache = new Map<number, GridWave[]>();
  private hidden = new Set<number>(); // channel ids switched off (kept across recordings)
  private recs: CaptureResult[] = [];
  private cur: { rec: CaptureResult; wave: GridWave; k: number; n: number } | null = null;
  private win: [number, number] = [0, 319];
  private cursor: number | null = null;
  private root!: HTMLElement;
  private railList!: HTMLElement;
  private body!: HTMLElement;
  private lanes: LaneUi[] = [];
  private axis!: HTMLElement;
  private cursorText!: HTMLElement;
  private resetBtn!: HTMLButtonElement;
  private brush: { lane: LaneUi; x0: number; x1: number; box: HTMLElement } | null = null;

  render(root: HTMLElement, param?: string): void {
    this.abort = new AbortController();
    this.root = root;
    root.closest("main")?.classList.add("wide");
    this.railList = el("div", { class: "wv-rail-groups" }, []);
    this.body = el("section", { class: "wv-body", "aria-live": "polite" }, [el("p", { class: "muted" }, ["Loading recordings…"])]);
    root.append(
      el("div", { class: "wv" }, [el("aside", { class: "wv-rail", "aria-label": "Recordings" }, [this.railList]), this.body]),
    );
    document.addEventListener("keydown", (e) => this.onKey(e), { signal: this.abort.signal });
    const [id, k] = (param ?? "").split("/");
    void this.load(id ? Number(id) : undefined, k ? Math.max(0, Number(k) - 1) : 0);
  }

  destroy(): void {
    this.abort.abort();
    this.root?.closest("main")?.classList.remove("wide");
  }

  // ---- data ------------------------------------------------------------------------------------

  private async load(want?: number, wantK = 0): Promise<void> {
    const signal = this.abort.signal;
    try {
      const s = await api.captureStatus(signal);
      const all = (s.results ?? []).filter((r) => (r.kind === "grid" || r.kind === "fault") && r.ok && r.size > 0);
      // Rail order: requested recordings first, then the fault-recording files.
      this.recs = [...all.filter((r) => r.kind === "grid"), ...all.filter((r) => r.kind === "fault")];
    } catch (err) {
      if (signal.aborted) return;
      this.body.replaceChildren(el("div", { class: "banner err" }, [`Could not list the recordings: ${(err as Error).message}`]));
      return;
    }
    if (this.recs.length === 0) {
      this.railList.replaceChildren();
      this.body.replaceChildren(
        el("div", { class: "wv-empty" }, [
          el("p", { class: "wv-empty-title" }, ["No grid or fault recordings on the dongle"]),
          el("p", { class: "muted" }, [
            "Record the grid waveform or export the fault recordings on the Captures page. Recordings stay on the " +
              "dongle until it restarts.",
          ]),
          el("a", { class: "btn primary", href: "#/capture" }, ["Go to Captures"]),
        ]),
      );
      return;
    }
    this.renderRail();
    const first = this.recs.find((r) => r.id === want) ?? this.recs[0];
    await this.select(first.id, first.id === want ? wantK : 0);
    // Thumbnails for the rest, one file at a time (a grid file is ~20 KB).
    for (const r of this.recs) {
      if (signal.aborted) return;
      if (!this.cache.has(r.id)) {
        try {
          await this.fetchWaves(r.id);
        } catch {
          /* the thumbnails stay empty */
        }
      }
      this.renderRail();
      await sleep(0, signal).catch(() => undefined);
    }
  }

  private async fetchWaves(id: number): Promise<GridWave[]> {
    const hit = this.cache.get(id);
    if (hit) return hit;
    const res = await fetch(api.captureFileUrl(id), { signal: this.abort.signal });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const waves = decodeRecordFile(await res.arrayBuffer());
    this.cache.set(id, waves);
    return waves;
  }

  /** Records per capture as far as known: the decoded file, else the status count. */
  private entries(): Entry[] {
    return this.recs.flatMap((rec) => {
      const n = this.cache.get(rec.id)?.length ?? (rec.kind === "fault" ? Math.max(1, rec.records || 1) : 1);
      return Array.from({ length: n }, (_, k) => ({ rec, k, n }));
    });
  }

  private async select(id: number, k = 0): Promise<void> {
    const rec = this.recs.find((r) => r.id === id);
    if (!rec) return;
    try {
      const waves = await this.fetchWaves(id);
      const kk = Math.min(Math.max(0, k), waves.length - 1);
      const wave = waves[kk];
      const keepWindow = this.cur?.wave.samples === wave.samples && this.cur?.wave.fs === wave.fs;
      this.cur = { rec, wave, k: kk, n: waves.length };
      if (!keepWindow) this.win = [0, wave.samples - 1];
      if (this.cursor !== null && this.cursor >= wave.samples) this.cursor = null;
      history.replaceState(null, "", rec.kind === "fault" ? `#/wave/${id}/${kk + 1}` : `#/wave/${id}`);
      this.renderRail();
      this.renderBody();
    } catch (err) {
      if (this.abort.signal.aborted) return;
      this.body.replaceChildren(el("div", { class: "banner err" }, [`Could not load recording ${id}: ${(err as Error).message}`]));
    }
  }

  private step(dir: number): void {
    if (!this.cur) return;
    const list = this.entries();
    const i = list.findIndex((e) => e.rec.id === this.cur!.rec.id && e.k === this.cur!.k);
    const next = list[i + dir];
    if (next) void this.select(next.rec.id, next.k);
  }

  // ---- rail -------------------------------------------------------------------------------------

  private renderRail(): void {
    const list = this.entries();
    const groups: [string, Entry[]][] = (
      [
        ["Grid recordings", list.filter((e) => e.rec.kind === "grid")],
        ["Fault recordings", list.filter((e) => e.rec.kind === "fault")],
      ] as [string, Entry[]][]
    ).filter(([, es]) => es.length > 0);
    this.railList.replaceChildren(
      ...groups.map(([title, es], g) =>
        el("div", { class: "wv-rail-group" }, [
          el("div", { class: "wv-rail-head" }, [
            el("span", { class: "wv-eyebrow" }, [title]),
            g === 0 ? el("a", { class: "wv-rail-new", href: "#/capture" }, ["Record"]) : "",
          ]),
          el("div", { class: "wv-rail-list", role: "list" }, es.map((e) => this.railEntry(e))),
        ]),
      ),
    );
  }

  private railEntry({ rec, k, n }: Entry): HTMLElement {
    const wave = this.cache.get(rec.id)?.[k];
    const t = wave ? splitTime(wave.time) : null;
    const thumb = svg("svg", { class: "wv-thumb", viewBox: "0 0 120 32", preserveAspectRatio: "none", "aria-hidden": "true" });
    const ac = wave?.channels.find((c) => c.id === 100);
    if (ac) {
      let peak = 1;
      for (const v of ac.samples) peak = Math.max(peak, Math.abs(v));
      thumb.append(
        svg("path", {
          d: tracePath(ac.samples, 0, ac.samples.length - 1, 120, (v) => 16 - (v / peak) * 13),
          "vector-effect": "non-scaling-stroke",
        }),
      );
    }
    const fault = rec.kind === "fault";
    const current = this.cur?.rec.id === rec.id && this.cur.k === k;
    let meta: (HTMLElement | string)[];
    if (fault) {
      const first = wave?.faults[0];
      meta = [
        el("span", { class: "wv-rec-meta wide" }, [`Fault record ${k + 1} of ${n}`]),
        el("span", { class: `wv-rec-note${first ? "" : " muted"}` }, [
          wave ? (first ? (first.name ?? `Code ${first.code}`) : "no fault bits") : "",
        ]),
      ];
    } else {
      const cycles = wave?.cycles ?? rec.cycles;
      const span = wave ? fmtMs((wave.samples / wave.fs) * 1000) : "";
      meta = [el("span", { class: "wv-rec-meta" }, [`${cycles} cycle${cycles === 1 ? "" : "s"}${span ? ` · ${span}` : ""}`])];
    }
    return el(
      "button",
      {
        class: `wv-rec${fault ? " fault" : ""}`,
        type: "button",
        role: "listitem",
        "aria-current": current ? "true" : undefined,
        onClick: () => void this.select(rec.id, k),
      },
      [thumb, el("span", { class: "wv-rec-time" }, [t?.clock ?? `#${rec.id}`]), ...meta],
    );
  }

  // ---- body -------------------------------------------------------------------------------------

  private renderBody(): void {
    const { rec, wave, k, n } = this.cur!;
    const fault = rec.kind === "fault";
    const t = splitTime(wave.time);
    const spanMs = (wave.samples / wave.fs) * 1000;
    const list = this.entries();
    const idx = list.findIndex((e) => e.rec.id === rec.id && e.k === k);
    const prev = el("button", { class: "btn wv-nav", type: "button", disabled: idx <= 0, title: "Previous recording in the list ( [ )", onClick: () => this.step(-1) }, ["‹ Previous"]);
    const next = el("button", { class: "btn wv-nav", type: "button", disabled: idx >= list.length - 1, title: "Next recording in the list ( ] )", onClick: () => this.step(1) }, ["Next ›"]);
    this.cursorText = el("span", { class: "wv-cursor-text" }, []);
    this.resetBtn = el("button", { class: "btn wv-reset", type: "button", onClick: () => this.zoom(0, wave.samples - 1) }, ["Zoom out"]) as HTMLButtonElement;
    this.axis = el("div", { class: "wv-axis", "aria-hidden": "true" }, []);

    const head = el("header", { class: "wv-head" }, [
      el("div", { class: "wv-title" }, [
        el("span", { class: "wv-eyebrow" }, [
          fault ? `Fault record ${k + 1} of ${n} · slot ${wave.slot} · inverter clock` : `Slot ${wave.slot} of 10 · inverter clock`,
        ]),
        el("h1", { class: "wv-clock" }, [t.clock, el("small", {}, [t.date])]),
        el(
          "dl",
          { class: "wv-facts" },
          (
            [
              ["Source", fault ? "Fault recording" : "Requested recording"],
              ["Cycle setting", String(wave.cycles)],
              ["Sample rate", rateText(wave.fs)],
              ["Window", fmtMs(spanMs)],
              ["Channels", String(wave.channels.length)],
            ] as const
          ).map(([k, v]) => el("div", {}, [el("dt", {}, [k]), el("dd", {}, [v])])),
        ),
      ]),
      el("div", { class: "wv-actions" }, [
        el("div", { class: "wv-navs" }, [prev, next]),
        el("div", { class: "wv-downloads" }, [
          el("a", { class: "btn", href: api.captureFileUrl(rec.id), download: "" }, ["Download file"]),
          el("a", { class: "btn", href: api.captureMetaUrl(rec.id), download: "" }, ["Metadata"]),
        ]),
      ]),
    ]);

    const toolbar = el("div", { class: "wv-toolbar" }, [
      this.cursorText,
      el("span", { class: "wv-hint muted" }, ["Drag across a trace to zoom · double-click to zoom out · ← → move the cursor"]),
      this.resetBtn,
    ]);

    const scope = el("div", { class: "wv-scope", tabindex: 0, "aria-label": "Waveforms. Arrow keys move the cursor, plus and minus zoom, Escape zooms out." }, []);
    this.lanes = [];
    for (const lane of LANES) {
      const chans = lane.ids.map((id) => wave.channels.find((c) => c.id === id)).filter((c): c is WaveChannel => !!c);
      if (chans.length === 0) continue;
      const ui = this.buildLane(lane, chans.slice(0, SLOTS), wave);
      scope.append(ui.wrap);
      this.lanes.push(ui);
    }
    scope.append(this.axis);

    this.body.replaceChildren(head, this.statusCard(wave), toolbar, scope, this.othersSection(wave), this.tableSection(wave));
    this.redraw();
  }

  /** The inverter's fault bits and raw status words at the moment the record was taken. */
  private statusCard(wave: GridWave): HTMLElement {
    const chips = wave.faults.length
      ? el(
          "ul",
          { class: "wv-faults" },
          wave.faults.map((f) =>
            el("li", { class: "wv-fault", title: `Status word ${f.word} bit ${f.bit}` }, [f.name ? `${f.code} · ${f.name}` : String(f.code)]),
          ),
        )
      : el("p", { class: "wv-nofault muted" }, ["No fault bits set at the trigger"]);
    const sw = wave.statusWords;
    return el("section", { class: "wv-status" }, [
      el("h2", {}, ["Status at the trigger"]),
      chips,
      el("p", { class: "wv-status-words" }, [
        `Status words 0-3 ${sw.slice(0, 4).map(hex4).join(" ")} · transient 26-27 ${wave.transient.map(hex4).join(" ")}`,
      ]),
    ]);
  }

  private buildLane(lane: Lane, chans: WaveChannel[], wave: GridWave): LaneUi {
    const height = lane.symmetric ? 176 : 104;
    const svgEl = svg("svg", { class: "wv-svg", viewBox: `0 0 ${VB_W} ${height}`, preserveAspectRatio: "none", "aria-hidden": "true" });
    const grid = svg("g", { class: "wv-grid" });
    svgEl.append(grid);
    const traces = chans.map((_, k) => {
      const p = svg("path", { class: `wv-trace s${k + 1}`, "vector-effect": "non-scaling-stroke" });
      svgEl.append(p);
      return p;
    });
    const ylabels = el("div", { class: "wv-ylabels" }, []);
    const dots = chans.map((_, k) => el("span", { class: `wv-dot s${k + 1}` }, []));
    const plot = el("div", { class: "wv-plot", style: `height:${height}px` }, [ylabels, svgEl, el("span", { class: "wv-cline" }, []), ...dots]);

    const values: HTMLElement[] = [];
    const legend = chans.map((c, k) => {
      const info = channelInfo(c.id);
      const st = stats(c.samples, wave.fs, info.scale);
      const value = el("strong", { class: "wv-val" }, []);
      values.push(value);
      const chip = el(
        "button",
        {
          class: `wv-chip s${k + 1}`,
          type: "button",
          "aria-pressed": this.hidden.has(c.id) ? "false" : "true",
          title: `Show or hide ${info.label}`,
          onClick: () => {
            if (this.hidden.has(c.id)) this.hidden.delete(c.id);
            else this.hidden.add(c.id);
            chip.setAttribute("aria-pressed", this.hidden.has(c.id) ? "false" : "true");
            this.redraw();
          },
        },
        [
          el("span", { class: "wv-swatch", "aria-hidden": "true" }, []),
          el("span", { class: "wv-chip-label" }, [info.label]),
          value,
          el("span", { class: "wv-chip-stat" }, [this.statText(st, info.unit, lane)]),
        ],
      );
      return chip;
    });

    const wrap = el("div", { class: `wv-lane ${lane.symmetric ? "ac" : "dc"}` }, [
      el("div", { class: "wv-lane-head" }, [
        el("div", { class: "wv-lane-title" }, [el("h2", {}, [lane.title]), el("span", { class: "muted" }, [lane.note])]),
        el("div", { class: "wv-legend" }, legend),
      ]),
      plot,
    ]);
    const ui: LaneUi = { lane, wrap, chans, plot, svgEl, grid, traces, ylabels, dots, values, height, yr: [0, 1] };
    this.bindPointer(ui);
    return ui;
  }

  private statText(st: ChannelStats, unit: string, lane: Lane): string {
    // A frequency only in the mains band: a channel dominated by switching ripple has none worth showing.
    const mains = st.freq !== null && st.freq > 40 && st.freq < 70;
    if (lane.symmetric) return `rms ${fmt(st.acRms, unit)}${mains ? ` · ${st.freq!.toFixed(2)} Hz` : ""}`;
    const pp = st.max - st.min;
    return `ripple ${unit ? `${pp.toFixed(1)} ${unit}` : Math.round(pp)} p-p`;
  }

  // ---- drawing ------------------------------------------------------------------------------------

  private redraw(): void {
    if (!this.cur) return;
    const { wave } = this.cur;
    const [a, b] = this.win;
    for (const ui of this.lanes) {
      const visible = ui.chans.filter((c) => !this.hidden.has(c.id));
      // Y range over the visible window.
      let lo = Infinity;
      let hi = -Infinity;
      for (const c of visible) {
        for (let i = Math.max(0, Math.floor(a)); i <= Math.min(c.samples.length - 1, Math.ceil(b)); i++) {
          lo = Math.min(lo, c.samples[i]);
          hi = Math.max(hi, c.samples[i]);
        }
      }
      if (!Number.isFinite(lo)) {
        lo = -1;
        hi = 1;
      }
      if (ui.lane.symmetric) {
        const m = Math.max(Math.abs(lo), Math.abs(hi), 1) * 1.08;
        lo = -m;
        hi = m;
      } else {
        const mid = (lo + hi) / 2;
        const half = Math.max((hi - lo) / 2, Math.abs(mid) * 0.004, 2) * 1.25;
        lo = mid - half;
        hi = mid + half;
      }
      ui.yr = [lo, hi];
      const y = (v: number) => ui.height - ((v - lo) / (hi - lo)) * ui.height;
      ui.chans.forEach((c, k) => {
        ui.traces[k].setAttribute("d", this.hidden.has(c.id) ? "" : tracePath(c.samples, a, b, VB_W, y));
      });
      // Graticule: ten time divisions and the value ticks.
      const scale = channelInfo(ui.chans[0].id).scale;
      const ticks = niceTicks(lo * scale, hi * scale, ui.lane.symmetric ? 8 : 3);
      ui.grid.replaceChildren();
      for (let d = 1; d < 10; d++) ui.grid.append(svg("line", { x1: d * 100, x2: d * 100, y1: 0, y2: ui.height, class: "div" }));
      ui.ylabels.replaceChildren();
      for (const tv of ticks) {
        const yy = y(tv / scale);
        if (yy < 6 || yy > ui.height - 6) continue;
        ui.grid.append(svg("line", { x1: 0, x2: VB_W, y1: yy, y2: yy, class: tv === 0 && ui.lane.symmetric ? "zero" : "tick" }));
        ui.ylabels.append(el("span", { style: `top:${((yy / ui.height) * 100).toFixed(2)}%` }, [scale === 1 ? String(Math.round(tv)) : `${+tv.toFixed(1)}`]));
      }
    }
    // Shared time axis (ms from the first sample).
    const ms = (i: number) => (i / wave.fs) * 1000;
    const t0 = ms(a);
    const t1 = ms(b);
    this.axis.replaceChildren(
      ...niceTicks(t0, t1, 8).map((tv) =>
        el("span", { style: `left:${(((tv - t0) / (t1 - t0)) * 100).toFixed(2)}%` }, [tv >= 100 ? tv.toFixed(0) : +tv.toFixed(1) + ""]),
      ),
      el("span", { class: "wv-axis-unit" }, ["ms"]),
    );
    this.resetBtn.hidden = a <= 0 && b >= wave.samples - 1;
    this.drawCursor();
  }

  private drawCursor(): void {
    if (!this.cur) return;
    const { wave } = this.cur;
    const [a, b] = this.win;
    const c = this.cursor;
    const inView = c !== null && c >= a && c <= b;
    const x = inView ? ((c! - a) / (b - a)) * 100 : 0;
    for (const ui of this.lanes) {
      const line = ui.plot.querySelector(".wv-cline") as HTMLElement;
      line.style.display = inView ? "block" : "none";
      line.style.left = `${x}%`;
      ui.chans.forEach((ch, k) => {
        const info = channelInfo(ch.id);
        const dot = ui.dots[k];
        const show = inView && !this.hidden.has(ch.id);
        dot.style.display = show ? "block" : "none";
        if (show) {
          const v = ch.samples[c!];
          const [lo, hi] = ui.yr;
          dot.style.left = `${x}%`;
          dot.style.top = `${(1 - (v - lo) / (hi - lo)) * 100}%`;
        }
        ui.values[k].textContent = c !== null ? fmt(ch.samples[c] * info.scale, info.unit) : "";
      });
    }
    this.cursorText.replaceChildren(
      c !== null
        ? el("span", {}, ["Cursor ", el("strong", {}, [fmtMs((c / wave.fs) * 1000)]), ` · sample ${c + 1} of ${wave.samples}`])
        : el("span", { class: "muted" }, ["Point at a trace to read every channel at that moment"]),
    );
  }

  // ---- interaction ----------------------------------------------------------------------------------

  private sampleAt(ui: LaneUi, clientX: number): number {
    const r = ui.svgEl.getBoundingClientRect();
    const f = Math.min(1, Math.max(0, (clientX - r.left) / r.width));
    const [a, b] = this.win;
    return Math.round(a + f * (b - a));
  }

  private bindPointer(ui: LaneUi): void {
    const p = ui.plot;
    p.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      const box = el("span", { class: "wv-brush" }, []);
      p.append(box);
      this.brush = { lane: ui, x0: e.clientX, x1: e.clientX, box };
      p.setPointerCapture(e.pointerId);
    });
    p.addEventListener("pointermove", (e) => {
      this.cursor = this.sampleAt(ui, e.clientX);
      this.drawCursor();
      if (this.brush && this.brush.lane === ui) {
        this.brush.x1 = e.clientX;
        const r = ui.svgEl.getBoundingClientRect();
        const l = Math.max(Math.min(this.brush.x0, this.brush.x1), r.left) - r.left;
        const w = Math.min(Math.abs(this.brush.x1 - this.brush.x0), r.width);
        const off = r.left - p.getBoundingClientRect().left;
        this.brush.box.style.left = `${off + l}px`;
        this.brush.box.style.width = `${w}px`;
      }
    });
    const end = (e: PointerEvent) => {
      const br = this.brush;
      if (!br || br.lane !== ui) return;
      br.box.remove();
      this.brush = null;
      if (Math.abs(br.x1 - br.x0) > 8) {
        const s0 = this.sampleAt(ui, Math.min(br.x0, br.x1));
        const s1 = this.sampleAt(ui, Math.max(br.x0, br.x1));
        this.zoom(s0, s1);
      } else {
        this.cursor = this.sampleAt(ui, e.clientX);
        this.drawCursor();
      }
    };
    p.addEventListener("pointerup", end);
    p.addEventListener("pointercancel", () => {
      this.brush?.box.remove();
      this.brush = null;
    });
    p.addEventListener("dblclick", () => this.zoom(0, (this.cur?.wave.samples ?? 320) - 1));
  }

  private zoom(a: number, b: number): void {
    if (!this.cur) return;
    const n = this.cur.wave.samples;
    if (b - a < 8) {
      const mid = (a + b) / 2;
      a = mid - 4;
      b = mid + 4;
    }
    a = Math.max(0, a);
    b = Math.min(n - 1, b);
    this.win = [a, b];
    this.redraw();
  }

  private onKey(e: KeyboardEvent): void {
    if (!this.cur || e.ctrlKey || e.metaKey || e.altKey || (e.target as HTMLElement).closest("input, select, textarea, dialog"))
      return;
    const n = this.cur.wave.samples;
    const [a, b] = this.win;
    const stepN = e.shiftKey ? 10 : 1;
    switch (e.key) {
      case "ArrowRight":
      case "ArrowLeft": {
        const base = this.cursor ?? Math.round((a + b) / 2);
        this.cursor = Math.min(n - 1, Math.max(0, base + (e.key === "ArrowRight" ? stepN : -stepN)));
        if (this.cursor < a || this.cursor > b) {
          const w = b - a;
          const na = Math.min(Math.max(0, this.cursor - w / 2), n - 1 - w);
          this.win = [na, na + w];
          this.redraw();
        } else {
          this.drawCursor();
        }
        break;
      }
      case "+":
      case "=": {
        const mid = this.cursor ?? (a + b) / 2;
        const w = (b - a) / 2;
        this.zoom(mid - w / 2, mid + w / 2);
        break;
      }
      case "-": {
        const mid = (a + b) / 2;
        const w = (b - a) * 2;
        this.zoom(mid - w / 2, mid + w / 2);
        break;
      }
      case "Escape":
        this.zoom(0, n - 1);
        break;
      case "[":
        this.step(-1);
        break;
      case "]":
        this.step(1);
        break;
      default:
        return;
    }
    e.preventDefault();
  }

  // ---- other channels and the table -------------------------------------------------------------------

  private othersSection(wave: GridWave): HTMLElement {
    const inLanes = new Set(LANES.flatMap((l) => l.ids));
    const tiles = wave.channels
      .filter((c) => !inLanes.has(c.id))
      .map((c) => {
        const st = stats(c.samples, wave.fs);
        const flat = st.max === st.min;
        const spark = svg("svg", { class: "wv-spark", viewBox: "0 0 100 24", preserveAspectRatio: "none", "aria-hidden": "true" });
        const lo = st.min - 1;
        const hi = st.max + 1;
        spark.append(svg("path", { d: tracePath(c.samples, 0, c.samples.length - 1, 100, (v) => 22 - ((v - lo) / (hi - lo)) * 20), "vector-effect": "non-scaling-stroke" }));
        const info = channelInfo(c.id);
        const named = info.label !== `Channel ${c.id}`;
        return el("div", { class: `wv-tile${flat ? " flat" : ""}` }, [
          el("span", { class: "wv-tile-id" }, [
            named ? `${info.label}${info.sure ? "" : "?"}` : `Ch ${c.id}`,
            named ? el("small", { class: "muted" }, [` ch ${c.id}`]) : "",
          ]),
          spark,
          el("span", { class: "wv-tile-val" }, [flat ? `${st.min}` : `${Math.round(st.mean)}`]),
          el("span", { class: "wv-tile-range muted" }, [flat ? "constant" : `${st.min} to ${st.max}`]),
        ]);
      });
    return el("section", { class: "wv-others" }, [
      el("div", { class: "wv-section-head" }, [
        el("h2", {}, ["Other channels"]),
        el("span", { class: "muted" }, ["Raw counts. A name with ? is read from the waveform's shape, not matched to an inverter value"]),
      ]),
      el("div", { class: "wv-tiles" }, tiles),
    ]);
  }

  private tableSection(wave: GridWave): HTMLElement {
    const rows = wave.channels.map((c) => {
      const info = channelInfo(c.id);
      const st = stats(c.samples, wave.fs, info.scale);
      const num = (v: number) => el("td", { class: "num" }, [fmt(v, info.unit)]);
      return el("tr", {}, [
        el("td", { class: "num" }, [String(c.id)]),
        el("td", {}, [info.sure ? info.label : `${info.label}${info.label.startsWith("Channel") ? "" : "?"}`]),
        num(st.min),
        num(st.max),
        num(st.mean),
        num(st.acRms),
        // A frequency only where the channel clearly oscillates (8 counts rms or more).
        el("td", { class: "num" }, [st.freq && st.acRms / info.scale >= 8 ? `${st.freq.toFixed(2)} Hz` : "–"]),
      ]);
    });
    return el("details", { class: "wv-table" }, [
      el("summary", {}, ["All channels as a table"]),
      el("table", { class: "diff-table" }, [
        el("thead", {}, [el("tr", {}, ["Ch", "Signal", "Min", "Max", "Mean", "AC rms", "Frequency"].map((h) => el("th", {}, [h])))]),
        el("tbody", {}, rows),
      ]),
    ]);
  }
}
