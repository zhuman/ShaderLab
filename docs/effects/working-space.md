# Working Space Integration

The active display profile (live OS-reported caps or any simulated preset / ICC the user has applied) is exposed to graphs through a single first-class node: **`Working Space`** (Parameter category). Effects that operate in a specific color space pull from it via the property-binding system.

- **Single source of truth**: The Working Space node's 14 typed analysis output fields mirror the active profile — `ActiveColorMode` (0=SDR, 1=WCG/ACM, 2=HDR), `HdrSupported`, `HdrUserEnabled`, `WcgSupported`, `WcgUserEnabled`, `IsSimulated`, `SdrWhiteNits`, `PeakNits`, `MinNits`, `MaxFullFrameNits`, plus the four CIE-xy primaries `RedPrimary` / `GreenPrimary` / `BluePrimary` / `WhitePoint` (each Float2).
- **Updated by `MainWindow::UpdateWorkingSpaceNodes()`**, which runs on `ApplyDisplayProfile`, `RevertToLiveDisplay`, the display-change callback, and once per render tick. Only marks the node dirty when at least one field actually changed, so binding consumers re-evaluate on profile changes only.
- **Bind, don't hide**: Effects that need to know the working-space primaries or peak nits expose them as bindable `Float2` / `Float` parameters (typically a `Custom` enum mode that gates them via `visibleWhen`, plus a few static convenience modes like `sRGB` / `BT.2020`). Wire those parameters from the Working Space node to follow the live profile, or set them by hand for strict static analysis. There is no "follow the working space" toggle anymore — wiring the binding **is** the toggle.
- **No legacy `_hidden` filter**: properties ending in `_hidden` used to be host-managed cbuffer slots filtered from the UI. Decision #51 stopped writing them; a Phase-0 cleanup deleted the filter sites. Old graphs still load — the stale keys sit in memory inert. Cross-version graph compatibility is not promised right now.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)