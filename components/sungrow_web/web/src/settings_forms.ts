// Pure logic of the Settings page forms (web login, MQTT broker, cloud version, boot slots):
// validation that mirrors the device (login_prefs.h, mqtt_settings.h, cloud_version.h), the
// fields a save posts, and the status texts. No DOM here so it is unit-tested; settings.ts renders.
import type { BootSlotInfo, CloudVersionInfo, MqttSettingsInfo, SlotJson } from "./api.js";

// ---- web login -------------------------------------------------------------------------------

export const LOGIN_USER_MAX = 31;
export const LOGIN_PASS_MIN = 8;
export const LOGIN_PASS_MAX = 63;

/** Printable ASCII without ':' (the digest A1 hash is "user:realm:pass"). */
function loginCharsOk(s: string): boolean {
  return /^[\x20-\x39\x3b-\x7e]*$/.test(s);
}

export interface LoginForm {
  username: string;
  password: string;
  confirm: string;
  current: string;
}

/** The first problem with a login change, or null when it can be posted. */
export function validateLogin(f: LoginForm): string | null {
  if (!f.current) return "Enter the current password.";
  if (f.username.length < 1 || f.username.length > LOGIN_USER_MAX) return "Username: 1–31 characters.";
  if (!loginCharsOk(f.username)) return "Username: letters, digits and punctuation, no ':'.";
  if (f.username.startsWith(" ") || f.username.endsWith(" ")) return "Username must not start or end with a space.";
  if (f.password.length < LOGIN_PASS_MIN || f.password.length > LOGIN_PASS_MAX) return "Password: 8–63 characters.";
  if (!loginCharsOk(f.password)) return "Password: printable characters, no ':'.";
  if (f.password !== f.confirm) return "The two passwords differ.";
  return null;
}

// ---- MQTT broker -----------------------------------------------------------------------------

export interface MqttForm {
  host: string;
  port: string; // as typed
  username: string;
  password: string; // "" = unchanged
  clearPassword: boolean;
  prefix: string;
  tls: boolean;
}

/** The form as the device last reported it (password never known: blank). */
export function mqttFormFrom(s: MqttSettingsInfo): MqttForm {
  return {
    host: s.host ?? "",
    port: String(s.port ?? 1883),
    username: s.username ?? "",
    password: "",
    clearPassword: false,
    prefix: s.prefix ?? "homeassistant",
    tls: s.tls ?? false,
  };
}

/** The first problem with the MQTT form, or null. An empty host turns MQTT off. */
export function validateMqtt(f: MqttForm): string | null {
  if (f.host.length > 127) return "Host: 127 characters at most.";
  if (/[\s]/.test(f.host) || !/^[\x21-\x7e]*$/.test(f.host)) return "Host: a host name or IP address, no spaces.";
  if (f.host.includes("://")) return "Host: a name or address, not a URL.";
  const port = Number(f.port);
  if (!/^\d+$/.test(f.port.trim()) || port < 1 || port > 65535) return "Port: 1–65535.";
  if (f.username.length > 63) return "Username: 63 characters at most.";
  if (f.password.length > 127) return "Password: 127 characters at most.";
  if (!/^[\x20-\x7e]*$/.test(f.password)) return "Password: printable characters only.";
  const prefix = f.prefix || "homeassistant";
  if (prefix.length > 31) return "Discovery prefix: 31 characters at most.";
  if (!/^[A-Za-z0-9_\-/]+$/.test(prefix)) return "Discovery prefix: letters, digits, '_', '-' and '/'.";
  if (prefix.startsWith("/") || prefix.endsWith("/")) return "Discovery prefix must not start or end with '/'.";
  return null;
}

/** The fields POST /api/mqtt_settings takes. A blank password leaves the stored one; clearPassword drops it. */
export function mqttPostFields(f: MqttForm): Record<string, string> {
  return {
    host: f.host.trim(),
    port: f.port.trim(),
    username: f.username,
    password: f.clearPassword ? "" : f.password,
    clear_password: f.clearPassword ? "1" : "0",
    prefix: f.prefix || "homeassistant",
    tls: f.tls ? "1" : "0",
  };
}

export interface Pill {
  text: string;
  cls: "ok" | "warn" | "err" | "";
}

/** The connection status pill of the MQTT card. */
export function mqttStatus(s: MqttSettingsInfo): Pill {
  if (!s.available && !s.configured) return { text: "not in this build", cls: "" };
  if (!s.configured) return { text: "off — no broker set", cls: "" };
  if (s.connected) return { text: "connected", cls: "ok" };
  if (s.last_error) return { text: `not connected: ${s.last_error}`, cls: "err" };
  return { text: "connecting…", cls: "warn" };
}

// ---- cloud version ---------------------------------------------------------------------------

export const CLOUD_VERSION_RE = /^WINET-SV\d{3}\.\d{3}\.\d{2}\.P\d{3}$/;

export function validateCloudVersion(v: string): string | null {
  if (v === "") return null; // clears the override
  if (!CLOUD_VERSION_RE.test(v)) return "Expected a stock version such as WINET-SV200.001.00.P046.";
  return null;
}

export const CLOUD_SOURCE_TEXT: Record<string, string> = {
  override: "set here",
  yaml: "set in the configuration",
  detected: "read from the stock image in the other slot",
  default: "the built-in default for this board",
};

/** One line describing where the reported version comes from. */
export function cloudVersionText(c: CloudVersionInfo): string {
  if (!c.available) return "Not available in this build.";
  const src = CLOUD_SOURCE_TEXT[c.source ?? ""] ?? c.source ?? "";
  return `${c.version || "—"} (${src})`;
}

/** Which quick actions the cloud-version card offers. */
export function cloudVersionActions(c: CloudVersionInfo): { useDetected: boolean; useDefault: boolean } {
  if (!c.available) return { useDetected: false, useDefault: false };
  const detected = c.detected ?? "";
  const dflt = c.default ?? "";
  return {
    // "Use detected": there is one and it is not what applies now.
    useDetected: detected !== "" && c.version !== detected,
    // "Use default": offered whenever the default is not what applies now.
    useDefault: dflt !== "" && c.version !== dflt,
  };
}

/** What a quick action posts as the override. Null = nothing to do. */
export function cloudVersionOverrideFor(c: CloudVersionInfo, action: "detected" | "default"): string | null {
  if (action === "detected") {
    // The detected value applies by itself once no override and no YAML value stand in the way.
    if (!c.detected) return null;
    return c.yaml ? c.detected : "";
  }
  return c.default ? c.default : null;
}

// ---- boot slots ------------------------------------------------------------------------------

export function slotText(s: SlotJson | null): string {
  if (!s) return "none";
  const addr = `0x${s.address.toString(16).toUpperCase()}`;
  if (!s.valid) return `${s.label} @${addr}: no valid app image`;
  return `${s.label} @${addr}: ${s.project || "?"} ${s.version || ""}`.trim();
}

/** Whether the "Boot other slot" button is offered, and the reason when it is not. */
export function bootSlotState(b: BootSlotInfo): { enabled: boolean; note: string } {
  if (!b.other) return { enabled: false, note: "This flash layout has no other OTA slot." };
  if (!b.other.valid) return { enabled: false, note: "The other slot holds no bootable image." };
  const stock = /^WINET-SV/.test(b.other.version);
  const kind = slotNetworkKind(b.other.version);
  return {
    enabled: true,
    note: stock
      ? "The other slot holds the stock Sungrow app: booting it is the way back to stock."
      : kind === "unknown"
        ? "The other slot holds another Rewired-S build."
        : `The other slot holds another Rewired-S build (${kind === "wifi" ? "Wi-Fi" : "Ethernet"}).`,
  };
}

/** Network kind of a Rewired-S image from the "+wifi" / "+ethernet" marker in its version. */
export function slotNetworkKind(version: string): "wifi" | "ethernet" | "unknown" {
  if (version.endsWith("+wifi")) return "wifi";
  if (version.endsWith("+ethernet")) return "ethernet";
  return "unknown";
}
