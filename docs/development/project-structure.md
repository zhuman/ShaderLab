# Project Structure

```
ShaderLab/
├── ShaderLab.slnx                  # Solution file
├── ShaderLab.vcxproj               # WinUI 3 app project (MSIX packaged app)
├── ShaderLabEngine.vcxproj         # Shared native engine DLL project
├── ShaderLabTests.vcxproj          # Standalone console test runner project
├── ShaderLabHeadless.vcxproj       # Console host project (no WinUI dependency)
├── packages.config                 # NuGet package manifest
├── Package.appxmanifest            # MSIX app identity
├── app.manifest                    # DPI awareness, heap type
├── EngineExport.h                  # SHADERLAB_API import/export macro + ABI version constant
├── EngineExport.cpp                # ShaderLab_GetAbiVersion() C export
├── Version.h                       # App version + graph format version
├── README.md                       # This file
├── CHANGELOG.md                    # Version history
├── Bootstrap.ps1                   # One-command fresh-clone setup (cert + ExprTk + restore)
│
├── pch.h / pch.cpp                 # App PCH (WinRT, WinUI, D2D, D3D, Win2D, STL)
├── pch_engine.h / pch_engine.cpp   # Engine/Test/Headless PCH (WinRT base, D2D, D3D, MF, STL)
├── App.xaml / .h / .cpp            # Application entry point
├── MainWindow.xaml / .h / .cpp     # Main window layout + initialization (~4700 lines)
├── MainWindow.WorkingSpace.cpp     # Display-profile selection + ICC loader + UpdateWorkingSpaceNodes shim
├── MainWindow.GraphFileIo.cpp      # Save/load + miniz embedded-media archive + heartbeat reaper
├── MainWindow.RenderTick.cpp       # OnRenderTick / RenderFrame / dirty-propagation pre-pass / output-window present
├── MainWindow.McpRoutes.cpp        # 16 UI-coupled MCP routes + GuiEngineCommandSink + JSON-RPC dispatcher (~1500 lines)
├── MainWindow.idl                  # WinRT interface definition
├── EffectDesignerWindow.xaml / .h / .cpp  # Effect Designer modal window
│
├── Engine/Mcp/                     # Engine DLL: MCP server + engine-pure routes
│   ├── McpHttpServer.h / .cpp      # Winsock2 TCP server, route registration, JSON-RPC
│   ├── EngineMcpRoutes.h / .cpp    # 20 engine-pure routes + IEngineCommandSink + EngineContext
│
├── Tests/                          # ShaderLabTests + smoke scripts
│   ├── TestRunner.cpp              # 113 tests (graph, evaluator, MCP, math bench)
│   ├── TestCommon.h                # Shared TEST() macro across TUs
│   ├── ShaderTestBench.h / .cpp    # D3D11 compute test harness for HLSL math
│   ├── Math/                       # 51 HLSL math tests
│   │   ├── TransferFunctionTests.cpp  # PQ, HLG, sRGB encode/decode round-trips
│   │   ├── ColorMatrixTests.cpp       # BT.709/2020/P3 matrix round-trips
│   │   ├── MobiusReinhardTests.cpp    # ICtCp tone-map curve invariants
│   │   ├── DeltaETests.cpp            # Sharma reference pairs for CIEDE2000
│   │   └── GamutTests.cpp             # CIE xy boundary tests
│   ├── RunMathTests.ps1            # Local runner for the math test bench
│   ├── RunHeadlessSmoke.ps1        # CI smoke (PNG + FP32 pixels + script batch)
│   └── fixtures/test_cli_basic.json   # Golden graph for headless smoke
│
├── ShaderLabHeadless/
│   └── Main.cpp                    # Console host: PNG render / --pixels / --script
│
├── Graph/                          # Engine: effect graph data model
│   ├── NodeType.h                  # NodeType enum
│   ├── PropertyValue.h             # std::variant type for node properties
│   ├── EffectNode.h                # EffectNode struct, ParameterDefinition, AnalysisFieldDef
│   ├── EffectEdge.h                # EffectEdge struct
│   ├── EffectGraph.h / .cpp        # DAG, topological sort, JSON, versioning
│
├── Rendering/                      # Engine: rendering + analysis (RenderEngine stays app-side)
│   ├── DisplayInfo.h               # DisplayCapabilities struct
│   ├── DisplayMonitor.h / .cpp     # WM_DISPLAYCHANGE + adapter-changed event + simulated profile
│   ├── DisplayProfile.h            # DisplayProfile struct + preset factories
│   ├── IccProfileParser.h / .cpp   # mscms.dll-based ICC reader
│   ├── PipelineFormat.h            # PipelineFormat struct (scRGB FP16 always)
│   ├── RenderEngine.h / .cpp       # App-only D3D11 + D2D1 + swap chain lifecycle
│   ├── GraphEvaluator.h / .cpp     # Topological walk, effect cache, dirty gating, D3D11 dispatch
    │   ├── D3D11ComputeRunner.h / .cpp # Generic D3D11 compute dispatch for user shaders
│   ├── D3D11ComputeRunner.h / .cpp # Generic D3D11 compute dispatch for user shaders
│   ├── PixelReadback.h / .cpp      # Engine helper: FP32 RGBA region readback
│   ├── CaptureNode.h / .cpp        # Engine helper: D2D + WIC PNG encode of any node's output
│   ├── WorkingSpaceSync.h / .cpp   # Engine helper: refresh Working Space parameter nodes
│   ├── MathExpression.h / .cpp     # ExprTk-backed expression evaluator (PCH disabled on .cpp)
│
├── Effects/                        # Engine: built-in effect wrappers + custom effect base
│   ├── ShaderLabEffects.h / .cpp   # 20+ ShaderLab effects (versioned) — embedded HLSL
│   ├── ColorMath.cpp               # Shared HLSL color math library (extracted from ShaderLabEffects)
    │   ├── ShaderLabEffects.h / .cpp   # 20+ ShaderLab effects (versioned) — embedded HLSL
│   ├── PropertyMetadata.h          # Effect property metadata for UI generation
│   ├── ImageLoader.h / .cpp        # WIC HDR/SDR image loading
│   ├── SourceNodeFactory.h / .cpp  # Source node creation (image / video / flood / DXGI / WGC) + per-frame tick
│   ├── EffectRegistry.h / .cpp     # 40+ built-in D2D effect registrations (9 categories)
│   ├── ShaderCompiler.h / .cpp     # D3DCompile + D3DReflect wrapper
│   ├── CustomPixelShaderEffect.h / .cpp     # ID2D1EffectImpl + ID2D1DrawTransform for user pixel shaders
│   ├── CustomComputeShaderEffect.h / .cpp   # ID2D1EffectImpl + ID2D1ComputeTransform for user D2D compute
│   ├── DxgiDuplicationSourceProvider.h / .cpp        # Live-capture provider for DXGI Desktop Duplication
│   ├── VideoSourceProvider.h / .cpp                  # Media Foundation video decode + frame upload
│   └── WindowsGraphicsCaptureSourceProvider.h / .cpp # Live-capture provider for the WinUI graphics-capture picker (app-side)
│
├── Controls/                       # App-only: UI controllers (decoupled from XAML views)
│   ├── OutputWindow.h / .cpp           # Per-Output-node OS window (independent SwapChainPanel)
│   ├── ShaderEditorController.h / .cpp # HLSL compile + D3DReflect auto-property generation
│   ├── NodeGraphController.h / .cpp    # Canvas node graph editor (D2D bezier edges, hit-test, pan/zoom)
│   ├── PixelInspectorController.h / .cpp # GPU readback (1×1 D2D1Bitmap1 → scRGB / sRGB / PQ / luminance)
│   ├── PixelTraceController.h / .cpp   # Recursive pixel trace through effect graph
│   ├── LogWindow.h / .cpp              # Per-node log overlay
│   └── NodeLog.h                       # NodeLog data structure
│
├── Shaders/                        # HLSL source files (user shaders)
├── Assets/                         # App icons, splash screen
├── third_party/
│   └── exprtk/                     # exprtk.hpp (downloaded by EnsureExprTk.ps1, gitignored)
├── scripts/
│   ├── EnsureDevCert.ps1           # Generates + installs CN=ShaderLab dev cert for F5
│   ├── EnsureExprTk.ps1            # Downloads exprtk.hpp on first build
│   └── Install.ps1                 # Per-arch unsigned-MSIX installer for end users
├── .github/
│   ├── workflows/
│   │   ├── ci.yml                  # PR / push CI build + tests + bootstrap-smoke
│   │   └── release.yml             # Tagged-release matrix (x64 + ARM64)
│   └── copilot-instructions.md
├── x64\Debug\ShaderLabEngine\      # Engine DLL output
├── x64\Debug\ShaderLab\            # WinUI app output
├── x64\Debug\ShaderLabTests\       # Console test output
├── x64\Debug\ShaderLabHeadless\    # Console host output
└── packages/                       # NuGet packages (restored)
```


---

Back to [docs/](../README.md) • [Repo root](../../README.md)