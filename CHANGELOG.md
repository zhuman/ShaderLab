# Changelog

All notable changes to ShaderLab will be documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

## [1.6.3] - 2026-05-08

### Fixed

- **CI Bootstrap.ps1: package restore was a silent no-op on packages.config.** `msbuild /t:Restore` against `ShaderLab.slnx` reports "Nothing to do. None of the projects specified contain packages to restore." for packages.config-style references — it only handles PackageReference by default. The downstream build then failed because `Microsoft.Windows.CppWinRT.props` (from a missing package) couldn't be imported. Switched Bootstrap.ps1 to call `nuget.exe restore <packages.config> -PackagesDirectory packages` directly when nuget.exe is on PATH (which it is in the CI smoke job after `Setup NuGet`); falls back to `msbuild /t:Restore /p:RestorePackagesConfig=true` when not. Also fixes the same shape of bug on `EnsureExprTk.ps1`'s missing `TargetDir` arg (folded into 1.6.2 conceptually but only manifested after the cert fix unblocked CI past step 1).

## [1.6.2] - 2026-05-08

### Fixed

- **CI Bootstrap.ps1 smoke job: missing `TargetDir` arg on `EnsureExprTk.ps1`.** Same shape as the 1.6.1 cert fix — `EnsureExprTk.ps1` declares `TargetDir` as a mandatory parameter; the vcxproj target supplies `$(ProjectDir)third_party\exprtk` but Bootstrap.ps1 was calling it bare. Now matches the vcxproj invocation.

## [1.6.1] - 2026-05-08

### Fixed

- **CI Bootstrap.ps1 smoke job failed on clean clone.** `Bootstrap.ps1` invoked `scripts\EnsureDevCert.ps1` without arguments, but that script declares `PfxPath` and `Password` as mandatory parameters. The local F5 path worked because the vcxproj's `EnsureDevSigningCertificate` MSBuild target supplies them via `$(MSBuildProjectDirectory)\$(PackageCertificateKeyFile)` and `$(PackageCertificatePassword)`; the bootstrap smoke job (which calls Bootstrap.ps1 directly on a fresh clone) didn't. Matched the vcxproj invocation: PFX at `$Repo\ShaderLab_TemporaryKey.pfx` + the public default password `shaderlab`.

## [1.6.0] - 2026-05-08

The "Phase 8 GPU-binding" release. Compute outputs now route directly to downstream compute consumers as SRVs (no CPU readback round-trip), with a per-frame skip-readback policy that avoids `Map()` calls when no consumer needs CPU values. Disk-persistent bytecode cache, eager precompile of GPU-binding variants, and a new `CustomComputeBridgeEffect` D2D wrapper unify the discovery channel for D3D11 compute custom effects. Two slow analysis viewers (Gamut Coverage at 4K, plus Vectorscope/Waveform Monitor when present) migrated to single-group D3D11 compute scatter. Plus a flurry of bug fixes from real-world heavy-graph testing.

### Added (since 1.5.0)

- **Phase 8 GPU-binding rollout (decisions #62, #63 in README; v1.6 work-in-progress).** The full Phase 8 stack is now in place:
  - `Effects/IEngineComputeOutput.h` — engine-internal COM interface (IID `831B9291-CCAB-40A2-B0BA-E847F5B9FA6C`) with `GetAnalysisSrv` + `GetLastEvaluatedFrame`. Pure POD ABI, no STL across the COM boundary. Field layout stays in the graph data model on `EffectNode::analysisOutput.fields` so the metadata isn't duplicated.
  - `gpuBindable` flag on `ParameterDefinition` + `gpuPublish` flag on `AnalysisFieldDescriptor`. JSON round-trip preserves them; defaults are false (current behavior unchanged).
  - `Effects/ShaderLabParamsHlsl.{h,cpp}` engine-embedded macro library (`shaderlab_params.hlsli`) with `SHADERLAB_GPU_BUFFER` / `SHADERLAB_PARAM` / `SHADERLAB_LOAD_PARAM` macros. The host injects `_SLPARAM_<name>_GPU=0|1` per gpuBindable parameter; macros switch via token-pasting to either declare a cbuffer slot or bind a `StructuredBuffer<float4>`. Effect authors write three declarative macro calls per bindable parameter; the `#ifdef` machinery exists exactly once in the shared header. `ShaderCompiler` gains a `D3D_SHADER_MACRO`-aware `CompileFromString` overload + an internal `ID3DInclude` resolver that maps the include name to the embedded string.
  - `D3D11ComputeRunner` becomes a no-op-refcounted COM object implementing `IEngineComputeOutput` and gains a cached `ID3D11ShaderResourceView` alongside its UAV. The runner gains `InstallPrecompiledShader(bytecode, shader)` + `DispatchWithImageOutput(input, cb, count, optionalImageTex)`; the original `Dispatch` is now a thin wrapper.
  - **`Effects/CustomComputeBridgeEffect.{h,cpp}` — D2D wrapper for D3D11 compute custom effects.** Single shared CLSID `6D69E5C2-1AC0-481E-9F94-3DB8CCAD5710`. Implements `ID2D1EffectImpl` + `ID2D1DrawTransform` (passthrough pixel shader) + `ICustomComputeBridge` (driver) + `IEngineComputeOutput` (consumer). D3D11 compute custom effects now route through `CreateOrGetEffect` like every other custom effect — uniform COM discovery via `effect->QueryInterface(IEngineComputeOutput, ...)`. Replaces the pre-Phase-8 special-case branch + parallel `m_d3d11RunnerCache` / `m_imageComputeCache` maps. -475 LoC net in `GraphEvaluator.cpp`.
  - **`Effects/BytecodeCache.{h,cpp}` — compile-once, reuse-everywhere bytecode store.** Substrate for the user's stated goal: when a node is inserted, eagerly precompile all of its gpu-binding variants (baseline + N variants for N gpuBindable params). Identity = `(canonicalSourceHash, paramSignatureHash, includeLibraryHash, macroBitset, entryPoint, target)`. Result struct preserves diagnostics so failed compiles cache their error string. 2 worker `std::jthread`s drain a FIFO queue; render thread's `GetOrCompile` returns Ready entries instantly, waits up to 50 ms for in-flight workers, falls back to inline compile on race. 256 MB LRU bound. Race-safe: late worker results never overwrite a Ready entry produced by an inline compile.
  - **Disk persistence under `%LOCALAPPDATA%\ShaderLab\bytecode\<effectIdSafe>\<version>\<keyHashHex>.cso`.** `BytecodeCache::SetDiskCacheRoot(path)` + `Effects::ConfigureBytecodeCache(rootPath, staleThresholdSec)` engine helper. `MainWindow::InitializeRenderEngine` wires it on engine init with a 90-day background reap. Atomic writes via temp + `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`. TryGet/GetOrCompile try disk before inline compile. Cross-session bytecode reuse: editor session A compiles a shader, session B reuses the bytecode without re-running D3DCompile.
  - **Bytecode cache reaper.** `BytecodeCache::ReapDisk(staleThresholdSec)` + `ClearDisk()` + telemetry stats. Headless gets `--reap-shader-cache`, `--clear-shader-cache`, `--reap-shader-cache-stale-sec=N` CLI flags (one-line JSON output, exits without graph). Status bar gets a broom button at the bottom-left that runs both reapers (graph-temp media dirs + shader bytecode cache) on a background thread and reports `Freed N MB · X graph dirs, Y shader variants` for 5 seconds with hover tooltip preserving the last result.
  - **`Effects/Performance.{h,cpp}` — Phase 8 GPU-binding feature flag.** `Performance::IsGpuBindingsEnabled()` / `SetGpuBindingsEnabled(bool)` + `GpuBindingDetections()` telemetry. v1.6 ships with the flag default ON. Headless gets `--enable-gpu-bindings` / `--disable-gpu-bindings` flags for A/B comparison. The evaluator's binding-resolution pass routes upstream `IEngineComputeOutput` SRVs directly to D3D11 compute consumers' t-slots via `CustomComputeBridgeEffect::SetGpuBinding`; pixel shader consumers (no bridge entry in `m_bridgeImplCache`) fall through to CPU readback gracefully. A compute upstream feeding a pixel shader downstream still works -- just at today's cbuffer-pack speed.
  - **`D3D11ComputeRunner` + `CustomComputeBridgeEffect` GPU-binding entries.** Runner's `DispatchWithImageOutput` accepts dispatch dimensions (default 1,1,1; image-producing per-pixel computes pass `(W/tx, H/ty, 1)`) and an extra-SRV vector (slot/SRV pairs bound at consumer-declared t-slots). Bridge gains `SetGpuBinding(slot, srv)` and `SetDispatchDims(x, y, z)` on `ICustomComputeBridge`; both are cleared after each `Dispatch` so stale state doesn't leak.
  - **GraphEvaluator GPU-binding plan.** `DispatchViaBridge` now walks the consumer's gpuBindable params, captures (slot, SRV) tuples for each one with an upstream `IEngineComputeOutput` producer (discovered via `m_bridgeImplCache`, since D2D's outer effect QI doesn't delegate arbitrary IIDs to the impl), computes a macroBitset, fetches the matching variant bytecode from `BytecodeCache` (eagerly precompiled at first encounter so it's typically a cache hit), installs it on the bridge, and registers the SRV bindings. After dispatch, the bridge's bytecode is restored to the baseline.
  - **First effect migration: ICtCp Tone Map (HDR -> SDR), now D3D11 compute.** Restructured from pixel shader to D3D11 compute with `[numthreads(8,8,1)]` per-pixel tiling and `RWTexture2D<float4>` output at u1. `SourcePeakNits` / `TargetPeakNits` remain `gpuBindable=true`; the bridge routes the upstream SRV directly when bound from a compute producer like `Luminance Statistics`. Descriptor version 11 -> 12.
- **Test bench coverage.** +14 `TestBytecodeCache` tests + 8 `TestGpuBindingRouting` tests (flag on/off × with/without binding × ICtCp output validation) on top of `TestLuminanceStatistics`. 149 total tests passing (was 119 before Phase 8).

### Changed

- **`StatisticsEffect` D2D wrapper class retired (decision #62).** The original `Image Statistics` graph node from decision #37 (Day 6) — `ID2D1EffectImpl` + `ID2D1DrawTransform` + custom `ID2D1StatisticsEffect` interface — was superseded by the dedicated `Channel Statistics` / `Luminance Statistics` / `Chromaticity Statistics` ShaderLab effects (custom `D3D11ComputeShader` definitions dispatching through `D3D11ComputeRunner`). The wrapper class still got registered with D2D at startup but no graph node referenced it, and the README's "Standalone D2D Application" example pointed at an API path nobody was consuming. Both `Effects/StatisticsEffect.{h,cpp}` and the dead `GraphEvaluator::ComputeImageStatistics` method are deleted (-637 LoC). The "Three Effect Types Compared" table in the README now correctly attributes the D3D11 hybrid path to the same `CustomComputeShaderEffect` class that handles D2D-tiled compute (the path is selected by `customEffect.shaderType`, not by a separate COM class).
- **`/render/image-stats` MCP route + `Rendering::GpuReduction` retired (decision #63).** Stats stop being architecturally special at the route level. The route was a parallel implementation of "reduce an image to channel stats" — duplicating logic the registered Statistics graph effects already provide. Agents now use the standard MCP workflow: insert the desired Statistics node via `/graph/add-node`, connect, force a render, read fields via `/analysis/<id>`. Removes ~480 LoC: the `RegisterImageStats` route handler (~90), `GraphEvaluator::ComputeStandaloneStats` (~50), the entire `Rendering/GpuReduction.{h,cpp}` helper (351). Engine-pure route count drops from 21 to **20**.
- **`ShaderLabHeadless` and the test runner now wrap `ProcessDeferredCompute` in `BeginDraw`/`EndDraw`.** `DispatchUserD3D11Compute` calls `dc->DrawImage` internally to pre-render the upstream chain into an FP32 bitmap, and outside an active D2D draw session that DrawImage silently no-ops — the compute shader reads a black input texture and emits Min/Max/Mean = 0. The GUI's `RenderFrame` was already wrapping correctly; the headless host's `runEval` / `RunRender` and the test bench were not. Now they do, plus `runEval` runs the dirty-propagation BFS that `MainWindow::OnRenderTick` does so a set-property on an upstream node correctly triggers downstream analysis nodes to re-dispatch.
- **CI smoke test rewritten** around the consolidated path. The 7-step script in `Tests/RunHeadlessSmoke.ps1` adds a `Luminance Statistics` node, connects it to the source, reads `Mean` analysis field, set-properties `Luminance` 80 → 200 on the source, re-renders, reads `Mean` again, asserts ratio = 2.5×. Same end-to-end coverage as the old `image-stats` path but through the standard graph-node workflow. The `image-stats` shorthand is removed from `--script` mode's op table.

### Fixed

- **D3D11 compute analysis nodes silently produced zero stats in headless contexts.** `DispatchUserD3D11Compute`'s internal `dc->DrawImage` requires an active D2D draw session; without one, the compute shader read a black input texture and emitted Min/Max/Mean = 0 with the histogram floor at bin 0. Caught by the new `TestLuminanceStatistics` unit test and fixed by wrapping the headless host's `runEval` / `RunRender` and the test bench's `Evaluate` shim in `dc->BeginDraw/EndDraw`. The latent bug had probably been there since the D3D11 hybrid compute landed in v1.4-era; it never manifested because the GUI's `RenderFrame` always wraps correctly and the GUI was the only host running stats analysis.
- **Split Comparison pivot off-center on multi-branch graphs.** D2D atlas-pads pixel-shader input intermediates to 4096×4096 when an upstream effect has multiple downstream consumers, so HLSL `Texture2D::GetDimensions()` returns the atlas size, not the output rect. The shader's `uv0 - (W*0.5, H*0.5)` recentering was pivoting at (2048, 2048) on a 3840×2160 video, putting the seam at the bottom edge. Fix: added `OutputW`/`OutputH` hidden cbuffer fields populated by the host from `GetImageLocalBounds(srcNode->cachedOutput)`. Generic mechanism — any pixel shader declaring `OutputW`/`OutputH` parameters gets host-injected dimensions.
- **`D2D1_PIXEL_OPTIONS_TRIVIAL_SAMPLING` now passed on every `CustomPixelShaderEffect::SetPixelShader` call.** Disables D2D's intermediate-atlas allocation so `uv0`/`SV_POSITION` report true output coordinates. Contract: every ShaderLab pixel shader reads inputs at the same coord as the output (cross-texel sampling effects belong on the compute path).
- **CIE Histogram produced an empty image output.** The shader declared `RWTexture2D<float4> Output : register(u0)`, but the runner binds the analysis structured buffer at `u0` and the image output at `u1`. Histogram writes were silently going to the (unused) analysis buffer. Fixed: `register(u0)` → `register(u1)`. Downstream CIE Chromaticity Plot now correctly shows the histogram scatter overlay.
- **Properties panel was un-editable on Clock-driven graphs.** The 4 Hz binding-value refresh was unconditionally calling `UpdatePropertiesPanel()` (which `Clear()`s the entire control tree) whenever the selected node had any property binding and any graph node was dirty — both true continuously while a Clock plays. Mid-edit clicks lost focus on every 250 ms tick. Fix: walk the focused element's parent chain via `VisualTreeHelper::GetParent` and skip the periodic rebuild while any descendant of `PropertiesPanel` holds keyboard focus. User-driven rebuild paths (selection change, explicit property mutation) still rebuild immediately.
- **Vectorscope and Waveform Monitor analysis viewers removed.** No clear use case after the migration to compute scatter; 318 lines net.
- **Gamut Coverage GPU lockup at 4K.** Pixel-shader path was 65K-iter inner loop × full source resolution = ~543B ops on a 4K HDR source. Migrated to single-group D3D11 compute scatter (numthreads(32,32,1), dispatch (1,1,1)) — ~46× speedup.
- **D2D HDR Tone Map post-PDC eval pass leaked compute dispatches into the next frame.** Caused two-frame flicker on tone-mapper output. Fixed via `SetDeferredComputeFrozen(bool)` flag gated around pass 3.
- **Skip-readback compute consumers stopped re-dispatching when bindings hadn't changed.** Broadened force-redispatch condition: when the skip-readback flag is on, ALL compute nodes redispatch every frame regardless of dirty state (bindings can be CPU-throttled while keeping image-output texture fresh).
- **`MapInputRectsToOutputRect` clamps to 4096×4096** so single-input pixel shaders with a hidden 1×1 dummy bitmap don't degenerate to a zero-size output rect.

## [1.5.0] - 2026-05-06

The "headless agent fidelity" release. Every health & scalability phase from the eight-phase plan landed (some partially), the engine grew a host-agnostic ABI + a console host (`ShaderLabHeadless.exe`), the MCP HTTP server moved out of `MainWindow.xaml.h` into the engine DLL with a sink-and-event-hook architecture so MCP-driven graph mutations take the same UI code path the user does, and a JSON `--script` batch mode lets agents run parameter sweeps without a logged-in user. Also: graph viewer DPI fix, two new live-capture sources (DXGI Desktop Duplication, Windows Graphics Capture), and a few user-visible behavior tweaks called out below.

### Added

- **`ShaderLabHeadless --script PATH --script-output PATH`** — JSON batch mode for parameter sweeps. Each step is either a raw `{method, path, body}` HTTP shape or a `{op: ...}` shorthand (`set-property`, `image-stats`, `pixel-region`, `capture-node`, `get-graph`, `get-node`, `analysis`, `render`). Output is a structured JSON document with one `{step, method, path, status, body}` entry per operation; bodies are parsed JSON when possible. Smoke test asserts a Luminance=80→200 set-property → image-stats sequence produces a 2.5× luminance mean ratio end-to-end, exercising set-property → dirty propagation → evaluator → GPU reduction → JSON response across the whole stack.
- **`IEngineCommandSink` event hook architecture (8 hooks).** `OnNodeAdded`, `OnNodeRemoved`, `OnNodeChanged`, `OnGraphCleared`, `OnGraphLoaded`, `OnGraphStructureChanged`, `OnCustomEffectRecompiled`, `OnDisplayProfileChanged`. The GUI's `MainWindow::GuiEngineCommandSink` overrides each hook to call the same UI methods MainWindow uses on native user interactions (`AutoLayout`, `RebuildLayout`, `PopulatePreviewNodeSelector`, `UpdateStatusBar`, `MarkAllDirty`, `CloseOutputWindow`, `PopulateAddNodeFlyout`, `ResetAfterGraphLoad`). The headless host leaves each hook as the default no-op. **MCP-driven mutations are now indistinguishable from native UI interactions** at the host level — an agent calling `/graph/add-node` triggers exactly the same downstream UI code path as the user clicking the toolbar.
- **Engine MCP routes — 21 of 21 engine-pure routes migrated to the engine DLL.** New `Engine/Mcp/EngineMcpRoutes.{h,cpp}` owns: `/registry`, `/effect/hlsl/<id>`, `/effect/compile`, `/graph/add-node`, `/graph/remove-node`, `/graph/connect`, `/graph/disconnect`, `/graph/set-property`, `/graph/load`, `/graph/clear`, `/graph/bind-property`, `/graph/unbind-property`, `GET /graph` (incl. `/graph/save`, `/graph/node/<id>`), `/custom-effects`, `/analysis/<id>`, `/render/image-stats`, `/render/pixel-region`, `/render/capture-node`, `/display/profiles`, `/display/profile`, `/display/profile/clear`. The remaining 16 routes in `MainWindow.McpRoutes.cpp` are intentionally app-side (UI-coupled or host-specific). `MainWindow.McpRoutes.cpp` shrank from **2670 → 1478 lines (-44.6%)**.
- **Engine helpers extracted (reusable across GUI + headless).** `Rendering/PixelReadback.{h,cpp}` (FP32 RGBA region readback), `Rendering/CaptureNode.{h,cpp}` (D2D + WIC PNG encode of any node), `Rendering/WorkingSpaceSync.{h,cpp}` (Working Space parameter node refresh), `Effects/ColorMath.cpp` (shared HLSL color math library).
- **Engine ABI versioning.** New `SHADERLAB_ENGINE_ABI_VERSION` constant in `EngineExport.h` (currently 1) plus C-linkage `ShaderLab_GetAbiVersion()` exported from the engine DLL. `MainWindow::MainWindow` calls it on startup and aborts with a friendly message-box if the loaded DLL's reported version doesn't match the headers we compiled against — catches the "you forgot to redeploy the engine" class of confusion that otherwise manifests as obscure runtime failures. Independent of `Version.h::VersionMajor` (the app version) and `Version.h::GraphFormatVersion` (the JSON schema). Bumped manually whenever a public engine symbol's signature or behavior breaks consumers.
- **`Bootstrap.ps1` at repo root + CI bootstrap-smoke job.** One-command setup for fresh clones: runs `scripts/EnsureDevCert.ps1` + `scripts/EnsureExprTk.ps1` + NuGet restore, with optional `-Build` flag for a Debug|x64 smoke build. CI runs Clean clone → `Bootstrap.ps1 -Build` → tests on every PR. Catches onboarding-cliff regressions automatically.
- **HLSL math test bench (51 tests).** `Tests/ShaderTestBench.{h,cpp}` D3D11 compute harness + 51 tests across 5 categories (transfer functions, color matrices, Möbius/Reinhard tone curves, ΔE, gamut boundaries). Surfaced two real bugs: `PQ_InvEOTF` API misuse (nits vs normalized) and `DeltaE2000` NaN when `C2 == 0`.
- **Engine-side `Rendering::ReadPixelRegion` helper + `ShaderLabHeadless --pixels` mode.** Status enum (`Success`/`NotFound`/`NotReady`/`InvalidRegion`/`D2DError`). `--pixels x,y,w,h` writes either packed binary (`uint32 W` + `uint32 H` header + `float[W*H*4]` row-major) or `.csv` rows. Bypasses PNG / HdrToneMap entirely — full-accuracy scRGB for MCP-driven dE sweeps and quantitative analysis.
- **D2D HdrToneMap pre-pass in ShaderLabHeadless** (default-on). New `--input-peak-nits N` (default 1000), `--output-peak-nits N` (default 80 = SDR), `--no-tonemap` flags. Inserts `CLSID_D2D1HdrToneMap` into the render path before PNG encode so HDR scRGB content above 1.0 doesn't silently saturate to 255.
- **DXGI Desktop Duplication source + Windows Graphics Capture source** (Zachary's contribution, integrated). New entries in the Add Node ▸ Source flyout: a "DXGI DuplicateOutput" submenu listing each enumerated adapter/output, and "Windows Graphics Capture..." opening the standard WinUI graphics capture picker. Both render a live capture stream into the graph as scRGB FP16 frames.
- **Graph viewer DPI scaling fix** (Zachary's contribution, integrated). The graph SwapChainPanel now sizes its backbuffer to physical pixels via `CompositionScaleX/Y` with a `CompositionScaleChanged` handler. Fixes blurry/aliased nodes on high-DPI monitors.

### Changed

- **Closing an Output node's external window now removes the node from the graph.** This was the intended behavior all along — `PresentOutputWindows` calls `RemoveNode` on close — but `EffectGraph::RemoveNode` previously refused to delete the last Output node, leaving a dangling Output with no window. The "always keep at least one Output node" protection has been removed; the render path tolerates an output-less graph fine (no nodes are "needed" so evaluation no-ops until you add a new Output). You can also now delete the last Output via right-click → Delete on the canvas.
- **Image Path / Browse… UI hidden for live-capture sources.** DXGI Desktop Duplication and Windows Graphics Capture sources own their bitmap from a live provider; there's no file path to browse to.
- **MainWindow.xaml.cpp split into sibling partial TUs.** Three extractions: `MainWindow.WorkingSpace.cpp` (~270 LoC), `MainWindow.GraphFileIo.cpp` (~770 LoC), `MainWindow.RenderTick.cpp` (~500 LoC). All three share the `winrt::ShaderLab::implementation::MainWindow` class via `MainWindow.xaml.h`; methods moved verbatim with no behavior change. `MainWindow.xaml.cpp` shrank from **6088 → 4730 lines (-22%)**. PropertiesPanel and Dialogs extractions deferred (interleaved with canvas rendering / spread across the file).
- **Effects/ShaderLabEffects.cpp shrank** from **3169 → 2839 lines (-10%)** by extracting the embedded HLSL color-math library into `Effects/ColorMath.cpp`.
- **`Rendering/ToneMapper` deleted.** ~280 LoC of dead code removed; the tone-mapper-as-class was never used (D2D's built-in `CLSID_D2D1HdrToneMap` is invoked directly by render paths). Project goal in `copilot-instructions.md` rewritten to reflect the actual mission (testbed for D2D pixel/compute effects with HDR/WCG focus).
- **10 ShaderLab effects' enum cbuffer fields migrated to `uint` HLSL declarations.** With the typed pack helper in place, the historical "declare as float, compare with `> 0.5`" workaround is no longer needed. Migrated: Luminance Heatmap, Gamut Highlight, Luminance Highlight, Gradient Generator, Delta E Comparator, Waveform Monitor, Gamut Coverage, Gamut Map, ICtCp Gamut Map, ICtCp Boundary. Effect versions bumped on each migrated effect.
- **Phase 0 cleanup.** Removed `_hidden` filter sites, orphan `Graph/EffectNode2.h`, stray `output.jxr`.

### Fixed

- **DXGI Desktop Duplication / Windows Graphics Capture sources don't update past the first frame.** `SourceNodeFactory::TickAndUploadLiveCaptures` was defined but never called. Wired into `OnRenderTick` so live captures advance every frame; the helper marks captured nodes dirty, which trips the existing `needsEval` gate and triggers the rest of the render pipeline.
- **`DeltaE2000` NaN when one input has C == 0 (a == b == 0).** Found by the Phase 2 test bench against Sharma reference pair 6. `atan2(0, 0)` is implementation-defined and was propagating NaN through `hp_avg = h1p + h2p`. Guarded both `h1p` and `h2p` with a `Cnp < 1e-10 ? 0.0 : atan2(...)` check. Effect version `Delta E Comparator` 3 → 4.

### Changed
- **Phase 5 (partial): extract HLSL color-math library into its own TU.** `Effects/ColorMath.cpp` now owns the embedded `s_colorMathHLSL` string and `GetColorMathHLSL()`. `Effects/ShaderLabEffects.cpp` shrank from **3169 → 2839 lines (-10%)**. Per-effect file split (decision: 30+ effects each registering via static initializer) is deferred — the static-init-order-across-TUs risk is real and the existing one-file-per-30-effects layout works. Phase 2 test bench would not catch a regression in effect registration order.
- **Phase 4 (partial): MainWindow.xaml.cpp split into sibling partial TUs.** Three of the five planned extractions: `MainWindow.WorkingSpace.cpp` (~270 LoC: display-profile selection, ICC loader, `UpdateWorkingSpaceNodes`), `MainWindow.GraphFileIo.cpp` (~770 LoC: save/load + miniz embedded-media archive + heartbeat / stale-temp-dir reaper), `MainWindow.RenderTick.cpp` (~500 LoC: `OnRenderTick`, `RenderFrame`, dirty-propagation pre-pass, video tick, output-window present). All three new TUs share the `winrt::ShaderLab::implementation::MainWindow` class via `MainWindow.xaml.h`; method bodies were moved verbatim with no behavior change. `MainWindow.xaml.cpp` shrank from **6088 → 4730 lines (-22%)**. PropertiesPanel and Dialogs extractions are deferred — the Properties-panel rebuild logic is interleaved with NodeGraphController canvas rendering inside one 2400-line section, and Dialogs are spread across the file; both need a closer look at method-boundary coupling before extraction.

### Added
- **Phase 3: typed PropertyValue → cbuffer pack helper.** Single `Effects::PackPropertyToCBuffer(dest, remaining, hlslType, hlslColumns, value)` helper in `Effects/ShaderCompiler.h/.cpp` that converts a `PropertyValue` to the destination cbuffer slot's declared HLSL type. The historical "memcpy of float bit pattern produces nonsense uints" bug class (CHANGELOG 1.3.9) cannot recur because the helper reflects each variable's `D3D_SHADER_VARIABLE_TYPE` and `static_cast<uint32_t>` / `<int32_t>` / `BOOL` before writing. Three previously-duplicated pack sites now share this single implementation: `CustomPixelShaderEffect::PackConstantBuffer`, `CustomComputeShaderEffect::PackConstantBuffer`, and the D3D11 compute path inside `GraphEvaluator::DispatchUserD3D11Compute`. CLAUDE.md gotcha #1 ("uint cbuffer params don't work") is replaced with a positive-guidance note: HLSL can now declare `uint`/`int`/`bool` enums with clean `Mode == 1` comparisons. Existing 13 effects keep their float-enum convention; migration is optional and deferred.
- **Phase 2: HLSL math test bench (`Tests/ShaderTestBench.h/.cpp`).** A tiny D3D11 compute harness that compiles + dispatches + reads back single-thread compute kernels, used to unit-test the color-math HLSL helpers in `GetColorMathHLSL()` (and the Delta E helpers via a per-test preamble). Tests assert against literal reference values from BT.2100 / SMPTE ST.2084 / Sharma CIEDE2000 / matrix algebra — no parallel C++ port to maintain. Runs on WARP in CI in single-digit seconds.
  - **51 new tests across 5 categories** in `Tests/Math/`: Transfer Functions (PQ EOTF/InvEOTF anchors at 0/100/1000/10000 nits, round trip, out-of-domain finite, ICtCp NaN guard), Color Matrices (algebraic D65 anchor, basis-vector orthogonality, 6-color round trip, ScRGB↔ICtCp round trip, negative-input clamp), Möbius/Reinhard (`f(0) == 0`, `f(peakIn) == peakOut` 1.3.8 anchor regression, `f'(0) ≈ 1`, `Expand(Compress(I,hi,lo),hi,lo) == I` round trip, out-of-domain saturation, monotonicity, NitsToI/IToNits round trip), Delta E (76 / 94 / 2000 identity, symmetry, 4 Sharma reference pairs), Gamut (sRGB↔linear toe + 6-color round trip, xyY↔XYZ round trip, PointInTriangle gamut containment, OKLab anchors, ScRGBToLab D65).
  - **Two real bugs surfaced while writing the proof-of-concept tests** (which is what the harness is for): (1) `PQ_InvEOTF` takes nits, not normalized [0,1] — easy API-misuse class that no existing test would catch; (2) `DeltaE2000` returns NaN when either input has C == 0 (a == b == 0) because `atan2(0, 0)` is implementation-defined and propagates through `hp_avg`. Sharma pair 6 (50,-1,2) vs (50,0,0) is currently *skipped* in the suite with an in-source comment; the fix path will guard `hp_avg` when `C1p * C2p < 1e-10` and re-add the pair. Tracked as a follow-up todo.
  - `Tests/TestCommon.h` extracts `TEST()` + pass/fail counters from the anonymous namespace in `TestRunner.cpp` so multi-TU test code can share the same summary line.
  - `Effects::GetColorMathHLSL()` is now exported via `SHADERLAB_API` so the standalone test runner can call it across the engine DLL boundary.
  - `Tests/RunMathTests.ps1` for local invocation. The existing CI `ShaderLabTests.exe --adapter warp` step already includes the new tests by inclusion.

### Removed
- **`Rendering/ToneMapper` class.** Decision #32 (Day 6) removed the built-in tone-mapper from the render path; the class itself lingered as default-`None` dead code through v1.4.1 — never instantiated at runtime, only included by `Controls/OutputWindow.h` for an unused declaration. `.github/copilot-instructions.md` still cited "fixing issues with the existing D2D tonemapper effect" as the project's #1 development focus, which was actively misleading new contributors and AI agents about where work actually happens. Phase-1 cleanup deletes `Rendering/ToneMapper.h` + `.cpp` (~280 LoC), removes the include from `OutputWindow.h`, drops the entries from both vcxproj files + `.filters`, and rewrites the project-identity / architecture / focus sections of `copilot-instructions.md` around the actual workflow: graph-built tone mappers (the ICtCp suite) plus the empirical fidelity loop (`Working Space` node + `Delta E Comparator` Grayscale dE + `Luminance Statistics`). README decision log gains entry #54; `.context/resume.md` rendering bullet and project-structure tree updated. README architecture mermaid diagram drops the `ToneMapper` node.
- **`_hidden` suffix filter and the `Graph/EffectNode2.h` orphan file.** Phase-0 health pass: the `_hidden` property name filter (in `MainWindow.xaml.cpp` Properties-panel rebuild and `Controls/NodeGraphController.cpp` data-pin discovery) had been demoted to legacy-compatibility-only by decision #51. Removed both filter sites; sink-only properties on the Working Space node are still kept off the UI by the existing customEffect declared-parameter filter, so no behavior change for current effects. Old graphs that still carry `WsRedX_hidden` / `MonMaxNits_hidden` / `SdrWhiteNits_hidden` keys load with the keys present in memory but inert (no shader cbuffer references them; non-customEffect nodes never had them; customEffect nodes filter their Properties panel by declared parameters). Cross-version graph compatibility is not currently promised. Decision log entries #35 and #51 updated; copilot-instructions and resume.md updated. Also deleted `Graph/EffectNode2.h` (empty 0-byte orphan, never referenced) and the stray `output.jxr` test artifact at the repo root (already gitignored).

## [1.4.1] - 2026-05-05

### Added
- **`OutputMode` parameter on the `Delta E Comparator` effect** (effectVersion 2 → 3). Two modes: `0 = Heatmap` (default — Turbo colormap, prior behavior) and `1 = Grayscale dE`. Grayscale mode writes `saturate(dE / MaxDeltaE)` to all three RGB channels, which makes a downstream `Luminance Statistics` node read true mean/p95/max color-difference values directly off the GPU — no CPU pixel readback required. Enables live in-graph dE telemetry while sweeping a tone-mapper's parameters: bind / drive any input, watch the mean dE field update in the Properties panel each frame.
  - Read all uniforms unconditionally before the mode branch (mirrors gotcha #2 in CLAUDE.md so D3DCompile can't strip `OutputMode` if the agent only ever uses heatmap mode).

### Changed
- **`README.md` decision log #52** updated with a cleaner empirical result on `Colors of Journey_1002.mp4`. Earlier entry said `ToneLift ≈ 0.6` matches D2D within ~10 %; that was matching the dark/mid *luminance levels* visually. A proper CIEDE2000 fidelity-to-source measurement (using the new Grayscale dE + LumStats live-readout pipeline) shows the *color-accuracy* optimum is `ToneLift ≈ 0.30`, with mean dE 9–32 % lower than D2D HDR Tone Map across three frames spanning 0.5 → 124 nit p95 source brightness. The default of `0.0` (pure peak compression, neutral) is retained.

## [1.4.0] - 2026-05-05

### Removed
- **Per-effect "follow the live monitor / working space" plumbing.** Removed all 5 host-side per-frame writer blocks from `MainWindow.xaml.cpp` (~150 LOC) and stripped every `_hidden` cbuffer field from 12 ShaderLab effects. The `Working Space` parameter node + property bindings (decision #51) are now the only path for tracking the active display profile from a graph.
  - **Effects converted** (12, all schema-broken — bumped `effectVersion`): Gamut Highlight, Luminance Highlight, CIE Chromaticity Plot, Gamut Source, Gamut Coverage, Gamut Map, ICtCp Gamut Map, ICtCp Boundary, ICtCp Tone Map (HDR → SDR), ICtCp Inverse Tone Map (SDR → HDR), ICtCp Highlight Desaturation, Luminance Statistics. Plus a light cleanup on the `Gamut Parameter` data node (dropped its `Working Space` value-enum entry).
  - **Replacement pattern**: each affected effect's gamut/range enum gains a `Custom` entry as the last index (existing static modes — `sRGB`, `DCI-P3`, `BT.2020`, etc. — are kept for strict-mode analysis without wiring 3 binds), and the host-managed primaries are promoted to first-class bindable `Float2` parameters (`RedPrimary`, `GreenPrimary`, `BluePrimary`, plus `WhitePoint` where applicable) gated by `visibleWhen "TargetGamut == 3"`. CIE Chromaticity Plot is the exception — its monitor primaries were always implicit, so the new `Float2` params are unconditional. Effects that consumed `(Ws)SdrWhiteNits_hidden` (ICtCp Tone Map / Inverse Tone Map / Highlight Desaturation / Luminance Statistics) drop the hidden field entirely; their existing numeric peak-nit parameter (`TargetPeakNits` / `SourcePeakNits` / `ClipNits`) becomes the single source.
  - **Old enum modes dropped**: every `Current Monitor` / `Working Space` entry from `TargetGamut`, `SourceGamut`, and `TargetRange`; entire `SdrWhiteSource` and `ClipSource` switch enums.
  - **Saved-graph breakage** (accepted, no other users yet): old graphs loaded from `.effectgraph` files keep stale `_hidden` properties in their `node.properties` map, but the shader cbuffer no longer references them and they're filtered out of the Properties panel + pin list (decision #35 retained for exactly this reason). Old `Working Space` enum-mode selections clamp to an invalid index → user re-picks. No graph format version bump.

### Added
- **HDR / gamut / tone-mapping MCP tool suite (10 new, 27 → 37 total).** Round 2 of the agent-driven HDR workflow: simulate any display profile, capture any node's output, drive the preview view, run GPU stats, sample pixel grids, and inspect HLSL — all without mutating UI state.
  - `list_display_profiles` — GET `/display/profiles`. Returns 7 built-in presets (sRGB SDR, sRGB 270, P3 600, P3 1000, BT.2020 1000, BT.2020 4000, AdobeRGB) + the active live/simulated profile + `isSimulated` flag. Full caps in JSON: hdr/peak/sdrWhite/min/maxFullFrame nits, primaries (CIE xy), gamut id.
  - `set_display_profile` — POST `/display/profile`. Apply a simulated profile via exactly one of `{preset, presetIndex, iccPath, custom}`. `custom` accepts the full `{name, hdrEnabled, sdrWhiteNits, peakNits, minNits, maxFullFrameNits, primaryRed, primaryGreen, primaryBlue, whitePoint, gamut}` schema; missing fields fall back to sane defaults. Drives `MainWindow::ApplyDisplayProfile`, marks the graph dirty, forces the next render frame.
  - `clear_simulated_profile` — POST `/display/profile/clear`. Reverts to the live OS-reported profile.
  - `render_capture_node` — POST `/render/capture-node` body `{nodeId, inline?}`. Captures any node's resolved image as PNG without touching `m_previewNodeId`. Forces a render frame first so dirty nodes evaluate. With `inline=true` returns MCP-native image content (`type:"image"`, `mimeType:"image/png"`); without it, writes a unique `%TEMP%\shaderlab_node_<pid>_<seq>.png` and returns the path. Disambiguates not-found (404) vs not-ready (409 + `notReady:true, reason:"dirty"|"missingInputs"`).
  - `preview_get_view` / `preview_set_view` / `preview_fit_view` — symmetric with the graph view tools but for the preview pane. zoom is clamped to [0.01, 100.0] (matches the wheel-zoom range, much wider than the graph view's [0.1, 5.0]); pan is unclamped. `preview_set_view` returns post-clamp values.
  - `image_stats` — POST `/render/image-stats` body `{nodeId, channels?, nonzeroOnly?}`. Per-channel min/max/mean/median/p95/sum + nonzero counts, GPU-reduced on a fresh FP32 render of the target. Defaults to luminance + R + G + B + A; pass `channels:["luminance"]` to skip the others. Backed by a new `GraphEvaluator::ComputeStandaloneStats(dc, image, channels, nonzeroOnly)` so MCP doesn't have to own a separate reduction instance or mutate the graph for diagnostics.
  - `read_pixel_region` — POST `/render/pixel-region` body `{nodeId, x, y, w, h}`. Reads a small w×h region of FP32 RGBA pixels (scRGB linear-light) row-major as a flat float array. Capped at 32×32 (1024 pixels) and per-axis at 64; oversize requests fail 400 with the cap quoted so agents can chunk. Out-of-bounds rects clip to the image; empty after clip → 404.
  - `effect_get_hlsl` — GET `/effect/hlsl/{nodeId}`. Reads a custom-effect node's HLSL source, parameter list, compile state, last runtime error. For ShaderLab library effects also returns `isLibraryEffect:true` + `shaderLabEffectId`/`shaderLabEffectVersion` so the agent knows what's read-only. Non-custom nodes return `hasCustomEffect:false` (200, not 404) so the agent can probe any node.
- **Graph snapshot + view-control MCP tools** (4 new). AI agents can now request a PNG of the live node-graph editor view and steer its pan/zoom exactly the way a user would. Implementation lives in `MainWindow::CaptureGraphAsPng()`, `MainWindow::FitGraphView()`, and `NodeGraphController::ContentBounds()`. The shared `MainWindow::RenderGraphScene()` helper drives both the live render tick and the off-screen snapshot so the two never drift.
  - `graph_snapshot` — POST `/graph/snapshot`. Always writes a unique `%TEMP%\shaderlab_graph_snapshot_<pid>_<seq>.png`. With `inline=true`, the JSON-RPC response carries MCP-native `content[].type=image` + `mimeType=image/png` so the agent gets the bytes directly. Snapshot renders the current pan/zoom at the live swap-chain panel size — same dimensions the user sees.
  - `graph_get_view` — GET `/graph/view`. Returns `{zoom, panX, panY, viewportW, viewportH, contentBounds, zoomLimits}`.
  - `graph_set_view` — POST `/graph/view` body `{zoom?, panX?, panY?}` (any subset). Applies via `NodeGraphController::SetZoom`/`SetPanOffset`, which mark the canvas dirty so the next 16ms render tick shows the new view. zoom is clamped to [0.1, 5.0]; pan has no clamp. Coord convention: `screen = zoom * canvas + pan`.
  - `graph_fit_view` — POST `/graph/view/fit` body `{padding?}` (viewport DIPs, default 40). No-op on empty graph.
- **`NodeGraphController::ContentBounds()`** — returns the AABB of all node visuals in canvas (pre-pan/zoom) space. `{0,0,0,0}` when no nodes are laid out.
- **MCP activity indicator on the toolbar.** A single dot next to the MCP toggle pulses amber when the server has handled a request in the last few seconds and turns off when idle. Provides at-a-glance feedback that an external agent is actually connected and active. The amber colour was chosen because the original green was nearly invisible against the toggle's blue Checked background.
- **`Working Space` parameter node + display-profile mirroring.** New ShaderLab Parameter effect (no input pins, no output image pin) that mirrors the active display profile from the top-bar profile selector — live OS-reported caps or any simulated preset / ICC the user has applied — into 14 typed analysis output fields: `ActiveColorMode` (0=SDR, 1=WCG/ACM, 2=HDR), `HdrSupported`, `HdrUserEnabled`, `WcgSupported`, `WcgUserEnabled`, `IsSimulated`, `SdrWhiteNits`, `PeakNits`, `MinNits`, `MaxFullFrameNits`, plus four CIE-xy primaries `RedPrimary` / `GreenPrimary` / `BluePrimary` / `WhitePoint` (each Float2). Bind any downstream property to these fields via the property-binding system — e.g. wire a tone-mapper's peak-nits to `working_space.PeakNits` and it tracks Display Settings or simulated profile changes automatically. Replaces ad-hoc per-effect "follow the working space" toggles. Updated in `MainWindow::UpdateWorkingSpaceNodes()` which runs on `ApplyDisplayProfile`, `RevertToLiveDisplay`, the display-change callback, and once per render tick (cheap node-list walk; only marks dirty when at least one field actually changed, so freshly-added nodes pick up live values immediately without hooking every AddNode site).
- **`DisplayCapabilities` extended with ACM / WCG state** (`activeColorMode`, `hdrSupported`, `hdrUserEnabled`, `wcgSupported`, `wcgUserEnabled`). Sourced from a new `DisplayMonitor::QueryAdvancedColorInfo2()` helper that calls `DisplayConfigGetDeviceInfo` with `DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2` (type 15, requires SDK 10.0.26100). Falls back to deriving `activeColorMode` from `caps.hdrEnabled` when the type-15 query is unavailable. Also trusts DisplayConfig's `bitsPerColorChannel` over the legacy DXGI heuristic, which reports 8 in many WCG configurations even when the actual scanout is 10-bit. All seven preset factories now stamp coherent ACM/WCG flags via the new `StampSimulatedColorMode()` helper so simulated profiles report a self-consistent display mode through the Working Space node.

### Changed
- **The "Regular parameter nodes" branch in `GraphEvaluator::Evaluate` now unpacks the full `PropertyValue` variant** (`bool` → 0/1 float, `int32_t` / `uint32_t` → float, `float2` / `float3` / `float4` → component[0..N]) into `AnalysisFieldValue::components`. Previously only `float` was unpacked, so any vector or boolean property on a parameter node was silently zero. Used by the Working Space node for primary chromaticities (Float2) and capability flags (Float-as-bool); also benefits any future host-managed parameter nodes that need richer types.
- **`Working Space Integration` README section rewritten** to describe the new "bind, don't hide" pattern for tracking the active display profile. Decision log gains entry #51; entry #35 (the `_hidden` suffix convention) cross-references #51 to record that the suffix filter is now legacy-compatibility only.
- **`ICtCp Tone Map (HDR → SDR)` gained a `ToneLift` parameter** (default `0.0` = identity, range `[0..1]`). Adds an anchored polynomial mid-bump on top of the existing I-channel Reinhard compression: `f(x) = x + a·x·(1−x)` evaluated in normalized `[0, peakOut]` I-space then scaled back. Designed to let the operator approximate D2D `HdrToneMap`'s "make HDR readable on SDR" dark-end lift without giving up the hue-preservation property of pure I-channel compression. Quantitatively benchmarked against D2D HDR Tone Map on the `Colors of Journey` HDR test clip: at `ToneLift ≈ 0.6`, dark-region nit counts match D2D within ~10%; at `ToneLift = 0` the effect is its prior pure-compression behavior. Effect bumped from version 9 → 10 (drops `ToneLift` initialized to default on existing graphs). Note: the docstring no longer claims "saturation is preserved by construction" — only hue is. Lifting I without rescaling Ct/Cp can desaturate slightly, especially at higher `ToneLift`.

### Fixed
- **`ICtCpToScRGB` could emit NaN/Inf when `pqLms` exceeded the PQ valid range `[0, 1]`.** The function transformed ICtCp → PQ-encoded LMS via the inverse matrix and then called `PQ_EOTF` on each component without clamping. For modified ICtCp inputs (e.g. tone-mapped I) or out-of-gamut chroma, the matrix product could push an LMS component above 1 (where `PQ_EOTF`'s denominator goes through zero and then negative around `V ≈ 1.16`) or below 0 (where `pow` of a negative is undefined). Added `pqLms = saturate(pqLms);` before the EOTF calls. In-domain values are unaffected; out-of-domain values now saturate to the nearest representable nit value rather than producing NaN that propagates into the inverse XYZ→scRGB matrix. Benefits all 8 ICtCp-using effects.
- **`DispatchSync` was UB on timeout.** The original wait-and-deref pattern closed the event after a 10-second timeout, then dereferenced `*result` without checking `WAIT_OBJECT_0` — and the in-flight lambda still held references to stack `result`/`ex`/`event` which dangled if the caller had returned. Replaced with a `shared_ptr<State>` capture so the lambda can safely complete after the caller times out, an explicit `WAIT_OBJECT_0` check that throws on timeout (caller's `catch` returns 500 cleanly), and a 30s timeout for the heavier compute/readback tools. Closed by the new round-2 routes that need it; old routes benefit too.
- **Snapshot capture no longer suppresses the live editor repaint.** `NodeGraphController::Render()` clears `m_needsRedraw` at the end. `CaptureGraphAsPng()` now snapshots the dirty flag before its render and re-sets it after, so an MCP-driven snapshot taken between frames doesn't cancel the next live render.

## [1.3.9] - 2026-05-05

### Fixed
- **D3D11 compute effects (Channel / Luminance / Chromaticity Statistics) never compiled or dispatched.** `DispatchUserD3D11Compute` started with `if (!node.customEffect->isCompiled()) return;` — but `isCompiled()` checks `compiledBytecode.empty()`, and for D3D11 compute the bytecode is only ever populated *inside* this function via the lazy `runner->CompileShader` path. Chicken-and-egg: every call early-returned, no bytecode was ever stored, the shader never ran, and analysis-output fields stayed empty forever. Removed the early-out for that case so the runner can compile on first dispatch and persist the bytecode back to `def.compiledBytecode` for the cbuffer reflection pass.
- **`D3D11ComputeRunner::CompileShader` discarded the compiled bytecode blob** after creating the `ID3D11ComputeShader` object, leaving `def.compiledBytecode` empty for D3D11 compute effects forever. Cached the blob in `m_bytecode` and exposed it via `GetCompiledBytecode()`; `DispatchUserD3D11Compute` now copies it onto `node.customEffect->compiledBytecode` after a successful compile so the cbuffer-reflection pass that packs user properties has something to reflect against.
- **D3D11 compute cbuffers received raw float bit patterns where shaders declared `uint`/`int`/`bool`.** The Properties panel stores enum-style fields (e.g. `Units`, `ClipSource`) as `float`, but the HLSL declares `cbuffer { uint Units; ... }`. Doing `memcpy(dest, &floatVal, 4)` writes `0x3F800000` (1,065,353,216) into the uint slot, so `if (Units == 1)` was never true and conditional code paths were dead. The pack code now reflects each variable's `D3D_SHADER_VARIABLE_TYPE` and converts via `static_cast<uint32_t>` / `static_cast<int32_t>` based on the declared HLSL type before writing.
- **Analysis-only compute nodes froze their output fields after the first frame.** `needsCompute` was only true on `node->dirty || fields.empty()`, and a Statistics node's own properties don't change frame-to-frame even as the upstream video does. Added a topological dirty-propagation pre-pass at the start of `Evaluate`: any node that's dirty marks all its direct downstream consumers dirty before the eval loop runs. Same pass is also done in `RenderFrame` after `TickAndUploadVideos`, so video-frame updates correctly flow through pixel-shader chains *and* analysis-only compute nodes in the same frame they're decoded. Both passes are idempotent — no-op when nothing is dirty.
- **Effect Designer "Output type" + analysis fields were blank when opening a built-in ShaderLab effect** (or any saved custom effect with typed analysis output). `LoadDefinition` populated inputs / parameters / thread-groups but never restored the `OutputTypeSelector` selection or the analysis-field rows, so opening *Channel Statistics* showed no record of its declared `Min` / `Max` / `Mean` / etc. Refactored the per-row construction in the Add-Field click handler into a reusable `AppendAnalysisFieldRow()` method, then taught `LoadDefinition` to map `def.analysisOutputType` → selector index and rebuild one row per `def.analysisFields` entry with name / type / array length restored.

## [1.3.8] - 2026-05-05

### Added
- **ICtCp tone-mapping suite (initial)**: three new effects under *Analysis → Tone Mapping*. `ICtCp Tone Map (HDR → SDR)` and `ICtCp Inverse Tone Map (SDR → HDR)` apply Reinhard compression / expansion to I in BT.2100 ICtCp space, leaving Ct/Cp untouched so hue and saturation are preserved by construction. `ICtCp Round-Trip Validator` is a diagnostic effect that outputs `|scRGB→ICtCp→scRGB - in| × Gain` — used to verify the conversion math is correct (renders black on a correct image). All three opt into `SdrWhiteNits_hidden` so future auto-bind affordances can drive Target/SourcePeakNits from the live OS SDR-white value.
- **Color-math HLSL helpers**: `NitsToI`, `IToNits`, `ReinhardCompressI`, `ReinhardExpandI` for I-channel tone mapping in PQ-encoded nit space. Live next to the existing `ScRGBToICtCp` / `ICtCpToScRGB` pair in `GetColorMathHLSL()`.
- **OS-reported SDR white level**: `DisplayMonitor` now queries `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)` so `sdrWhiteLevelNits` tracks the Windows *Settings → Display → HDR → "SDR content brightness"* slider in real time. Falls back to 80 nits when the call isn't available (older Windows builds, virtual outputs). Status bar now displays it next to the peak nit value (e.g. `Max Luminance: 1000 nits (SDR white 240)`). Simulated `DisplayProfile` presets continue to override the live value through the existing path.
- **`SdrWhiteNits_hidden` / `WsSdrWhiteNits_hidden`** hidden-default keys for effects that need scRGB-1.0-in-nits. Injected per frame from `RenderFrame` with the same change-detected pattern as `MonMaxNits_hidden` and `WsRedX_hidden`.
- **Add Node flyout sub-grouping**: `ShaderLabEffectDescriptor` gained a `subcategory` field. The Analysis category — which had grown to 16+ entries — is now folded into **Comparison**, **Gamut Mapping**, **Highlights**, **Scopes**, **Statistics**, and **Tone Mapping** sub-trees. Effects without a subcategory remain at the top level under their category.

### Changed
- **`Perceptual Gamut Map` renamed to `ICtCp Gamut Map`** (effectId + display name) for consistency with the existing `ICtCp Boundary` effect and the new ICtCp tone-mapping suite. Saved graphs that reference the old ID resolve through a legacy alias in `FindById`, so existing `.effectgraph` files still load — they'll surface the standard "Update Effect" prompt on next save. Effect version bumped 7 → 8.

### Fixed
- **MCP server now accepts `Transfer-Encoding: chunked` requests.** Visual Studio's MCP client uses chunked HTTP rather than `Content-Length`; our raw-Winsock reader was passing the chunk framing (`<hex-size>\r\n<payload>\r\n0\r\n\r\n`) straight to `JsonObject::Parse`, which threw `0x83750007 "Invalid JSON string"` on every connect. The reader now detects chunked encoding, waits for the terminating zero-length chunk, and decodes the framing into the actual JSON payload. Content-Length requests are unchanged. Same path also adds: a non-throwing `JsonObject::TryParse` wrapper that returns a JSON-RPC `-32700 Parse error` response instead of bubbling a `winrt::hresult_error`; correct `202 Accepted` (not `200 OK`) for `notifications/*` per the MCP Streamable HTTP spec; a small `GET /` health-check route so clients that probe before posting don't get a 404; and proper status-text for 202/204/400/500 in the HTTP response builder.
- **All five ICtCp pixel shaders were sampling the same edge texel for every output pixel.** They were calling `Source.Sample(Sampler, uv)` with `uv` from `TEXCOORD`, but D2D `TEXCOORD` is in pixel/scene space, not normalized [0,1]. Every output pixel was reading from texel ~256 with edge clamping, so the entire image came out as one constant color regardless of input. Switched to `Source.Load(int3(uv, 0))` to match the Gamut Highlight / scope pattern that was already correct. Affects: `ICtCp Round-Trip Validator`, `ICtCp Tone Map (HDR → SDR)`, `ICtCp Inverse Tone Map (SDR → HDR)`, `ICtCp Saturation`, `ICtCp Highlight Desaturation`. Effect versions bumped to invalidate cached bytecode.
- **`ReinhardCompressI` / `ReinhardExpandI` were unanchored.** The original form `peakOut * t / (1 + t*(1 - ratio))` had `f(peakIn) = peakOut / (2 - ratio)` instead of `peakOut`, so peaks did not map to peaks: HDR→SDR compression undershot the SDR target, SDR→HDR expansion overshot the HDR target by hundreds of percent (a 1000-nit SDR-white input produced ~14,943 nits at TargetPeakNits=1000). Replaced with the anchored Möbius `f(I) = I·peakIn·peakOut / (peakIn·peakOut + I·(peakIn − peakOut))` and its analytic inverse — same shape near zero (slope 1, "Reinhard" feel in the dark end), but `f(0) = 0`, `f(peakIn) = peakOut` exactly, with a smooth shoulder. Round-trip error stays at FP16 noise (~7e-10 per channel at Gain=100k).
- **`ReinhardExpandI` had its peakIn/peakOut convention inverted in the call site.** The doc-comment said `peakIn = SDR, peakOut = HDR`, but the algebra is the literal inverse of `ReinhardCompressI(peakIn=HDR, peakOut=SDR)`, so the call inside the Inverse Tone Map shader was running the curve in the wrong direction (input got dimmer with higher Strength). Both the doc-comment and the call site now agree: peakIn is the *uncompressed* (HDR) range, peakOut is the *compressed* (SDR) range, for both Compress and Expand.
- **Over-range inputs to the ICtCp tone-mapping helpers walked toward the Möbius asymptote.** A 10,000-nit input through an Inverse Tone Map configured for SourcePeakNits=100 produced 1.4 million nits because the helper extrapolated past its anchor onto the rising branch beyond peakIn. Both `ReinhardCompressI` and `ReinhardExpandI` now clamp their input to the valid input range before mapping, so out-of-domain inputs saturate cleanly to the target peak instead of overshooting.

## [1.3.7] - 2026-05-05

### Changed
- Moved the version + effect-library version readout from the bottom-status-bar to the title bar. Frees the right side of the status bar so the FPS / frame-timing readout has room to breathe (it had been clipping against the version text on smaller windows).

### Fixed
- First selection of a node in a freshly-loaded graph or a freshly-added node would render the preview at the wrong zoom (whatever the previous viewport happened to be). Subsequent selections were correct because by then the cached output existed. Now click defers `FitPreviewToView` until the next eval has populated `cachedOutput`, so the very first fit measures real bounds.
- Newly-added Clock and other parameter nodes did not show their on-node UI (play/pause button, seek slider) until the user clicked them. The post-eval `RebuildLayout` only fired on user-forced renders; the add-node path now sets `m_forceRender = true`, triggering the same path so the controls appear on the first frame after add.

## [1.3.6] - 2026-05-05

### Fixed
- Save / load of `.effectgraph` archives with embedded media froze the UI for tens of seconds while miniz compressed/decompressed. The ProgressBar painted once at the initial 16 ms yield then sat unchanged because the work ran synchronously on the UI thread. The save/load now runs on a threadpool thread (miniz is pure native, no WinRT marshalling concerns) and the progress callback marshals each update back through `DispatcherQueue` so the bar animates smoothly.
- Video sources bound to a paused Clock free-ran in a ~1 s loop instead of holding a static frame. Cause: `TickAndUploadVideos` advanced the decoder via `provider->Tick(deltaSec)` whenever the bound `Time` value didn't differ from the current decode position by more than one frame, then the next iteration's "small forward gap" branch let it drift further. Fixed by remembering each provider's previous bound `Time` value — if it didn't change between ticks, hold the frame at exactly that position rather than ticking.

### Internal
- Resolved compiler warnings: `C4456` (shadowed `first` local in `WriteNodeJson`), `C4189` (unused `slashPos`, `imgDipX/Y`, `isPixelTraceTab`), `C4100` (unreferenced `sender` parameter on `OnPreviewPointerMoved`).

## [1.3.5] - 2026-05-05

### Fixed
- Right-hand properties panel jittered horizontally during video playback. Cause: every 250 ms while ANY graph node was dirty (always true while a video was playing) we rebuilt the entire properties panel, re-creating Slider + NumberBox pairs whose auto widths varied frame to frame. Now the rebuild only fires when the selected node has at least one `propertyBinding` whose live value can change between ticks.
- Visual Assets (app tile, splash, badges) were broken in shipped MSIX. The `Assets/` directory contained 21 corrupted `*.scale-200.scale-N.png` files (VS regenerated them by feeding existing scale-200 PNGs back as source — none of those names match any MRT lookup pattern), plus several 0-byte placeholder files. Cleaned them out and stripped the matching entries from `ShaderLab.vcxproj` / `.filters`. The remaining originals will resolve through MRT correctly; user-supplied bear artwork still needs to be dropped over the placeholder PNGs to actually appear.

## [1.2.7] - 2026-05-04

### Added
- **`--reap-now` command-line flag**: scans `%TEMP%` for orphan `ShaderLab-*` media folders left behind by a crashed instance, deletes any whose `.heartbeat` (or directory mtime) is older than the staleness threshold, prints a per-folder summary to stderr, and exits with the count of folders removed. Optional `--reap-stale-sec=<N>` overrides the default 150 s threshold for scripted regression tests.

### Fixed
- The previous v1.2.6 fix for the embed-media checkbox used `winrt::box_value(bool)`, which returns `IInspectable` not `IReference<bool>`, and tripped the `IReference<bool>` private-constructor diagnostic. Switched to `PropertyValue::CreateBoolean(...).as<IReference<bool>>()` which is the canonical conversion.

## [1.2.6] - 2026-05-04

### Fixed
- Save As crashed (`0xC000027B` — unhandled exception in coroutine `SaveGraphAsAsync`) when the user kept the *Embed referenced media* checkbox checked. The embed-media `ContentDialog` was initializing the checkbox with `IReference<bool>(m_embedMedia)`, whose `void*` constructor reinterprets a `bool` as a WinRT interface pointer rather than boxing it. Replaced with the canonical `winrt::box_value(m_embedMedia)`.

## [1.2.5] - 2026-05-04

### Added
- **Crash-safe temp media cleanup**: each extracted `.effectgraph` media folder gets a hidden `.heartbeat` sentinel that the running app touches every 60 s. On startup, ShaderLab scans `%TEMP%` for `ShaderLab-*` folders whose heartbeat (or directory mtime, for legacy folders) is older than 150 s and offers a *“Clean up old graph media”* `ContentDialog` (`Delete` / `Keep`). Live folders never go stale, so the prompt only appears when a previous instance crashed without running its `Closed` handler.

## [1.2.4] - 2026-05-04

### Added
- **Embedded media in `.effectgraph`**: source nodes that reference an external file (image, video, ICC profile) can have those files bundled inside the saved zip under `media/`. The Save As flow shows a follow-up `ContentDialog` with an *“Embed referenced media”* checkbox (default on) whenever the live graph has at least one external file path. JSON inside the archive uses canonical `media://<name>` tokens; on load the file is extracted to a unique GUID-named subdirectory under `%TEMP%` and the live graph is rewritten to point at the extracted path. Stale extraction directories are deleted at app shutdown.
- **Save / load progress dialog**: a `ContentDialog` with a `ProgressBar` and per-file status line tracks each entry as it is read or written. The actual zip work runs on a background thread so the UI keeps refreshing during large media archives.

### Changed
- `EffectGraphFile::Save` / `Load` API now take an optional media list and progress callback; the load result is a struct that includes the extraction directory and `media://` token map.
- `Ctrl+S` now goes through the same progress / embed flow rather than the old silent overwrite -- the user sees the same UI whether they overwrite or use Save As.

## [1.2.3] - 2026-05-03

### Added
- **`.effectgraph` file format**: native graph container. Standard ZIP archive (no compression, no third-party libraries — Win32 `CreateFile` + a small in-house store-only writer in `Rendering/EffectGraphFile.cpp`) holding `graph.json` plus a reserved empty `media/` folder for future media embedding (so a shared graph will eventually open with no broken file paths).
- **File type association**: `.effectgraph` is registered in `Package.appxmanifest`. Double-clicking a file in Explorer launches ShaderLab with the graph already loaded. `App::OnLaunched` parses the command line for the file path and hands it to `MainWindow::SetPendingOpenPath` to load after rendering is initialized.
- **Save / Save As keyboard accelerators**: `Ctrl+S` saves over the last picked path (or prompts on first save). `Ctrl+Shift+S` always shows the picker. `Ctrl+O` opens. The Save Graph button advertises both shortcuts in its tooltip.
- **Unsaved-changes guard**: `MainWindow` tracks `m_unsavedChanges` and shows a `Save / Discard / Cancel` `ContentDialog` when the window is closed via `AppWindow.Closing`. Title bar shows the file name with a `*` suffix while edits are unsaved.

### Changed
- `MainWindow::SaveGraphAsync` now overwrites the previously-picked path silently; previously every save reopened the picker.
- App version string in the toolbar now derives from `Version.h` instead of a hard-coded `v1.1.0`.

### Removed
- Legacy `.json` graph save / load support. `.effectgraph` is the only graph container; existing `.json` files from pre-1.2.3 builds will no longer open. There were no released users at the time of the cutover.

### Fixed
- Toolbar version text was stuck on `v1.1.0` regardless of the actual app version.

## [1.2.2] - 2026-05-02

### Added
- **Random parameter node**: takes a single `Seed` float input and outputs a deterministic, well-mixed `Result` in `[0, 1)`. The output is a pure function of the seed (SplitMix64-style integer mixer on the float's bit pattern), so identical seeds reproduce identical values and any change to the seed (e.g. a tick from an upstream Clock or Numeric Expression) yields a fresh random number. Implemented in `Rendering/GraphEvaluator.cpp`; registered in `Effects/ShaderLabEffects.cpp` (`effectId = "Random"`).

## [1.2.1] - 2026-05-01

### Added
- ICC profile reading now goes through `mscms.dll` (`OpenColorProfileW` + `GetColorProfileElement`) instead of an in-house binary parser. Public `IccProfileParser::LoadFromFile` API and `IccProfileData` struct are unchanged. Engine link list gains `mscms.lib`.
- Render `DispatcherQueueTimer` now ticks at the active monitor's refresh rate (`EnumDisplaySettings(ENUM_CURRENT_SETTINGS).dmDisplayFrequency`), clamped to [60, 240] Hz, refreshed on every display change. 120 / 144 / 165 / 240 Hz panels and high-FPS video sources run at native cadence. Interval is set in microseconds so non-integer-ms periods (e.g. 144 Hz ≈ 6.944 ms) stay accurate.

### Fixed
- Per-node connection log entries used Unicode arrows (`←` / `→`) that rendered as garbled glyphs in the LogWindow. Replaced with plain ASCII `from` / `to` wording in both `MainWindow::OnEdgeAdded` and the `/graph/connect` MCP route.

## [1.2.0] - 2026-04-30

### Added
- **Numeric Expression node**: a single ExprTk-powered math node replaces the legacy Add / Subtract / Multiply / Divide / Min / Max nodes.
  - Dynamic input list (`A..Z`, 26-input cap) editable from the Properties panel (`➕ Add Input`, per-row `✕`).
  - Expression rendered under the node title on the canvas as `= <formula>`.
  - Single `Result` analysis output bindable to any downstream scalar property.
  - Expression and input list round-trip through graph JSON.
- **Alt+click edge delete**: Alt + Left-click on any image or data-binding edge removes it (bezier hit-test in `NodeGraphController::HitTestEdge`).
- **ARM64 release builds**: GitHub Actions release workflow now produces both `ShaderLab-<version>-x64.zip` and `ShaderLab-<version>-arm64.zip`.
- **Smarter `Install.ps1`**: detects host architecture, installs bundled dependency MSIXes (VCLibs, Windows App Runtime) before the ShaderLab MSIX, and is robust to Microsoft.* dependency packages sharing the folder.
- Pre-build script `scripts\EnsureExprTk.ps1` that downloads `exprtk.hpp` (MIT) into `third_party\exprtk\` on first build.

### Changed
- New nodes (toolbar / context menu / MCP `graph_add_node`) now spawn at the **center of the current viewport** in graph coordinates, accounting for pan / zoom.
- Closing an OutputWindow forces a single render pass so the deleted Output node disappears from the canvas immediately.
- `Package.appxmanifest` `Publisher` stays plain `CN=ShaderLab` in the repo; the release workflow injects the unsigned-namespace OID just before MSBuild so signed F5 deploys keep working.
- README and decision log updated through entry #48.
- MCP tool count is now 23 (added `node_logs`, `perf_timings`).

### Fixed
- 0xC0000005 in MSVC Release builds when first evaluating a Numeric Expression — fixed by disabling ExprTk's regex / IO / enhanced subsystems via `exprtk_disable_*` macros before `#include`-ing `exprtk.hpp` (with PCH disabled on `Rendering/MathExpression.cpp`).
- Signed F5 deploy was rejected after we tried to keep the unsigned-namespace OID in `Package.appxmanifest` — separating the OID into a release-only injection step restores both flows.

## [1.1.0] - 2026-04-23

### Added
- **ShaderLab Effects Library**: 9 built-in effects with embedded HLSL and shared color math
  - Analysis: Luminance Heatmap, Out-of-Gamut Highlight, CIE Chromaticity Plot, Vectorscope
  - Source: Gamut Source, Color Checker, Zone Plate, Gradient Generator, HDR Test Pattern
- **Effect Designer**: Dedicated window for creating custom pixel/compute shader effects
  - Parameter types: float, float2, float3, float4, int, uint, bool, enum
  - Enum parameters with named dropdown labels
  - Bool parameters with ToggleSwitch control
  - Analysis output field declaration for compute shaders
  - HLSL auto-formatting and scaffold generation
- **MCP Server**: 21-tool JSON-RPC 2.0 server for AI agent integration (port 47808)
  - Graph manipulation, property control, HLSL compilation, render capture
  - Analysis output reading, pixel trace, effect listing, display info
- **Property Binding System**: Per-component Grasshopper-style data flow between analysis outputs and effect properties
  - Visual data pins (orange diamonds) on node graph
  - Type-safe bindings with component picking (float4.x → float)
- **Versioning System**: App version 1.1.0, graph format version 2
  - Forward compatibility check on graph load
  - Version display in status bar and title bar
- **Monitor Gamut Detection**: Live primaries from DXGI_OUTPUT_DESC1
  - Updates on monitor change
  - Feeds Out-of-Gamut Highlight "Current Monitor" mode
- **GPU Info**: Status bar shows hardware adapter name or "Software (WARP)"
- **Display Profile Mocking**: Override live display with presets or ICC profiles
- **Pixel Trace**: Recursive per-node pixel readback through effect graph
- Dark theme, Segoe Fluent Icons toolbar, dot grid canvas, refined node colors

### Changed
- Pipeline always uses scRGB FP16 (no sRGB fallback on SDR displays)
- Tone mapper defaults to None (inactive) — DWM/ACM handles display conversion
- Render loop is dirty-gated (skips evaluate when no nodes changed)
- Node graph canvas is dirty-gated (skips redraw when nothing changed)
- Analysis readback only runs on dirty frames

### Fixed
- Compile-on-connected-node crash (D2D access violation from sentinel input rects)
- Out-of-Gamut Highlight initial state showing no highlighting
- Monitor primaries being zeroed by HLSL optimizer
- Custom effects duplication menu and name uniqueness enforcement

## [1.0.0] - 2026-04-20

### Added
- Initial release
- Node-based DAG graph editor for D2D effect composition
- 40+ built-in D2D effects across 9 categories
- Custom pixel shader effects (ID2D1DrawTransform)
- Custom compute shader effects (ID2D1ComputeTransform)
- Live HLSL hot-reload with D3DCompile
- WIC HDR/SDR image loading
- Pixel inspector with scRGB → sRGB/PQ/luminance conversion
- Tone mapping (5 modes)
- Display change monitoring (WM_DISPLAYCHANGE + DXGI adapter events)
- Graph JSON serialization
