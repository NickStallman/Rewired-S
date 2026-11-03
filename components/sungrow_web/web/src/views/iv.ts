import { api, sleep, type CaptureResult } from "../api.js";
import { el, svg } from "../dom.js";
import { niceTicks } from "../gridwave.js";
import { decodeViAll, type IvInput, type IvScan } from "../ivcurve.js";
import type { View } from "./types.js";

// I-V curve viewer (#/iv/<id>): the rail lists the I-V scans on the dongle; the body shows, per
// PV input, the current and the power against voltage with the maximum power point marked, then
// a table of every input. All inputs share one voltage axis and one scale per quantity, so the
// strings compare at a glance. Files are fetched raw from /api/capture_file and decoded here.

const PW = 440; // plot area in viewBox units
const PH = 200;
const ML = 46; // margins for the tick labels
const MB = 30;
const MT = 10;
const MR = 12;

/** SVG path of the points (xs[k], ys[k]) mapped from the ranges onto 0..w by h..0. */
function linePath(xs: ArrayLike<number>, ys: ArrayLike<number>, xRange: [number, number], yRange: [number, number], w: number, h: number): string {
  const sx = w / (xRange[1] - xRange[0] || 1);
  const sy = h / (yRange[1] - yRange[0] || 1);
  let d = "";
  for (let k = 0; k < Math.min(xs.length, ys.length); k++) {
    const x = (xs[k] - xRange[0]) * sx;
    const y = h - (ys[k] - yRange[0]) * sy;
    d += `${k === 0 ? "M" : "L"}${x.toFixed(2)},${y.toFixed(2)}`;
  }
  return d;
}

/** Upper end of an axis from 0: the first nice tick at or above `max`. */
function axisMax(max: number): number {
  if (!(max > 0)) return 1;
  const ticks = niceTicks(0, max * 1.05, 8);
  const step = ticks.length > 1 ? ticks[1] - ticks[0] : max;
  return Math.ceil((max * 1.05) / step) * step;
}

function fmtNum(v: number, digits: number): string {
  return Number.isFinite(v) ? v.toFixed(digits) : "–";
}

function when(r: CaptureResult): { clock: string; date: string } {
  if (!r.wall_start) return { clock: `#${r.id}`, date: "time unknown" };
  const d = new Date(r.wall_start * 1000);
  return {
    clock: d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false }),
    date: d.toLocaleDateString(undefined, { day: "numeric", month: "short", year: "numeric" }),
  };
}

function range(a: Float64Array): [number, number] {
  let lo = Infinity;
  let hi = -Infinity;
  for (const x of a) {
    lo = Math.min(lo, x);
    hi = Math.max(hi, x);
  }
  return [lo, hi];
}

interface Scales {
  v: [number, number];
  i: [number, number];
  p: [number, number];
}

export class IvView implements View {
  readonly id = "iv";
  readonly label = "I-V curves";
  readonly navHidden = true;
  readonly navParent = "capture";

  private abort = new AbortController();
  private cache = new Map<number, IvScan>();
  private recs: CaptureResult[] = [];
  private cur: CaptureResult | null = null;
  private root!: HTMLElement;
  private railList!: HTMLElement;
  private body!: HTMLElement;

  render(root: HTMLElement, param?: string): void {
    this.abort = new AbortController();
    this.root = root;
    root.closest("main")?.classList.add("wide");
    this.railList = el("div", { class: "wv-rail-list", role: "list" }, []);
    this.body = el("section", { class: "wv-body", "aria-live": "polite" }, [el("p", { class: "muted" }, ["Loading I-V scans…"])]);
    root.append(
      el("div", { class: "wv" }, [
        el("aside", { class: "wv-rail", "aria-label": "I-V scans" }, [
          el("div", { class: "wv-rail-head" }, [
            el("span", { class: "wv-eyebrow" }, ["I-V scans"]),
            el("a", { class: "wv-rail-new", href: "#/capture" }, ["Scan"]),
          ]),
          this.railList,
        ]),
        this.body,
      ]),
    );
    void this.load(param ? Number(param) : undefined);
  }

  destroy(): void {
    this.abort.abort();
    this.root?.closest("main")?.classList.remove("wide");
  }

  // ---- data ------------------------------------------------------------------------------------

  private async load(want?: number): Promise<void> {
    const signal = this.abort.signal;
    try {
      const s = await api.captureStatus(signal);
      this.recs = (s.results ?? []).filter((r) => r.kind === "iv" && r.ok && r.size > 0);
    } catch (err) {
      if (signal.aborted) return;
      this.body.replaceChildren(el("div", { class: "banner err" }, [`Could not list the I-V scans: ${(err as Error).message}`]));
      return;
    }
    if (this.recs.length === 0) {
      this.railList.replaceChildren();
      this.body.replaceChildren(
        el("div", { class: "wv-empty" }, [
          el("p", { class: "wv-empty-title" }, ["No I-V scans on the dongle"]),
          el("p", { class: "muted" }, ["Run an I-V scan on the Captures page. Scans stay on the dongle until it restarts."]),
          el("a", { class: "btn primary", href: "#/capture" }, ["Go to Captures"]),
        ]),
      );
      return;
    }
    this.renderRail();
    const first = this.recs.find((r) => r.id === want) ?? this.recs[0];
    await this.select(first.id);
    for (const r of this.recs) {
      if (signal.aborted) return;
      if (!this.cache.has(r.id)) {
        try {
          await this.fetchScan(r.id);
        } catch {
          /* the rail entry keeps no input count */
        }
      }
      this.renderRail();
      await sleep(0, signal).catch(() => undefined);
    }
  }

  private async fetchScan(id: number): Promise<IvScan> {
    const hit = this.cache.get(id);
    if (hit) return hit;
    const res = await fetch(api.captureFileUrl(id), { signal: this.abort.signal });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const scan = decodeViAll(await res.arrayBuffer());
    this.cache.set(id, scan);
    return scan;
  }

  private async select(id: number): Promise<void> {
    const rec = this.recs.find((r) => r.id === id);
    if (!rec) return;
    try {
      const scan = await this.fetchScan(id);
      this.cur = rec;
      history.replaceState(null, "", `#/iv/${id}`);
      this.renderRail();
      this.renderBody(rec, scan);
    } catch (err) {
      if (this.abort.signal.aborted) return;
      this.body.replaceChildren(el("div", { class: "banner err" }, [`Could not load I-V scan ${id}: ${(err as Error).message}`]));
    }
  }

  // ---- rail -------------------------------------------------------------------------------------

  private renderRail(): void {
    this.railList.replaceChildren(
      ...this.recs.map((r) => {
        const scan = this.cache.get(r.id);
        const n = scan?.inputs.filter((x) => x.present).length;
        return el(
          "button",
          {
            class: "wv-rec",
            type: "button",
            role: "listitem",
            "aria-current": this.cur?.id === r.id ? "true" : undefined,
            onClick: () => void this.select(r.id),
          },
          [
            el("span", { class: "wv-rec-time" }, [when(r).clock]),
            el("span", { class: "wv-rec-meta" }, [n === undefined ? "" : `${n} input${n === 1 ? "" : "s"}`]),
          ],
        );
      }),
    );
  }

  // ---- body -------------------------------------------------------------------------------------

  private renderBody(rec: CaptureResult, scan: IvScan): void {
    const t = when(rec);
    const present = scan.inputs.filter((x) => x.present && x.points > 0);
    let vMax = 0;
    let vMin = Infinity;
    let iMax = 0;
    let pMax = 0;
    for (const x of present) {
      const [vl, vh] = range(x.v);
      vMax = Math.max(vMax, vh);
      vMin = Math.min(vMin, vl);
      iMax = Math.max(iMax, range(x.i)[1]);
      pMax = Math.max(pMax, x.mpp?.p ?? 0);
    }
    const scales: Scales = { v: [0, axisMax(vMax)], i: [0, axisMax(iMax)], p: [0, axisMax(pMax)] };
    const points = present.map((x) => x.points);
    const pointsText = points.length === 0 ? "–" : Math.min(...points) === Math.max(...points) ? String(points[0]) : `${Math.min(...points)}-${Math.max(...points)}`;

    const head = el("header", { class: "wv-head" }, [
      el("div", { class: "wv-title" }, [
        el("span", { class: "wv-eyebrow" }, [`I-V scan · ${rec.inverter || "inverter"}`]),
        el("h1", { class: "wv-clock" }, [t.clock, el("small", {}, [t.date])]),
        el(
          "dl",
          { class: "wv-facts" },
          (
            [
              ["Inputs", `${present.length} of ${scan.inputs.length}`],
              ["Points per input", pointsText],
              ["Sweep", present.length ? `${Math.round(vMax)} V down to ${Math.round(vMin)} V` : "–"],
              ["File", rec.file_name || `${rec.size} B`],
            ] as const
          ).map(([k, v]) => el("div", {}, [el("dt", {}, [k]), el("dd", {}, [v])])),
        ),
      ]),
      el("div", { class: "wv-actions" }, [
        el("div", { class: "wv-downloads" }, [
          el("a", { class: "btn", href: api.captureFileUrl(rec.id), download: "" }, ["Download file"]),
          el("a", { class: "btn", href: api.captureMetaUrl(rec.id), download: "" }, ["Metadata"]),
        ]),
      ]),
    ]);

    const note = el("p", { class: "iv-note muted" }, [
      "The sweep runs from the operating point down to 50 V, so the curve does not reach open circuit or short circuit.",
    ]);
    const lanes = scan.inputs.map((x) => this.inputLane(x, scales));
    this.body.replaceChildren(head, note, el("div", { class: "wv-scope" }, lanes), this.tableSection(scan));
  }

  private inputLane(x: IvInput, s: Scales): HTMLElement {
    const title = el("div", { class: "wv-lane-title" }, [el("h2", {}, [`PV input ${x.index}`])]);
    if (!x.present || x.points === 0 || !x.mpp) {
      title.append(el("span", { class: "muted" }, ["Not swept: the inverter reports no voltage on this input"]));
      return el("div", { class: "wv-lane iv-lane absent" }, [el("div", { class: "wv-lane-head" }, [title])]);
    }
    const m = x.mpp;
    title.append(
      el("span", { class: "iv-mpp" }, [
        `MPP ${fmtNum(m.p, 0)} W at ${fmtNum(m.v, 0)} V / ${fmtNum(m.i, 2)} A`,
      ]),
    );
    const pw = Float64Array.from(x.v, (v, k) => v * x.i[k]);
    return el("div", { class: "wv-lane iv-lane" }, [
      el("div", { class: "wv-lane-head" }, [title]),
      el("div", { class: "iv-plots" }, [
        this.plot("Current", "A", x.v, x.i, s.v, s.i, [m.v, m.i], `${fmtNum(m.i, 2)} A`, "s1"),
        this.plot("Power", "W", x.v, pw, s.v, s.p, [m.v, m.p], `${fmtNum(m.p, 0)} W`, "s2"),
      ]),
    ]);
  }

  private plot(
    name: string,
    unit: string,
    xs: Float64Array,
    ys: Float64Array,
    xr: [number, number],
    yr: [number, number],
    mark: [number, number],
    markLabel: string,
    slot: string,
  ): HTMLElement {
    const root = svg("svg", {
      class: "iv-svg",
      viewBox: `0 0 ${ML + PW + MR} ${MT + PH + MB}`,
      role: "img",
      "aria-label": `${name} against voltage, ${unit} over V`,
    });
    const g = svg("g", { transform: `translate(${ML},${MT})` });
    root.append(g);
    const grid = svg("g", { class: "wv-grid" });
    g.append(grid);
    const sx = (v: number) => ((v - xr[0]) / (xr[1] - xr[0])) * PW;
    const sy = (v: number) => PH - ((v - yr[0]) / (yr[1] - yr[0])) * PH;
    for (const tv of niceTicks(xr[0], xr[1], 6)) {
      grid.append(svg("line", { x1: sx(tv), x2: sx(tv), y1: 0, y2: PH, class: tv === 0 ? "zero" : "div" }));
      const lab = svg("text", { x: sx(tv), y: PH + 16, class: "iv-tick", "text-anchor": "middle" });
      lab.textContent = String(+tv.toFixed(1));
      g.append(lab);
    }
    for (const tv of niceTicks(yr[0], yr[1], 4)) {
      grid.append(svg("line", { x1: 0, x2: PW, y1: sy(tv), y2: sy(tv), class: tv === 0 ? "zero" : "tick" }));
      const lab = svg("text", { x: -6, y: sy(tv) + 4, class: "iv-tick", "text-anchor": "end" });
      lab.textContent = String(+tv.toFixed(1));
      g.append(lab);
    }
    g.append(svg("path", { class: `wv-trace ${slot}`, d: linePath(xs, ys, xr, yr, PW, PH) }));
    const mx = sx(mark[0]);
    const my = sy(mark[1]);
    const label = svg("text", {
      x: mx > PW * 0.7 ? mx - 9 : mx + 9,
      y: my < 18 ? my + 16 : my - 8,
      class: "iv-mark-label",
      "text-anchor": mx > PW * 0.7 ? "end" : "start",
    });
    label.textContent = `MPP ${markLabel}`;
    g.append(svg("circle", { class: "iv-mark", cx: mx, cy: my, r: 5 }), label);
    return el("figure", { class: "iv-plot" }, [el("figcaption", { class: "muted" }, [`${name} (${unit}) against voltage (V)`]), root]);
  }

  private tableSection(scan: IvScan): HTMLElement {
    const rows = scan.inputs.map((x) => {
      const m = x.mpp;
      const ok = x.present && x.points > 0;
      const vr = ok ? range(x.v) : null;
      const ir = ok ? range(x.i) : null;
      const num = (s: string) => el("td", { class: "num" }, [s]);
      return el("tr", {}, [
        el("td", {}, [`PV input ${x.index}`]),
        num(ok ? String(x.points) : "–"),
        num(vr ? `${fmtNum(vr[0], 0)} to ${fmtNum(vr[1], 0)} V` : "–"),
        num(ir ? `${fmtNum(ir[0], 2)} to ${fmtNum(ir[1], 2)} A` : "–"),
        num(m ? `${fmtNum(m.v, 0)} V` : "–"),
        num(m ? `${fmtNum(m.i, 2)} A` : "–"),
        num(m ? `${fmtNum(m.p, 0)} W` : "–"),
        el("td", { class: ok ? (x.repeatOk ? "ok" : "err") : "muted" }, [ok ? (x.repeatOk ? "ok" : "differs") : "–"]),
      ]);
    });
    return el("section", { class: "wv-others" }, [
      el("div", { class: "wv-section-head" }, [el("h2", {}, ["All inputs"])]),
      el("div", { class: "iv-table" }, [
        el("table", { class: "diff-table" }, [
          el("thead", {}, [
            el("tr", {}, ["Input", "Points", "V range", "I range", "MPP V", "MPP I", "MPP P", "I repeat"].map((h) => el("th", {}, [h]))),
          ]),
          el("tbody", {}, rows),
        ]),
      ]),
    ]);
  }
}
