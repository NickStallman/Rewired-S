// Unit tests for setup_logic.ts. Run with `npm run test` (esbuild bundles this file for node, then
// `node --test` runs it). The node built-ins are fetched through process.getBuiltinModule so the
// SPA's tsconfig needs no @types/node.
import {
  diffItems,
  evalRegulation,
  frameGroups,
  MAX_FRAME_REGS,
  orderCommitFirst,
  pairsForPending,
  u32Join,
  u32Split,
  validateRange,
  type Pair,
  type SettingSpec,
} from "./setup_logic.js";

interface Assert {
  deepStrictEqual(a: unknown, b: unknown, msg?: string): void;
  strictEqual(a: unknown, b: unknown, msg?: string): void;
  ok(v: unknown, msg?: string): void;
}
declare const process: { getBuiltinModule(id: string): unknown };
const { test } = process.getBuiltinModule("node:test") as { test(name: string, fn: () => void): void };
const assert = process.getBuiltinModule("node:assert/strict") as Assert;

const DELAY: SettingSpec = { address: 32362, dtype: "U32", scale: 0.01, accuracy: 2, unit: "s", min: 0, max: 600 };
const UV1: SettingSpec = { address: 32322, dtype: "U16", scale: 0.1, accuracy: 1, unit: "V", min: 46, max: 299 };
const COUNTRY: SettingSpec = { address: 32310, dtype: "U16", scale: 1 };
const Q1: SettingSpec = { address: 31869, dtype: "S16", scale: 0.1, accuracy: 1, unit: "%", min: -100, max: 100 };

test("u32 split/join is low word first", () => {
  assert.deepStrictEqual(u32Split(150), [150, 0]); // 1.5 s at 0.01 s
  assert.deepStrictEqual(u32Split(0x12345678), [0x5678, 0x1234]);
  assert.strictEqual(u32Join(150, 0), 150);
  assert.strictEqual(u32Join(0x5678, 0x1234), 0x12345678);
  assert.strictEqual(u32Join(0xffff, 0xffff), 0xffffffff);
});

test("pairsForPending writes U32 lo at address, hi at address+1", () => {
  const pairs = pairsForPending([DELAY], new Map([[32362, 150]]), new Map());
  assert.deepStrictEqual(pairs, [
    [32362, 150],
    [32363, 0],
  ]);
});

test("pairsForPending drops entries equal to the current value", () => {
  const current = new Map([
    [32362, 150],
    [32363, 0],
    [32322, 2000],
  ]);
  const pending = new Map([
    [32362, 150], // unchanged U32
    [32322, 1800], // changed
  ]);
  assert.deepStrictEqual(pairsForPending([DELAY, UV1], pending, current), [[32322, 1800]]);
  // Unknown current value: kept.
  assert.deepStrictEqual(pairsForPending([UV1], new Map([[32322, 2000]]), new Map()), [[32322, 2000]]);
  // S16 negative travels as its 16-bit pattern.
  assert.deepStrictEqual(pairsForPending([Q1], new Map([[31869, 0xfc18]]), new Map()), [[31869, 0xfc18]]);
});

test("orderCommitFirst is a stable partition", () => {
  const pairs: Pair[] = [
    [32322, 1],
    [32362, 2],
    [32310, 6],
    [32363, 0],
    [30036, 13],
  ];
  assert.deepStrictEqual(orderCommitFirst(pairs, [32310, 32311, 30036, 30037]), [
    [32310, 6],
    [30036, 13],
    [32322, 1],
    [32362, 2],
    [32363, 0],
  ]);
  assert.deepStrictEqual(orderCommitFirst(pairs, []), pairs);
});

test("diffItems uses the item current value, else the map; unknown counts as changed", () => {
  const rows = diffItems(
    [
      { a: 32310, new: 6, cur: 6, valid: true, kind: "country" },
      { a: 30036, new: 13, cur: 12, valid: true },
      { a: 32322, new: 2000, cur: 0, valid: false },
      { a: 32323, new: 2650 },
    ],
    new Map([[32323, 2650]]),
  );
  assert.deepStrictEqual(
    rows.map((r) => [r.a, r.cur, r.changed]),
    [
      [32310, 6, false],
      [30036, 12, true],
      [32322, null, true],
      [32323, 2650, false],
    ],
  );
  assert.strictEqual(rows[0].kind, "country");
});

test("validateRange checks display units", () => {
  assert.strictEqual(validateRange(UV1, 460), null); // 46.0 V
  assert.strictEqual(validateRange(UV1, 2990), null);
  assert.ok(validateRange(UV1, 459)?.includes("between 46.0 and 299.0 V"));
  assert.ok(validateRange(UV1, 2991) !== null);
  assert.strictEqual(validateRange(DELAY, 60000), null); // 600 s
  assert.ok(validateRange(DELAY, 60001) !== null);
  assert.strictEqual(validateRange(Q1, 0xfc18), null); // -100.0 %
  assert.ok(validateRange(Q1, 0xfc17) !== null); // -100.1 %
  assert.strictEqual(validateRange(COUNTRY, 6), null); // no range
});

test("evalRegulation: stock ';'-separated clauses", () => {
  const vals = new Map([
    [32318, 2650],
    [32322, 1800],
    [32326, 1700],
  ]);
  const look = (a: number) => vals.get(a);
  const r = evalRegulation("@32326<=@32322;@32326>=@32318", look);
  assert.strictEqual(r.length, 2);
  assert.deepStrictEqual(r[0].addrs, [32326, 32322]);
  assert.strictEqual(r[0].op, "<=");
  assert.strictEqual(r[0].ok, true);
  assert.strictEqual(r[1].op, ">=");
  assert.strictEqual(r[1].ok, false);
  assert.strictEqual(r[1].lhs, 1700);
  assert.strictEqual(r[1].rhs, 2650);
});

test("evalRegulation: every operator", () => {
  const look = (a: number) => ({ 1: 5, 2: 5, 3: 7 })[a as 1 | 2 | 3];
  const ok = (rule: string) => evalRegulation(rule, look)[0].ok;
  assert.strictEqual(ok("@1<@3"), true);
  assert.strictEqual(ok("@1<@2"), false);
  assert.strictEqual(ok("@1<=@2"), true);
  assert.strictEqual(ok("@3>@1"), true);
  assert.strictEqual(ok("@1>@2"), false);
  assert.strictEqual(ok("@1>=@2"), true);
  assert.strictEqual(ok("@1=@2"), true);
  assert.strictEqual(ok("@1=@3"), false);
});

test("evalRegulation: '+' / '-' expressions and constants", () => {
  const vals: Record<number, number> = { 10: 3, 11: 4, 12: 10, 13: 2 };
  const look = (a: number) => vals[a];
  const [c] = evalRegulation("@10+@11<=@12-@13", look);
  assert.strictEqual(c.lhs, 7);
  assert.strictEqual(c.rhs, 8);
  assert.strictEqual(c.ok, true);
  assert.deepStrictEqual(c.addrs, [10, 11, 12, 13]);
  assert.strictEqual(evalRegulation("@10 + @11 > @12 - @13", look)[0].ok, false);
  assert.strictEqual(evalRegulation("@12-@10-@11>=3", look)[0].ok, true);
});

test("evalRegulation: unknown peers and malformed clauses are unknown", () => {
  const look = (a: number) => (a === 1 ? 5 : undefined);
  const r = evalRegulation("@1<=@2; ;@1>=0", look);
  assert.strictEqual(r.length, 2);
  assert.strictEqual(r[0].ok, null);
  assert.strictEqual(r[0].lhs, 5);
  assert.strictEqual(r[0].rhs, null);
  assert.strictEqual(r[1].ok, true);
  const bad = evalRegulation("@1@2<=@3;garbage", look);
  assert.strictEqual(bad[0].op, null);
  assert.strictEqual(bad[0].ok, null);
  assert.strictEqual(bad[1].op, null);
});

test("frameGroups merges consecutive addresses up to 123 registers", () => {
  assert.deepStrictEqual(
    frameGroups([
      [32310, 6],
      [30036, 13],
      [32322, 1],
      [32323, 2],
      [32324, 3],
      [32362, 150],
      [32363, 0],
      [32361, 9], // order break: new frame
    ]),
    [
      { start: 32310, n: 1 },
      { start: 30036, n: 1 },
      { start: 32322, n: 3 },
      { start: 32362, n: 2 },
      { start: 32361, n: 1 },
    ],
  );
  const long: Pair[] = Array.from({ length: 130 }, (_, i) => [1000 + i, 0] as Pair);
  assert.deepStrictEqual(frameGroups(long), [
    { start: 1000, n: MAX_FRAME_REGS },
    { start: 1000 + MAX_FRAME_REGS, n: 130 - MAX_FRAME_REGS },
  ]);
  assert.deepStrictEqual(frameGroups([]), []);
});
