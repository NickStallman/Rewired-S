// Unit tests for settings_forms.ts (`npm run test`).
import {
  bootSlotState,
  cloudVersionActions,
  cloudVersionOverrideFor,
  cloudVersionText,
  mqttFormFrom,
  mqttPostFields,
  mqttStatus,
  slotText,
  validateCloudVersion,
  validateLogin,
  validateMqtt,
  type MqttForm,
} from "./settings_forms.js";
import type { BootSlotInfo, MqttSettingsInfo } from "./api.js";

interface Assert {
  deepStrictEqual(a: unknown, b: unknown, msg?: string): void;
  strictEqual(a: unknown, b: unknown, msg?: string): void;
  ok(v: unknown, msg?: string): void;
  match(s: string, re: RegExp, msg?: string): void;
}
declare const process: { getBuiltinModule(id: string): unknown };
const { test } = process.getBuiltinModule("node:test") as { test(name: string, fn: () => void): void };
const assert = process.getBuiltinModule("node:assert/strict") as Assert;

const login = (o: Partial<{ username: string; password: string; confirm: string; current: string }>) => ({
  username: "admin",
  password: "new-password",
  confirm: "new-password",
  current: "rewired-s",
  ...o,
});

test("validateLogin mirrors the device rules", () => {
  assert.strictEqual(validateLogin(login({})), null);
  assert.match(validateLogin(login({ current: "" }))!, /current/);
  assert.match(validateLogin(login({ username: "" }))!, /1–31/);
  assert.match(validateLogin(login({ username: "x".repeat(32) }))!, /1–31/);
  assert.match(validateLogin(login({ username: "ad:min" }))!, /':'/);
  assert.match(validateLogin(login({ username: " admin" }))!, /space/);
  assert.match(validateLogin(login({ password: "short", confirm: "short" }))!, /8–63/);
  assert.match(validateLogin(login({ password: "p".repeat(64), confirm: "p".repeat(64) }))!, /8–63/);
  assert.match(validateLogin(login({ password: "pass:word1", confirm: "pass:word1" }))!, /':'/);
  assert.match(validateLogin(login({ confirm: "other-password" }))!, /differ/);
  assert.strictEqual(validateLogin(login({ password: "with space ok", confirm: "with space ok" })), null);
});

const info = (o: Partial<MqttSettingsInfo> = {}): MqttSettingsInfo => ({
  available: true,
  source: "runtime",
  managed_by_yaml: false,
  configured: true,
  host: "192.0.2.20",
  port: 1883,
  username: "mqtt",
  has_password: true,
  prefix: "homeassistant",
  tls: false,
  connected: false,
  last_error: "",
  ...o,
});

test("mqttFormFrom never carries a password and fills defaults", () => {
  const f = mqttFormFrom(info());
  assert.deepStrictEqual(f, {
    host: "192.0.2.20",
    port: "1883",
    username: "mqtt",
    password: "",
    clearPassword: false,
    prefix: "homeassistant",
    tls: false,
  });
  const empty = mqttFormFrom({ available: true, source: "none", managed_by_yaml: false, configured: false });
  assert.strictEqual(empty.port, "1883");
  assert.strictEqual(empty.prefix, "homeassistant");
});

test("validateMqtt mirrors mqtt_settings.h", () => {
  const f = mqttFormFrom(info());
  assert.strictEqual(validateMqtt(f), null);
  assert.strictEqual(validateMqtt({ ...f, host: "" }), null); // off
  assert.strictEqual(validateMqtt({ ...f, host: "h".repeat(127) }), null);
  assert.match(validateMqtt({ ...f, host: "h".repeat(128) })!, /127/);
  assert.match(validateMqtt({ ...f, host: "my broker" })!, /spaces/);
  assert.match(validateMqtt({ ...f, host: "mqtt://b" })!, /URL/);
  assert.match(validateMqtt({ ...f, port: "0" })!, /Port/);
  assert.match(validateMqtt({ ...f, port: "70000" })!, /Port/);
  assert.match(validateMqtt({ ...f, port: "12a" })!, /Port/);
  assert.strictEqual(validateMqtt({ ...f, username: "u".repeat(63) }), null);
  assert.match(validateMqtt({ ...f, username: "u".repeat(64) })!, /Username/);
  assert.strictEqual(validateMqtt({ ...f, password: "p".repeat(64) }), null); // a 64-character broker password must fit
  assert.match(validateMqtt({ ...f, password: "p".repeat(128) })!, /Password/);
  assert.match(validateMqtt({ ...f, password: "tab\there" })!, /printable/);
  assert.match(validateMqtt({ ...f, prefix: "home assistant" })!, /prefix/);
  assert.match(validateMqtt({ ...f, prefix: "/ha" })!, /start or end/);
  assert.match(validateMqtt({ ...f, prefix: "ha/+" })!, /prefix/);
  assert.strictEqual(validateMqtt({ ...f, prefix: "" }), null); // defaults to homeassistant
  assert.strictEqual(validateMqtt({ ...f, prefix: "site-1/ha_disc" }), null);
});

test("mqttPostFields: blank password = unchanged, clear flag drops it", () => {
  const f: MqttForm = { ...mqttFormFrom(info()), host: " broker.lan ", port: " 8883 ", tls: true, prefix: "" };
  assert.deepStrictEqual(mqttPostFields(f), {
    host: "broker.lan",
    port: "8883",
    username: "mqtt",
    password: "",
    clear_password: "0",
    prefix: "homeassistant",
    tls: "1",
  });
  assert.strictEqual(mqttPostFields({ ...f, password: "new-pass" }).password, "new-pass");
  const cleared = mqttPostFields({ ...f, password: "typed-anyway", clearPassword: true });
  assert.strictEqual(cleared.password, "");
  assert.strictEqual(cleared.clear_password, "1");
});

test("mqttStatus pill", () => {
  assert.deepStrictEqual(mqttStatus(info({ connected: true })), { text: "connected", cls: "ok" });
  assert.deepStrictEqual(mqttStatus(info({ last_error: "connection refused: bad username or password" })), {
    text: "not connected: connection refused: bad username or password",
    cls: "err",
  });
  assert.deepStrictEqual(mqttStatus(info()), { text: "connecting…", cls: "warn" });
  assert.deepStrictEqual(mqttStatus(info({ configured: false, host: "" })), { text: "off — no broker set", cls: "" });
  assert.deepStrictEqual(
    mqttStatus({ available: false, source: "none", managed_by_yaml: false, configured: false }),
    { text: "not in this build", cls: "" },
  );
});

test("cloud version validation, text and quick actions", () => {
  assert.strictEqual(validateCloudVersion(""), null);
  assert.strictEqual(validateCloudVersion("WINET-SV200.001.00.P046"), null);
  assert.match(validateCloudVersion("WINET-SV200.001.00.P046B001-dir")!, /stock version/);
  assert.match(validateCloudVersion("P046")!, /stock version/);
  const detected = {
    available: true,
    version: "WINET-SV200.001.00.P044",
    source: "detected" as const,
    override: "",
    yaml: "",
    detected: "WINET-SV200.001.00.P044",
    default: "WINET-SV200.001.00.P046",
  };
  assert.strictEqual(cloudVersionText(detected), "WINET-SV200.001.00.P044 (read from the stock image in the other slot)");
  assert.deepStrictEqual(cloudVersionActions(detected), { useDetected: false, useDefault: true });
  assert.strictEqual(cloudVersionOverrideFor(detected, "default"), "WINET-SV200.001.00.P046");
  const overridden = { ...detected, version: "WINET-SV200.001.00.P046", source: "override" as const, override: "WINET-SV200.001.00.P046" };
  assert.deepStrictEqual(cloudVersionActions(overridden), { useDetected: true, useDefault: false });
  assert.strictEqual(cloudVersionOverrideFor(overridden, "detected"), ""); // clearing the override suffices
  const yamlSet = { ...detected, version: "WINET-SV200.001.00.P040", source: "yaml" as const, yaml: "WINET-SV200.001.00.P040" };
  assert.strictEqual(cloudVersionOverrideFor(yamlSet, "detected"), "WINET-SV200.001.00.P044"); // YAML beats detected: override
  const none = { ...detected, detected: "", version: "WINET-SV200.001.00.P046", source: "default" as const };
  assert.deepStrictEqual(cloudVersionActions(none), { useDetected: false, useDefault: false });
  assert.strictEqual(cloudVersionOverrideFor(none, "detected"), null);
  assert.strictEqual(cloudVersionText({ available: false }), "Not available in this build.");
});

test("boot slot text and button state", () => {
  const run = { label: "ota_0", address: 0x110000, valid: true, project: "rewired-s", version: "2026.9.1", idf: "v5.4" };
  const stock = { label: "ota_1", address: 0x610000, valid: true, project: "winet", version: "WINET-SV200.001.00.P046B001-dir", idf: "v4.4" };
  assert.strictEqual(slotText(run), "ota_0 @0x110000: rewired-s 2026.9.1");
  assert.strictEqual(slotText({ ...stock, valid: false, project: "", version: "" }), "ota_1 @0x610000: no valid app image");
  assert.strictEqual(slotText(null), "none");
  const b: BootSlotInfo = { running: run, other: stock, armed: false, can_switch: true };
  assert.deepStrictEqual(bootSlotState(b), { enabled: true, note: "The other slot holds the stock Sungrow app: booting it is the way back to stock." });
  assert.deepStrictEqual(bootSlotState({ ...b, other: { ...run, label: "ota_1" } }).enabled, true);
  assert.match(bootSlotState({ ...b, other: { ...run, label: "ota_1" } }).note, /another Rewired-S/);
  assert.deepStrictEqual(bootSlotState({ ...b, other: null, can_switch: false }), { enabled: false, note: "This flash layout has no other OTA slot." });
  assert.deepStrictEqual(bootSlotState({ ...b, other: { ...stock, valid: false }, can_switch: false }).enabled, false);
});
