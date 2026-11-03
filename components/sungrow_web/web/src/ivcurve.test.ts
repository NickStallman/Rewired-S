// Unit tests for ivcurve.ts (VI_ALL decoder, MPP, padding). Run with `npm run test`.
import { decodeViAll, mpp, trimPadding } from "./ivcurve.js";

interface Assert {
  deepStrictEqual(a: unknown, b: unknown, msg?: string): void;
  strictEqual(a: unknown, b: unknown, msg?: string): void;
  ok(v: unknown, msg?: string): void;
  throws(fn: () => unknown, re?: RegExp): void;
}
declare const process: { getBuiltinModule(id: string): unknown };
const { test } = process.getBuiltinModule("node:test") as { test(name: string, fn: () => void): void };
const assert = process.getBuiltinModule("node:assert/strict") as Assert;

/** Raw voltage (1 V) and current (0.01 A) series: `n` points from 400 V down to 50 V, padded to 128. */
function sweep(voc: number, isc: number, n: number): [number[], number[]] {
  const v: number[] = [];
  const i: number[] = [];
  for (let k = 0; k < 128; k++) {
    const kk = Math.min(k, n - 1);
    const volts = Math.round(400 - (350 * kk) / (n - 1));
    v.push(volts);
    i.push(Math.round(100 * isc * (1 - (volts / voc) ** 8)));
  }
  return [v, i];
}

/** VI_ALL bytes: per input the voltage, current and repeated current records. */
function file(inputs: [number[], number[], number[]?][], marker = 0x80): ArrayBuffer {
  const recs = inputs.flatMap(([v, i, rep]) => [v, i, rep ?? i]);
  const buf = new ArrayBuffer(recs.length * 258);
  const view = new DataView(buf);
  recs.forEach((s, r) => {
    s.forEach((x, k) => view.setUint16(r * 258 + 2 * k, x, false));
    view.setUint8(r * 258 + 256, marker);
    view.setUint8(r * 258 + 257, r % 3 === 2 ? 1 : 0);
  });
  return buf;
}

test("decodes two inputs with their MPP", () => {
  const a = sweep(450, 10.5, 128);
  const b = sweep(430, 8.2, 128);
  const scan = decodeViAll(file([a, b]));
  assert.strictEqual(scan.records, 6);
  assert.strictEqual(scan.inputs.length, 2);
  const [p1, p2] = scan.inputs;
  assert.strictEqual(p1.index, 1);
  assert.strictEqual(p2.index, 2);
  assert.ok(p1.present && p2.present);
  assert.strictEqual(p1.points, 128);
  assert.strictEqual(p1.v[0], 400);
  assert.strictEqual(p1.v[127], 50);
  assert.strictEqual(p1.i[127], a[1][127] / 100);
  assert.ok(p1.repeatOk);
  // The MPP is the largest product of the decoded points.
  let best = 0;
  for (let k = 0; k < 128; k++) best = Math.max(best, p2.v[k] * p2.i[k]);
  assert.ok(p2.mpp !== null && Math.abs(p2.mpp.p - best) < 1e-9);
  assert.ok(p2.mpp!.v > 250 && p2.mpp!.v < 400, `mpp at ${p2.mpp!.v} V`);
  assert.strictEqual(p2.mpp!.p, p2.mpp!.v * p2.mpp!.i);
});

test("mpp and trimPadding on plain arrays", () => {
  assert.deepStrictEqual(mpp([300, 200, 100], [1, 2, 2.5]), { k: 1, v: 200, i: 2, p: 400 });
  assert.strictEqual(mpp([], []), null);
  assert.strictEqual(trimPadding([400, 300, 200, 200, 200], [1, 2, 3, 3, 3]), 3);
  // Equal voltage with a different current is not padding.
  assert.strictEqual(trimPadding([400, 300, 300], [1, 2, 3]), 3);
  assert.strictEqual(trimPadding([5, 5, 5], [1, 1, 1]), 1);
});

test("padding is removed, an absent input is flagged, a differing repeat is reported", () => {
  const a = sweep(450, 10, 90);
  const zero = new Array<number>(128).fill(0);
  const c = sweep(440, 9, 128);
  const rep = [...c[1]];
  rep[3] += 1;
  const scan = decodeViAll(file([a, [zero, zero], [c[0], c[1], rep]]));
  const [p1, p2, p3] = scan.inputs;
  assert.strictEqual(p1.points, 90);
  assert.strictEqual(p1.v.length, 90);
  assert.strictEqual(p1.v[89], 50);
  assert.strictEqual(p2.present, false);
  assert.strictEqual(p2.points, 0);
  assert.strictEqual(p2.mpp, null);
  assert.strictEqual(p3.index, 3);
  assert.strictEqual(p3.repeatOk, false);
});

test("a record without the end marker is refused", () => {
  assert.throws(() => decodeViAll(file([sweep(450, 10, 128)], 0x00)), /end marker/);
});
