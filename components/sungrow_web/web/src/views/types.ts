// A view owns a DOM subtree and its own timers/subscriptions. render() builds into `root`
// (already emptied by the router); destroy() must stop every interval / fetch / EventSource.
export interface View {
  readonly id: string;
  readonly label: string;
  /** Routable but hidden from the top-level nav (reached from within another view, e.g. Devices). */
  readonly navHidden?: boolean;
  /** For a navHidden view: which top-level tab id to keep highlighted while it is active. */
  readonly navParent?: string;
  /** `param` is the path segment after the view id (e.g. "1" from #/installer/1 — a device index). */
  render(root: HTMLElement, param?: string): void;
  destroy(): void;
}
