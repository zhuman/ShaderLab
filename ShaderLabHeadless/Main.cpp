// ShaderLabHeadless — console host for the ShaderLab engine
// ============================================================
//
// Render any node of an .effectgraph (JSON) to a PNG file with no
// SwapChainPanel / WinUI dependency. Phase 7 deliverable. The Phase 7
// spike (Tests/TestRunner.cpp::TestHeadlessReadback, commit 9f14401)
// proved a single ID3D11Device + ID2D1DeviceContext is enough to
// evaluate a graph and read pixels back; this binary packages that
// capability as a CLI so the empirical fidelity loop (Working Space +
// Delta E + Luminance Statistics) can run without a logged-in user.
//
// Usage:
//     ShaderLabHeadless --graph PATH --node ID --output PNG_PATH [options]
//
// Required arguments:
//     --graph PATH    .effectgraph JSON file (zip/embedded media not yet supported)
//     --node ID       Numeric node id from the graph to render
//     --output PATH   PNG output path
//
// Options:
//     --width N       Output width in pixels (default: 1024)
//     --height N      Output height in pixels (default: 1024)
//     --adapter X     'warp' or 'default' (default: 'default'; CI uses warp)
//     --port N        MCP port (reserved for the full Phase 7 MCP migration; not yet active)
//
// Exit code: 0 on success, non-zero on any failure.
//
// **Not yet implemented (queued for future work):**
//   * .effectgraph zip archives with embedded media (only plain JSON for v1)
//   * MCP HTTP server (the full move from MainWindow.McpRoutes.cpp is queued)
//   * --script JSON file for batch parameter sweeps
//   * HDR-preserving JXR output (PNG truncates above 1.0 scRGB)
//
// What it DOES prove: the engine, graph evaluator, custom-effect cache,
// and pixel readback path all work without any UI thread or swap chain.

#include "pch_engine.h"
#include "EngineExport.h"
#include "Graph/EffectGraph.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/DisplayMonitor.h"
#include "Effects/EffectRegistry.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/BytecodeCache.h"
#include "Effects/Performance.h"
#include "Rendering/PixelReadback.h"
#include "Engine/Mcp/McpHttpServer.h"
#include "Engine/Mcp/EngineMcpRoutes.h"

#include <winrt/Windows.Data.Json.h>
#include <wincodec.h>
#include <shlobj.h>
#include <KnownFolders.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    struct Args
    {
        std::wstring graphPath;
        uint32_t     nodeId{ 0 };
        bool         hasNodeId{ false };
        std::wstring outputPath;
        uint32_t     width{ 1024 };
        uint32_t     height{ 1024 };
        bool         useWarp{ false };
        uint16_t     mcpPort{ 47809 };  // q-p7-mcp-port-conflict default
        // D2D HdrToneMap parameters: InputMaxLuminance is the peak nit
        // value of the source content; OutputMaxLuminance is the peak
        // nit value the SDR PNG can represent (80 == scRGB 1.0). The
        // tone mapper saturates inputs above 1000 nits to 80 nits SDR
        // by default, which is what we want for visual inspection of
        // HDR test patterns rendered through the engine.
        float        inputPeakNits{ 1000.0f };
        float        outputPeakNits{ 80.0f };
        // Skip the tone map entirely (raw scRGB -> sRGB clamp). Useful
        // when the graph already produced SDR-range output and we
        // don't want HdrToneMap's mid-tone lift muddying the result.
        bool         skipToneMap{ false };
        // FP32 RGBA pixel-region readback (alternate output mode).
        // When set, --output is interpreted as a raw FP32 binary blob
        // (extension .bin / .raw) or a CSV file (extension .csv). No
        // PNG / tonemap path is used. Designed for MCP-driven full-
        // accuracy sampling and quantitative analysis.
        bool         pixelMode{ false };
        int32_t      pixelX{ 0 };
        int32_t      pixelY{ 0 };
        uint32_t     pixelW{ 1 };
        uint32_t     pixelH{ 1 };

        // Script batch mode. When set, the host loads a graph and then
        // walks an array of MCP-style operations (set-property, image-
        // stats, capture-node, pixel-region, ...) writing per-step
        // responses out as a JSON document. Designed for parameter
        // sweeps where the agent wants 50+ engine queries per session
        // without paying HTTP round-trip overhead each one.
        std::wstring scriptPath;
        std::wstring scriptOutputPath;  // empty -> stdout

        // p8-cache-reaper: bytecode-cache management modes. When set,
        // the headless host runs the requested op then exits without
        // loading a graph or starting MCP. Useful for CI / cleanup.
        bool          reapShaderCache{ false };
        bool          clearShaderCache{ false };
        uint64_t      reapStaleSec{ 90ull * 24 * 60 * 60 };  // 90 days default

        // p8-feature-flag: opt in to the Phase 8 GPU-binding fast path
        // for the duration of this run. Persists no state; affects only
        // this process. Defaults to engine default (off for v1.6).
        std::optional<bool> enableGpuBindings;
    };

    void PrintUsage(const wchar_t* exeName)
    {
        std::wprintf(
L"Usage: %ls --graph PATH --node ID --output PNG_PATH [options]\n"
L"\n"
L"Required:\n"
L"  --graph PATH    .effectgraph JSON file\n"
L"  --node ID       Numeric node id to render\n"
L"  --output PATH   PNG output path\n"
L"\n"
L"Options:\n"
L"  --width N                Output width (default: 1024)\n"
L"  --height N               Output height (default: 1024)\n"
L"  --adapter X              'warp' or 'default' (default: default)\n"
L"  --port N                 Reserved for MCP server (default: 47809)\n"
L"  --input-peak-nits N      D2D HdrToneMap input peak (default: 1000)\n"
L"  --output-peak-nits N     D2D HdrToneMap output peak (default: 80 = SDR)\n"
L"  --no-tonemap             Skip HdrToneMap, raw scRGB -> sRGB clamp\n"
L"\n"
L"Pixel-region readback mode (raw FP32 RGBA, no tonemap):\n"
L"  --pixels x,y,w,h         Read a region from the node's output as\n"
L"                           FP32 RGBA, write to --output. Output\n"
L"                           extension .bin/.raw -> packed binary\n"
L"                           (W,H header as two uint32, then floats);\n"
L"                           .csv -> text 'x,y,r,g,b,a' rows.\n"
L"                           Designed for MCP-driven full-accuracy\n"
L"                           sampling. Region is clipped to image bounds.\n"
L"\n"
L"Script batch mode (parameter sweeps without HTTP round-trips):\n"
L"  --script PATH            JSON file with an array of MCP ops to run\n"
L"                           against the loaded graph. Each op is either\n"
L"                           {method,path,body} (raw HTTP shape) or\n"
L"                           {op,...} shorthand (set-property, pixel-region,\n"
L"                           pixel-region, capture-node, render).\n"
L"                           In script mode --node and --output are not\n"
L"                           required; instead provide --script-output for\n"
L"                           the per-step response document.\n"
L"  --script-output PATH     Write the JSON response document here\n"
L"                           (default: stdout).\n"
L"\n"
L"Bytecode cache management (Phase 8 cache reaper):\n"
L"  --reap-shader-cache      Delete cached shader bytecode (.cso) under\n"
L"                           %%LOCALAPPDATA%%\\ShaderLab\\bytecode\\ that has not\n"
L"                           been accessed within --reap-shader-cache-stale-sec\n"
L"                           seconds. Reports filesDeleted/bytesFreed JSON.\n"
L"                           Exits without loading a graph.\n"
L"  --clear-shader-cache     Delete every cached shader bytecode entry.\n"
L"                           Exits without loading a graph.\n"
L"  --reap-shader-cache-stale-sec N\n"
L"                           Threshold in seconds for --reap-shader-cache\n"
L"                           (default: 7776000 = 90 days).\n"
L"\n"
L"GPU-binding fast path (Phase 8 feature flag, default ON in v1.6):\n"
L"  --enable-gpu-bindings    Force GPU bindings on (default).\n"
L"                           The evaluator routes upstream-effect SRVs\n"
L"                           directly to D3D11 compute consumers'\n"
L"                           t-slots; pixel shader consumers fall back\n"
L"                           to CPU readback gracefully. Telemetry via\n"
L"                           Performance::GpuBindingDetections().\n"
L"  --disable-gpu-bindings   Force GPU bindings off; every binding goes\n"
L"                           through CPU readback (the pre-v1.6 path).\n",
            exeName);
    }

    // Bare-bones argv parsing. Each flag takes one positional argument.
    // Unknown flags fail closed with a usage hint so typos don't silently
    // produce wrong output.
    bool ParseArgs(int argc, wchar_t* argv[], Args& out)
    {
        for (int i = 1; i < argc; ++i)
        {
            std::wstring_view a = argv[i];
            auto needNext = [&](const wchar_t* name) -> wchar_t* {
                if (i + 1 >= argc) {
                    std::wprintf(L"ERROR: %ls requires an argument\n", name);
                    return nullptr;
                }
                return argv[++i];
            };
            if (a == L"--graph")        { auto v = needNext(L"--graph"); if (!v) return false; out.graphPath = v; }
            else if (a == L"--node")    { auto v = needNext(L"--node"); if (!v) return false; out.nodeId = static_cast<uint32_t>(std::wcstoul(v, nullptr, 10)); out.hasNodeId = true; }
            else if (a == L"--output")  { auto v = needNext(L"--output"); if (!v) return false; out.outputPath = v; }
            else if (a == L"--width")   { auto v = needNext(L"--width"); if (!v) return false; out.width = static_cast<uint32_t>(std::wcstoul(v, nullptr, 10)); }
            else if (a == L"--height")  { auto v = needNext(L"--height"); if (!v) return false; out.height = static_cast<uint32_t>(std::wcstoul(v, nullptr, 10)); }
            else if (a == L"--adapter") { auto v = needNext(L"--adapter"); if (!v) return false; out.useWarp = (std::wstring_view{v} == L"warp"); }
            else if (a == L"--port")    { auto v = needNext(L"--port"); if (!v) return false; out.mcpPort = static_cast<uint16_t>(std::wcstoul(v, nullptr, 10)); }
            else if (a == L"--input-peak-nits")  { auto v = needNext(L"--input-peak-nits"); if (!v) return false; out.inputPeakNits = static_cast<float>(std::wcstod(v, nullptr)); }
            else if (a == L"--output-peak-nits") { auto v = needNext(L"--output-peak-nits"); if (!v) return false; out.outputPeakNits = static_cast<float>(std::wcstod(v, nullptr)); }
            else if (a == L"--no-tonemap") { out.skipToneMap = true; }
            else if (a == L"--pixels")
            {
                auto v = needNext(L"--pixels"); if (!v) return false;
                // Parse "x,y,w,h" into the four fields.
                wchar_t* p = const_cast<wchar_t*>(v);
                int32_t  parsed[4] = { 0, 0, 0, 0 };
                for (int idx = 0; idx < 4; ++idx)
                {
                    wchar_t* end = nullptr;
                    long val = std::wcstol(p, &end, 10);
                    if (end == p) {
                        std::wprintf(L"ERROR: --pixels requires 'x,y,w,h' (got '%ls')\n", v);
                        return false;
                    }
                    parsed[idx] = static_cast<int32_t>(val);
                    p = end;
                    if (*p == L',') ++p;
                    else if (idx < 3) {
                        std::wprintf(L"ERROR: --pixels requires 4 comma-separated values (got '%ls')\n", v);
                        return false;
                    }
                }
                if (parsed[2] <= 0 || parsed[3] <= 0) {
                    std::wprintf(L"ERROR: --pixels w and h must be positive (got %d,%d)\n", parsed[2], parsed[3]);
                    return false;
                }
                out.pixelX = parsed[0];
                out.pixelY = parsed[1];
                out.pixelW = static_cast<uint32_t>(parsed[2]);
                out.pixelH = static_cast<uint32_t>(parsed[3]);
                out.pixelMode = true;
            }
            else if (a == L"--script")        { auto v = needNext(L"--script"); if (!v) return false; out.scriptPath = v; }
            else if (a == L"--script-output") { auto v = needNext(L"--script-output"); if (!v) return false; out.scriptOutputPath = v; }
            else if (a == L"--reap-shader-cache")   { out.reapShaderCache = true; }
            else if (a == L"--clear-shader-cache")  { out.clearShaderCache = true; }
            else if (a == L"--reap-shader-cache-stale-sec")
            {
                auto v = needNext(L"--reap-shader-cache-stale-sec"); if (!v) return false;
                out.reapStaleSec = static_cast<uint64_t>(std::wcstoull(v, nullptr, 10));
            }
            else if (a == L"--enable-gpu-bindings")  { out.enableGpuBindings = true;  }
            else if (a == L"--disable-gpu-bindings") { out.enableGpuBindings = false; }
            else if (a == L"--help" || a == L"-h" || a == L"-?") { PrintUsage(argv[0]); return false; }
            else { std::wprintf(L"ERROR: unknown argument '%ls'\n", argv[i]); return false; }
        }
        // Required-arg policy depends on mode:
        //   cache mode   -> no required args
        //   script mode  -> --graph + --script   (--node, --output ignored)
        //   render mode  -> --graph + --node + --output
        if (out.reapShaderCache || out.clearShaderCache)
        {
            // No further validation -- cache modes don't need a graph.
        }
        else if (!out.scriptPath.empty())
        {
            if (out.graphPath.empty()) {
                std::wprintf(L"ERROR: --graph is required (script mode)\n");
                return false;
            }
        }
        else if (out.graphPath.empty() || out.outputPath.empty() || !out.hasNodeId)
        {
            std::wprintf(L"ERROR: --graph, --node, and --output are required\n");
            return false;
        }
        return true;
    }

    // Read entire file into a string. Returns empty string on failure
    // (caller checks). Strips a leading UTF-8 BOM if present so the JSON
    // parser doesn't fail on it; .effectgraph files saved by the GUI app
    // start with a BOM.
    std::string ReadFileUtf8(const std::wstring& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string s = ss.str();
        if (s.size() >= 3 &&
            static_cast<uint8_t>(s[0]) == 0xEF &&
            static_cast<uint8_t>(s[1]) == 0xBB &&
            static_cast<uint8_t>(s[2]) == 0xBF)
        {
            s.erase(0, 3);
        }
        return s;
    }

    // Encode an FP32 RGBA buffer as PNG via WIC. PNG is 8-bit per channel;
    // values are gamma-encoded sRGB after a clamp to [0, 1]. This is lossy
    // for HDR scRGB output (anything above 1.0 saturates to 255). For HDR
    // fidelity, switch to JXR (D2D's native FP16 path) -- queued.
    HRESULT SaveFp32AsPng(IWICImagingFactory* wic,
        const float* rgba, uint32_t w, uint32_t h, uint32_t pitchBytes,
        const std::wstring& path)
    {
        // Convert FP32 RGBA to sRGB-gamma 8-bit BGRA.
        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
        auto sRgbEncode = [](float c) -> uint8_t {
            float clamped = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
            float enc = (clamped <= 0.0031308f)
                ? clamped * 12.92f
                : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
            int v = static_cast<int>(enc * 255.0f + 0.5f);
            return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        };
        for (uint32_t y = 0; y < h; ++y)
        {
            const float* srcRow = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(rgba) + y * pitchBytes);
            uint8_t* dstRow = bgra.data() + y * w * 4;
            for (uint32_t x = 0; x < w; ++x)
            {
                const float* px = srcRow + x * 4;
                dstRow[x * 4 + 0] = sRgbEncode(px[2]); // B
                dstRow[x * 4 + 1] = sRgbEncode(px[1]); // G
                dstRow[x * 4 + 2] = sRgbEncode(px[0]); // R
                dstRow[x * 4 + 3] = static_cast<uint8_t>(
                    px[3] < 0.0f ? 0 : (px[3] > 1.0f ? 255 : static_cast<int>(px[3] * 255.0f + 0.5f)));
            }
        }

        winrt::com_ptr<IWICStream> stream;
        HRESULT hr = wic->CreateStream(stream.put());
        if (FAILED(hr)) return hr;
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) return hr;

        winrt::com_ptr<IWICBitmapEncoder> encoder;
        hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
        if (FAILED(hr)) return hr;
        hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) return hr;

        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        hr = encoder->CreateNewFrame(frame.put(), nullptr);
        if (FAILED(hr)) return hr;
        hr = frame->Initialize(nullptr);
        if (FAILED(hr)) return hr;
        hr = frame->SetSize(w, h);
        if (FAILED(hr)) return hr;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&fmt);
        if (FAILED(hr)) return hr;
        hr = frame->WritePixels(h, w * 4, w * h * 4, bgra.data());
        if (FAILED(hr)) return hr;
        hr = frame->Commit();
        if (FAILED(hr)) return hr;
        return encoder->Commit();
    }
}

int wmain(int argc, wchar_t* argv[]);

int RunScript(const Args& args);

// Render path extracted into a function so all locals (D2D bitmaps,
// GraphEvaluator, com_ptrs) destruct cleanly before wmain calls
// MFShutdown / uninit_apartment. The dangling order otherwise produced
// hangs on D2D device teardown.
int RunRender(const Args& args)
{
    UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    winrt::com_ptr<ID3D11Device> baseDevice;
    winrt::com_ptr<ID3D11DeviceContext> baseCtx;
    HRESULT hr = D3D11CreateDevice(nullptr,
        args.useWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, d3dFlags,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseCtx.put());
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: D3D11CreateDevice failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 3;
    }
    auto d3dDevice = baseDevice.as<ID3D11Device5>();
    auto d3dContext = baseCtx.as<ID3D11DeviceContext4>();

    // Match the test runner: enable D3D10 multithread protection so any
    // background-thread DXVA2 work the engine might spin up doesn't crash.
    winrt::com_ptr<ID3D10Multithread> mt;
    d3dDevice.as(mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    winrt::com_ptr<ID2D1Factory7> d2dFactory;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory7), reinterpret_cast<void**>(d2dFactory.put()));

    winrt::com_ptr<IDXGIDevice> dxgiDev;
    baseDevice->QueryInterface(dxgiDev.put());
    winrt::com_ptr<ID2D1Device6> d2dDevice;
    d2dFactory->CreateDevice(dxgiDev.as<IDXGIDevice>().get(),
        reinterpret_cast<ID2D1Device**>(d2dDevice.put()));
    winrt::com_ptr<ID2D1DeviceContext5> dc;
    d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        reinterpret_cast<ID2D1DeviceContext**>(dc.put()));

    // Register custom effect classes with the D2D factory (CustomPixelShader,
    // CustomComputeShader). Without this, custom-effect
    // nodes in the graph fail to instantiate.
    winrt::com_ptr<ID2D1Factory1> factory1;
    d2dFactory->QueryInterface(factory1.put());
    ShaderLab::Effects::RegisterEngineD2DEffects(factory1.get());

    // ---- Load the graph ----------------------------------------------------
    auto graphJson = ReadFileUtf8(args.graphPath);
    if (graphJson.empty()) {
        std::wprintf(L"FATAL: could not read graph file '%ls'\n", args.graphPath.c_str());
        return 4;
    }
    // Convert UTF-8 to UTF-16 for FromJson (winrt::hstring).
    int wcCount = MultiByteToWideChar(CP_UTF8, 0, graphJson.data(),
        static_cast<int>(graphJson.size()), nullptr, 0);
    std::wstring graphJsonW(wcCount, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, graphJson.data(),
        static_cast<int>(graphJson.size()), graphJsonW.data(), wcCount);

    ShaderLab::Graph::EffectGraph graph;
    try {
        graph = ShaderLab::Graph::EffectGraph::FromJson(winrt::hstring(graphJsonW));
    } catch (winrt::hresult_error const& e) {
        std::wprintf(L"FATAL: graph JSON parse failed (0x%08X): %ls\n",
            static_cast<uint32_t>(e.code()), e.message().c_str());
        return 5;
    }

    if (!graph.FindNode(args.nodeId)) {
        std::wprintf(L"FATAL: node id %u not found in graph\n", args.nodeId);
        return 6;
    }

    // ---- Evaluate the graph (long-lived evaluator!) ------------------------
    // The evaluator owns the per-node ID2D1Effect cache; each node's
    // cachedOutput is a non-owning pointer into that cache. Keep it alive
    // until after readback. (See Phase 7 spike for the lifetime gotcha.)
    ShaderLab::Effects::SourceNodeFactory sourceFactory;
    ShaderLab::Rendering::GraphEvaluator evaluator;

    graph.MarkAllDirty();
    for (auto& node : const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(graph.Nodes()))
    {
        if (node.type == ShaderLab::Graph::NodeType::Source)
        {
            try {
                sourceFactory.PrepareSourceNode(node, dc.get(), 0.0,
                    d3dDevice.get(), d3dContext.get());
            } catch (...) {
                // Source prep can fail (missing media, etc); the evaluator
                // will report cachedOutput=nullptr below if so.
            }
        }
    }
    // Two-pass evaluate inside an active D2D draw session so that
    // DispatchUserD3D11Compute's internal DrawImage actually renders.
    // (Outside BeginDraw/EndDraw, DrawImage silently no-ops and any
    // D3D11 compute analysis reads black input.)
    dc->SetTarget(nullptr);
    dc->BeginDraw();
    evaluator.Evaluate(graph, dc.get());
    evaluator.Evaluate(graph, dc.get());
    evaluator.ProcessDeferredCompute(graph, dc.get());
    if (graph.HasDirtyNodes())
        evaluator.Evaluate(graph, dc.get());
    dc->EndDraw();

    auto* node = graph.FindNode(args.nodeId);
    if (!node->cachedOutput) {
        std::wprintf(L"FATAL: node %u has no cachedOutput after evaluate (missing inputs or eval error)\n", args.nodeId);
        if (!node->runtimeError.empty()) {
            std::wprintf(L"        node.runtimeError = %ls\n", node->runtimeError.c_str());
        }
        return 7;
    }

    // ---- Pixel-region readback mode (alternate output path) ----------------
    // No tonemap, no PNG encode -- raw FP32 RGBA pixels for full-accuracy
    // analysis (designed to be the engine path the future MCP
    // read_pixel_region route uses; see p7-mcp-move).
    if (args.pixelMode)
    {
        auto rr = ::ShaderLab::Rendering::ReadPixelRegion(
            graph, args.nodeId,
            args.pixelX, args.pixelY, args.pixelW, args.pixelH,
            dc.get());
        switch (rr.status)
        {
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::Success: break;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::NotFound:
            std::wprintf(L"FATAL: pixel readback NotFound for node %u\n", args.nodeId);
            return 7;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::NotReady:
            std::wprintf(L"FATAL: pixel readback NotReady for node %u (dirty or missing inputs)\n", args.nodeId);
            return 7;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::InvalidRegion:
            std::wprintf(L"FATAL: pixel readback InvalidRegion (clipped to nothing inside image)\n");
            return 8;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::D2DError:
        default:
            std::wprintf(L"FATAL: pixel readback D2DError\n");
            return 10;
        }

        // Pick output format from extension. .csv -> text rows.
        // .bin/.raw/anything-else -> packed binary (uint32 W, uint32 H,
        // then float[W*H*4] RGBA row-major).
        auto endsWith = [](const std::wstring& s, const wchar_t* suffix) {
            size_t sl = std::wcslen(suffix);
            return s.size() >= sl && std::equal(s.end() - sl, s.end(), suffix);
        };
        bool csv = endsWith(args.outputPath, L".csv") || endsWith(args.outputPath, L".CSV");

        std::ofstream f(args.outputPath, std::ios::binary);
        if (!f) {
            std::wprintf(L"FATAL: could not open output '%ls'\n", args.outputPath.c_str());
            return 13;
        }
        if (csv)
        {
            f << "x,y,r,g,b,a\n";
            char buf[256];
            for (uint32_t row = 0; row < rr.actualHeight; ++row) {
                for (uint32_t col = 0; col < rr.actualWidth; ++col) {
                    const float* px = rr.pixels.data()
                        + (static_cast<size_t>(row) * rr.actualWidth + col) * 4;
                    int n = std::snprintf(buf, sizeof(buf),
                        "%d,%d,%.9g,%.9g,%.9g,%.9g\n",
                        args.pixelX + static_cast<int32_t>(col),
                        args.pixelY + static_cast<int32_t>(row),
                        px[0], px[1], px[2], px[3]);
                    f.write(buf, n);
                }
            }
        }
        else
        {
            // Binary header so consumers can self-describe (uint32 W,
            // uint32 H = the *actual* clipped region, in case the
            // request extended past the image edge).
            uint32_t hdr[2] = { rr.actualWidth, rr.actualHeight };
            f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
            f.write(reinterpret_cast<const char*>(rr.pixels.data()),
                static_cast<std::streamsize>(rr.pixels.size() * sizeof(float)));
        }
        f.close();

        std::wprintf(L"OK: read %ux%u pixel region (%s) from node %u -> %ls\n",
            rr.actualWidth, rr.actualHeight, csv ? L"csv" : L"binary",
            args.nodeId, args.outputPath.c_str());
        return 0;
    }

    // ---- Render to FP32 target + readback ----------------------------------
    D2D1_BITMAP_PROPERTIES1 targetProps{};
    targetProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
    targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    targetProps.dpiX = 96.0f;
    targetProps.dpiY = 96.0f;
    winrt::com_ptr<ID2D1Bitmap1> target;
    hr = dc->CreateBitmap(D2D1::SizeU(args.width, args.height),
        nullptr, 0, targetProps, target.put());
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: CreateBitmap (target) failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 8;
    }

    D2D1_BITMAP_PROPERTIES1 stagingProps = targetProps;
    stagingProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    winrt::com_ptr<ID2D1Bitmap1> staging;
    hr = dc->CreateBitmap(D2D1::SizeU(args.width, args.height),
        nullptr, 0, stagingProps, staging.put());
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: CreateBitmap (staging) failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 9;
    }

    // Build the input image for the render pass: either the cached
    // output directly (--no-tonemap) or the same image piped through
    // a CLSID_D2D1HdrToneMap effect (default). The HdrToneMap effect's
    // documented behavior is "fixed BT.2408-style mid-tone lift" -- see
    // README decision log #52 for the empirical analysis. For PNG
    // visual-inspection output we want the lift; for raw scRGB pixel
    // sampling use --no-tonemap (or, future work, the FP16 readback
    // path tracked as p7-headless-fp16-pixel-readback).
    winrt::com_ptr<ID2D1Effect> toneMap;
    winrt::com_ptr<ID2D1Image> toneMappedOut;
    ID2D1Image* renderInput = node->cachedOutput;
    if (!args.skipToneMap)
    {
        hr = dc->CreateEffect(CLSID_D2D1HdrToneMap, toneMap.put());
        if (FAILED(hr)) {
            std::wprintf(L"FATAL: CreateEffect(HdrToneMap) failed 0x%08X\n", static_cast<uint32_t>(hr));
            return 10;
        }
        toneMap->SetInput(0, node->cachedOutput);
        toneMap->SetValue(D2D1_HDRTONEMAP_PROP_INPUT_MAX_LUMINANCE,  args.inputPeakNits);
        toneMap->SetValue(D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE, args.outputPeakNits);
        toneMap->SetValue(D2D1_HDRTONEMAP_PROP_DISPLAY_MODE,
            (args.outputPeakNits <= 80.0f)
                ? D2D1_HDRTONEMAP_DISPLAY_MODE_SDR
                : D2D1_HDRTONEMAP_DISPLAY_MODE_HDR);
        toneMap->GetOutput(toneMappedOut.put());
        renderInput = toneMappedOut.get();
    }

    float oldDpiX, oldDpiY;
    dc->GetDpi(&oldDpiX, &oldDpiY);
    dc->SetDpi(96.0f, 96.0f);
    dc->SetTarget(target.get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawImage(renderInput);
    hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    dc->SetDpi(oldDpiX, oldDpiY);
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: D2D EndDraw failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 10;
    }

    D2D1_POINT_2U dstPt = { 0, 0 };
    D2D1_RECT_U srcRect = { 0, 0, args.width, args.height };
    hr = staging->CopyFromBitmap(&dstPt, target.get(), &srcRect);
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: CopyFromBitmap failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 11;
    }

    D2D1_MAPPED_RECT mapped{};
    hr = staging->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: Map staging failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 12;
    }

    // ---- Encode PNG via WIC ------------------------------------------------
    winrt::com_ptr<IWICImagingFactory> wic;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wic.put()));
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: WIC factory failed 0x%08X\n", static_cast<uint32_t>(hr));
        staging->Unmap();
        return 13;
    }

    hr = SaveFp32AsPng(wic.get(),
        reinterpret_cast<const float*>(mapped.bits),
        args.width, args.height, mapped.pitch,
        args.outputPath);
    staging->Unmap();
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: SaveFp32AsPng failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 14;
    }

    std::wprintf(L"OK: rendered node %u (%ux%u) -> %ls\n",
        args.nodeId, args.width, args.height, args.outputPath.c_str());

    // Explicit teardown order: drop the D2D/D3D resources before
    // ::MFShutdown / uninit_apartment so the destructors don't race
    // shutdown. Symptom that motivated this: the process hung after the
    // OK print on first runs.
    staging = nullptr;
    target = nullptr;
    wic = nullptr;
    dc = nullptr;
    d2dDevice = nullptr;
    dxgiDev = nullptr;
    d2dFactory = nullptr;
    factory1 = nullptr;
    d3dContext = nullptr;
    d3dDevice = nullptr;
    mt = nullptr;
    baseCtx = nullptr;
    baseDevice = nullptr;
    return 0;
}

// Headless command sink — synchronous Dispatch (no UI thread to marshal
// to), all event hooks are no-ops since headless doesn't have a canvas
// or status bar to keep in sync. The engine routes were designed so
// that with this sink + a properly populated EngineContext, every
// route migrated in Phase 7 works identically to the GUI host.
struct HeadlessSink : ShaderLab::Mcp::IEngineCommandSink
{
    std::function<void(ShaderLab::Mcp::EngineContext&)> populateContext;

    ShaderLab::McpHttpServer::Response Dispatch(
        std::function<ShaderLab::McpHttpServer::Response(
            ShaderLab::Mcp::EngineContext&)> closure) override
    {
        ShaderLab::Mcp::EngineContext ctx{};
        populateContext(ctx);
        return closure(ctx);
    }
};

// Walk a script JSON document of MCP-style operations, dispatching each
// through the engine route registry. Output is a JSON document with one
// {step, status, body} entry per operation.
//
// Accepted step shapes:
//   {"method":"GET|POST", "path":"/...", "body": <object|string>}
//   {"op":"<shorthand>", ...key/value args...}
//
// Shorthand mappings (so common sweep ops don't need the verbose form):
//   set-property   -> POST /graph/set-property
//   pixel-region   -> POST /render/pixel-region
//   capture-node   -> POST /render/capture-node
//   render         -> internal: forces a fresh evaluation (useful as a
//                     barrier between mutations and readbacks).
//   get-graph      -> GET  /graph
//   get-node       -> GET  /graph/node/{nodeId}
//   analysis       -> GET  /analysis/{nodeId}
int RunScript(const Args& args)
{
    namespace WDJ = winrt::Windows::Data::Json;

    // ---- Engine setup (same shape as RunRender) ---------------------------
    UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    winrt::com_ptr<ID3D11Device> baseDevice;
    winrt::com_ptr<ID3D11DeviceContext> baseCtx;
    HRESULT hr = D3D11CreateDevice(nullptr,
        args.useWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, d3dFlags,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseCtx.put());
    if (FAILED(hr)) {
        std::wprintf(L"FATAL: D3D11CreateDevice failed 0x%08X\n", static_cast<uint32_t>(hr));
        return 3;
    }
    auto d3dDevice = baseDevice.as<ID3D11Device5>();
    auto d3dContext = baseCtx.as<ID3D11DeviceContext4>();

    winrt::com_ptr<ID3D10Multithread> mt;
    d3dDevice.as(mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    winrt::com_ptr<ID2D1Factory7> d2dFactory;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory7), reinterpret_cast<void**>(d2dFactory.put()));

    winrt::com_ptr<IDXGIDevice> dxgiDev;
    baseDevice->QueryInterface(dxgiDev.put());
    winrt::com_ptr<ID2D1Device6> d2dDevice;
    d2dFactory->CreateDevice(dxgiDev.as<IDXGIDevice>().get(),
        reinterpret_cast<ID2D1Device**>(d2dDevice.put()));
    winrt::com_ptr<ID2D1DeviceContext5> dc;
    d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        reinterpret_cast<ID2D1DeviceContext**>(dc.put()));

    winrt::com_ptr<ID2D1Factory1> factory1;
    d2dFactory->QueryInterface(factory1.put());
    ShaderLab::Effects::RegisterEngineD2DEffects(factory1.get());

    // ---- Load graph -------------------------------------------------------
    auto graphJson = ReadFileUtf8(args.graphPath);
    if (graphJson.empty()) {
        std::wprintf(L"FATAL: could not read graph file '%ls'\n", args.graphPath.c_str());
        return 4;
    }
    int wcCount = MultiByteToWideChar(CP_UTF8, 0, graphJson.data(),
        static_cast<int>(graphJson.size()), nullptr, 0);
    std::wstring graphJsonW(wcCount, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, graphJson.data(),
        static_cast<int>(graphJson.size()), graphJsonW.data(), wcCount);

    ShaderLab::Graph::EffectGraph graph;
    try {
        graph = ShaderLab::Graph::EffectGraph::FromJson(winrt::hstring(graphJsonW));
    } catch (winrt::hresult_error const& e) {
        std::wprintf(L"FATAL: graph JSON parse failed (0x%08X): %ls\n",
            static_cast<uint32_t>(e.code()), e.message().c_str());
        return 5;
    }

    ShaderLab::Effects::SourceNodeFactory sourceFactory;
    ShaderLab::Rendering::GraphEvaluator evaluator;
    ShaderLab::Rendering::DisplayMonitor displayMonitor;  // headless: live caps default

    // Prep source nodes once (loads media off disk). Properties on
    // source nodes are typically static (file path); set-property on
    // them won't trigger re-load, but most sweeps target downstream
    // effect properties.
    graph.MarkAllDirty();
    for (auto& node : const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(graph.Nodes()))
    {
        if (node.type == ShaderLab::Graph::NodeType::Source)
        {
            try {
                sourceFactory.PrepareSourceNode(node, dc.get(), 0.0,
                    d3dDevice.get(), d3dContext.get());
            } catch (...) {}
        }
    }

    auto runEval = [&]() {
        // Propagate dirty flags downstream so D3D11 compute effects
        // re-dispatch when upstream sources change. Mirrors the BFS
        // walk in MainWindow::OnRenderTick -- without this, a
        // set-property on an upstream node only re-evaluates that
        // node, leaving downstream analysis nodes' cached output
        // stale.
        {
            std::vector<uint32_t> queue;
            for (const auto& node : graph.Nodes())
                if (node.dirty) queue.push_back(node.id);
            for (size_t i = 0; i < queue.size(); ++i)
            {
                for (const auto* edge : graph.GetOutputEdges(queue[i]))
                {
                    auto* dn = graph.FindNode(edge->destNodeId);
                    if (dn && !dn->dirty)
                    {
                        dn->dirty = true;
                        queue.push_back(edge->destNodeId);
                    }
                }
            }
        }

        // Wrap the evaluator passes in a D2D draw session.
        // DispatchUserD3D11Compute internally calls dc->DrawImage to
        // pre-render the upstream chain into an FP32 bitmap before
        // handing the texture off to D3D11. Without an active
        // BeginDraw/EndDraw, that DrawImage silently no-ops and the
        // compute shader reads a black texture (Min/Max/Mean = 0).
        // Mirrors MainWindow::RenderFrame's structure.
        dc->SetTarget(nullptr);
        dc->BeginDraw();
        evaluator.Evaluate(graph, dc.get());
        evaluator.Evaluate(graph, dc.get());  // two-pass for late init
        evaluator.ProcessDeferredCompute(graph, dc.get());
        // Some compute nodes mark downstream dirty; one more sweep
        // picks those up while the draw session is still active.
        if (graph.HasDirtyNodes())
            evaluator.Evaluate(graph, dc.get());
        dc->EndDraw();
    };
    runEval();  // initial frame so capture/pixel-region routes have something to read

    // ---- Build server + sink + register routes ----------------------------
    HeadlessSink sink;
    sink.populateContext = [&](ShaderLab::Mcp::EngineContext& ctx) {
        ctx.graph          = &graph;
        ctx.evaluator      = &evaluator;
        ctx.displayMonitor = &displayMonitor;
        ctx.sourceFactory  = &sourceFactory;
        ctx.dc             = dc.get();
        ctx.d3dDevice      = d3dDevice.get();
        ctx.d3dContext     = d3dContext.get();
        ctx.renderFrame    = runEval;  // routes that need fresh eval call this
    };

    ShaderLab::McpHttpServer server;
    ShaderLab::Mcp::RegisterEngineRoutes(server, sink);

    // ---- Read script JSON -------------------------------------------------
    auto scriptText = ReadFileUtf8(args.scriptPath);
    if (scriptText.empty()) {
        std::wprintf(L"FATAL: could not read script file '%ls'\n", args.scriptPath.c_str());
        return 4;
    }
    int scriptWc = MultiByteToWideChar(CP_UTF8, 0, scriptText.data(),
        static_cast<int>(scriptText.size()), nullptr, 0);
    std::wstring scriptW(scriptWc, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, scriptText.data(),
        static_cast<int>(scriptText.size()), scriptW.data(), scriptWc);

    // Script can be either a top-level array of steps OR an object with
    // a "steps" array. The latter leaves room for future top-level
    // metadata (description, version, env hints) without a breaking
    // schema change.
    WDJ::JsonArray steps{ nullptr };
    {
        WDJ::JsonValue v{ nullptr };
        if (!WDJ::JsonValue::TryParse(winrt::hstring(scriptW), v)) {
            std::wprintf(L"FATAL: script is not valid JSON\n");
            return 5;
        }
        if (v.ValueType() == WDJ::JsonValueType::Array) {
            steps = v.GetArray();
        } else if (v.ValueType() == WDJ::JsonValueType::Object) {
            auto obj = v.GetObject();
            if (!obj.HasKey(L"steps")) {
                std::wprintf(L"FATAL: script object must have a 'steps' array\n");
                return 5;
            }
            steps = obj.GetNamedArray(L"steps");
        } else {
            std::wprintf(L"FATAL: script must be a JSON array or object with 'steps'\n");
            return 5;
        }
    }

    // ---- Walk steps -------------------------------------------------------
    // Translate each step to (method, path, body string), invoke the
    // server, and accumulate {step, status, body} into a result array.
    // body is parsed as JSON when possible so consumers can navigate
    // structured fields without re-parsing.
    auto stepBodyToString = [](const WDJ::JsonValue& v) -> std::string {
        if (v.ValueType() == WDJ::JsonValueType::String)
        {
            // Already-stringified body (escape-hatch when the agent
            // wants byte-exact control over the request body).
            auto h = v.GetString();
            int n = WideCharToMultiByte(CP_UTF8, 0, h.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, h.c_str(), -1, s.data(), n, nullptr, nullptr);
            return s;
        }
        // Object / number / bool / null -> stringify.
        auto h = v.Stringify();
        int n = WideCharToMultiByte(CP_UTF8, 0, h.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, h.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    };

    WDJ::JsonArray results;
    for (uint32_t i = 0; i < steps.Size(); ++i)
    {
        WDJ::JsonObject stepObj{ nullptr };
        if (steps.GetAt(i).ValueType() != WDJ::JsonValueType::Object)
        {
            WDJ::JsonObject err;
            err.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
            err.Insert(L"status", WDJ::JsonValue::CreateNumberValue(400));
            err.Insert(L"error", WDJ::JsonValue::CreateStringValue(L"Step is not an object"));
            results.Append(err);
            continue;
        }
        stepObj = steps.GetObjectAt(i);

        std::wstring method, path;
        std::string  body;

        if (stepObj.HasKey(L"op"))
        {
            // Shorthand: translate to (method, path, body).
            auto op = std::wstring(stepObj.GetNamedString(L"op"));
            WDJ::JsonObject bodyObj;
            for (auto kv : stepObj)
            {
                if (kv.Key() == L"op") continue;
                bodyObj.Insert(kv.Key(), kv.Value());
            }
            // Stringify the body once; reused for every shorthand that
            // mirrors a POST route. Stringify() returns the canonical
            // JSON encoding so the engine routes parse it identically
            // to a real HTTP request body.
            auto bodyHstr = bodyObj.Stringify();
            int bn = WideCharToMultiByte(CP_UTF8, 0, bodyHstr.c_str(), -1,
                nullptr, 0, nullptr, nullptr);
            std::string bodyStr(bn > 0 ? bn - 1 : 0, '\0');
            if (bn > 0) WideCharToMultiByte(CP_UTF8, 0, bodyHstr.c_str(), -1,
                bodyStr.data(), bn, nullptr, nullptr);

            if      (op == L"set-property")  { method = L"POST"; path = L"/graph/set-property";  body = bodyStr; }
            else if (op == L"pixel-region")  { method = L"POST"; path = L"/render/pixel-region"; body = bodyStr; }
            else if (op == L"capture-node")  { method = L"POST"; path = L"/render/capture-node"; body = bodyStr; }
            else if (op == L"get-graph")     { method = L"GET";  path = L"/graph";               body.clear(); }
            else if (op == L"get-node")      {
                method = L"GET";
                uint32_t id = stepObj.HasKey(L"nodeId")
                    ? static_cast<uint32_t>(stepObj.GetNamedNumber(L"nodeId")) : 0;
                path = L"/graph/node/" + std::to_wstring(id);
                body.clear();
            }
            else if (op == L"analysis")      {
                method = L"GET";
                uint32_t id = stepObj.HasKey(L"nodeId")
                    ? static_cast<uint32_t>(stepObj.GetNamedNumber(L"nodeId")) : 0;
                path = L"/analysis/" + std::to_wstring(id);
                body.clear();
            }
            else if (op == L"render")        {
                // Internal: force a fresh evaluation. Useful as a
                // barrier between a batch of property mutations and a
                // subsequent readback when the agent wants to be
                // explicit about ordering. Image-stats / pixel-region /
                // capture-node already trigger evaluation themselves,
                // so this is rarely needed.
                runEval();
                WDJ::JsonObject ok;
                ok.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
                ok.Insert(L"status", WDJ::JsonValue::CreateNumberValue(200));
                WDJ::JsonObject inner;
                inner.Insert(L"ok", WDJ::JsonValue::CreateBooleanValue(true));
                ok.Insert(L"body", inner);
                results.Append(ok);
                continue;
            }
            else
            {
                WDJ::JsonObject err;
                err.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
                err.Insert(L"status", WDJ::JsonValue::CreateNumberValue(400));
                err.Insert(L"error", WDJ::JsonValue::CreateStringValue(
                    winrt::hstring(L"Unknown op: " + op)));
                results.Append(err);
                continue;
            }
        }
        else if (stepObj.HasKey(L"path"))
        {
            method = stepObj.HasKey(L"method")
                ? std::wstring(stepObj.GetNamedString(L"method"))
                : L"GET";
            path = std::wstring(stepObj.GetNamedString(L"path"));
            if (stepObj.HasKey(L"body"))
                body = stepBodyToString(stepObj.GetNamedValue(L"body"));
        }
        else
        {
            WDJ::JsonObject err;
            err.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
            err.Insert(L"status", WDJ::JsonValue::CreateNumberValue(400));
            err.Insert(L"error", WDJ::JsonValue::CreateStringValue(
                L"Step must have either 'op' or 'path'"));
            results.Append(err);
            continue;
        }

        auto resp = server.RouteRequest(method, path, body);

        WDJ::JsonObject entry;
        entry.Insert(L"step", WDJ::JsonValue::CreateNumberValue(i));
        entry.Insert(L"method", WDJ::JsonValue::CreateStringValue(winrt::hstring(method)));
        entry.Insert(L"path", WDJ::JsonValue::CreateStringValue(winrt::hstring(path)));
        entry.Insert(L"status", WDJ::JsonValue::CreateNumberValue(resp.statusCode));

        // Attempt to parse response body as JSON; fall back to raw
        // string if parsing fails (e.g. plain text error responses).
        if (!resp.body.empty())
        {
            int rwc = MultiByteToWideChar(CP_UTF8, 0, resp.body.data(),
                static_cast<int>(resp.body.size()), nullptr, 0);
            std::wstring rwstr(rwc, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, resp.body.data(),
                static_cast<int>(resp.body.size()), rwstr.data(), rwc);

            WDJ::JsonValue parsed{ nullptr };
            if (WDJ::JsonValue::TryParse(winrt::hstring(rwstr), parsed))
                entry.Insert(L"body", parsed);
            else
                entry.Insert(L"body", WDJ::JsonValue::CreateStringValue(winrt::hstring(rwstr)));
        }
        else
        {
            entry.Insert(L"body", WDJ::JsonValue::CreateNullValue());
        }
        results.Append(entry);
    }

    // ---- Emit response document ------------------------------------------
    WDJ::JsonObject outDoc;
    outDoc.Insert(L"abiVersion", WDJ::JsonValue::CreateNumberValue(
        static_cast<double>(::ShaderLab_GetAbiVersion())));
    outDoc.Insert(L"stepCount", WDJ::JsonValue::CreateNumberValue(
        static_cast<double>(steps.Size())));
    outDoc.Insert(L"results", results);

    auto outH = outDoc.Stringify();
    int outN = WideCharToMultiByte(CP_UTF8, 0, outH.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string outUtf8(outN > 0 ? outN - 1 : 0, '\0');
    if (outN > 0) WideCharToMultiByte(CP_UTF8, 0, outH.c_str(), -1, outUtf8.data(), outN, nullptr, nullptr);

    if (!args.scriptOutputPath.empty())
    {
        std::ofstream f(args.scriptOutputPath, std::ios::binary);
        if (!f) {
            std::wprintf(L"FATAL: cannot open --script-output '%ls' for write\n",
                args.scriptOutputPath.c_str());
            return 7;
        }
        f.write(outUtf8.data(), static_cast<std::streamsize>(outUtf8.size()));
        std::wprintf(L"OK: ran %u step(s) -> %ls\n",
            steps.Size(), args.scriptOutputPath.c_str());
    }
    else
    {
        std::fwrite(outUtf8.data(), 1, outUtf8.size(), stdout);
        std::fputc('\n', stdout);
    }

    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::wprintf(L"ShaderLabHeadless (engine ABI %u)\n",
        static_cast<unsigned>(::ShaderLab_GetAbiVersion()));

    Args args;
    if (!ParseArgs(argc, argv, args)) {
        if (argc <= 1) PrintUsage(argv[0]);
        return 1;
    }

    if (::ShaderLab_GetAbiVersion() != SHADERLAB_ENGINE_ABI_VERSION) {
        std::wprintf(L"FATAL: engine ABI mismatch (header %u, DLL %u)\n",
            static_cast<unsigned>(SHADERLAB_ENGINE_ABI_VERSION),
            static_cast<unsigned>(::ShaderLab_GetAbiVersion()));
        return 2;
    }

    // Cache management modes (Phase 8 cache reaper). Run before any
    // engine init so we don't pay the D3D startup cost for a one-shot
    // cleanup. Wires the same %LOCALAPPDATA% root the GUI app uses.
    if (args.reapShaderCache || args.clearShaderCache)
    {
        wchar_t* localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) ||
            !localAppData)
        {
            std::wprintf(L"ERROR: could not resolve %%LOCALAPPDATA%%\n");
            return 3;
        }
        std::wstring cacheRoot = std::wstring(localAppData) + L"\\ShaderLab\\bytecode";
        ::CoTaskMemFree(localAppData);
        ShaderLab::Effects::BytecodeCache::Instance().SetDiskCacheRoot(cacheRoot);
        auto stats = args.clearShaderCache
            ? ShaderLab::Effects::BytecodeCache::Instance().ClearDisk()
            : ShaderLab::Effects::BytecodeCache::Instance().ReapDisk(args.reapStaleSec);
        std::wprintf(
            L"{\"filesDeleted\":%zu,\"bytesFreed\":%zu,\"errors\":%zu,\"mode\":\"%ls\"}\n",
            stats.filesDeleted, stats.bytesFreed, stats.errors,
            args.clearShaderCache ? L"clear" : L"reap");
        return 0;
    }

    winrt::init_apartment();
    ::MFStartup(MF_VERSION);

    // p8-feature-flag: apply --enable-gpu-bindings / --disable-gpu-bindings
    // before any engine work so the evaluator's binding-resolution pass
    // sees the correct flag state. Engine default is OFF for v1.6.
    if (args.enableGpuBindings.has_value())
        ShaderLab::Performance::SetGpuBindingsEnabled(*args.enableGpuBindings);

    // Run all engine work inside an inner scope so the GraphEvaluator
    // and other engine objects destruct before MFShutdown / apartment
    // teardown. (GraphEvaluator owns com_ptrs to D2D effects that need
    // the factory alive at destruction time.)
    int rc = !args.scriptPath.empty() ? RunScript(args) : RunRender(args);

    ::MFShutdown();
    winrt::uninit_apartment();
    return rc;
}
