import { el } from "../dom.js";
import type { View } from "./types.js";

const MAX_LINES = 500;
// ESPHome log level letter -> class suffix (colour). Format: "[HH:MM:SS][D][tag:line]: msg".
const ANSI = /\x1b\[[0-9;]*m/g;
const LEVEL_RE = /\]\[([EWIDVC])\]/;

// Logs view — live log console fed by ESPHome's web_server /events SSE stream.
// The view owns the EventSource and tears it down in destroy() so leaving the tab closes the socket.
export class DebugView implements View {
  readonly id = "debug";
  readonly label = "Logs";

  private source: EventSource | null = null;
  private consoleEl!: HTMLElement;
  private statusPill!: HTMLElement;
  private paused = false;
  private autoscroll = true;
  private lineCount = 0;

  render(root: HTMLElement): void {
    root.append(el("h1", { class: "view-title" }, ["Logs"]));

    this.statusPill = el("span", { class: "pill" }, ["connecting…"]);
    const pauseBtn = el("button", { class: "btn", onClick: () => this.togglePause(pauseBtn) }, ["Pause"]);
    const clearBtn = el("button", { class: "btn", onClick: () => this.clearConsole() }, ["Clear"]);
    const scrollToggle = el("input", {
      type: "checkbox",
      checked: this.autoscroll,
      onChange: (e: Event) => (this.autoscroll = (e.target as HTMLInputElement).checked),
    });

    root.append(
      el("div", { class: "console-bar" }, [
        this.statusPill,
        pauseBtn,
        clearBtn,
        el("label", { class: "toggle" }, [scrollToggle, "auto-scroll"]),
      ]),
    );

    this.consoleEl = el("div", {
      class: "console",
      role: "log",
      "aria-label": "Live device log",
      "aria-live": "polite",
    });
    root.append(this.consoleEl);
    this.connect();
  }

  destroy(): void {
    this.source?.close();
    this.source = null;
  }

  private connect(): void {
    // EventSource reconnects on its own; we just reflect state in the status pill.
    const src = new EventSource("/events");
    this.source = src;
    src.addEventListener("open", () => this.setStatus("connected", "online"));
    src.addEventListener("error", () => this.setStatus("reconnecting…", "pending"));
    src.addEventListener("log", (e) => this.onLog((e as MessageEvent).data as string));
    // ESPHome also emits "state"/"ping"; we ignore them in the console.
  }

  private setStatus(txt: string, dot: "online" | "pending"): void {
    this.statusPill.textContent = txt;
    this.statusPill.style.color = dot === "online" ? "var(--ok)" : "var(--warn)";
  }

  private onLog(raw: string): void {
    if (this.paused || !raw) return;
    const clean = raw.replace(ANSI, "");
    const m = LEVEL_RE.exec(clean);
    const lvl = m ? m[1] : "";
    const line = el("span", { class: `line lv-${lvl}` }, [clean]);
    this.consoleEl.append(line);
    if (++this.lineCount > MAX_LINES) {
      this.consoleEl.firstChild?.remove();
      this.lineCount--;
    }
    if (this.autoscroll) this.consoleEl.scrollTop = this.consoleEl.scrollHeight;
  }

  private togglePause(btn: HTMLElement): void {
    this.paused = !this.paused;
    btn.textContent = this.paused ? "Resume" : "Pause";
    btn.classList.toggle("primary", this.paused);
  }

  private clearConsole(): void {
    this.consoleEl.replaceChildren();
    this.lineCount = 0;
  }
}
