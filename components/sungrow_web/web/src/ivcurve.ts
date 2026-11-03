// I-V scan file (VI_ALL) decoder. The file is a run of 258-byte records: 256 data bytes (128
// big-endian u16), then 0x80, then a last-of-group flag byte. Each PV input takes 3 records:
// voltage in 1 V, current in 0.01 A, and a repeat of the current. The sweep runs from the
// operating point down to 50 V, so the curve holds neither the open-circuit nor the short-circuit
// point. The inverter pads an unfinished sweep with its last point and writes an absent input as
// all-zero voltage.

const RECORD_BYTES = 258;
const DATA_WORDS = 128;
const MARKER = 0x80;
const RECORDS_PER_INPUT = 3;

export interface IvPoint {
  /** Index of the point in the sweep. */
  k: number;
  v: number;
  i: number;
  p: number;
}

export interface IvInput {
  /** PV input number, from 1. */
  index: number;
  /** False for an input the inverter did not sweep (all-zero voltage). */
  present: boolean;
  /** Voltage (V) and current (A) of the sweep, padding removed. */
  v: Float64Array;
  i: Float64Array;
  points: number;
  /** The repeated current series matches the first one. */
  repeatOk: boolean;
  /** Maximum power point; null when the input holds no points. */
  mpp: IvPoint | null;
}

export interface IvScan {
  inputs: IvInput[];
  records: number;
}

/** Number of points before the trailing run of repeated (v, i) pairs; the run's first point counts. */
export function trimPadding(v: ArrayLike<number>, i: ArrayLike<number>): number {
  let n = Math.min(v.length, i.length);
  while (n > 1 && v[n - 1] === v[n - 2] && i[n - 1] === i[n - 2]) n--;
  return n;
}

/** The point of highest v * i over the sweep without its padding. */
export function mpp(v: ArrayLike<number>, i: ArrayLike<number>): IvPoint | null {
  const n = trimPadding(v, i);
  let best: IvPoint | null = null;
  for (let k = 0; k < n; k++) {
    const p = v[k] * i[k];
    if (best === null || p > best.p) best = { k, v: v[k], i: i[k], p };
  }
  return best;
}

export function decodeViAll(buf: ArrayBuffer): IvScan {
  const view = new DataView(buf);
  const records = Math.floor(buf.byteLength / RECORD_BYTES);
  const series: Uint16Array[] = [];
  for (let r = 0; r < records; r++) {
    const o = r * RECORD_BYTES;
    if (view.getUint8(o + 2 * DATA_WORDS) !== MARKER) throw new Error(`I-V record ${r + 1} has no end marker`);
    const s = new Uint16Array(DATA_WORDS);
    for (let k = 0; k < DATA_WORDS; k++) s[k] = view.getUint16(o + 2 * k, false);
    series.push(s);
  }
  const inputs: IvInput[] = [];
  for (let g = 0; g + 1 < series.length; g += RECORDS_PER_INPUT) {
    const rawV = series[g];
    const rawI = series[g + 1];
    const rep = series[g + 2];
    const present = rawV.some((x) => x !== 0);
    const vAll = Float64Array.from(rawV);
    const iAll = Float64Array.from(rawI, (x) => x / 100);
    const n = present ? trimPadding(vAll, iAll) : 0;
    const v = vAll.slice(0, n);
    const i = iAll.slice(0, n);
    inputs.push({
      index: inputs.length + 1,
      present,
      v,
      i,
      points: n,
      repeatOk: rep !== undefined && rep.every((x, k) => x === rawI[k]),
      mpp: n > 0 ? mpp(v, i) : null,
    });
  }
  return { inputs, records };
}
