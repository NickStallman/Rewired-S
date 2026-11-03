// Diagnostics-report upload status -> the one-line result shown under "Send diagnostics".
// Pure (no DOM) so it is unit-tested; the Settings view renders it.
import type { DiagSendStatus } from "./api.js";

/** What the report contains, listed before it is sent. */
export const DIAG_CONTENTS: readonly string[] = [
  "the build and board",
  "the detected devices and their register map",
  "feature state (cloud, MQTT, Modbus-TCP, writes)",
  "memory use",
  "the reset journal",
  "the recent log tail",
];
export const DIAG_SCRUB_NOTE =
  "Serial numbers, the Wi-Fi name and network addresses are replaced before the report leaves the dongle.";

export interface DiagResult {
  text: string;
  ok: boolean | null; // true = sent, false = failed, null = neutral
}

/** Result line for an upload status; null while nothing has happened yet this boot. */
export function diagResultText(s: DiagSendStatus): DiagResult | null {
  if (!s.available) return { text: "Diagnostics upload is not configured in this build.", ok: null };
  switch (s.state) {
    case "running":
      return { text: "Sending…", ok: null };
    case "done":
      return { text: `Sent. Reference ${s.id || "?"}: quote it in a bug report.`, ok: true };
    case "failed":
      return { text: `Not sent: ${s.error || "upload failed"}.`, ok: false };
    default:
      return null;
  }
}
