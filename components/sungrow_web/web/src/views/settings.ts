import { api, verdictText, type BootSlotInfo, type CloudVersionInfo, type DiagSendStatus, type MqttSettingsInfo, type WriteResult } from "../api.js";
import { DIAG_CONTENTS, DIAG_SCRUB_NOTE, diagResultText } from "../diag_status.js";
import { el } from "../dom.js";
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
} from "../settings_forms.js";
import { confirmDialog } from "./setup.js";
import type { View } from "./types.js";

// Settings — the DONGLE's own ESPHome entities (the dongle is the native-API HA device; the
// connected inverter/battery/meter are MQTT and live under Devices). State is read live from
// web_server's SSE /events stream; controls POST to web_server's REST API
// (/<domain>/<object_id>/<action>). Entities are rendered generically by domain (switch →
// toggle, button → press, sensor/text_sensor/binary_sensor → read-only value) and updated in
// place (no flicker). The write-gate switch is pinned prominently at the top.
//
// Ahead of the entity sections sit the provisioning cards of the pre-built images (docs/prebuilt.md):
// Web login (/api/web_login; hidden when the login is compiled in), Home Assistant / MQTT broker
// (/api/mqtt_settings), iSolarCloud identity (/api/cloud_version) and Boot slots (/api/boot_slot).
// The form logic lives in settings_forms.ts (unit-tested); this file only renders.

interface Entity {
  id: string; // "<domain>-<object_id>" as sent by ESPHome
  domain: string;
  objectId: string;
  name: string;
  state: string; // formatted display string
  on: boolean | null; // for switch/binary_sensor
}

interface Row {
  entity: Entity;
  root: HTMLElement;
  valueEl: HTMLElement;
  toggle: HTMLButtonElement | null;
}

type SectionId = "write" | "cloud" | "local" | "diag" | "actions" | "debug" | "other";

// The inverter write-gate switch, matched by entity NAME (web_server v3 keys everything by name).
const WRITE_SWITCH = "Inverter writes enabled";
// Debug/engineering buttons tucked away under their own group (by name).
const DEBUG_BUTTONS = new Set(["Dump identity file", "List ext flash files"]);
// Entities that belong under Diagnostics although their domain says otherwise.
const DIAG_ENTITIES = new Set(["Usage reporting"]);
// The boot-slot entities are driven from the Boot slots card (confirm dialog); hidden as rows.
const HIDDEN_ENTITIES = new Set(["Allow boot switch", "Boot other slot"]);

const SECTIONS: { id: SectionId; title: string }[] = [
  { id: "write", title: "Write access" },
  { id: "cloud", title: "iSolarCloud" },
  { id: "local", title: "Local access" },
  { id: "diag", title: "Diagnostics" },
  { id: "actions", title: "Actions" },
  { id: "debug", title: "Debug" },
  { id: "other", title: "Other" },
];

// Known dongle buttons are seeded so Actions is populated even before their SSE state arrives.
const SEED_BUTTONS: { name: string }[] = [
  { name: "RS485 rescan" },
  { name: "Restart" },
  { name: "Restart (safe mode)" },
];

// Short explanations shown as tooltips (and inline where helpful). Keyed by entity NAME.
const TOOLTIPS: Record<string, string> = {
  "Inverter writes enabled":
    "Gates inverter writes from Modbus-TCP, MQTT and the Home Assistant action; while off those are rejected. Writes from this web page (Devices controls, Installer, Setup) are authorised by your login instead, and Installer and Setup batches also ask for a confirmation.",
  "Modbus-TCP server": "Expose a Modbus-TCP :502 slave on the LAN so other software can read/write the inverter through this dongle.",
  "RS485 rescan": "Re-scan the RS485 bus for connected devices (inverter/meter/battery). Use after wiring changes.",
  Restart: "Reboot the dongle. Brief (~5 s) loss of monitoring; does not affect the inverter.",
  "Restart (safe mode)": "Reboot into ESPHome safe mode (skips the app) to recover from a boot loop or bad config.",
  Uptime: "How long the dongle has been running since its last boot.",
  "Cloud connected": "Whether the dongle currently has a live connection to iSolarCloud.",
  "Cloud publish count": "Number of data batches the dongle has uploaded to iSolarCloud since boot.",
  "Inverter writes ok": "Count of successful inverter register writes since boot.",
  "Inverter writes failed": "Count of inverter register writes that errored (exception / timeout / verify mismatch) since boot.",
  "Modbus-TCP clients": "Current number of LAN clients connected to the Modbus-TCP :502 server.",
  "Modbus-TCP requests": "Total Modbus-TCP requests served since boot.",
  "Modbus-TCP errors": "Modbus-TCP requests that returned an exception since boot.",
  "Ext flash mounted": "Whether the dongle's external SPI flash (config/log storage) is mounted.",
  "Identity valid": "Whether the dongle's stored identity/calibration file is present and valid.",
  Button: "State of the dongle's physical button.",
  "Usage reporting":
    "Send an anonymous daily usage report (versions, board, connected device models, feature state) and error events to the developer, and upload a diagnostics report automatically after a crash or on an unrecognised device. Remembered across restarts. Send diagnostics works either way.",
  "MQTT connected": "The connection to the MQTT broker configured under Home Assistant above is up.",
  Firmware: "The published Rewired-S release this dongle can update to (checked every 6 hours).",
};

function sectionFor(e: Entity): SectionId {
  if (e.name === WRITE_SWITCH) return "write";
  if (DIAG_ENTITIES.has(e.name)) return "diag";
  if (e.name.toLowerCase().startsWith("cloud")) return "cloud";
  if (e.domain === "button") return DEBUG_BUTTONS.has(e.name) ? "debug" : "actions";
  if (e.domain === "switch") return "local"; // local-access switches (Modbus-TCP server, …)
  if (e.domain === "sensor" || e.domain === "text_sensor" || e.domain === "binary_sensor") return "diag";
  return "other";
}

function parseState(data: Record<string, unknown>): Entity | null {
  const id = typeof data.id === "string" ? data.id : "";
  const slash = id.indexOf("/");
  if (slash < 0) return null;
  // web_server v3 ids are "<domain>/<name>". The domain/name fields are only sent on the first
  // (DETAIL_ALL) event, so derive them from the id and prefer the explicit fields when present.
  const domain = typeof data.domain === "string" && data.domain ? data.domain : id.slice(0, slash);
  const name = typeof data.name === "string" && data.name ? data.name : id.slice(slash + 1);
  const objectId = name; // v3 commands target the (URL-encoded) name
  let on: boolean | null = null;
  if (typeof data.value === "boolean") on = data.value;
  else if (data.state === "ON") on = true;
  else if (data.state === "OFF") on = false;
  let state = "";
  if (typeof data.state === "string") state = data.state;
  else if (data.value !== undefined && data.value !== null) state = String(data.value);
  return { id, domain, objectId, name, state, on };
}

export class SettingsView implements View {
  readonly id = "settings";
  readonly label = "Settings";

  private source: EventSource | null = null;
  private statusPill!: HTMLElement;
  private sectionBodies = new Map<SectionId, HTMLElement>();
  private sectionCards = new Map<SectionId, HTMLElement>();
  private rows = new Map<string, Row>();
  private diagPoll: number | null = null;
  private diagAbort = new AbortController();

  render(root: HTMLElement): void {
    // Re-init per-mount state (destroyed on tab leave, re-rendered on return): clear the entity
    // maps so they don't hold rows from the previous mount's (now detached) DOM. connect() opens a
    // fresh EventSource; ESPHome re-sends all current states on connect.
    this.sectionBodies = new Map<SectionId, HTMLElement>();
    this.sectionCards = new Map<SectionId, HTMLElement>();
    this.rows = new Map<string, Row>();

    root.append(el("h1", { class: "view-title" }, ["Settings"]));
    this.statusPill = el("span", { class: "pill" }, ["connecting…"]);
    root.append(el("div", { class: "console-bar" }, [el("span", { class: "muted" }, ["Dongle"]), this.statusPill]));

    this.formsAbort = new AbortController();
    root.append(this.loginCard(), this.mqttCard(), this.cloudCard(), this.bootSlotCard());

    for (const s of SECTIONS) {
      const body = el("div", { class: "settings-grid" }, []);
      const card = el("section", { class: "settings-section", hidden: true }, [
        el("h2", { class: "settings-title" }, [s.title]),
        body,
      ]);
      this.sectionBodies.set(s.id, body);
      this.sectionCards.set(s.id, card);
      root.append(card);
    }

    // Seed known buttons so Actions is usable immediately.
    for (const b of SEED_BUTTONS) {
      this.upsert({ id: `button/${b.name}`, domain: "button", objectId: b.name, name: b.name, state: "", on: null });
    }

    this.diagAbort = new AbortController();
    this.addDiagRow();
    this.connect();
  }

  destroy(): void {
    this.source?.close();
    this.source = null;
    this.diagAbort.abort();
    this.formsAbort.abort();
    if (this.diagPoll !== null) window.clearTimeout(this.diagPoll);
    this.diagPoll = null;
    if (this.mqttPoll !== null) window.clearTimeout(this.mqttPoll);
    this.mqttPoll = null;
  }

  // ---- provisioning cards (pre-built images) ----------------------------------------------------
  private formsAbort = new AbortController();
  private mqttPoll: number | null = null;

  /** A labelled form row: `label` on the left, `control` on the right (settings-row look). */
  private formRow(label: string, control: HTMLElement, help?: string): HTMLElement {
    const lab = el("label", { class: "settings-label" }, [el("span", { class: "settings-name" }, [label])]);
    if (help) lab.append(el("span", { class: "settings-desc muted" }, [help]));
    return el("div", { class: "settings-row settings-form-row" }, [lab, el("div", { class: "settings-control" }, [control])]);
  }

  private input(type: string, attrs: Record<string, string | boolean> = {}): HTMLInputElement {
    return el("input", { class: "ctrl-input settings-input", type, spellcheck: "false", ...attrs }) as HTMLInputElement;
  }

  private static showResult(node: HTMLElement, r: WriteResult | string, ok?: boolean): void {
    if (typeof r === "string") {
      node.className = ok === undefined ? "settings-desc muted" : ok ? "settings-desc ok" : "settings-desc err";
      setText(node, r);
      return;
    }
    node.className = r.ok ? "settings-desc ok" : "settings-desc err";
    setText(node, r.ok ? r.message || "Saved." : verdictText(r));
  }

  // Web login: hidden when the login is compiled in (login: yaml); a banner while the shipped
  // defaults are in use. Every change asks for the current password.
  private loginCard(): HTMLElement {
    const banner = el("div", { class: "banner warn", hidden: true }, [
      "This dongle still uses the default login (admin / rewired-s). Anyone on your network who knows it can control the inverter: set your own below.",
    ]);
    const result = el("span", { class: "settings-desc muted" }, []);
    const user = this.input("text", { autocomplete: "username", maxlength: "31" });
    const pass = this.input("password", { autocomplete: "new-password", maxlength: "63" });
    const confirm = this.input("password", { autocomplete: "new-password", maxlength: "63" });
    const current = this.input("password", { autocomplete: "current-password", maxlength: "63" });
    const save = el("button", { class: "btn primary", type: "button" }, ["Change login"]) as HTMLButtonElement;
    const body = el("div", { class: "settings-grid" }, [
      this.formRow("Username", user),
      this.formRow("New password", pass, "8–63 characters, no ':'"),
      this.formRow("Repeat new password", confirm),
      this.formRow("Current password", current),
      el("div", { class: "settings-row settings-actions" }, [result, save]),
    ]);
    const card = el("section", { class: "settings-section", hidden: true }, [
      el("h2", { class: "settings-title" }, ["Web login"]),
      el("p", { class: "settings-desc muted" }, [
        "The username and password this page and the native OTA upload ask for. Stored on the dongle; the 30 s button hold resets it.",
      ]),
      banner,
      body,
    ]);
    const load = async () => {
      try {
        const info = await api.webLogin(this.formsAbort.signal);
        if (info.managed !== "runtime") return; // compiled in from secrets: nothing to offer
        card.hidden = false;
        banner.hidden = !info.default;
        if (!user.value) user.value = info.username;
      } catch {
        /* not a pre-built image, or offline: the card stays hidden */
      }
    };
    save.addEventListener("click", async () => {
      const f = { username: user.value, password: pass.value, confirm: confirm.value, current: current.value };
      const problem = validateLogin(f);
      if (problem) {
        SettingsView.showResult(result, problem, false);
        return;
      }
      save.disabled = true;
      SettingsView.showResult(result, "Saving…");
      try {
        const r = await api.webLoginChange({
          username: f.username,
          password: f.password,
          confirm: f.confirm,
          current_password: f.current,
        });
        SettingsView.showResult(result, r);
        if (r.ok) {
          pass.value = confirm.value = current.value = "";
          banner.hidden = true;
          SettingsView.showResult(result, "Login changed. Your browser will ask for the new one on the next request.", true);
        }
      } catch (err) {
        SettingsView.showResult(result, (err as Error).message, false);
      }
      save.disabled = false;
    });
    void load();
    return card;
  }

  // Home Assistant: the MQTT broker the per-device discovery publishes to. Password write-only
  // (blank = unchanged); after a save the status is polled for ~10 s to show the outcome.
  private mqttCard(): HTMLElement {
    const pill = el("span", { class: "pill" }, ["…"]);
    const note = el("span", { class: "settings-desc muted" }, []);
    const result = el("span", { class: "settings-desc muted" }, []);
    const host = this.input("text", { maxlength: "127", placeholder: "192.0.2.20 or broker.lan", autocomplete: "off" });
    const port = this.input("number", { min: "1", max: "65535" });
    const user = this.input("text", { maxlength: "63", autocomplete: "off" });
    const pass = this.input("password", { maxlength: "127", autocomplete: "new-password", placeholder: "unchanged" });
    const clear = el("input", { type: "checkbox" }) as HTMLInputElement;
    const prefix = this.input("text", { maxlength: "31", placeholder: "homeassistant", autocomplete: "off" });
    const tls = el("input", { type: "checkbox" }) as HTMLInputElement;
    const save = el("button", { class: "btn primary", type: "button" }, ["Save"]) as HTMLButtonElement;
    const inputs = [host, port, user, pass, clear, prefix, tls, save];
    const passRow = el("div", { class: "settings-control" }, [
      pass,
      el("label", { class: "toggle" }, [clear, "clear"]),
    ]);
    const body = el("div", { class: "settings-grid" }, [
      this.formRow("Broker host", host, "Leave empty to turn MQTT off"),
      this.formRow("Port", port),
      this.formRow("Username", user),
      el("div", { class: "settings-row settings-form-row" }, [
        el("label", { class: "settings-label" }, [
          el("span", { class: "settings-name" }, ["Password"]),
          el("span", { class: "settings-desc muted" }, ["Never shown; blank keeps the stored one"]),
        ]),
        passRow,
      ]),
      this.formRow("Discovery prefix", prefix),
      this.formRow("TLS", el("label", { class: "toggle" }, [tls, "encrypt (port 8883 usually)"])),
      el("div", { class: "settings-row settings-actions" }, [result, save]),
    ]);
    const card = el("section", { class: "settings-section", hidden: true }, [
      el("div", { class: "console-bar" }, [el("h2", { class: "settings-title" }, ["Home Assistant"]), pill]),
      el("p", { class: "settings-desc muted" }, [
        "Every inverter, battery and meter on the RS485 bus appears in Home Assistant as its own device through MQTT discovery. Point this at the broker your Home Assistant uses (the Mosquitto add-on, usually).",
      ]),
      note,
      body,
    ]);
    const paint = (s: MqttSettingsInfo) => {
      const st = mqttStatus(s);
      pill.className = `pill ${st.cls}`;
      setText(pill, st.text);
      if (s.managed_by_yaml) {
        setText(note, "The broker is set in this dongle's configuration (secrets.yaml); change it there and rebuild.");
        for (const i of inputs) i.disabled = true;
      } else if (!s.available) {
        setText(note, "Runtime MQTT settings are not part of this build.");
        for (const i of inputs) i.disabled = true;
      }
    };
    const fill = (s: MqttSettingsInfo) => {
      const f: MqttForm = mqttFormFrom(s);
      host.value = f.host;
      port.value = f.port;
      user.value = f.username;
      pass.value = "";
      pass.placeholder = s.has_password ? "unchanged (one is stored)" : "none stored";
      clear.checked = false;
      prefix.value = f.prefix;
      tls.checked = f.tls;
    };
    let polls = 0;
    const poll = async () => {
      this.mqttPoll = null;
      try {
        const s = await api.mqttSettings(this.formsAbort.signal);
        paint(s);
        if (polls > 0 && !this.formsAbort.signal.aborted && !s.connected && polls < 10) {
          polls++;
          this.mqttPoll = window.setTimeout(poll, 1000);
        } else {
          polls = 0;
        }
      } catch {
        /* offline; the next visit retries */
      }
    };
    const load = async () => {
      try {
        const s = await api.mqttSettings(this.formsAbort.signal);
        if (!s.available && !s.configured) return; // no sungrow_mqtt at all
        card.hidden = false;
        fill(s);
        paint(s);
      } catch {
        /* keep hidden */
      }
    };
    save.addEventListener("click", async () => {
      const f: MqttForm = {
        host: host.value,
        port: port.value,
        username: user.value,
        password: pass.value,
        clearPassword: clear.checked,
        prefix: prefix.value,
        tls: tls.checked,
      };
      const problem = validateMqtt(f);
      if (problem) {
        SettingsView.showResult(result, problem, false);
        return;
      }
      save.disabled = true;
      SettingsView.showResult(result, "Saving…");
      try {
        const r = await api.mqttSettingsSave(mqttPostFields(f));
        SettingsView.showResult(result, r);
        if (r.ok) {
          pass.value = "";
          clear.checked = false;
          polls = 1; // watch the reconnect for ~10 s
          if (this.mqttPoll !== null) window.clearTimeout(this.mqttPoll);
          this.mqttPoll = window.setTimeout(poll, 1000);
        }
      } catch (err) {
        SettingsView.showResult(result, (err as Error).message, false);
      }
      save.disabled = false;
    });
    void load();
    return card;
  }

  // iSolarCloud identity: the firmware version the dongle reports, where it came from, and the
  // override (a stock version string) or the quick "Use detected" / "Use default" actions.
  private cloudCard(): HTMLElement {
    const value = el("span", { class: "settings-value" }, ["…"]);
    const detected = el("span", { class: "settings-value" }, ["—"]);
    const result = el("span", { class: "settings-desc muted" }, []);
    const override = this.input("text", { maxlength: "39", placeholder: "WINET-SV200.001.00.P046", autocomplete: "off" });
    const useDetected = el("button", { class: "btn", type: "button" }, ["Use detected"]) as HTMLButtonElement;
    const useDefault = el("button", { class: "btn", type: "button" }, ["Use default"]) as HTMLButtonElement;
    const set = el("button", { class: "btn primary", type: "button" }, ["Set"]) as HTMLButtonElement;
    const card = el("section", { class: "settings-section", hidden: true }, [
      el("h2", { class: "settings-title" }, ["iSolarCloud identity"]),
      el("p", { class: "settings-desc muted" }, [
        "iSolarCloud is told a dongle firmware version. It should be the one the dongle had on stock firmware (the stock web UI's About page), so the plant looks unchanged. A pre-built image reads it from the stock app left in the other flash slot.",
      ]),
      el("div", { class: "settings-grid" }, [
        el("div", { class: "settings-row" }, [
          el("div", { class: "settings-label" }, [el("span", { class: "settings-name" }, ["Reported version"])]),
          el("div", { class: "settings-control" }, [value]),
        ]),
        el("div", { class: "settings-row" }, [
          el("div", { class: "settings-label" }, [el("span", { class: "settings-name" }, ["Detected in the other slot"])]),
          el("div", { class: "settings-control" }, [detected, useDetected, useDefault]),
        ]),
        this.formRow("Override", override, "A stock version such as WINET-SV300.001.03.P024; empty clears it"),
        el("div", { class: "settings-row settings-actions" }, [result, set]),
      ]),
    ]);
    let info: CloudVersionInfo | null = null;
    const paint = (c: CloudVersionInfo) => {
      info = c;
      setText(value, cloudVersionText(c));
      setText(detected, c.detected || "none");
      const a = cloudVersionActions(c);
      useDetected.hidden = !a.useDetected;
      useDefault.hidden = !a.useDefault;
      useDefault.title = c.default ? `Report ${c.default}` : "";
      override.value = c.override ?? "";
    };
    const load = async () => {
      try {
        const c = await api.cloudVersion(this.formsAbort.signal);
        if (!c.available) return;
        card.hidden = false;
        paint(c);
      } catch {
        /* keep hidden */
      }
    };
    const post = async (value_: string) => {
      const problem = validateCloudVersion(value_);
      if (problem) {
        SettingsView.showResult(result, problem, false);
        return;
      }
      set.disabled = useDetected.disabled = useDefault.disabled = true;
      try {
        const r = await api.cloudVersionSet(value_);
        SettingsView.showResult(result, r.ok ? `Now reporting ${r.message}.` : verdictText(r), r.ok);
        await load();
      } catch (err) {
        SettingsView.showResult(result, (err as Error).message, false);
      }
      set.disabled = useDetected.disabled = useDefault.disabled = false;
    };
    set.addEventListener("click", () => void post(override.value.trim()));
    useDetected.addEventListener("click", () => {
      const v = info ? cloudVersionOverrideFor(info, "detected") : null;
      if (v !== null) void post(v);
    });
    useDefault.addEventListener("click", () => {
      const v = info ? cloudVersionOverrideFor(info, "default") : null;
      if (v !== null) void post(v);
    });
    void load();
    return card;
  }

  // Boot slots: which slot runs, what the other one holds, and the confirmed switch to it (the
  // way back to the stock app). The confirm dialog stands in for the "Allow boot switch" switch.
  private bootSlotCard(): HTMLElement {
    const running = el("span", { class: "settings-value" }, ["…"]);
    const other = el("span", { class: "settings-value" }, ["…"]);
    const note = el("span", { class: "settings-desc muted" }, []);
    const fallback = el("p", { class: "settings-desc muted", hidden: true }, []);
    const result = el("span", { class: "settings-desc muted" }, []);
    const boot = el("button", { class: "btn", type: "button", disabled: true }, ["Boot other slot"]) as HTMLButtonElement;
    const card = el("section", { class: "settings-section", hidden: true }, [
      el("h2", { class: "settings-title" }, ["Boot slots"]),
      el("p", { class: "settings-desc muted" }, [
        "The flash holds two app slots. After the first install the other slot still has the stock Sungrow app; booting it is the way back to stock, and the stock updater can bring Rewired-S back the same way.",
      ]),
      el("div", { class: "settings-grid" }, [
        el("div", { class: "settings-row" }, [
          el("div", { class: "settings-label" }, [el("span", { class: "settings-name" }, ["Running slot"])]),
          el("div", { class: "settings-control" }, [running]),
        ]),
        el("div", { class: "settings-row" }, [
          el("div", { class: "settings-label" }, [el("span", { class: "settings-name" }, ["Other slot"]), note]),
          el("div", { class: "settings-control" }, [other]),
        ]),
        el("div", { class: "settings-row settings-actions" }, [result, boot]),
      ]),
      fallback,
    ]);
    let info: BootSlotInfo | null = null;
    const load = async () => {
      try {
        const b = await api.bootSlot(this.formsAbort.signal);
        if (!b.running) return;
        info = b;
        card.hidden = false;
        setText(running, slotText(b.running));
        setText(other, slotText(b.other));
        const st = bootSlotState(b);
        setText(note, st.note);
        boot.disabled = !st.enabled;
        fallback.hidden = !b.fallback;
        if (b.fallback) setText(fallback, `Network fallback: ${b.fallback}.`);
      } catch {
        /* keep hidden */
      }
    };
    boot.addEventListener("click", async () => {
      if (!info?.other) return;
      const ok = await confirmDialog(
        "Boot the other slot?",
        [
          el("p", {}, [`The dongle will restart into ${slotText(info.other)}.`]),
          el("p", {}, [
            "If that is the stock app, this web UI, MQTT and Modbus-TCP stop; the stock app's own web UI and the iSolarCloud connection take over. The stock updater (or the stock app's own OTA) can install Rewired-S again.",
          ]),
          el("p", { class: "muted" }, ["Only the boot record is rewritten; nothing else on the flash changes."]),
        ],
        "Boot other slot",
      );
      if (!ok) return;
      boot.disabled = true;
      SettingsView.showResult(result, "Switching…");
      try {
        const { token } = await api.confirm(0, api.BOOT_SLOT_CONFIRM_ADDR);
        const r = await api.bootOtherSlot(token);
        SettingsView.showResult(result, r.ok ? "Boot record switched. Restarting in 3 s…" : verdictText(r), r.ok);
        if (!r.ok) boot.disabled = false;
      } catch (err) {
        SettingsView.showResult(result, (err as Error).message, false);
        boot.disabled = false;
      }
    });
    void load();
    return card;
  }

  // "Send diagnostics": a fixed row at the top of Diagnostics (not an ESPHome entity). The report
  // goes to the developer's collector; the same text can be downloaded for an issue instead.
  private addDiagRow(): void {
    const body = this.sectionBodies.get("diag")!;
    const result = el("span", { class: "settings-desc muted" }, []);
    const note = el("span", { class: "settings-desc muted" }, []);
    const send = el("button", { class: "btn", type: "button", onClick: () => this.sendDiag(result) }, [
      "Send diagnostics",
    ]) as HTMLButtonElement;
    const download = el("a", { class: "btn", href: api.diagReportUrl, download: "rewired-s-diagnostics.txt" }, [
      "Download report",
    ]);
    const label = el("div", { class: "settings-label" }, [
      el("span", { class: "settings-name" }, ["Diagnostics report"]),
      el("span", { class: "settings-desc muted" }, [
        "Send a report to the developer to help with a bug, or download it to attach to an issue.",
      ]),
      note,
      result,
    ]);
    const row = el("div", { class: "settings-row" }, [label, el("div", { class: "settings-control" }, [send, download])]);
    row.dataset.name = ""; // sorts before every entity name, so the row stays first
    body.prepend(row);
    this.sectionCards.get("diag")!.hidden = false;

    const show = (s: DiagSendStatus) => {
      if (s.available && s.telemetry === false)
        setText(note, "Automatic reporting is off in this dongle's configuration; sending is manual only.");
      if (!s.available) {
        send.disabled = true;
        download.setAttribute("aria-disabled", "true");
      }
      const r = diagResultText(s);
      if (r) {
        result.className = r.ok === null ? "settings-desc muted" : r.ok ? "settings-desc ok" : "settings-desc err";
        setText(result, r.text);
      }
      return s.available && s.state === "running";
    };
    const poll = async () => {
      this.diagPoll = null;
      let again = false;
      try {
        again = show(await api.diagStatus(this.diagAbort.signal));
      } catch {
        /* the next status fetch reports it */
      }
      if (again && !this.diagAbort.signal.aborted) this.diagPoll = window.setTimeout(poll, 1000);
    };
    this.diagStatusPoll = poll;
    void poll();
  }

  private diagStatusPoll: (() => Promise<void>) | null = null;

  private async sendDiag(result: HTMLElement): Promise<void> {
    const ok = await confirmDialog(
      "Send diagnostics",
      [
        el("p", {}, ["The report contains:"]),
        el(
          "ul",
          {},
          DIAG_CONTENTS.map((c) => el("li", {}, [c])),
        ),
        el("p", { class: "muted" }, [DIAG_SCRUB_NOTE]),
      ],
      "Send",
    );
    if (!ok) return;
    result.className = "settings-desc muted";
    setText(result, "Sending…");
    try {
      const r = await api.diagSend();
      if (!r.ok) {
        result.className = "settings-desc err";
        setText(result, `Not sent: ${r.message}.`);
        return;
      }
    } catch (err) {
      result.className = "settings-desc err";
      setText(result, (err as Error).message);
      return;
    }
    if (this.diagPoll !== null) window.clearTimeout(this.diagPoll);
    this.diagPoll = window.setTimeout(() => void this.diagStatusPoll?.(), 1000);
  }

  private connect(): void {
    const src = new EventSource("/events");
    this.source = src;
    src.addEventListener("open", () => this.setStatus("connected", "var(--ok)"));
    src.addEventListener("error", () => this.setStatus("reconnecting…", "var(--warn)"));
    src.addEventListener("state", (e) => {
      try {
        const data = JSON.parse((e as MessageEvent).data as string) as Record<string, unknown>;
        const ent = parseState(data);
        if (ent) this.upsert(ent);
      } catch {
        /* ignore malformed event */
      }
    });
  }

  private setStatus(txt: string, color: string): void {
    this.statusPill.textContent = txt;
    this.statusPill.style.color = color;
  }

  private upsert(ent: Entity): void {
    if (HIDDEN_ENTITIES.has(ent.name)) return;
    const existing = this.rows.get(ent.id);
    if (existing) {
      existing.entity = ent;
      this.paint(existing);
      return;
    }
    const row = this.build(ent);
    this.rows.set(ent.id, row);
    const section = sectionFor(ent);
    const body = this.sectionBodies.get(section)!;
    // Pin the write gate first; otherwise insert alphabetically by name for a stable order.
    const sibling = [...body.children].find(
      (c) => (c as HTMLElement).dataset.name! > ent.name && ent.name !== WRITE_SWITCH,
    );
    body.insertBefore(row.root, sibling ?? null);
    this.sectionCards.get(section)!.hidden = false;
    this.paint(row);
  }

  private build(ent: Entity): Row {
    const prominent = ent.name === WRITE_SWITCH;
    const valueEl = el("span", { class: "settings-value" }, []);
    let toggle: HTMLButtonElement | null = null;
    let control: HTMLElement;

    if (ent.domain === "switch") {
      toggle = el("button", {
        class: "toggle-switch",
        type: "button",
        role: "switch",
        onClick: () => this.toggleSwitch(ent),
      }) as HTMLButtonElement;
      toggle.append(el("span", { class: "knob" }, []));
      control = toggle;
    } else if (ent.domain === "button") {
      control = el("button", { class: "btn", type: "button", onClick: () => this.press(ent) }, ["Press"]);
    } else {
      control = valueEl; // read-only sensors show the value as the control slot
    }

    const help = TOOLTIPS[ent.name];
    const label = el("div", { class: "settings-label" }, [
      el("span", { class: "settings-name", title: help ?? ent.name }, [ent.name]),
    ]);
    if (help) label.append(el("span", { class: "settings-desc muted" }, [help]));

    const right =
      ent.domain === "switch" || ent.domain === "button"
        ? el("div", { class: "settings-control" }, ent.domain === "switch" ? [valueEl, control] : [control])
        : el("div", { class: "settings-control" }, [valueEl]);

    const root = el("div", { class: `settings-row ${prominent ? "prominent" : ""}` }, [label, right]);
    root.dataset.name = ent.name;
    return { entity: ent, root, valueEl, toggle };
  }

  private paint(row: Row): void {
    const e = row.entity;
    if (row.toggle) {
      const on = e.on === true;
      row.toggle.setAttribute("aria-checked", on ? "true" : "false");
      row.toggle.classList.toggle("on", on);
      setText(row.valueEl, e.on === null ? "" : on ? "On" : "Off");
    } else if (e.domain === "binary_sensor") {
      setText(row.valueEl, e.on === null ? e.state : e.on ? "Yes" : "No");
    } else if (e.domain !== "button") {
      setText(row.valueEl, e.state || "—");
    }
  }

  private async toggleSwitch(ent: Entity): Promise<void> {
    const row = this.rows.get(ent.id);
    const turnOn = !(row?.entity.on === true);
    try {
      await api.command("switch", ent.objectId, turnOn ? "turn_on" : "turn_off");
      // state confirmation arrives via the SSE 'state' event; no optimistic flip.
    } catch (err) {
      this.flash(row, `failed: ${(err as Error).message}`);
    }
  }

  private async press(ent: Entity): Promise<void> {
    const row = this.rows.get(ent.id);
    try {
      await api.command("button", ent.objectId, "press");
      this.flash(row, "pressed");
    } catch (err) {
      this.flash(row, `failed: ${(err as Error).message}`);
    }
  }

  private flash(row: Row | undefined, msg: string): void {
    if (!row) return;
    setText(row.valueEl, msg);
  }
}

function setText(node: HTMLElement, text: string): void {
  if (node.textContent !== text) node.textContent = text;
}
