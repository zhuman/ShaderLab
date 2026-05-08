# Copilot Instructions

## Project Identity

ShaderLab is a WinUI 3 desktop application (C++/WinRT) for developing, testing, and debugging Direct2D shader effects with full HDR/WCG support. The primary focus is building tone-mapping and color-correction effects as graph nodes, with empirical fidelity tooling — Delta E Comparator + Luminance Statistics + Working Space node form a closed-loop CIEDE2000 readout that lets us tune effect parameters against measured color accuracy, not visual impression.

## Hard Rules

- **C++/WinRT only** — never generate C# code. Direct COM access to `ID2D1EffectImpl`, `ID2D1DrawTransform`, `ID2D1ComputeTransform` is the reason this project exists.
- **`docs/` is the living architecture documentation tree.** Update the relevant file under `docs/architecture/`, `docs/effects/`, etc. when a significant architectural change lands. Append a new entry to [`docs/history/decision-log.md`](../docs/history/decision-log.md) with a Mermaid diagram for any choice that future contributors will need to understand the *why* of. The repo-root `README.md` is intentionally slim — install + build + a pointer to `docs/` — and should not be expanded with technical detail.
- **All new `.cpp` files must `#include "pch.h"` as the first include** — precompiled header is mandatory (`pch.h` aggregates WinRT, D2D, D3D, Win2D, DXGI, WIC, and STL headers).

## Build

- Open `ShaderLab.slnx` in Visual Studio 2022 17.8+
- NuGet packages restore automatically (packages.config style, not PackageReference)
- Build target: **Debug | x64** (also supports ARM64, Release)
- No command-line build scripts exist; use MSBuild via VS or `msbuild ShaderLab.vcxproj /p:Configuration=Debug /p:Platform=x64`
- Required: Windows App SDK 1.8, Windows 10 SDK 10.0.26100+, Win2D 1.3.0
- Linked native libs: `d3d11.lib`, `d2d1.lib`, `dxgi.lib`, `d3dcompiler.lib`, `dxguid.lib`, `windowscodecs.lib`
- `/bigobj` is enabled; language standard is C++20 (VS 18+) or C++17 (VS 17)

## Architecture

The codebase is split between an engine DLL (`ShaderLabEngine.dll`, host-agnostic), a WinUI 3 app (`ShaderLab.exe`), a console host (`ShaderLabHeadless.exe`), and a test runner (`ShaderLabTests.exe`). The split is documented in detail in README's *Engine / Host Split* section.

```
ShaderLab.exe (WinUI 3 host)
  ├── MainWindow (XAML)         — split across .xaml.cpp / .WorkingSpace.cpp / .GraphFileIo.cpp / .RenderTick.cpp / .McpRoutes.cpp
  ├── RenderEngine              — D3D11 + D2D1 device stack, DXGI swap chain, BeginDraw/EndDraw/Present
  ├── Controls/                 — NodeGraphController (canvas), OutputWindow (per-Output OS window),
  │                               ShaderEditorController, PixelInspectorController, PixelTraceController
  └── EffectDesignerWindow      — Modal for creating/editing custom pixel/compute shader effects

ShaderLabEngine.dll (host-agnostic)
  ├── Graph/                    — EffectGraph, EffectNode, EffectEdge, NodeType, PropertyValue
  ├── Rendering/
  │   ├── GraphEvaluator        — Topological walk + per-node D2D effect cache + ProcessDeferredCompute
  │   ├── DisplayMonitor        — HDR/SDR detection, WM_DISPLAYCHANGE + adapter-changed jthread
  │   ├── D3D11ComputeRunner    — Generic D3D11 compute dispatch (RWStructuredBuffer<float4>),
  │   │                           also implements IEngineComputeOutput (Phase 8 GPU-binding interface)
  │   ├── PixelReadback         — FP32 RGBA region readback helper
  │   ├── CaptureNode           — D2D + WIC PNG encode of any node's output
  │   ├── WorkingSpaceSync      — Working Space parameter node refresh
  │   ├── IccProfileParser      — mscms.dll-based ICC reader
  │   └── MathExpression        — ExprTk-backed expression evaluator (Numeric Expression node)
  ├── Effects/
  │   ├── ShaderLabEffects      — 35 ShaderLab effects (analysis/source/tone-map/parameter) with embedded HLSL
  │   ├── ColorMath.cpp         — Shared HLSL color math library (BT.709/BT.2020/P3, PQ/HLG, ICtCp)
  │   ├── EffectRegistry        — 40+ wrapped D2D effects across 9 categories
  │   ├── ShaderCompiler        — D3DCompile + D3DReflect + ID3DInclude resolver for shaderlab_params.hlsli
  │   ├── ShaderLabParamsHlsl   — Engine-embedded macro library for GPU-binding-aware parameters
  │   ├── IEngineComputeOutput  — COM interface for upstream effects exposing GPU-resident analysis
  │   ├── CustomPixelShaderEffect / CustomComputeShaderEffect — Custom-effect base classes
  │   ├── SourceNodeFactory     — Image / video / Flood / DXGI Desktop Duplication / Windows Graphics Capture
  │   └── DxgiDuplicationSourceProvider, WindowsGraphicsCaptureSourceProvider, VideoSourceProvider
  └── Engine/Mcp/
      ├── McpHttpServer         — Winsock2 TCP server, route registration, JSON-RPC dispatcher
      └── EngineMcpRoutes       — 20 engine-pure routes + IEngineCommandSink + EngineContext

ShaderLabHeadless.exe (console host, no WinUI dependency)
  └── Main.cpp                  — PNG render / --pixels FP32 readback / --script JSON batch mode
```

Render loop: `DispatcherQueueTimer` driven by the active monitor's refresh rate (clamped to 60–240 Hz, decision #50) → dirty-propagation BFS → `BeginDraw` → `GraphEvaluator.Evaluate()` → `ProcessDeferredCompute()` (D3D11 compute analysis nodes; **must** be inside the `BeginDraw`/`EndDraw` so the internal `dc->DrawImage` actually runs — decision #63) → `DrawImage(previewOutput)` → `EndDraw` → `Present`. There is no built-in tone-mapping pass — users build tone mappers as graph effects (the ICtCp suite is the preferred path).

## Namespace Convention

All code lives under `ShaderLab::` with sub-namespaces matching directories:
- `ShaderLab::Graph` — data model (EffectGraph, EffectNode, EffectEdge, PropertyValue)
- `ShaderLab::Rendering` — device stack, swap chain, evaluator, display monitoring, GPU reduction
- `ShaderLab::Effects` — effect registry, custom effects, shader compilation, image loading
- `ShaderLab::Controls` — UI controllers (decoupled from XAML views)
- `winrt::ShaderLab::implementation` — WinRT XAML types (App, MainWindow)

## Code Conventions

### COM & Pointer Patterns
- **Member COM objects**: Always `winrt::com_ptr<T>` with `m_` prefix (e.g., `m_d3dDevice`, `m_swapChain`)
- **API output params**: Use `.put()` for COM creation, `.get()` for passing to raw APIs, `.as<T>()` for QueryInterface
- **D2D cached outputs in graph nodes**: Raw `ID2D1Image*` (non-owning; render engine manages lifetime)
- **Custom D2D effects**: Manual `IUnknown` ref counting with `InterlockedIncrement/Decrement` on `LONG m_refCount` — these implement `ID2D1EffectImpl` directly, not via WinRT

### Error Handling
- Initialization paths: `winrt::check_hresult()` to throw on failure
- Render/runtime paths: `SUCCEEDED(hr)` / `FAILED(hr)` checks with early return (no exceptions in hot paths)
- Graph operations: Return `bool` for success/failure; `nullptr` for not-found
- Topological sort: Throws `std::logic_error` on cycle detection

### Naming
- **Member variables**: `m_` prefix (`m_graph`, `m_renderEngine`, `m_refCount`)
- **Methods**: PascalCase (`AddNode`, `PrepareForRender`, `TopologicalSort`)
- **Enums**: PascalCase values (`NodeType::BuiltInEffect`, `AnalysisFieldType::Float2`)
- **Structs**: PascalCase, used for plain data (`EffectNode`, `DisplayCapabilities`, `ShaderVariable`)
- **Classes**: PascalCase, used for stateful objects with methods (`EffectGraph`, `RenderEngine`, `GraphEvaluator`)

### File Organization
- **Header-only**: Small data structs and inline constants (`NodeType.h`, `PropertyValue.h`, `DisplayInfo.h`, `PipelineFormat.h`, `EffectEdge.h`, `EffectNode.h`)
- **Header + .cpp**: Stateful classes with complex logic
- **One class per file pair** (matching names)
- **Anonymous namespaces** for file-local helpers (e.g., JSON serialization helpers in `EffectGraph.cpp`)

### Property System
- Node properties stored as `std::map<std::wstring, PropertyValue>` where `PropertyValue = std::variant<float, int32_t, uint32_t, bool, std::wstring, float2, float3, float4, D2D1_MATRIX_5X4_F, std::vector<float>>`
- JSON serialization uses type tags (`"float"`, `"float4"`, etc.) for round-trip fidelity
- Shader constant buffers packed from PropertyValue via D3DReflect offsets (byte-level memcpy)

## Custom D2D Effect Pattern

When creating new D2D effects (the core purpose of this tool):
1. Implement `ID2D1EffectImpl` + `ID2D1DrawTransform` (pixel shader) or `ID2D1ComputeTransform` (D2D-tiled compute shader). For full-image reductions that need atomics / cross-tile groupshared, use `customEffect.shaderType == D3D11ComputeShader` instead — those run via `D3D11ComputeRunner` in the deferred-compute pass.
2. Register with `ID2D1Factory1::RegisterEffectFromString()` using XML schema + `D2D1_VALUE_TYPE_BINDING` macros
3. Keep only `InputCount` as a D2D-exposed property; manage shader bytecode and cbuffers host-side
4. Use `ShaderCompiler` for `D3DCompile` (ps_5_0 / cs_5_0) + `D3DReflect` for cbuffer discovery
5. Register effects in `Effects::RegisterEngineD2DEffects()` at engine init (called from app + headless + tests)
6. Follow `CustomPixelShaderEffect` / `CustomComputeShaderEffect` as templates

## Tone Mapping & Color Correction (Primary Development Focus)

Active development centers on **tone-mapping and color-correction effects authored as graph nodes** — not a built-in tone-mapping pass. The render pipeline is intentionally pass-through (scRGB FP16 in, scRGB FP16 out); users compose tone mappers and color correction from graph effects, validate them with empirical fidelity tooling, and iterate.

- **Pipeline is scRGB FP16 linear light** (1.0 = 80 nits SDR white). No fixed built-in tone-mapping pass — DWM/ACM handles final display conversion.
- **The ICtCp suite** in `Effects/ShaderLabEffects.cpp` (Tone Map / Inverse Tone Map / Saturation / Highlight Desaturation / Round-Trip Validator / Gamut Map / Boundary) is the preferred path for tone-mapping work. Math lives in `GetColorMathHLSL()` (PQ/HLG transfer functions, BT.709/2020/P3 matrices, anchored Reinhard / Möbius, scRGB↔ICtCp).
- **Empirical fidelity loop**: `Delta E Comparator` (Heatmap or Grayscale dE mode) + `Luminance Statistics` (D3D11 compute reduction) + `Working Space` node (mirrors active display profile) lets you measure CIEDE2000 mean/p95/max color difference *while sweeping a parameter*, on the GPU, every frame. This is the canonical way to evaluate any new tone-mapping or color-correction work.
- **Working Space node** is the single path for tracking the active display profile. Effects expose first-class `RedPrimary` / `GreenPrimary` / `BluePrimary` / `WhitePoint` (Float2) and peak/SDR-white nit parameters; bind them from `working_space.*` outputs to follow Display Settings or simulated profiles automatically.
- **D2D's built-in `CLSID_D2D1HdrToneMap`** applies a fixed BT.2408-style mid-tone lift independent of `InputMaxLuminance` (decision log #52). Useful as a reference, not a building block — its lift is opinionated and fixed.

## Key Technical Context

- **Pipeline is always scRGB FP16**: `DXGI_FORMAT_R16G16B16A16_FLOAT` with `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`. No pipeline format switching — DWM/ACM handles final display conversion.
- **Swap chain**: `CreateSwapChainForComposition` + `ISwapChainPanelNative` (WinUI 3 requirement). Color space set via `SetColorSpace1()`.
- **Display monitoring**: Dual path — `WM_DISPLAYCHANGE` via hidden message-only HWND + `IDXGIFactory7::RegisterAdaptersChangedEvent` on a jthread.
- **Graph serialization**: `Windows.Data.Json` (zero extra dependencies). GUID fields use `StringFromGUID2`/`CLSIDFromString`.
- **Effect registry**: Singleton with 40+ built-in D2D effects across 9 categories. Case-insensitive name lookup.
- **ShaderLab effects library**: 35 built-in effects in `Effects/ShaderLabEffects.h/.cpp` across categories: Analysis (Heatmaps + Scopes + Statistics + Tone-Mapping), Color Processing (Gamut Map + ICtCp Gamut Map), Source / Generator, Composition (Split Comparison), and the data-only Parameter / Clock / Numeric Expression / Random / Working Space nodes. Embedded HLSL with shared color math from `Effects/ColorMath.cpp`. Auto-compiled at first use.
- **MCP server**: JSON-RPC 2.0 server on port 47808 (47809 for headless to avoid shared-machine conflicts). The server itself + 20 engine-pure routes live in `Engine/Mcp/{McpHttpServer,EngineMcpRoutes}.{h,cpp}`; 16 UI-coupled / host-specific routes stay in `MainWindow.McpRoutes.cpp`. Both hosts register the same engine-side route set through the same `IEngineCommandSink` interface (decision #58). Engine-side routes are uniform: pure mutation closures dispatched via `sink.Dispatch`, with 8 event hooks (`OnNodeAdded`, `OnNodeRemoved`, `OnNodeChanged`, `OnGraphCleared`, `OnGraphLoaded`, `OnGraphStructureChanged`, `OnCustomEffectRecompiled`, `OnDisplayProfileChanged`) the GUI overrides to keep its UI in sync.
- **Versioning**: `Version.h` defines app version (currently **1.5.0**) and graph format version (2). Both are stored in saved graphs. Forward compatibility check on load. `EngineExport.h::SHADERLAB_ENGINE_ABI_VERSION` is independent — bumped manually on engine ABI breaks; mismatch between header and DLL aborts startup with a friendly message-box.
- **Refresh-rate-driven render loop**: `DispatcherQueueTimer` interval is set from the active monitor's refresh rate (clamped to 60–240 Hz). Dirty-gated: skips evaluate when no nodes changed. Re-applied on every display change so dragging the window across monitors picks up the new rate.
- **`ProcessDeferredCompute` requires an active D2D draw session**: it calls `dc->DrawImage` internally to pre-render the upstream chain into an FP32 bitmap, and outside `BeginDraw`/`EndDraw` that DrawImage silently no-ops. The GUI's `RenderFrame`, the headless host's `runEval` / `RunRender`, and the test bench all wrap accordingly.
- **Ctrl+Enter** compiles shader from the editor TextBox.

## Adding New Files

1. Create `.h` and `.cpp` in the appropriate directory (`Graph/`, `Rendering/`, `Effects/`, `Controls/`)
2. Add both to `ShaderLab.vcxproj` under the correct `<ClInclude>` / `<ClCompile>` `<ItemGroup>`
3. Add to `ShaderLab.vcxproj.filters` for VS Solution Explorer organization
4. Use the matching `ShaderLab::SubNamespace` namespace
5. `#include "pch.h"` as the first line in every `.cpp` file

## D2D Custom Effect Gotchas

These are critical lessons learned during development. Any AI agent generating or modifying D2D custom effect code **must** account for these:

1. **Typed cbuffer pack (Phase 3+)**: `uint`, `int`, and `bool` cbuffer slots in HLSL are now packed correctly even when the corresponding `PropertyValue` is stored as `float` (the default for enum-style parameters). The `Effects::PackPropertyToCBuffer` helper reflects each cbuffer variable's `D3D_SHADER_VARIABLE_TYPE` and converts via `static_cast<uint32_t>` / `<int32_t>` / `BOOL` before writing. So you *can* declare `uint Mode` in HLSL and use clean `if (Mode == 1)` comparisons. **Pre-Phase-3 historical convention** (still works, used by all existing ShaderLab effects): declare enums as `float` in HLSL with `> 0.5` / `> 1.5` threshold comparisons. New effects can use either; mixing within one effect is fine.
2. **HLSL compiler with `D3DCOMPILE_WARNINGS_ARE_ERRORS` optimizes out cbuffer variables** not referenced on ALL code paths. Read all cbuffer vars at top of `main()` before any branches.
3. **D2D custom effects need TWO evaluation passes** for newly created effects — first creates/initializes, second produces correct output. Don't expect correct output on the first frame after creation.
4. **`RegisterWithInputCount` requires `inputCount >= 1`**. Zero-input source effects must use a hidden dummy bitmap input.
5. **`MapInputRectsToOutputRect` with `SetFixedOutputSize`** must check fixed size FIRST, before examining input rect. Getting this order wrong causes incorrect output sizing.
6. **D2D `TEXCOORD` values are in pixel/scene space**, NOT normalized [0,1]. Always use `GetDimensions()` and divide to get normalized UVs.
7. **D2D custom effect transforms must NOT pass through infinite input rects** in `MapInputRectsToOutputRect`. Infinite rects cause D2D to attempt unbounded rendering.
8. **`ForceUploadConstantBuffer()` uploads cbuffer but doesn't invalidate cached output**. Need an input toggle trick (disconnect+reconnect dummy input) to force D2D to re-evaluate the effect.
9. **Monitor gamut comes from `DXGI_OUTPUT_DESC1` primaries** — use `RedPrimary`, `GreenPrimary`, `BluePrimary`, `WhitePoint` fields. These are CIE xy chromaticity coordinates.
10. **Always write primaries into OOG properties on every evaluate** (ensures correct values on first frame), but only mark dirty on actual change (prevents feedback loops between property writes and evaluation).

## ShaderLab Effects Library

The built-in effects library lives in `Effects/ShaderLabEffects.h/.cpp`:

- **Embedded HLSL**: Each effect's shader code is stored as a `const char*` string constant. No external `.hlsl` files.
- **Shared color math**: A common HLSL library (BT.709/BT.2020/P3 color matrices, PQ/HLG transfer functions, CIE XYZ↔xy conversions, luminance calculations) is prepended to each shader at compile time.
- **Auto-compile**: Effects are compiled via `ShaderCompiler` at first use (when added to graph). Compiled bytecode is cached.
- **Categories**:
  - **Analysis → Heatmaps** (PS): Luminance Heatmap, Gamut Highlight, Luminance Highlight, Nit Map, Delta E Comparator
  - **Analysis → Scopes** (PS or CS): Vectorscope, Waveform Monitor, CIE Histogram (CS), CIE Chromaticity Plot
  - **Analysis → Statistics** (D3D11 compute, data-only): Channel Statistics, Luminance Statistics, Chromaticity Statistics
  - **Analysis → Tone Mapping** (PS, ICtCp suite): ICtCp Round-Trip Validator, ICtCp Tone Map, ICtCp Inverse Tone Map, ICtCp Saturation, ICtCp Highlight Desaturation
  - **Analysis → Gamut**: Gamut Coverage (CS), ICtCp Boundary (PS)
  - **Color Processing** (PS): Gamut Map, ICtCp Gamut Map
  - **Source / Generator** (PS): Gamut Source, Color Checker, Zone Plate, Gradient Generator, HDR Test Pattern
  - **Composition**: Split Comparison
  - **Parameter / Data-only**: Float / Integer / Toggle / Gamut Parameter, Clock, Numeric Expression (ExprTk), Random, Working Space
- No `_hidden` suffix convention. (Removed in Phase-0 cleanup. Sink-only properties — e.g., the Working Space node's `ActiveColorMode`, `SdrWhiteNits`, primaries — live in `ShaderLabEffectDescriptor::hiddenDefaults` without any `_hidden` suffix, and are kept off the UI by the customEffect declared-parameter filter. The Working Space node + property bindings is the only path for tracking the active display profile.)
- **Analysis outputs**: Compute shader analysis effects declare `AnalysisFieldDef` entries that describe their output fields (name, type, count). These are read back to CPU via `GraphEvaluator` and can be bound to downstream properties.

## Effect Designer

`EffectDesignerWindow.xaml/.h/.cpp` provides a dedicated modal window for creating custom effects:

- **Shader types**: Pixel shader (`ps_5_0`) and compute shader (`cs_5_0`)
- **Parameter definition**: Users define parameters with types: `float`, `float2`, `float3`, `float4`, `int`, `uint`, `bool`, `enum`
  - **Enum params**: Comma-separated label definition (e.g., `"Off,Low,Medium,High"`) generates a dropdown in the Properties panel
  - **Bool params**: Rendered as `ToggleSwitch` controls (not `NumberBox`)
- **Analysis output fields**: For compute shaders, users can declare typed output fields that will be readable via `read_analysis_output`
- **HLSL auto-formatting**: Basic indent/brace formatting applied on edit
- **Scaffold generation**: Templates generate a starting HLSL shader with the declared parameters already in the cbuffer
- **Graph integration**: "Add to Graph" creates a new node; "Update in Graph" recompiles and updates an existing node
