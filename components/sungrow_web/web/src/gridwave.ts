// Grid-waveform record decoder. All words are big-endian u16. A file starts with a 3-word prefix:
//   0-1 record count (low word first), 2 record length in bytes (one record, prefix excluded).
// Each record then holds, numbered as if it followed the prefix directly:
//   3 recorder slot (1..10), 4-9 year month day hour minute second (inverter local time),
//   11-14 the inverter's status words 0-3 at the trigger, 15-16 the transient status words 26-27,
//   18 channel count, then from word 21 per channel 332 words: 12-word sub-header + 320 signed
//   samples. sub[0] id, sub[1] sample count, sub[2] ring position (oldest = (sub[2] + 160) mod n),
//   sub[4] decimation: sample rate = 18 kHz / sub[4].
// A requested recording holds one record; the fault-recording file holds up to 10 back to back,
// each one cycle long.

import { FAULT_CODES } from "./meta.gen.js";

export const BASE_HZ = 18000;
const PREFIX_WORDS = 3;
const HEADER_WORDS = 21;
const SUB_WORDS = 12;
const MAX_RECORDS = 10;
/** Fault code of bit 0 in status words 0-3. */
const STATUS_CODE_BASE = [2, 100, 19, 200] as const;

export interface WaveChannel {
  id: number;
  samples: Int16Array; // oldest first
}

/** One fault bit set in the status words at the trigger. */
export interface FaultHit {
  word: 0 | 1 | 2 | 3;
  bit: number;
  code: number;
  /** Name from the inverter fault list; null for a code the list does not name. */
  name: string | null;
}

export interface GridWave {
  /** Recorder slot the inverter wrote the record to (1..10). */
  slot: number;
  /** Inverter local time as written in the record ("YYYY-MM-DD hh:mm:ss"). */
  time: string;
  /** Sample rate in Hz. */
  fs: number;
  /** Decimation word (4 x cycle setting). */
  decimation: number;
  cycles: number;
  samples: number;
  /** Status words 0-3 and transient status words 26-27 at the trigger, raw (file words 11-16). */
  statusWords: number[];
  /** Fault bits of status words 0-3, sorted by code. */
  faults: FaultHit[];
  /** Transient status words 26-27, raw. */
  transient: [number, number];
  channels: WaveChannel[];
}

/** The fault codes whose bits are set in status words 0-3, sorted by code. */
export function statusFaultCodes(words: ArrayLike<number>): FaultHit[] {
  const names = FAULT_CODES.inverter ?? {};
  const out: FaultHit[] = [];
  for (let w = 0; w < 4; w++) {
    const v = words[w] ?? 0;
    for (let bit = 0; bit < 16; bit++) {
      if (!(v & (1 << bit))) continue;
      const code = STATUS_CODE_BASE[w] + bit;
      out.push({ word: w as FaultHit["word"], bit, code, name: names[code] ?? null });
    }
  }
  return out.sort((a, b) => a.code - b.code);
}

/**
 * Decode the record whose word 0 sits at file word `baseWord`; reads stop at file word `nWords`
 * (the end of the record or of the file).
 */
export function decodeRecordAt(view: DataView, baseWord: number, nWords: number): GridWave {
  const len = Math.min(nWords, Math.floor(view.byteLength / 2)) - baseWord;
  const w = (i: number) => (i < len ? view.getUint16(2 * (baseWord + i), false) : 0);
  if (len < HEADER_WORDS + SUB_WORDS) throw new Error("file too short for a grid-waveform record");
  const nch = w(18);
  const pad = (n: number) => String(n).padStart(2, "0");
  const time = `${w(4)}-${pad(w(5))}-${pad(w(6))} ${pad(w(7))}:${pad(w(8))}:${pad(w(9))}`;
  const channels: WaveChannel[] = [];
  let decimation = 0;
  let samples = 0;
  for (let k = 0; k < nch; k++) {
    const o = HEADER_WORDS + k * (SUB_WORDS + w(HEADER_WORDS + 1));
    const n = w(o + 1);
    if (n === 0 || o + SUB_WORDS + n > len) break;
    if (k === 0) {
      decimation = w(o + 4);
      samples = n;
    }
    const start = (w(o + 2) + n / 2) % n;
    const out = new Int16Array(n);
    for (let i = 0; i < n; i++) out[i] = view.getInt16(2 * (baseWord + o + SUB_WORDS + ((start + i) % n)), false);
    channels.push({ id: w(o), samples: out });
  }
  if (channels.length === 0) throw new Error("no channels in the record");
  const fs = decimation > 0 ? BASE_HZ / decimation : 0;
  const statusWords = [11, 12, 13, 14, 15, 16].map(w);
  return {
    slot: w(3),
    time,
    fs,
    decimation,
    cycles: decimation / 4,
    samples,
    statusWords,
    faults: statusFaultCodes(statusWords),
    transient: [statusWords[4], statusWords[5]],
    channels,
  };
}

/** Every record of a file: count from the prefix (1..10), stopping before a record that overruns. */
export function decodeRecordFile(buf: ArrayBuffer): GridWave[] {
  const view = new DataView(buf);
  const nWords = Math.floor(buf.byteLength / 2);
  if (nWords < PREFIX_WORDS) throw new Error("file too short for a grid-waveform record");
  const word = (i: number) => view.getUint16(2 * i, false);
  const count = Math.min(MAX_RECORDS, Math.max(1, word(0) + word(1) * 65536));
  const recWords = Math.floor(word(2) / 2);
  // A record length too short to hold one record: read the file as a single record.
  if (recWords < HEADER_WORDS - PREFIX_WORDS + SUB_WORDS) return [decodeRecordAt(view, 0, nWords)];
  const out: GridWave[] = [];
  for (let k = 0; k < count; k++) {
    const base = k * recWords;
    const recEnd = base + PREFIX_WORDS + recWords;
    if (k > 0 && recEnd > nWords) break;
    out.push(decodeRecordAt(view, base, Math.min(recEnd, nWords)));
  }
  return out;
}

/** The first record of a file. */
export function decodeGridWave(buf: ArrayBuffer): GridWave {
  return decodeRecordFile(buf)[0];
}

// ---- what the channels are ------------------------------------------------------------------

export interface ChannelInfo {
  label: string;
  /** Physical unit, or "" for raw counts. */
  unit: string;
  /** Value per count (1 for raw counts). */
  scale: number;
  /** False when the name is a reading of the waveform's shape, not matched to an inverter value. */
  sure: boolean;
}

// Channel identities of the inverter's recording table, matched against its own readings at about
// 1 kW and 6.5 kW output. The inverter records each variable x 10, hence 0.1 per count throughout;
// 101 is the limit as a fraction x 10, so one count is one percent.
const KNOWN: Record<number, ChannelInfo> = {
  100: { label: "Grid voltage", unit: "V", scale: 0.1, sure: true },
  102: { label: "cos(output angle)", unit: "", scale: 0.1, sure: true },
  101: { label: "Active-power limit", unit: "%", scale: 1, sure: true },
  103: { label: "Inverter output voltage", unit: "V", scale: 0.1, sure: true },
  107: { label: "Inverter current", unit: "A", scale: 0.1, sure: true },
  108: { label: "Current reference template", unit: "", scale: 0.1, sure: true },
  109: { label: "PWM compare", unit: "counts", scale: 0.1, sure: true },
  110: { label: "PV1 voltage", unit: "V", scale: 0.1, sure: true },
  111: { label: "PV2 voltage", unit: "V", scale: 0.1, sure: true },
  112: { label: "PV3 voltage", unit: "V", scale: 0.1, sure: true },
  113: { label: "PV4 voltage", unit: "V", scale: 0.1, sure: true },
  114: { label: "Control mode", unit: "", scale: 0.1, sure: true },
  115: { label: "Control state", unit: "", scale: 0.1, sure: true },
  127: { label: "PV1 current", unit: "A", scale: 0.1, sure: true },
  128: { label: "PV2 current", unit: "A", scale: 0.1, sure: true },
  129: { label: "PV3 current", unit: "A", scale: 0.1, sure: true },
  130: { label: "PV4 current", unit: "A", scale: 0.1, sure: true },
  131: { label: "Battery converter enable", unit: "", scale: 0.1, sure: true },
  132: { label: "Battery converter mode", unit: "", scale: 0.1, sure: true },
  143: { label: "DC bus", unit: "V", scale: 0.1, sure: true },
  144: { label: "Bus upper half", unit: "V", scale: 0.1, sure: true },
  145: { label: "Bus lower half", unit: "V", scale: 0.1, sure: true },
  146: { label: "Backup port current", unit: "A", scale: 0.1, sure: true },
  147: { label: "Leakage current", unit: "mA", scale: 0.1, sure: true },
  160: { label: "Battery voltage", unit: "V", scale: 0.1, sure: true },
  161: { label: "Battery current", unit: "A", scale: 0.1, sure: true },
  170: { label: "Output phase angle", unit: "rad", scale: 0.1, sure: true },
  171: { label: "cos(output angle)", unit: "", scale: 0.1, sure: true },
  172: { label: "Grid phase angle", unit: "rad", scale: 0.1, sure: true },
  173: { label: "Channel 173", unit: "", scale: 1, sure: false },
};

export function channelInfo(id: number): ChannelInfo {
  return KNOWN[id] ?? { label: `Channel ${id}`, unit: "", scale: 1, sure: false };
}

/** The lanes the viewer draws, in order; one unit per lane. Every other channel goes to the grid. */
export interface Lane {
  key: string;
  title: string;
  note: string;
  ids: number[];
  /** Centre the scale on zero (AC signals). */
  symmetric: boolean;
}

export const LANES: Lane[] = [
  { key: "acv", title: "AC voltage", note: "The grid, and the inverter's own output on its side of the grid relay", ids: [100, 103], symmetric: true },
  { key: "aci", title: "AC current", note: "Inverter output and the backup port", ids: [107, 146], symmetric: true },
  { key: "bus", title: "DC bus", note: "Bus voltage; its ripple is at twice mains frequency", ids: [143], symmetric: false },
  { key: "half", title: "Bus halves", note: "The two capacitor halves; together they make the bus", ids: [144, 145], symmetric: false },
  { key: "batv", title: "Battery voltage", note: "At the inverter's battery input", ids: [160], symmetric: false },
  { key: "bati", title: "Battery current", note: "Discharge is positive", ids: [161], symmetric: false },
  { key: "pv12", title: "PV inputs 1 and 2", note: "Input voltage", ids: [110, 111], symmetric: false },
  { key: "pv34", title: "PV inputs 3 and 4", note: "Input voltage", ids: [112, 113], symmetric: false },
  { key: "pvi", title: "PV currents", note: "Inputs 1 to 3; input 4 is in the table", ids: [127, 128, 129], symmetric: false },
  { key: "phase", title: "Phase angles", note: "The grid angle the inverter tracks, and its own output angle (radians)", ids: [172, 170], symmetric: false },
  {
    key: "x147",
    title: "Leakage current",
    note: "The residual (earth leakage) current; the DSP records it in 0.1 mA steps and splits it against the grid angle as a leakage monitor does",
    ids: [147],
    symmetric: true,
  },
  {
    key: "x109",
    title: "PWM compare",
    note: "The inverter bridge's duty command in carrier counts (0 to 2778 at 18 kHz); it repeats at twice mains frequency because the modulator switches legs every half cycle",
    ids: [109],
    symmetric: false,
  },
];

// ---- statistics -----------------------------------------------------------------------------

export interface ChannelStats {
  min: number;
  max: number;
  mean: number;
  /** RMS of the AC part (mean removed). */
  acRms: number;
  /** Frequency of the dominant mean crossing, Hz; null when the channel does not oscillate. */
  freq: number | null;
}

export function stats(x: ArrayLike<number>, fs: number, scale = 1): ChannelStats {
  const n = x.length;
  let min = Infinity;
  let max = -Infinity;
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const v = x[i];
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
  }
  const mean = sum / n;
  let sq = 0;
  for (let i = 0; i < n; i++) sq += (x[i] - mean) ** 2;
  const acRms = Math.sqrt(sq / n);
  // Rising mean crossings with hysteresis (half the AC rms, so harmonics do not count), interpolated.
  let freq: number | null = null;
  const hyst = acRms * 0.5;
  if (acRms > 0 && max - min > 8 && fs > 0) {
    const cross: number[] = [];
    let armed = false;
    for (let i = 1; i < n; i++) {
      if (x[i] < mean - hyst) armed = true;
      if (armed && x[i - 1] < mean && x[i] >= mean) {
        cross.push(i - 1 + (mean - x[i - 1]) / (x[i] - x[i - 1]));
        armed = false;
      }
    }
    if (cross.length >= 2) {
      freq = ((cross.length - 1) * fs) / (cross[cross.length - 1] - cross[0]);
      // Mean and RMS over whole periods only (a record holds a fractional number of them).
      const a = Math.round(cross[0]);
      const b = Math.round(cross[cross.length - 1]);
      let s2 = 0;
      for (let i = a; i < b; i++) s2 += x[i];
      const m2 = s2 / (b - a);
      let q2 = 0;
      for (let i = a; i < b; i++) q2 += (x[i] - m2) ** 2;
      return { min: min * scale, max: max * scale, mean: m2 * scale, acRms: Math.sqrt(q2 / (b - a)) * scale, freq };
    }
  }
  return { min: min * scale, max: max * scale, mean: mean * scale, acRms: acRms * scale, freq };
}

/** Nice axis ticks over [lo, hi]: about `count` steps of 1, 2 or 5 x 10^k. */
export function niceTicks(lo: number, hi: number, count = 4): number[] {
  if (!(hi > lo)) return [lo];
  const raw = (hi - lo) / count;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? raw;
  const out: number[] = [];
  for (let t = Math.ceil(lo / step) * step; t <= hi + step * 1e-9; t += step) out.push(Math.abs(t) < step * 1e-9 ? 0 : t);
  return out;
}
