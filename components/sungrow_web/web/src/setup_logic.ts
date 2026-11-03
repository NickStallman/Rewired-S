// Pure helpers shared by the Installer and Setup views (no DOM, unit-tested by setup_logic.test.ts).
//
// Register conventions: every value is a 16-bit holding register addressed by its 1-based document
// address. A U32 setting spans `address` and `address + 1` low word first (stock nByteOrder 2): the
// low word sits at `address`. S16 values travel as their two's-complement 16-bit pattern.

/** One register write: [address, raw 16-bit value]. */
export type Pair = [number, number];

/** The subset of an Installer catalogue row the helpers need. */
export interface SettingSpec {
  address: number;
  dtype: "U16" | "S16" | "U32";
  scale: number;
  accuracy?: number;
  unit?: string;
  min?: number; // display units
  max?: number;
  rangeSource?: string;
}

/** Split a 32-bit raw value into its register words, low word first: [lo, hi]. */
export function u32Split(raw: number): [number, number] {
  const v = raw >>> 0;
  return [v & 0xffff, (v >>> 16) & 0xffff];
}

/** Join two register words (lo at `address`, hi at `address + 1`) into the 32-bit raw value. */
export function u32Join(lo: number, hi: number): number {
  return ((hi & 0xffff) * 0x10000 + (lo & 0xffff)) >>> 0;
}

/** The setting's current combined raw value from a register map, or null when a word is missing. */
export function currentRaw(s: SettingSpec, current: Map<number, number>): number | null {
  const lo = current.get(s.address);
  if (lo === undefined) return null;
  if (s.dtype !== "U32") return lo & 0xffff;
  const hi = current.get(s.address + 1);
  return hi === undefined ? null : u32Join(lo, hi);
}

/** Raw bit pattern -> signed/unsigned number as the dtype defines it (S16 0x8000.. are negative). */
export function rawToNumber(s: SettingSpec, raw: number): number {
  return s.dtype === "S16" && raw >= 0x8000 ? raw - 0x10000 : raw;
}

/** Raw bit pattern -> display value (number in the setting's unit). */
export function rawToDisplay(s: SettingSpec, raw: number): number {
  return rawToNumber(s, raw) * (s.scale || 1);
}

/** Display value -> raw bit pattern, clamped to the dtype (S16 as two's complement). */
export function displayToRaw(s: SettingSpec, display: number): number {
  let raw = Math.round(display / (s.scale || 1));
  const max = s.dtype === "U32" ? 0xffffffff : s.dtype === "S16" ? 0x7fff : 0xffff;
  const min = s.dtype === "S16" ? -0x8000 : 0;
  raw = Math.max(min, Math.min(max, raw));
  return s.dtype === "U32" ? raw >>> 0 : raw & 0xffff;
}

/**
 * Register pairs for the pending edits (setting address -> combined raw). U32 settings become two
 * pairs, low word at `address` then high word at `address + 1`. An entry whose value equals the
 * current one is dropped; an entry with an unknown current value is kept.
 */
export function pairsForPending(
  settings: SettingSpec[],
  pending: Map<number, number>,
  current: Map<number, number>,
): Pair[] {
  const byAddr = new Map(settings.map((s) => [s.address, s]));
  const out: Pair[] = [];
  for (const [addr, raw] of pending) {
    const s = byAddr.get(addr);
    if (!s) continue;
    const cur = currentRaw(s, current);
    if (s.dtype === "U32") {
      if (cur !== null && cur === raw >>> 0) continue;
      const [lo, hi] = u32Split(raw);
      out.push([s.address, lo], [s.address + 1, hi]);
    } else {
      if (cur !== null && cur === (raw & 0xffff)) continue;
      out.push([s.address, raw & 0xffff]);
    }
  }
  return out;
}

/** Stable partition: pairs whose address is a commit register first, the rest in their order. */
export function orderCommitFirst(pairs: Pair[], commitAddrs: Iterable<number>): Pair[] {
  const commit = new Set(commitAddrs);
  return [...pairs.filter((p) => commit.has(p[0])), ...pairs.filter((p) => !commit.has(p[0]))];
}

export interface DiffInput {
  a: number;
  new: number;
  cur?: number | null;
  valid?: boolean;
  kind?: string;
}
export interface DiffRow {
  a: number;
  new: number;
  cur: number | null; // null = not read / invalid
  kind: string;
  changed: boolean;
}

/**
 * Compare profile items with the inverter's current values. The current value comes from the item
 * (`cur` with `valid !== false`) or else from `current`. An unknown current value counts as a change.
 */
export function diffItems(items: DiffInput[], current?: Map<number, number>): DiffRow[] {
  return items.map((it) => {
    let cur: number | null = null;
    if (typeof it.cur === "number" && it.valid !== false) cur = it.cur & 0xffff;
    else if (current?.has(it.a)) cur = current.get(it.a)! & 0xffff;
    const nv = it.new & 0xffff;
    return { a: it.a, new: nv, cur, kind: it.kind ?? "", changed: cur === null || cur !== nv };
  });
}

/** Range check of a raw value in display units. Returns null when valid, else a message. */
export function validateRange(s: SettingSpec, raw: number): string | null {
  const v = rawToDisplay(s, raw);
  const eps = (s.scale || 1) / 2;
  const lo = s.min;
  const hi = s.max;
  if ((lo !== undefined && v < lo - eps) || (hi !== undefined && v > hi + eps)) {
    const dp = s.accuracy ?? 0;
    const unit = s.unit ? ` ${s.unit}` : "";
    const range =
      lo !== undefined && hi !== undefined
        ? `between ${lo.toFixed(dp)} and ${hi.toFixed(dp)}${unit}`
        : lo !== undefined
          ? `at least ${lo.toFixed(dp)}${unit}`
          : `at most ${hi!.toFixed(dp)}${unit}`;
    return `must be ${range}`;
  }
  return null;
}

// ---- regulation rules (stock getParam grammar) ------------------------------------------------
//
//   rule   := clause (";" clause)*
//   clause := expr op expr          op := "<=" | ">=" | "<" | ">" | "="
//   expr   := term (("+" | "-") term)*
//   term   := "@" address | number

export type RegOp = "<" | "<=" | ">" | ">=" | "=";

export interface ClauseResult {
  clause: string; // the clause text, trimmed
  op: RegOp | null; // null = unparseable
  addrs: number[]; // every register the clause references
  lhs: number | null; // evaluated sides (null when unknown)
  rhs: number | null;
  ok: boolean | null; // null = unknown (a referenced register has no value, or unparseable)
}

interface Term {
  sign: 1 | -1;
  addr?: number;
  num?: number;
}

function parseExpr(src: string): Term[] | null {
  const s = src.replace(/\s+/g, "");
  if (s === "") return null;
  const terms: Term[] = [];
  const re = /([+-]?)(@\d+|\d+(?:\.\d+)?)/y;
  let pos = 0;
  while (pos < s.length) {
    re.lastIndex = pos;
    const m = re.exec(s);
    if (!m) return null;
    if (terms.length > 0 && m[1] === "") return null; // two terms without an operator
    const sign: 1 | -1 = m[1] === "-" ? -1 : 1;
    terms.push(m[2][0] === "@" ? { sign, addr: Number(m[2].slice(1)) } : { sign, num: Number(m[2]) });
    pos = re.lastIndex;
  }
  return terms;
}

function evalExpr(terms: Term[], lookup: (addr: number) => number | undefined): number | null {
  let sum = 0;
  for (const t of terms) {
    const v = t.addr !== undefined ? lookup(t.addr) : t.num;
    if (v === undefined || !Number.isFinite(v)) return null;
    sum += t.sign * v;
  }
  return sum;
}

/** Evaluate every clause of a stock regulation rule. `lookup` gives a register's value or undefined. */
export function evalRegulation(rule: string, lookup: (addr: number) => number | undefined): ClauseResult[] {
  const out: ClauseResult[] = [];
  for (const raw of rule.split(";")) {
    const clause = raw.trim();
    if (clause === "") continue;
    const m = /^(.*?)(<=|>=|<|>|=)(.*)$/.exec(clause);
    const lt = m ? parseExpr(m[1]) : null;
    const rt = m ? parseExpr(m[3]) : null;
    if (!m || !lt || !rt) {
      out.push({ clause, op: null, addrs: [], lhs: null, rhs: null, ok: null });
      continue;
    }
    const op = m[2] as RegOp;
    const addrs = [...lt, ...rt].filter((t) => t.addr !== undefined).map((t) => t.addr!);
    const lhs = evalExpr(lt, lookup);
    const rhs = evalExpr(rt, lookup);
    let ok: boolean | null = null;
    if (lhs !== null && rhs !== null) {
      const eps = 1e-9;
      switch (op) {
        case "<":
          ok = lhs < rhs - eps;
          break;
        case "<=":
          ok = lhs <= rhs + eps;
          break;
        case ">":
          ok = lhs > rhs + eps;
          break;
        case ">=":
          ok = lhs >= rhs - eps;
          break;
        case "=":
          ok = Math.abs(lhs - rhs) <= eps;
          break;
      }
    }
    out.push({ clause, op, addrs, lhs, rhs, ok });
  }
  return out;
}

/** Every register address a rule references. */
export function regulationAddrs(rule: string): number[] {
  return [...rule.matchAll(/@(\d+)/g)].map((m) => Number(m[1]));
}

// ---- frames -----------------------------------------------------------------------------------

/** Largest fn 16 frame the server sends (Modbus limit for a write-multiple). */
export const MAX_FRAME_REGS = 123;

/**
 * The server's merge rule for a write batch: consecutive pairs with consecutive addresses share a
 * frame, up to MAX_FRAME_REGS registers; any gap or order break starts a new frame.
 */
export function frameGroups(pairs: Pair[]): { start: number; n: number }[] {
  const out: { start: number; n: number }[] = [];
  for (const [a] of pairs) {
    const last = out[out.length - 1];
    if (last && a === last.start + last.n && last.n < MAX_FRAME_REGS) last.n++;
    else out.push({ start: a, n: 1 });
  }
  return out;
}
