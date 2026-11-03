// esbuild bundler for the sungrow_web SPA.
//
//   npm ci            # once, installs esbuild + typescript (dev-only, never on the device)
//   npm run build     # -> dist/app.js + dist/app.css (minified, tree-shaken)
//   npm run watch     # rebuild on change
//   npm run serve     # dev server on http://localhost:8008 (serves dist/ + ../index.html)
//   npm run test      # bundle src/*.test.ts for node into dist/test/ and run them with node --test
//
// app.ts imports app.css, so esbuild emits a sibling dist/app.css automatically.
// The device-side filenames carry a content hash; that hashing + gzip + C++ header
// generation is done by new_firmware/tools/gen_web_assets.py (which calls this build).
import { spawnSync } from "node:child_process";
import { readdirSync } from "node:fs";
import * as esbuild from "esbuild";

const serve = process.argv.includes("--serve");
const watch = process.argv.includes("--watch");
const runTests = process.argv.includes("--test");

/** @type {import('esbuild').BuildOptions} */
const options = {
  entryPoints: ["src/app.ts"],
  bundle: true,
  outdir: "dist",
  format: "iife",
  target: ["es2020"],
  platform: "browser",
  minify: !serve,
  sourcemap: serve ? "inline" : false,
  legalComments: "none",
  loader: { ".css": "css" },
  logLevel: "info",
};

if (runTests) {
  // Pure-logic unit tests: one ESM bundle per *.test.ts, run by node's built-in test runner.
  const tests = readdirSync("src").filter((f) => f.endsWith(".test.ts")).map((f) => `src/${f}`);
  await esbuild.build({
    entryPoints: tests,
    bundle: true,
    outdir: "dist/test",
    outExtension: { ".js": ".mjs" },
    format: "esm",
    platform: "node",
    target: ["node20"],
    logLevel: "warning",
  });
  const files = tests.map((f) => `dist/test/${f.slice(4).replace(/\.ts$/, ".mjs")}`);
  const r = spawnSync(process.execPath, ["--test", ...files], { stdio: "inherit" });
  process.exit(r.status ?? 1);
} else if (serve) {
  const ctx = await esbuild.context(options);
  await ctx.watch();
  const { host, port } = await ctx.serve({ servedir: ".", port: 8008 });
  console.log(`dev server: http://${host}:${port}/  (open index.html)`);
} else if (watch) {
  const ctx = await esbuild.context(options);
  await ctx.watch();
  console.log("watching for changes...");
} else {
  await esbuild.build(options);
}
