// Typed client for the on-device sungrow_web JSON API (components/sungrow_web/spa_handler.cpp).
//
//   GET  /api/devices                           -> DeviceInfo[]
//   GET  /api/signals?device=N[&class=fast&keys=0x2080,...] -> SignalsResponse (filters AND)
//   POST /api/write?device&key&value            -> WriteResult (routine controls; the rule key is
//                                                  resolved to the device's bound register)
//   POST /api/confirm?device&address            -> confirm token bound to a batch sentinel address
//   POST /api/setup_write?device&confirm        -> WriteResult (Installer batch, body "addr:val,...")
//   POST /api/grid_read_start?device            -> start an on-demand read (body "addr,addr,...")
//   GET  /api/grid_read?device                  -> GridReadResponse (read cache + batch progress)
//   GET  /api/setup_options?device              -> SetupOptions (Setup picker)
//   GET  /api/setup_preview?device&option       -> SetupPreview (server-resolved profile vs inverter)
//   POST /api/setup_apply?device&option&confirm -> WriteResult (apply the previewed profile)
//   POST /api/setup_start?device&confirm        -> WriteResult (descriptor power switch := start)
//   POST /api/diag_send                         -> WriteResult (upload the diagnostics report)
//   GET  /api/diag_send                         -> DiagSendStatus (last upload, telemetry flag)
//   GET  /api/diag_report                       -> the diagnostics report as a text download
//   GET  /api/web_login                         -> WebLoginInfo (runtime login: user, defaults in use)
//   POST /api/web_login (form)                  -> WriteResult; 409 MANAGED_BY_YAML when compiled in
//   GET  /api/mqtt_settings                     -> MqttSettingsInfo (never the password)
//   POST /api/mqtt_settings (form)              -> WriteResult (blank password = unchanged)
//   GET  /api/cloud_version                     -> CloudVersionInfo (reported version + its source)
//   POST /api/cloud_version (form override=)    -> WriteResult ("" clears the override)
//   GET  /api/boot_slot                         -> BootSlotInfo (running / other OTA slot)
//   POST /api/boot_slot?confirm                 -> WriteResult (boots the other slot, restarts)
//
// Writes from this page are authorised by the web login (the Installer and Setup batches also by a
// confirm token); the "Inverter writes enabled" switch gates Modbus-TCP, MQTT and the HA action.

import type { XmlFamilyName } from "./setup_profiles.gen.js";

export interface LinkStats {
  sent: number;
  received: number;
  timeouts: number;
  exceptions?: number;
  loss_pct: number; // -1 when no attempts yet (NaN on device)
}

// Poll plan + freshness (master.h refresh tiers).
export interface PollInfo {
  initial_pass_done: boolean;
  fresh_age_ms: number; // ms since the most frequently polled read answered; -1 = never
  reads: { fast: number; live: number; slow: number; static: number };
  unserved: number; // reads that returned no values (polled at the static period)
}

export interface DeviceInfo {
  index: number;
  address: number;
  dev_type: number;
  dev_code: number;
  model: string;
  serial?: string; // device S/N (sig 0x0002), "" until read
  protocol: number;
  online: boolean;
  discovered: boolean;
  state: string; // "DISCOVER" | "RUN"
  family?: string; // "inverter" | "battery" | ... ; "" / absent until discovered
  xml_family?: XmlFamilyName; // grid-code XML family from (inverter type, protocol)
  inv_type?: number; // holding 30644 read at boot
  inv_type_known?: boolean;
  proto_code?: string; // devinfo protocol code, e.g. "AA10"
  dev_type_source?: string; // where dev_type came from; "none" = unknown
  descriptor?: DescriptorJson | null; // stock family descriptor; null = none
  stats: LinkStats;
  poll?: PollInfo;
  // Control profile the firmware attached ("hybrid" | "string"; null = none, so no controls) and
  // the follow-up of the last start/stop command.
  control?: { profile?: string | null; expect: ExpectInfo | null };
}

// Progress of the device's last start/stop command (ControlCore::expect_status). `key` is the
// control it was written through ("start_stop", "hybrid_start_stop", "power_switch").
export interface ExpectInfo {
  kind: "start" | "stop";
  key: string;
  text: string; // "stop pending: output ramping", "started", ...
  final: boolean; // the outcome is decided
  ok: boolean; // final: the command took effect
  age_ms: number; // since the command (pending) or since the outcome (final)
  state: string | null; // last running-state code, "0x1300"
}

// Stock family descriptor as the server sends it (register 0 = the family has none).
export interface DescriptorJson {
  inv_type: number;
  protocol: number;
  country_reg: number;
  grid_reg: number;
  power_switch_reg: number;
}

/** The descriptor's registers (null where the family has none). */
export interface DescriptorRegs {
  country: number | null;
  grid: number | null;
  powerSwitch: number | null;
}
export function descriptorRegs(d: DescriptorJson | null | undefined): DescriptorRegs | null {
  if (!d) return null;
  return { country: d.country_reg || null, grid: d.grid_reg || null, powerSwitch: d.power_switch_reg || null };
}

export interface SignalValue {
  key: string; // sigid hex, "0x2080"
  value: number | string | null;
  valid: boolean;
  updated_ms: number; // device uptime millis of last update
}

export interface SignalsResponse {
  device: number;
  now_ms: number;
  signals: SignalValue[];
}

// Server-side subset of /api/signals. `cls` keeps the signals of one poll tier of the device's
// register table (derived 0xFxxx signals always pass); `keys` keeps the listed sigids (at most 64).
// Both given = both must match.
export type SignalClass = "fast" | "live" | "slow" | "static";
export interface SignalsQuery {
  cls?: SignalClass;
  keys?: string[];
}

function signalsUrl(device: number, q?: SignalsQuery): string {
  let url = `/api/signals?device=${device}`;
  if (q?.cls) url += `&class=${q.cls}`;
  if (q?.keys && q.keys.length > 0) url += `&keys=${q.keys.join(",")}`;
  return url;
}

// Synchronous ControlCore gate verdict; OK = queued. `message` carries the detail, e.g.
// "range:32323", "relation:32323<=32327", "register 30646".
export interface WriteResult {
  ok: boolean;
  verdict: string; // OK | DISABLED | NOT_ALLOWED | BAD_VALUE | OFFLINE | BUSY | INVALID | CONFIRM_REQUIRED | PREVIEW_REQUIRED | UNAVAILABLE
  message: string;
  total?: number; // registers queued by a batch
  note?: string; // Setup apply: items skipped because the model's power stage would not accept them
}

export const VERDICT_TEXT: Record<string, string> = {
  OK: "Queued",
  DISABLED: "Writes disabled",
  NOT_ALLOWED: "Not allowed",
  BAD_VALUE: "Value out of range",
  OFFLINE: "Device offline",
  BUSY: "Busy — retry",
  INVALID: "Invalid request",
  CONFIRM_REQUIRED: "Confirmation needed",
  PREVIEW_REQUIRED: "Preview expired — preview again",
  UNAVAILABLE: "Writes not available",
  MANAGED_BY_YAML: "Set in the configuration",
  WRONG_CURRENT: "Current password is wrong",
  MISMATCH: "Passwords differ",
  STORE_FAILED: "Could not store",
  NOT_ARMED: "Not armed",
  NO_OTHER_SLOT: "No other slot",
  NO_VALID_IMAGE: "No valid image in the other slot",
  SET_FAILED: "Boot record not written",
};

/** Readable text for a rejected write. */
export function verdictText(r: WriteResult): string {
  const v = VERDICT_TEXT[r.verdict] ?? r.verdict;
  return r.message ? `${v}: ${r.message}` : v;
}

// One register value from GET /api/grid_read.
export interface GridReadValue {
  a: number; // 1-based document address
  v: number | null; // raw 16-bit value; null when invalid
  valid: boolean;
  reason?: string; // why an invalid value has none: off-list | exception | timeout | pending
}
export interface SetupFrame {
  start: number;
  n: number;
  // pending | ok | exception | timeout | expired | verify_mismatch | verify_failed | offline |
  // rate_limited | queue_full | invalid
  status: string;
  ex?: number; // Modbus exception code (0 = none)
}
export interface SetupProgress {
  device?: number; // the device the batch belongs to
  busy: boolean;
  total: number;
  done: number;
  ok: number;
  failed: number;
  deadline_ms?: number; // time left before the remaining frames are dropped
  frames?: SetupFrame[];
}
export interface GridReadResponse {
  device?: number;
  complete: boolean;
  values: GridReadValue[];
  setup?: SetupProgress;
  stale?: boolean; // the cache belongs to another device
}

export interface SetupOptionInfo {
  code: number;
  label: string;
  needparam: number;
  needreadtype: number;
  variants: { ref5479: number[]; nsets: number; nregs: number }[];
}
// Start-or-poll: `complete:false` until the reads of 5479 and the current country / company are in.
export interface SetupOptions {
  device: number;
  family: XmlFamilyName;
  descriptor: DescriptorJson | null;
  feature: { w32b5: boolean; w29b12: boolean };
  complete: boolean;
  reg5479: number | null;
  current: { country: number | null; grid_code: number | null; company: number | null; grid_type: number | null };
  countries: { code: number; label: string }[];
  options: SetupOptionInfo[];
}
export interface SetupPreviewItem {
  a: number;
  new: number;
  cur: number | null;
  valid: boolean;
  kind: string; // country | company | grid_type | set
  changed: boolean;
}
// Start-or-poll: `complete:false` while the reads run.
export interface SetupPreview {
  complete: boolean;
  status?: string; // "ok", a ResolveStatus name, or "error" (prepare failed: see error / message)
  error?: string; // OFFLINE | BUSY
  message?: string;
  reg5479?: number | null;
  variant?: number | null; // index into the option's variants
  items?: SetupPreviewItem[];
  frames?: { start: number; n: number }[];
  n_changed?: number;
  note?: string; // items the apply will skip (outside the model's power-stage range), "" when none
}

async function getJson<T>(url: string, signal?: AbortSignal): Promise<T> {
  const res = await fetch(url, { signal, headers: { Accept: "application/json" } });
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  return (await res.json()) as T;
}

async function postJson<T>(url: string): Promise<T> {
  const res = await fetch(url, { method: "POST", headers: { Accept: "application/json" } });
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  return (await res.json()) as T;
}

// POST an application/x-www-form-urlencoded body (the Settings page forms). The device parses it
// into request args (1 KB cap on the device, far above these forms). A non-2xx reply that still
// carries a JSON WriteResult (409 MANAGED_BY_YAML) is returned as that result, not thrown.
export async function postForm<T>(url: string, fields: Record<string, string>): Promise<T> {
  const body = new URLSearchParams();
  for (const [k, v] of Object.entries(fields)) body.set(k, v);
  const res = await fetch(url, {
    method: "POST",
    headers: { Accept: "application/json", "Content-Type": "application/x-www-form-urlencoded" },
    body: body.toString(),
  });
  const ct = res.headers.get("content-type") ?? "";
  if (!res.ok && !ct.includes("json")) throw new Error(`${url}: HTTP ${res.status}`);
  return (await res.json()) as T;
}

// POST with a text/plain body (setup_write / grid_read_start). text/plain routes to the device's
// raw-body handler (no 512-byte form-body cap), so a whole batch fits.
async function postText<T>(url: string, body: string): Promise<T> {
  const res = await fetch(url, {
    method: "POST",
    headers: { Accept: "application/json", "Content-Type": "text/plain" },
    body,
  });
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  return (await res.json()) as T;
}

/** GET /api/diag_send: the diagnostics report upload (sungrow_analytics). */
export interface DiagSendStatus {
  available: boolean; // false when no sungrow_analytics is configured (no other fields then)
  telemetry?: boolean; // automatic reporting (usage pings, error events, automatic uploads) is on
  state?: "idle" | "running" | "done" | "failed"; // the last upload, this boot
  id?: string; // collector reference of the last successful upload
  error?: string; // why the last upload failed
  trigger?: string; // manual | crash | stall | watchdog | offline | cloud | heap | modbus | device
  age_ms?: number; // since the last state change
}

/** GET /api/capture: diagnostic captures (I-V scan, grid waveform, fault recordings). */
export type CaptureKind = "iv" | "grid" | "fault";
export interface CaptureDevice {
  device: number;
  online: boolean;
  iv: boolean; // functionEnum bit 3
  grid: boolean; // functionEnum bit 19
  fault: boolean; // stored fault recordings can be exported
  mppt: number; // PV inputs the inverter reports (0 = unknown)
  model: string;
}
export interface CaptureResult {
  id: number;
  kind: CaptureKind;
  ok: boolean;
  size: number;
  cycles: number;
  records: number; // records in the file (fault recordings: up to 10)
  wall_start: number; // unix seconds, 0 = clock not set
  duration_ms: number;
  result: string;
  file_name: string;
  inverter: string;
}
export interface CaptureStatus {
  available: boolean;
  running?: boolean;
  device?: number; // store index of the device being captured while running
  kind?: CaptureKind;
  step?: string;
  progress?: number; // -1 = unknown
  elapsed_ms?: number;
  scan_ms?: number; // I-V scan: time in the scanning step
  iv_fault?: number; // I-V scan: refusal code the inverter reports (0 = none)
  iv_fault_text?: string | null;
  iv_starts?: number;
  generation?: number;
  devices?: CaptureDevice[];
  results?: CaptureResult[];
}

/** GET /api/web_login. */
export interface WebLoginInfo {
  managed: "yaml" | "runtime"; // yaml: compiled in from secrets, the form is not offered
  username: string; // "" when managed by YAML
  default: boolean; // the shipped placeholders are still in use
}

/** GET /api/mqtt_settings (sungrow_mqtt runtime settings). The password is never returned. */
export interface MqttSettingsInfo {
  available: boolean; // runtime settings compiled in (sungrow_mqtt runtime_settings)
  source: "yaml" | "runtime" | "none";
  managed_by_yaml: boolean; // the configuration names the broker: read-only
  configured: boolean; // a broker is set (client running)
  host?: string;
  port?: number;
  username?: string;
  has_password?: boolean;
  prefix?: string;
  tls?: boolean;
  connected?: boolean;
  last_error?: string; // "" after a successful connect
}

/** GET /api/cloud_version (sungrow_board). */
export interface CloudVersionInfo {
  available: boolean;
  version?: string; // what iSolarCloud is told
  source?: "override" | "yaml" | "detected" | "default";
  override?: string; // "" = none
  yaml?: string; // "" = not set (pre-built images)
  detected?: string; // "" = no stock image seen in the other slot
  default?: string; // the board's built-in stock version
}

/** One OTA slot of GET /api/boot_slot. */
export interface SlotJson {
  label: string;
  address: number;
  valid: boolean; // holds a valid app image
  project: string; // app descriptor project name ("" when invalid)
  version: string;
  idf: string;
}
export interface BootSlotInfo {
  running: SlotJson | null;
  other: SlotJson | null; // null: no other OTA slot
  armed: boolean; // the "Allow boot switch" switch is on
  can_switch: boolean; // the other slot holds a valid image
  fallback?: string; // network fallback state (Ethernet images with common/ethernet_fallback.yaml)
}

const tok = (confirm: string) => encodeURIComponent(confirm);

export const api = {
  devices: (signal?: AbortSignal) => getJson<DeviceInfo[]>("/api/devices", signal),
  // signals(device, signal) fetches the full set; signals(device, {cls, keys}, signal) a subset.
  signals: (device: number, query?: SignalsQuery | AbortSignal, signal?: AbortSignal) =>
    query instanceof AbortSignal
      ? getJson<SignalsResponse>(signalsUrl(device), query)
      : getJson<SignalsResponse>(signalsUrl(device, query), signal),

  // Routine register write of the control `key` (WRITE_META key; the server resolves the register
  // bound to the device's protocol). value is a RAW register value.
  write: (device: number, key: string, value: number) =>
    postJson<WriteResult>(`/api/write?device=${device}&key=${encodeURIComponent(key)}&value=${value}`),
  confirm: (device: number, address: number) =>
    postJson<{ token: string; ttl_ms: number }>(`/api/confirm?device=${device}&address=${address}`),

  // Sentinel addresses the batch confirm tokens are bound to.
  SETUP_WRITE_CONFIRM_ADDR: 65535, // Installer batch (/api/setup_write)
  SETUP_APPLY_CONFIRM_ADDR: 65534, // Setup profile (/api/setup_apply)
  SETUP_START_CONFIRM_ADDR: 65533, // Setup start (/api/setup_start)

  // Installer batch: `pairs` = [address, rawValue][], written in this order.
  setupWrite: (device: number, pairs: [number, number][], confirm: string) =>
    postText<WriteResult>(
      `/api/setup_write?device=${device}&confirm=${tok(confirm)}`,
      pairs.map(([a, v]) => `${a}:${v & 0xffff}`).join(","),
    ),
  // Start an on-demand read of `addrs` into the device's read cache.
  gridReadStart: (device: number, addrs: number[]) =>
    postText<{ ok: boolean }>(`/api/grid_read_start?device=${device}`, addrs.join(",")),
  // Poll the read cache + batch progress.
  gridRead: (device: number, signal?: AbortSignal) =>
    getJson<GridReadResponse>(`/api/grid_read?device=${device}`, signal),

  // Setup: server-resolved stock grid profiles.
  setupOptions: (device: number, signal?: AbortSignal) =>
    getJson<SetupOptions>(`/api/setup_options?device=${device}`, signal),
  setupPreview: (device: number, option: number, signal?: AbortSignal) =>
    getJson<SetupPreview>(`/api/setup_preview?device=${device}&option=${option}`, signal),
  setupApply: (device: number, option: number, confirm: string) =>
    postJson<WriteResult>(`/api/setup_apply?device=${device}&option=${option}&confirm=${tok(confirm)}`),
  setupStart: (device: number, confirm: string) =>
    postJson<WriteResult>(`/api/setup_start?device=${device}&confirm=${tok(confirm)}`),

  // Diagnostic captures.
  CAPTURE_CONFIRM_ADDR: 65532,
  captureStatus: (signal?: AbortSignal) => getJson<CaptureStatus>("/api/capture", signal),
  captureStart: (device: number, kind: CaptureKind, cycles: number, confirm: string) =>
    postJson<WriteResult>(
      `/api/capture_start?device=${device}&kind=${kind}${kind === "grid" ? `&cycles=${cycles}` : ""}&confirm=${tok(confirm)}`,
    ),
  captureCancel: () => postJson<WriteResult>("/api/capture_cancel"),
  captureFileUrl: (id: number) => `/api/capture_file?id=${id}`,
  captureMetaUrl: (id: number) => `/api/capture_meta?id=${id}`,

  // Diagnostics report: manual upload to the developer's collector, and the same report as a file.
  diagSend: () => postJson<WriteResult>("/api/diag_send"),
  diagStatus: (signal?: AbortSignal) => getJson<DiagSendStatus>("/api/diag_send", signal),
  diagReportUrl: "/api/diag_report",

  // Settings page of the pre-built images (docs/prebuilt.md).
  webLogin: (signal?: AbortSignal) => getJson<WebLoginInfo>("/api/web_login", signal),
  webLoginChange: (fields: { username: string; password: string; confirm: string; current_password: string }) =>
    postForm<WriteResult>("/api/web_login", fields),
  mqttSettings: (signal?: AbortSignal) => getJson<MqttSettingsInfo>("/api/mqtt_settings", signal),
  mqttSettingsSave: (fields: Record<string, string>) => postForm<WriteResult>("/api/mqtt_settings", fields),
  cloudVersion: (signal?: AbortSignal) => getJson<CloudVersionInfo>("/api/cloud_version", signal),
  cloudVersionSet: (override: string) => postForm<WriteResult>("/api/cloud_version", { override }),
  // Boot slots: the switch to the other slot takes a confirm token bound to (device 0, 65531).
  BOOT_SLOT_CONFIRM_ADDR: 65531,
  bootSlot: (signal?: AbortSignal) => getJson<BootSlotInfo>("/api/boot_slot", signal),
  bootOtherSlot: (confirm: string) => postJson<WriteResult>(`/api/boot_slot?confirm=${tok(confirm)}`),

  // Drive an ESPHome entity over web_server REST (switch toggle, button press, ...). web_server v3
  // matches by entity NAME, so `entity` is the name (which may contain spaces) and must be encoded.
  command: (domain: string, entity: string, action: string) =>
    fetch(`/${domain}/${encodeURIComponent(entity)}/${action}`, { method: "POST" }).then((r) => {
      if (!r.ok) throw new Error(`${domain}/${entity}/${action}: HTTP ${r.status}`);
    }),
};

// ---- shared helpers --------------------------------------------------------------------------

/** Grid-code families with a stock XML page set (Setup and Installer are offered for these). */
const REAL_FAMILIES: readonly string[] = ["str_pv", "res_es", "res_new_pv", "unit"];

export function isRealFamily(f: string | undefined): f is XmlFamilyName {
  return f !== undefined && REAL_FAMILIES.includes(f);
}

/** A device whose type is known: it answered discovery, or its dev_type comes from YAML/inference. */
export function isIdentified(d: DeviceInfo): boolean {
  return d.discovered || (d.dev_type_source !== undefined && d.dev_type_source !== "none");
}

export function sleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(new Error("aborted"));
      return;
    }
    const t = setTimeout(resolve, ms);
    signal?.addEventListener(
      "abort",
      () => {
        clearTimeout(t);
        reject(new Error("aborted"));
      },
      { once: true },
    );
  });
}

/**
 * Read `addrs` through the device's read cache: start the read, then poll until complete or the
 * timeout. `onValues` sees every poll. Resolves with the last response (null when nothing came back).
 * The server keeps one read cache, owned by the device of the latest start; a stale reply is skipped.
 */
export async function readRegisters(
  device: number,
  addrs: number[],
  signal: AbortSignal,
  onValues?: (r: GridReadResponse) => void,
  pollMs = 800,
  timeoutMs = 25000,
): Promise<GridReadResponse | null> {
  await api.gridReadStart(device, addrs);
  const deadline = Date.now() + timeoutMs;
  let last: GridReadResponse | null = null;
  for (;;) {
    await sleep(pollMs, signal);
    const r = await api.gridRead(device, signal);
    if (!r.stale && (r.device === undefined || r.device === device)) {
      last = r;
      onValues?.(r);
      if (r.complete) break;
    }
    if (Date.now() > deadline) break;
  }
  return last;
}

/** Valid values of a read as an address -> raw map, plus the reasons of the invalid ones. */
export function splitValues(r: GridReadResponse | null): { values: Map<number, number>; reasons: Map<number, string> } {
  const values = new Map<number, number>();
  const reasons = new Map<number, string>();
  for (const v of r?.values ?? []) {
    if (v.valid !== false && v.v !== null) values.set(v.a, v.v);
    else reasons.set(v.a, v.reason ?? "invalid");
  }
  return { values, reasons };
}
