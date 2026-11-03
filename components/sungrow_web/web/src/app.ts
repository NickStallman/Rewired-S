import "./app.css";
import { clear, el } from "./dom.js";
import { CaptureView } from "./views/capture.js";
import { DashboardView } from "./views/dashboard.js";
import { DebugView } from "./views/debug.js";
import { DevicesView } from "./views/devices.js";
import { InstallerView } from "./views/installer.js";
import { IvView } from "./views/iv.js";
import { SettingsView } from "./views/settings.js";
import { SetupView } from "./views/setup.js";
import { WaveView } from "./views/wave.js";
import type { View } from "./views/types.js";

// Single-page shell for the Sungrow dongle. Framework-free; hash-routed between a small set
// of views. Each view owns its own polling/subscriptions and is destroyed on navigation.

class App {
  private views: View[] = [
    new DashboardView(),
    new DevicesView(),
    new SettingsView(),
    new InstallerView(),
    new SetupView(),
    new CaptureView(),
    new WaveView(),
    new IvView(),
    new DebugView(),
  ];
  private current: View | null = null;
  private currentParam: string | undefined;
  private outlet!: HTMLElement;
  private nav!: HTMLElement;

  mount(appRoot: HTMLElement): void {
    clear(appRoot);
    appRoot.removeAttribute("aria-busy");

    appRoot.append(
      el("div", { class: "app-shell" }, [
        el("header", { class: "topbar" }, [
          el("span", { class: "brand" }, ["Rewired-S", el("small", {}, ["local control"])]),
          el("span", { class: "spacer" }, []),
          el("span", { class: "pill" }, ["sungrow_web"]),
        ]),
        (this.nav = el("nav", { class: "tabs", "aria-label": "Sections" }, [])),
        (this.outlet = el("main", { id: "outlet" }, [])),
      ]),
    );

    for (const v of this.views) {
      if (v.navHidden) continue; // routable (#/id) but not a top-level tab (e.g. Setup/Installer, reached from Devices)
      this.nav.append(
        el("button", { type: "button", "data-view": v.id, onClick: () => this.go(v.id) }, [v.label]),
      );
    }

    window.addEventListener("hashchange", () => this.route());
    this.route();
  }

  private route(): void {
    const raw = location.hash.replace(/^#\/?/, "");
    const [id, ...rest] = raw.split("/");
    const vid = id || this.views[0].id;
    const view = this.views.find((v) => v.id === vid) ?? this.views[0];
    this.activate(view, rest.join("/") || undefined);
  }

  private go(id: string): void {
    if (location.hash === `#/${id}`) {
      this.route(); // same hash: re-activate directly
    } else {
      location.hash = `#/${id}`;
    }
  }

  private activate(view: View, param?: string): void {
    if (this.current === view && this.currentParam === param) return;
    this.current?.destroy();
    this.current = view;
    this.currentParam = param;
    clear(this.outlet);
    const activeId = view.navParent ?? view.id; // keep the parent tab lit for nested views
    for (const btn of this.nav.querySelectorAll<HTMLButtonElement>("button[data-view]")) {
      const active = btn.dataset.view === activeId;
      if (active) btn.setAttribute("aria-current", "page");
      else btn.removeAttribute("aria-current");
    }
    view.render(this.outlet, param);
  }
}

function boot(): void {
  const root = document.getElementById("app");
  if (root) new App().mount(root);
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", boot);
} else {
  boot();
}
