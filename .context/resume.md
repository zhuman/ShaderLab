# ShaderLab — Development Context (Resume Point)

## Project Identity

**ShaderLab** is a WinUI 3 desktop application (C++/WinRT) for developing, testing, and debugging Direct2D shader effects with full HDR and wide color gamut (WCG) support, with a particular focus on tone-mapping and color-correction R&D.

- **Location**: `C:\Users\david\source\repos\ShaderLab\ShaderLab.slnx`
- **Version**: **1.5.0** released; Phase 8 GPU-binding work-in-progress on top (4 commits stacked locally, none pushed).
- **Graph format version**: **2** (unchanged).
- **Engine ABI version**: **1** (`SHADERLAB_ENGINE_ABI_VERSION` in `EngineExport.h`).
- **Language**: C++/WinRT — direct COM access to `ID2D1EffectImpl`, `ID2D1DrawTransform`, `ID2D1ComputeTransform`.
- **Branch / repo state**: `main`, tagged `v1.5.0`. Working tree clean; the 4 stacked commits are Phase 8 prep + cleanup, not yet pushed.

> Authoritative sources of truth: [`docs/`](../docs/README.md) (architecture tree + per-file references) and especially [`docs/history/decision-log.md`](../docs/history/decision-log.md) (**63 entries**), `CHANGELOG.md` (per-version diffs, including `[Unreleased]` for the post-1.5 work), `Version.h` (numeric version), `.github/copilot-instructions.md` (AI agent rules). This file is a fast-orientation summary; it can drift — re-check the docs tree before relying on details.

---

## Solution Layout (4 projects)

| Project | Output | Purpose |
|--------|--------|---------|
| `ShaderLabEngine.vcxproj` | `ShaderLabEngine.dll` | Pure-native engine: graph model, evaluator, ICC reader, video, ExprTk math, D3D11 compute runner, `IEngineComputeOutput` COM interface, MCP HTTP server + 20 engine-pure routes. Exported via `SHADERLAB_API`. |
| `ShaderLab.vcxproj` | `ShaderLab.exe` (MSIX) | WinUI 3 packaged app. `RenderEngine`, all XAML, controllers, `MainWindow.McpRoutes.cpp` (16 UI-coupled routes + JSON-RPC dispatcher + `GuiEngineCommandSink`). Depends on the engine DLL. |
| `ShaderLabTests.vcxproj` | `ShaderLabTests.exe` | Standalone console test runner (`Tests/TestRunner.cpp` + `Tests/Math/*`). 119 tests including HLSL math bench. CI uses `--adapter warp`. |
| `ShaderLabHeadless.vcxproj` | `ShaderLabHeadless.exe` | Console host: PNG render / FP32 pixel readback (`--pixels`) / JSON batch script mode (`--script`). No WinUI dependency. |

This split (decision #41 + #58) keeps WinUI out of the test path, lets engine logic be exercised in isolation, and gives MCP agents a fully-functional logged-out host for parameter sweeps.

---

## Complete Feature Set (v1.5.0 + Phase 8 in-progress)

### Core
- Node-based DAG graph editor for D2D effect composition.
- 40+ wrapped built-in D2D effects (`Effects/EffectRegistry.cpp`) across 9 categories.
- 35 ShaderLab built-in effects (`Effects/ShaderLabEffects.cpp` + `Effects/ColorMath.cpp`).
- Custom pixel shader effects (`ID2D1DrawTransform`).
- Custom D2D compute shader effects (`ID2D1ComputeTransform`, per-tile dispatch).
- Custom **D3D11 compute shader effects** (`D3D11ComputeRunner`) — bypass D2D tiling for full-image reductions with atomics and groupshared memory. The runner now also implements `IEngineComputeOutput` (Phase 8 GPU-binding interface).
- Live HLSL hot-reload with `D3DCompile` + `D3DReflect` auto-property discovery.
- Effect Designer modal window for authoring custom pixel / D2D-compute / D3D11-compute effects with full parameter definition.
- Graph JSON serialization with versioning (format version 2) — saved as `.effectgraph` zip files (DEFLATE via miniz) with optional **embedded media**.

### ShaderLab Built-in Effects (`Effects/ShaderLabEffects.cpp`)

Grouped by `category` + optional `subcategory` (Add Node flyout sub-grouping):

- **Analysis → Highlights**: Luminance Heatmap, Nit Map, Gamut Highlight, Luminance Highlight.
- **Analysis → Scopes**: CIE Histogram (CS), CIE Chromaticity Plot, Vectorscope, Waveform Monitor.
- **Analysis → Comparison**: Delta E Comparator (CIEDE2000), Split Comparison.
- **Analysis → Gamut Mapping**: Gamut Map (Clip / Nearest / Compress / Fit), ICtCp Gamut Map, Gamut Coverage.
- **Analysis → Tone Mapping (ICtCp suite)**: ICtCp Round-Trip Validator, ICtCp Tone Map (HDR → SDR), ICtCp Inverse Tone Map (SDR → HDR), ICtCp Saturation, ICtCp Highlight Desaturation. Bind their numeric peak/SDR-white parameters to the `Working Space` node's analysis outputs to track Display Settings or simulated profiles automatically.
- **Analysis → Statistics** (D3D11 compute, data-only): Channel Statistics, Luminance Statistics, Chromaticity Statistics. The legacy `StatisticsEffect` D2D wrapper class was retired (decision #62) along with its dedicated `/render/image-stats` MCP route + `Rendering::GpuReduction` (decision #63) — agents now use the standard `/graph/add-node` + `/analysis/<id>` workflow against these effects.
- **Source / Generators**: Gamut Source, ICtCp Boundary, Color Checker, Zone Plate, Gradient Generator, HDR Test Pattern.
- **Live capture sources**: DXGI Desktop Duplication (per-output enumerated), Windows Graphics Capture (WinUI picker). Per-frame ticking via `SourceNodeFactory::TickAndUploadLiveCaptures` from `OnRenderTick`.
- **Data / Parameter nodes** (no shader, evaluator-handled): Float, Integer, Toggle, Gamut, Clock, Numeric Expression (ExprTk, A..Z inputs), Random (deterministic seed → [0,1) hash), **Working Space** (mirrors active display profile into 14 typed analysis fields).

Every effect carries a stable `effectId` + numeric `effectVersion`; saved graphs detect upgrades and offer per-node / batch upgrade in the Properties panel.

### Property System
- `PropertyValue` variant: `float`, `int32`, `uint32`, `bool`, `wstring`, `float2`, `float3`, `float4`, `D2D1_MATRIX_5X4_F`, `vector<float>`.
- Per-component property bindings (Grasshopper-style data flow), with array (whole-vector) bindings for LUT-shaped fields.
- Enum labels for named dropdown parameters; `bool` rendered as `ToggleSwitch`.
- No `_hidden` suffix convention (removed in Phase-0 cleanup, v1.4.x). Earlier saved graphs may carry stale `WsRedX_hidden` / `MonMaxNits_hidden` / `SdrWhiteNits_hidden` keys; those load into memory but are inert (no shader cbuffer references them, no UI surfaces them). Cross-version graph compatibility is not currently promised. Sink-only properties (e.g., the Working Space node's `ActiveColorMode`, `SdrWhiteNits`, primaries) live in `ShaderLabEffectDescriptor::hiddenDefaults` without the `_hidden` suffix and are kept off the UI by the customEffect declared-parameter filter.
- `visibleWhen` conditional visibility on parameters (`"Mode == 1"`, `"Strength > 0"`, etc.).
- Visual data pins (orange diamonds) on the node graph for binding connections.

### Rendering
- **Always scRGB FP16 pipeline** (`DXGI_FORMAT_R16G16B16A16_FLOAT`, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`). DWM/ACM handles final display conversion.
- **Refresh-rate-driven render loop** (60–240 Hz) — interval re-derived from `EnumDisplaySettings(dmDisplayFrequency)` on every display change.
- Dirty-gated render loop with **dirty propagation pre-pass** (any dirty node marks its direct downstream consumers dirty before evaluation; runs again after `TickAndUploadVideos` so video updates flow through analysis-only compute nodes too).
- No built-in tone-mapping pass in the render path — users build tone mappers as graph effects (the ICtCp suite is the preferred path). Decision-log entry #54 retired the legacy `Rendering/ToneMapper` class in the Phase-1 cleanup.
- Display profile mocking (presets + ICC file loading via `mscms.dll`).
- Monitor gamut detection from `DXGI_OUTPUT_DESC1` primaries.
- **OS-reported SDR white level** queried via `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)`, tracks the *Settings → Display → HDR → "SDR content brightness"* slider; exposed to graphs as `working_space.SdrWhiteNits`. Effects pull the value via property bindings — no per-frame host injection.
- GPU info display (hardware adapter name or "Software (WARP)").
- **DXVA2 / Media Foundation video sources** with `ID3D10Multithread::SetMultithreadProtected(TRUE)` so background-thread Lock2D from the decoder doesn't crash D3D11.
- `OutputWindow` system: each `Output` node gets its own OS window with independent SwapChainPanel, pan/zoom, save-to-file. Bidirectional sync (close window ↔ delete node).
- D2D-rendered node graph canvas with pan/zoom, bezier edges (Alt+click delete via bezier hit-test), color-coded nodes, dot grid, dark theme.

### MCP Server (Phase 7 architecture, post-1.5.0)
JSON-RPC 2.0 over Streamable HTTP (`POST /`). Implements `Content-Length` **and** `Transfer-Encoding: chunked`. Has `GET /` health-check + correct `202 Accepted` for `notifications/*`. Toolbar shows an amber activity dot when the server has handled a request in the last few seconds.

- **GUI host**: port **47808** (auto-increments).
- **Headless host (`ShaderLabHeadless --script`)**: port **47809** (different default to avoid shared-machine conflicts).

The server itself + 20 engine-pure routes live in `Engine/Mcp/{McpHttpServer,EngineMcpRoutes}.{h,cpp}`; 16 UI-coupled / host-specific routes stay in `MainWindow.McpRoutes.cpp`. Both hosts register the same engine-side route set through the same `IEngineCommandSink` interface — routes call `sink.Dispatch(closure)` where the closure receives a fresh `EngineContext`. After successful mutation, closures fire one of 8 event hooks (`OnNodeAdded`, `OnNodeRemoved`, `OnNodeChanged`, `OnGraphCleared`, `OnGraphLoaded`, `OnGraphStructureChanged`, `OnCustomEffectRecompiled`, `OnDisplayProfileChanged`); the GUI overrides each hook to call the same UI methods native interactions use, so MCP-driven mutations are indistinguishable from native UI interactions at the host level.

**Engine-pure routes** (in `Engine/Mcp/EngineMcpRoutes.cpp`): `/registry`, `/effect/hlsl/<id>`, `/effect/compile`, `/graph/add-node`, `/graph/remove-node`, `/graph/connect`, `/graph/disconnect`, `/graph/set-property`, `/graph/load`, `/graph/clear`, `/graph/bind-property`, `/graph/unbind-property`, `GET /graph` (incl. `/graph/save`, `/graph/node/<id>`), `/custom-effects`, `/analysis/<id>`, `/render/pixel-region`, `/render/capture-node`, `/display/profiles`, `/display/profile`, `/display/profile/clear`. The previously-engine `/render/image-stats` was retired in decision #63.

**App-side routes** (in `MainWindow.McpRoutes.cpp`): UI-coupled (`/graph/snapshot`, `/graph/view*`, `/preview/view*`, `/render/preview-node`, `/render/capture`, `/render/pixel-trace`, `/render/pixel/<x>/<y>`) and host-specific (`/`, `POST /` JSON-RPC dispatcher, `/context`, `/perf`, `/node/<id>/logs`).

The **Working Space** parameter node — a strict sink with no input pins — mirrors the active display profile (live or any simulated preset/ICC) into 14 typed analysis output fields (`ActiveColorMode`, `Hdr/WcgSupported`/`UserEnabled`, `IsSimulated`, `SdrWhiteNits`, `PeakNits`, `MinNits`, `MaxFullFrameNits`, plus four CIE-xy primaries as Float2). Bind any downstream property to drive an effect from the live working space — e.g. wire a tone-mapper's peak-nits to `working_space.PeakNits` and it tracks Display Settings or simulated profile changes automatically. Updated by `Rendering::UpdateWorkingSpaceNodes` (engine helper, called from `MainWindow::UpdateWorkingSpaceNodes` shim and the engine display-profile MCP routes).

GUI MCP routes that mutate engine state run through `MainWindow::DispatchSync` to marshal to the UI thread; engine-side route bodies execute inside the closure passed to `IEngineCommandSink::Dispatch`.

### Effect Designer
- Three shader types: pixel (`ps_5_0`), D2D compute (`cs_5_0`), **D3D11 compute** (`cs_5_0`, host-dispatched).
- Parameter types: float, float2, float3, float4, int, uint, bool, enum.
- Enum parameters with comma-separated label definition → ComboBox.
- Bool parameters render as `ToggleSwitch`.
- Analysis output fields with typed declarations (Float, Float2, Float3, Float4, FloatArray, Float2/3/4Array).
- HLSL auto-formatting and scaffold generation per shader type (D3D11 scaffold injects auto `Width`/`Height` cbuffer + stride-reduction template).
- "Edit in Effect Designer" opens any built-in effect for inspection / fork. `LoadDefinition` correctly restores Output Type selector + analysis-field rows.
- Add to Graph / Update in Graph buttons.

### Versioning
- `Version.h`: App **1.5.0**, Graph format version **2**, plus `LibraryVersion()` (sum of all effect versions).
- `EngineExport.h::SHADERLAB_ENGINE_ABI_VERSION` = **1** (independent of app version; bumped manually on engine ABI breaks; mismatch between header and DLL aborts startup with a friendly message-box).
- Status bar shows pipeline / display / FPS; **title bar** shows app version + library version.
- Saved graphs include `formatVersion` + `appVersion`; loading newer-format graphs shows an error dialog. Per-effect `effectId`/`effectVersion` round-trip and surface upgrade prompts.

### UI / UX
- Segoe Fluent Icons toolbar with tooltips.
- `.effectgraph` file-type association (FTA) + Ctrl+S accelerators + unsaved-changes guard + async save/load with progress dialog.
- Auto-arrange resets viewport so off-screen graphs come back into view.
- New nodes spawn at the **center of the current viewport** (graph coords, accounting for pan/zoom).
- Closing an `OutputWindow` forces a single render pass so the deleted Output node disappears immediately.

---

## D2D Custom Effect Gotchas (Hard-Won Knowledge)

These are critical lessons learned during development. Any AI agent or developer working on custom D2D effects **must** be aware of these:

1. **Typed cbuffer pack (Phase 3+)**: `uint`, `int`, and `bool` cbuffer slots in HLSL are now packed correctly even when the corresponding `PropertyValue` is stored as `float` (the default for enum-style parameters). The `Effects::PackPropertyToCBuffer` helper reflects each cbuffer variable's `D3D_SHADER_VARIABLE_TYPE` and converts via `static_cast<uint32_t>` / `<int32_t>` / `BOOL` before writing. So you *can* declare `uint Mode` in HLSL and use clean `if (Mode == 1)` comparisons. **Pre-Phase-3 historical convention** (still works, used by all existing ShaderLab effects): declare enums as `float` in HLSL with `> 0.5` / `> 1.5` threshold comparisons.
2. **HLSL compiler optimizes out cbuffer variables** not referenced on ALL code paths when `D3DCOMPILE_WARNINGS_ARE_ERRORS` is set. Read all cbuffer vars at top of `main()` before any branches.
3. **D2D custom effects need TWO evaluation passes** for newly created effects — first creates/initializes, second produces correct output. The evaluator handles this with `m_justCreated` deferring analysis readback by one frame.
4. **`RegisterWithInputCount` requires `inputCount >= 1`**. Zero-input source effects use a hidden dummy 1×1 bitmap input.
5. **`MapInputRectsToOutputRect` with `SetFixedOutputSize`** must check fixed size FIRST, before input rect.
6. **D2D `TEXCOORD` values are in pixel/scene space**, NOT normalized [0,1]. Use `GetDimensions()` and divide, or call `Source.Load(int3(uv, 0))` directly.
7. **D2D custom effect transforms must NOT pass through infinite input rects** in `MapInputRectsToOutputRect`. Store the requested output rect from `MapOutputRectToInputRects` and return it.
8. **`ForceUploadConstantBuffer()` uploads cbuffer but doesn't invalidate cached output**. Need input toggle trick (disconnect+reconnect dummy input) to force re-evaluation.
9. **Variable-input D2D custom effects** (`<Inputs minimum='0' maximum='8'/>`) require BOTH `ID2D1Effect::SetInputCount(N)` (external) AND updating the transform node's internal count. Without the external call, `SetInput()` fails with `E_INVALIDARG`.
10. **Monitor gamut from `DXGI_OUTPUT_DESC1` primaries** (`RedPrimary`, `GreenPrimary`, `BluePrimary`, `WhitePoint`). Always write primaries into the cbuffer on every evaluate (correct on first frame), only mark dirty on actual change (prevents feedback loops).
11. **D2D → D3D11 texture handoff requires `dc->Flush()`** between `DrawImage` and any D3D11 read of the underlying texture. D2D batches commands until `EndDraw()` or `Flush()` — without an explicit flush, D3D11 reads zeros. Applied in `DispatchUserD3D11Compute`.
12. **`ProcessDeferredCompute` requires an active D2D draw session** (decision #63). It calls `dc->DrawImage` internally to pre-render the upstream chain into an FP32 bitmap, and outside `BeginDraw`/`EndDraw` that DrawImage silently no-ops — the compute reads black input and emits Min/Max/Mean = 0. The GUI's `RenderFrame`, the headless host's `runEval` / `RunRender`, and the test bench all wrap accordingly.
13. **D3D11 compute output → D2D bitmap interop**: `CreateBitmapFromDxgiSurface` must set `bp.dpiX/dpiY = 96.0f`. Default 0 DPI causes `GetImageLocalBounds` to return zero-size bounds.
14. **D3D11 multithread protection** (`ID3D10Multithread::SetMultithreadProtected(TRUE)`) must be enabled when using DXVA2 video decode on background threads with `Lock2D` on GPU buffers.
15. **D3D11 compute cbuffers**: when HLSL declares `uint`/`int`/`bool` but the property is stored as `float`, the pack code must reflect the declared `D3D_SHADER_VARIABLE_TYPE` and `static_cast` to the right type before writing — raw `memcpy` of a float bit-pattern produces nonsense ints/uints.

---

## Build / Deploy / Launch

### Prerequisites
- Visual Studio 2022 17.8+ **or** VS 2026 Insiders (C++ Desktop + UWP workloads).
- Windows App SDK 1.8.
- Windows 10 SDK 10.0.26100+.
- PowerShell 5.1+.
- Internet on first build (for `EnsureExprTk.ps1` + `EnsureMiniz.ps1`).

### Build
```pwsh
# Via Visual Studio
Open ShaderLab.slnx → Build → Debug | x64

# Via MSBuild
msbuild ShaderLab.slnx /p:Configuration=Debug /p:Platform=x64
```

Pre-build scripts run automatically on first build:
- `scripts\EnsureDevCert.ps1` — generates / installs the local F5 dev cert (`CN=ShaderLab`).
- `scripts\EnsureExprTk.ps1` — downloads `exprtk.hpp` (MIT) into `third_party\exprtk\`.
- `scripts\EnsureMiniz.ps1` — downloads `miniz` (MIT) for `.effectgraph` zip DEFLATE.

NuGet packages restore automatically (packages.config style).

### Configurations
- `Debug | x64`, `Release | x64`, `Debug | ARM64`, `Release | ARM64`.

### Deploy (local F5)
```pwsh
Add-AppxPackage -Register "x64\Debug\ShaderLab\AppxManifest.xml"
```
**Never deploy from `AppX\`** — it accumulates stale artifacts that cause XAML 0xc000027b crashes. Always deploy from `x64\Debug\ShaderLab\AppxManifest.xml`. After building, close existing running instances (`Stop-Process`) before redeploying.

### Releases
GitHub Actions `release.yml` runs as a matrix (x64, ARM64). Just before MSBuild, the workflow injects the unsigned-namespace OID into `Package.appxmanifest`'s `Publisher` so the resulting MSIX is installable via `Add-AppxPackage -AllowUnsigned`. The in-repo manifest stays plain `CN=ShaderLab` so signed F5 deploys keep working. End-user `Install.ps1` detects host arch, installs bundled VCLibs / WindowsAppRuntime dependency MSIXes, then ShaderLab.

### Linked Libraries
`d3d11.lib`, `d2d1.lib`, `dxgi.lib`, `d3dcompiler.lib`, `dxguid.lib`, `windowscodecs.lib`, `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib`, `mscms.lib`.

### CI
`.github/workflows/ci.yml` builds Debug+Release x64 and runs `ShaderLabTests.exe --adapter warp`. Tests include graph DAG / topo sort / cycle detection, JSON round-trip, all ShaderLab effects compile-and-evaluate (analysis + source + tone-mapping), property bindings propagation, Numeric Expression input/output round-trip, Clock node, and three-node chain integration.

---

## Project Structure

```
ShaderLab\
├── ShaderLab.slnx                  # Solution
├── ShaderLab.vcxproj               # WinUI 3 packaged app (MSIX)
├── ShaderLabEngine.vcxproj         # Engine DLL (shared by app + tests)
├── ShaderLabTests.vcxproj          # Console test runner
├── packages.config                 # NuGet manifest
├── Package.appxmanifest            # MSIX identity (plain CN=ShaderLab)
├── app.manifest                    # DPI awareness, heap type
├── EngineExport.h / .cpp           # SHADERLAB_API + ABI version + ShaderLab_GetAbiVersion C export
├── Version.h                       # App 1.5.0, graph format 2
├── README.md                       # Slim repo intro + pointer to docs/
├── docs/                           # Architecture tree (architecture / effects / ui-ux / hosts / development / history)
├── docs/effects/new-effect-defaults.md  # D2D effect default-property reference
├── CHANGELOG.md                    # Version history
├── Bootstrap.ps1                   # One-command fresh-clone setup
│
├── pch.h / pch.cpp                 # App PCH
├── pch_engine.h / pch_engine.cpp   # Engine + Test + Headless PCH
├── App.xaml / .h / .cpp            # WinUI 3 entry point
├── MainWindow.xaml / .h / .cpp     # Main window (~4700 lines after Phase 4 split)
├── MainWindow.WorkingSpace.cpp     # Display-profile selection + UpdateWorkingSpaceNodes shim
├── MainWindow.GraphFileIo.cpp     # Save/load + miniz embedded media + heartbeat reaper
├── MainWindow.RenderTick.cpp       # OnRenderTick / RenderFrame / dirty propagation
├── MainWindow.McpRoutes.cpp       # 16 UI-coupled MCP routes + GuiEngineCommandSink + JSON-RPC dispatcher
├── EffectDesignerWindow.*          # Effect Designer modal window
│
├── Tests\
│   ├── TestRunner.cpp              # Standalone test entry point (119 tests)
│   ├── ShaderTestBench.{h,cpp}     # D3D11 compute test harness
│   ├── Math\                       # 51 HLSL math tests across 5 categories
│   ├── TestCommon.h                # Shared TEST() macro
│   ├── RunTests.ps1 / RunMathTests.ps1 / RunHeadlessSmoke.ps1 / RunCliTests.ps1
│   └── fixtures\test_cli_basic.json # Golden graph for headless smoke
│
├── ShaderLabHeadless\
│   └── Main.cpp                    # Console host: PNG render / --pixels / --script
│
├── Engine\Mcp\                     # (engine) MCP server + engine-pure routes
│   ├── McpHttpServer.{h,cpp}       # Winsock2 + HTTP + chunked transport
│   └── EngineMcpRoutes.{h,cpp}     # 20 engine-pure routes + IEngineCommandSink + EngineContext
│
├── Graph\                          # (engine) DAG data model
│   ├── NodeType.h / PropertyValue.h
│   ├── EffectNode.h / EffectEdge.h
│   └── EffectGraph.h / .cpp        # DAG, topo sort, JSON, bindings, versioning
│
├── Rendering\                      # (engine) eval + display + math
│   ├── RenderEngine.h / .cpp       # (app-only) D3D11 + D2D1 + swap chain
│   ├── GraphEvaluator.h / .cpp     # Topological eval, dirty propagation, deferred D3D11 compute
│   ├── FalseColorOverlay.h / .cpp  # False color rendering overlay
│   ├── DisplayMonitor.h / .cpp     # HDR/SDR detection, primaries, OS SDR white, jthread
│   ├── DisplayProfile.h            # Profile structs, presets
│   ├── DisplayInfo.h               # DisplayCapabilities + monitor primaries
│   ├── PipelineFormat.h            # scRGB FP16 (always)
│   ├── IccProfileParser.h / .cpp   # mscms.dll-based ICC reader
│   ├── D3D11ComputeRunner.{h,cpp}  # Generic D3D11 compute dispatcher; implements IEngineComputeOutput
│   ├── PixelReadback.{h,cpp}       # FP32 RGBA region readback helper
│   ├── CaptureNode.{h,cpp}         # D2D + WIC PNG encode of any node
│   ├── WorkingSpaceSync.{h,cpp}    # Working Space parameter node refresh
│   ├── EffectGraphFile.{h,cpp}     # .effectgraph zip (miniz) + embedded media
│   └── MathExpression.{h,cpp}      # ExprTk evaluator (PCH disabled, math-only flags)
│
├── Effects\                        # (engine) effect catalog + custom effect base
│   ├── ShaderLabEffects.{h,cpp}    # 35 ShaderLab effects with embedded HLSL
│   ├── ColorMath.cpp               # Shared HLSL color math library
│   ├── EffectRegistry.{h,cpp}      # 40+ wrapped D2D effect catalog
│   ├── IEngineComputeOutput.h      # Phase 8 COM interface for GPU-resident analysis
│   ├── ShaderLabParamsHlsl.{h,cpp} # Engine-embedded shaderlab_params.hlsli macro library
│   ├── CustomPixelShaderEffect.*   # ID2D1DrawTransform implementation
│   ├── CustomComputeShaderEffect.* # ID2D1ComputeTransform implementation
│   ├── ShaderCompiler.{h,cpp}      # D3DCompile + D3DReflect wrapper + ID3DInclude resolver
│   ├── ImageLoader.{h,cpp}         # WIC HDR/SDR image loading
│   ├── VideoSourceProvider.{h,cpp} # MF video decoding → D2D bitmaps
│   ├── DxgiDuplicationSourceProvider.{h,cpp}        # Live DXGI Desktop Duplication capture
│   ├── WindowsGraphicsCaptureSourceProvider.{h,cpp} # WinUI graphics-capture picker
│   ├── SourceNodeFactory.{h,cpp}   # Source node creation + per-frame live-capture tick
│   └── PropertyMetadata.h          # Effect property metadata
│
├── Controls\                       # (app) editor controllers
│   ├── NodeGraphController.*       # D2D canvas node graph editor
│   ├── ShaderEditorController.*    # Live HLSL compile controller
│   ├── PixelInspectorController.*  # GPU readback pixel inspection
│   ├── PixelTraceController.*      # Recursive pixel trace through graph
│   ├── OutputWindow.*              # Per-Output-node OS window
│   ├── LogWindow.*                 # Log viewer
│   └── NodeLog.h                   # Per-node log entry types
│
├── third_party\
│   └── exprtk\                     # exprtk.hpp (downloaded, gitignored)
│
├── scripts\
│   ├── EnsureDevCert.ps1
│   ├── EnsureExprTk.ps1
│   ├── EnsureMiniz.ps1
│   └── Install.ps1                 # Per-arch unsigned-MSIX installer
│
├── .github\
│   ├── workflows\
│   │   ├── ci.yml                  # Build + tests + bootstrap-smoke on every push / PR
│   │   └── release.yml             # x64 + ARM64 matrix, OID injection
│   └── copilot-instructions.md     # AI agent rules
└── .context\
    └── resume.md                   # This file
```

---

## Active Development Focus

**Phase 8 — GPU-binding for analysis chains** (v1.6 work-in-progress, 4 commits stacked locally). Goal: eliminate the `Map()` round-trip when an upstream compute analysis effect's output feeds a downstream effect's parameter via the data-pin binding system. Today every analysis field is read back to CPU, written into a `PropertyValue`, packed into a cbuffer, and uploaded — pointless GPU→CPU→GPU on integrated GPUs (~1ms stall per analysis node per frame). The architecture lands incrementally:

1. **Foundation** (✅ committed): `IEngineComputeOutput` COM interface + `gpuBindable` / `gpuPublish` data-model flags + `shaderlab_params.hlsli` engine-embedded macro library + `ShaderCompiler` macro/include support + `D3D11ComputeRunner` becomes a no-op-refcounted COM impl with a cached SRV.
2. **Bridge effect** (in-progress, `p8-bridge-effect`): generalize the retired StatisticsEffect pattern so D3D11 compute custom effects are wrapped in a D2D effect (`CustomComputeBridgeEffect`) — `node->cachedEffect` non-null, single discovery channel via QI on cachedEffect, the special-case branch at `GraphEvaluator.cpp` line 146-194 collapses into `CreateOrGetEffect`.
3. **Bytecode cache** (`p8-cache-mem` then `p8-cache-disk` then `p8-cache-reaper`): variant precompile keyed on `(effectId, version, sourceHash, macroBitset)`. N+1 eager shapes per insert, lazy multi-bind variants, on-disk persistence at `%LOCALAPPDATA%\ShaderLab\bytecode\`, version/source-drift reaper.
4. **Evaluator QI hookup** (`p8-evaluator-qi`): for each property binding, QI upstream effect for `IEngineComputeOutput`. If supported AND consumer parameter is gpuBindable, bind the SRV directly into the consumer's `t`-slot, skip CPU readback for that field. Behind `ShaderLab::Performance::EnableGpuBindings` feature flag.
5. **Migrate first-class effects** (`p8-migrate-ictcp` then more): mark `TargetPeakNits` / `SourcePeakNits` etc. on the ICtCp suite as `gpuBindable`, wrap their HLSL with `SHADERLAB_PARAM` / `SHADERLAB_LOAD_PARAM` macros.
6. **Disk-cleanup status-bar button**: unified broom button in the bottom-left status bar that runs both reapers (orphan graph media + bytecode-cache version drift) and reports freed bytes.

The thesis driving recent **product** work (ICtCp tone mapping, dE fidelity loop) carries through: I (intensity) is decoupled from Ct/Cp (chromaticity), so manipulating I alone preserves hue and saturation by construction; the empirical fidelity loop (`Working Space` + `Delta E Comparator` Grayscale dE + `Luminance Statistics` live readout) lets us tune effect parameters against measured CIEDE2000 color difference rather than visual impression. Phase 8 is engine perf work that unblocks running that loop fast enough on lower-powered hardware.

---

## Potential Future Work

- **More tone-mapping operators** in the ICtCp subcategory (BT.2390, hue-preserving ACES, adaptive).
- **Auto-bind affordances** so SDR-white / monitor-peak hidden defaults can be wired from any matching upstream output without manual binding.
- **Effect Designer export** — emit standalone C++ header / module files for D3D11 compute effects so teams can fork them into their own codebases.
- **External binary import** — load pre-compiled D2D effect DLLs (`ID2D1EffectImpl`) and `.cso` compute binaries directly into the graph.
- **Multi-dispatch GPU reduction pyramid** for images > ~33 MP (current `D3D11ComputeRunner` dispatches a single 1024-thread group).
- **Hide `Prim*` data pins from OOG-style nodes** — host-managed hidden properties should never surface as connectable orange diamonds.
