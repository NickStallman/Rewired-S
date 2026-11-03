// Unit tests for diag_status.ts. Run with `npm run test`.
import { diagResultText } from "./diag_status.js";

interface Assert {
  deepStrictEqual(a: unknown, b: unknown, msg?: string): void;
  strictEqual(a: unknown, b: unknown, msg?: string): void;
}
declare const process: { getBuiltinModule(id: string): unknown };
const { test } = process.getBuiltinModule("node:test") as { test(name: string, fn: () => void): void };
const assert = process.getBuiltinModule("node:assert/strict") as Assert;

test("not configured", () => {
  assert.deepStrictEqual(diagResultText({ available: false }), {
    text: "Diagnostics upload is not configured in this build.",
    ok: null,
  });
});

test("idle shows nothing", () => {
  assert.strictEqual(diagResultText({ available: true, telemetry: true, state: "idle" }), null);
});

test("running, done and failed", () => {
  assert.deepStrictEqual(diagResultText({ available: true, state: "running" }), { text: "Sending…", ok: null });
  assert.deepStrictEqual(diagResultText({ available: true, state: "done", id: "a1b2c3d4" }), {
    text: "Sent. Reference a1b2c3d4: quote it in a bug report.",
    ok: true,
  });
  assert.deepStrictEqual(diagResultText({ available: true, state: "failed", error: "HTTP 429" }), {
    text: "Not sent: HTTP 429.",
    ok: false,
  });
  assert.deepStrictEqual(diagResultText({ available: true, state: "failed", error: "" }), {
    text: "Not sent: upload failed.",
    ok: false,
  });
});
