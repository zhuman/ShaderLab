# Engine / Host Split

The codebase is divided between a host-agnostic engine DLL and one or more host applications:

- **`ShaderLabEngine.dll`** owns everything that doesn't need a UI thread or a swap chain: the `EffectGraph` model + JSON serialization, the `GraphEvaluator` (per-node D2D effect cache, dirty propagation, two-pass evaluate), `SourceNodeFactory` (image / video / DXGI / WGC sources), `EffectRegistry` (40+ wrapped D2D effects + 20+ ShaderLab effects with embedded HLSL), `DisplayMonitor` + ICC parsing, the `Effects/CustomPixelShaderEffect` / `CustomComputeShaderEffect` COM classes, the generic D3D11 compute dispatch helper (`Rendering/D3D11ComputeRunner.{h,cpp}`), the `ShaderCompiler` (D3DCompile + D3DReflect), and **the MCP HTTP server itself plus all 20 engine-pure routes** (`Engine/Mcp/McpHttpServer.{h,cpp}` + `Engine/Mcp/EngineMcpRoutes.{h,cpp}`). Engine-pure helpers extracted for reuse: `Rendering/PixelReadback.{h,cpp}` (FP32 RGBA region readback), `Rendering/CaptureNode.{h,cpp}` (D2D + WIC PNG encode), `Rendering/WorkingSpaceSync.{h,cpp}` (Working Space parameter node refresh).

- **`ShaderLab.exe`** (the WinUI 3 host) keeps everything that genuinely needs WinUI: `MainWindow.xaml.{h,cpp}` (which itself is split into sibling partial TUs `MainWindow.WorkingSpace.cpp`, `MainWindow.GraphFileIo.cpp`, `MainWindow.RenderTick.cpp`, `MainWindow.McpRoutes.cpp` for the 16 UI-coupled routes), `Controls/NodeGraphController` (canvas rendering), `Controls/OutputWindow` (per-Output OS window), `Controls/ShaderEditorController`, the Effect Designer modal window, and `RenderEngine` (D3D11 + D2D1 device stack, `SwapChainPanel` binding).

- **`ShaderLabHeadless.exe`** (see below) reuses everything from the engine DLL with no WinUI dependency.

## `IEngineCommandSink` event hook architecture

When MCP routes mutate engine state, they fire through an `IEngineCommandSink` (`Engine/Mcp/EngineMcpRoutes.h`) so that:

1. The host marshals the mutation closure to whatever thread is appropriate (UI thread for the GUI app via `DispatcherQueue`; synchronous direct-call for headless).
2. The host runs **event hooks** afterwards on the same thread to keep its UI / output windows / preview selector in sync.

The eight hooks are: `OnNodeAdded`, `OnNodeRemoved`, `OnNodeChanged`, `OnGraphCleared`, `OnGraphLoaded`, `OnGraphStructureChanged`, `OnCustomEffectRecompiled`, `OnDisplayProfileChanged`. The GUI's `MainWindow::GuiEngineCommandSink` overrides each one to call the same UI methods that handle native user interactions (`AutoLayout`, `RebuildLayout`, `PopulatePreviewNodeSelector`, `PopulateAddNodeFlyout`, `UpdateStatusBar`, `MarkAllDirty`, `CloseOutputWindow`, `ResetAfterGraphLoad`). The headless host leaves each hook as the default no-op. **Result: an MCP client calling `/graph/add-node` triggers exactly the same downstream UI code path as the user clicking the toolbar.**

The 16 routes that remain in `MainWindow.McpRoutes.cpp` are intentionally app-side because they are either UI-coupled (`/graph/snapshot`, `/graph/view*`, `/preview/view*`, `/render/preview-node`, `/render/capture`, `/render/pixel-trace`) or host-specific (`/`, `POST /` JSON-RPC dispatcher, `/context`, `/perf`, `/node/<id>/logs`, `/render/pixel/<x>/<y>`).


---

Back to [docs/](../README.md) • [Repo root](../../README.md)