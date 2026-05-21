# ShaderLab Documentation

The repo root [README.md](../README.md) covers identity, install, and getting-started. Everything below is the deeper technical reference, organized by audience.

> **Looking for the changelog?** [`CHANGELOG.md`](../CHANGELOG.md) at the repo root tracks every version. The [decision log](history/decision-log.md) below tracks architectural choices with rationale (independent of release boundaries).

---

## Architecture

Reference for "how does ShaderLab work under the hood".

- [Architecture Overview](architecture/overview.md) — high-level diagram of the components (rendering, graph, effects, UI controllers, hosts).
- [Threading Model](architecture/threading-model.md) — UI thread vs render worker, offscreen-blit composition, dispatcher, output-window sinks.
- [Pipeline Format Strategy](architecture/pipeline-format.md) — why the pipeline is always scRGB FP16 and how DWM/ACM handles the final display conversion.
- [Effect Graph Model](architecture/effect-graph-model.md) — `EffectGraph` / `EffectNode` / `EffectEdge` / `PropertyValue`, JSON serialization, dirty tracking.
- [Topological Evaluation](architecture/topological-evaluation.md) — Kahn's algorithm, evaluation order, cycle detection.
- [Display Monitoring](architecture/display-monitoring.md) — DXGI adapter-change events, `WM_DISPLAYCHANGE`, ICC profile parsing, SDR white level.
- [Display Profile Mocking](architecture/display-profile-mocking.md) — simulated SDR/HDR/WCG environments and the testing harness.
- [Compute Shader Analysis Pipeline](architecture/compute-analysis-pipeline.md) — D2D compute conventions, CPU readback, analysis output schema.
- [D2D / D3D11 Hybrid Compute System](architecture/d2d-d3d11-hybrid-compute.md) — `CustomComputeBridgeEffect`, `D3D11ComputeRunner`, GPU-binding routing, COM class hierarchy.
- [Engine / Host Split](architecture/engine-host-split.md) — `ShaderLabEngine.dll` ABI, `IEngineCommandSink`, host event hooks.

## Effects

Reference for the effect catalog and per-effect mechanics.

- [Built-in Effect Catalog](effects/builtin-catalog.md) — the ~35 ShaderLab effects (Analysis, Color, Source, Tone Mapping, Parameter).
- [Effect Versioning System](effects/effect-versioning.md) — how `effectVersion` bumps are detected on graph load.
- [Effect Designer](effects/effect-designer.md) — the modal window for authoring custom pixel/compute shaders.
- [Numeric Expression Node (ExprTk)](effects/numeric-expression.md) — single-input math expression parameter node.
- [Parameter Nodes](effects/parameter-nodes.md) — Float / Integer / Toggle / Gamut Parameter and Clock.
- [Property Bindings (Data Pins)](effects/property-bindings.md) — wiring analysis fields to downstream parameters.
- [Working Space Integration](effects/working-space.md) — host-driven node mirroring the active display profile.
- [New Effect Defaults](effects/new-effect-defaults.md) — default property values for every newly added D2D effect.

## UI / UX

- [Graph Editor UX](ui-ux/graph-editor.md) — keyboard / mouse shortcuts, color coding, inline data display.
- [Multi-Output Windows](ui-ux/multi-output-windows.md) — the secondary preview windows.
- [Animation System](ui-ux/animation-system.md) — Clock-driven dirty propagation.
- [Conditional Parameter Visibility](ui-ux/conditional-parameter-visibility.md) — `visibleWhen` expressions on parameters.

## Hosts

- [ShaderLabHeadless (Console Host)](hosts/headless.md) — the no-UI host for CI / batch scripting.
- [MCP Server (AI Agent Integration)](hosts/mcp-server.md) — JSON-RPC 2.0 server, tool catalog, route distribution.

## Development

- [Build Instructions](development/build.md) — prerequisites, configurations, dependency map.
- [Project Structure](development/project-structure.md) — full file tree with per-file descriptions.

## History

- [Decision Log](history/decision-log.md) — chronological architectural decisions with rationale (60+ entries).

---

Back to [Repo root](../README.md).
