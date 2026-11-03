// Unit tests for gridwave.ts (decoder, statistics, axis ticks). Run with `npm run test`.
import { FAULT_CODES } from "./meta.gen.js";
import { channelInfo, decodeGridWave, decodeRecordFile, niceTicks, stats, statusFaultCodes } from "./gridwave.js";

interface Assert {
  deepStrictEqual(a: unknown, b: unknown, msg?: string): void;
  strictEqual(a: unknown, b: unknown, msg?: string): void;
  ok(v: unknown, msg?: string): void;
}
declare const process: { getBuiltinModule(id: string): unknown };
const { test } = process.getBuiltinModule("node:test") as { test(name: string, fn: () => void): void };
const assert = process.getBuiltinModule("node:assert/strict") as Assert;

type Chans = [number, (i: number) => number][];

interface RecordOpts {
  slot?: number;
  /** [hour, minute, second] */
  clock?: [number, number, number];
  /** File words 11-16: status words 0-3 and transient words 26-27. */
  status?: number[];
}

/** Record words from word 3 on (the part behind the file prefix); `chans` = [id, sample(i)] with ring position `ring`. */
function recordBody(chans: Chans, decimation: number, ring: number, o: RecordOpts = {}): number[] {
  const n = 320;
  const [hh, mm, ss] = o.clock ?? [3, 11, 20];
  const st = o.status ?? [0, 0, 0, 0, 0, 0];
  const words: number[] = [o.slot ?? 7, 2026, 10, 30, hh, mm, ss, 0, ...st, 32, chans.length, 0, 0];
  for (const [id, f] of chans) {
    words.push(id, n, ring, 10 * decimation, decimation, 160, 10 * decimation, decimation, 160, 0, 0, 0);
    // Stored as a ring: logical sample i sits at (start + i) mod n, start = (ring + 160) mod n.
    const start = (ring + 160) % n;
    const slot = new Array<number>(n);
    for (let i = 0; i < n; i++) slot[(start + i) % n] = f(i);
    words.push(...slot);
  }
  return words;
}

/** A file of `bodies` behind the 3-word prefix (count low word first, record length in bytes). */
function makeFile(bodies: number[][]): ArrayBuffer {
  const words = [bodies.length & 0xffff, bodies.length >>> 16, bodies[0].length * 2, ...bodies.flat()];
  const buf = new ArrayBuffer(words.length * 2);
  const v = new DataView(buf);
  words.forEach((x, i) => v.setUint16(2 * i, x & 0xffff, false));
  return buf;
}

/** A one-record file shaped like the SH10RS recording. */
function makeRecord(chans: Chans, decimation: number, ring: number, o: RecordOpts = {}): ArrayBuffer {
  return makeFile([recordBody(chans, decimation, ring, o)]);
}

test("decodes header, sample rate and ring order", () => {
  const rec = makeRecord(
    [
      [100, (i) => Math.round(3400 * Math.sin((2 * Math.PI * 50 * i) / 4500))],
      [160, () => 3910],
    ],
    4,
    207,
  );
  const g = decodeGridWave(rec);
  assert.strictEqual(g.time, "2026-10-30 03:11:20");
  assert.strictEqual(g.slot, 7);
  assert.strictEqual(g.fs, 4500);
  assert.strictEqual(g.cycles, 1);
  assert.strictEqual(g.samples, 320);
  assert.deepStrictEqual(
    g.channels.map((c) => c.id),
    [100, 160],
  );
  // Ring undone: logical order starts at the sine's zero.
  assert.strictEqual(g.channels[0].samples[0], 0);
  assert.ok(g.channels[0].samples[22] > 3300);
  assert.strictEqual(g.channels[1].samples[5], 3910);
});

test("negative samples are signed", () => {
  const g = decodeGridWave(makeRecord([[107, () => -129]], 32, 54));
  assert.strictEqual(g.channels[0].samples[0], -129);
  assert.strictEqual(g.fs, 562.5);
  assert.strictEqual(g.cycles, 8);
});

test("status words at the trigger: slot, fault bits and transient words", () => {
  const g = decodeGridWave(makeRecord([[100, () => 0]], 4, 0, { slot: 3, status: [0x0400, 0x0001, 0x0002, 0x0010, 0, 0x20] }));
  assert.strictEqual(g.slot, 3);
  assert.deepStrictEqual(g.statusWords, [0x0400, 0x0001, 0x0002, 0x0010, 0, 0x20]);
  assert.deepStrictEqual(
    g.faults.map((f) => f.code),
    [12, 20, 100, 204],
  );
  assert.deepStrictEqual(
    g.faults.map((f) => f.name),
    [12, 20, 100, 204].map((c) => FAULT_CODES.inverter[c]),
  );
  assert.ok(g.faults.every((f) => typeof f.name === "string"));
  assert.deepStrictEqual(g.faults[0], { word: 0, bit: 10, code: 12, name: FAULT_CODES.inverter[12] });
  assert.deepStrictEqual(g.transient, [0, 0x20]);
});

test("an unnamed fault code keeps its number", () => {
  // Status word 3 bit 15 is code 215, which the fault list does not name.
  assert.strictEqual(FAULT_CODES.inverter[215], undefined);
  assert.deepStrictEqual(statusFaultCodes([0, 0, 0, 0x8000]), [{ word: 3, bit: 15, code: 215, name: null }]);
  assert.deepStrictEqual(statusFaultCodes([0, 0, 0, 0]), []);
});

test("a fault-recording file splits into its records", () => {
  const ch: Chans = [
    [100, (i) => i],
    [143, () => 3900],
  ];
  const file = makeFile([
    recordBody(ch, 4, 10, { slot: 9, clock: [1, 2, 3], status: [0x0400, 0, 0, 0, 0, 0] }),
    recordBody(ch, 4, 20, { slot: 8, clock: [4, 5, 6] }),
    recordBody(ch, 4, 30, { slot: 7, clock: [7, 8, 9] }),
  ]);
  const recs = decodeRecordFile(file);
  assert.deepStrictEqual(
    recs.map((r) => r.slot),
    [9, 8, 7],
  );
  assert.deepStrictEqual(
    recs.map((r) => r.time),
    ["2026-10-30 01:02:03", "2026-10-30 04:05:06", "2026-10-30 07:08:09"],
  );
  assert.ok(recs.every((r) => r.cycles === 1 && r.channels.length === 2 && r.channels[0].samples[5] === 5));
  assert.deepStrictEqual(
    recs.map((r) => r.faults.map((f) => f.code)),
    [[12], [], []],
  );
  assert.strictEqual(decodeGridWave(file).slot, 9);
  // A truncated last record is left out.
  assert.strictEqual(decodeRecordFile(file.slice(0, file.byteLength - 100)).length, 2);
});

test("stats: rms, mean and frequency of a sine", () => {
  const x = Array.from({ length: 320 }, (_, i) => 200 + 3400 * Math.sin((2 * Math.PI * 50 * i) / 4500));
  const s = stats(x, 4500, 0.1);
  assert.ok(Math.abs(s.acRms - 340 / Math.SQRT2) < 3, `rms ${s.acRms}`);
  assert.ok(Math.abs(s.mean - 20) < 2, `mean ${s.mean}`);
  assert.ok(s.freq !== null && Math.abs(s.freq - 50) < 0.1, `freq ${s.freq}`);
  // A flat channel has no frequency.
  assert.strictEqual(stats(new Array(320).fill(627), 4500).freq, null);
});

test("channel info and ticks", () => {
  assert.strictEqual(channelInfo(143).unit, "V");
  assert.strictEqual(channelInfo(109).unit, "counts");
  assert.strictEqual(channelInfo(109).scale, 0.1);
  assert.strictEqual(channelInfo(101).unit, "%");
  assert.strictEqual(channelInfo(173).sure, false);
  assert.strictEqual(channelInfo(147).unit, "mA");
  assert.deepStrictEqual(niceTicks(-340, 340, 4), [-200, 0, 200]);
  assert.deepStrictEqual(niceTicks(0, 71, 4), [0, 20, 40, 60]);
});
