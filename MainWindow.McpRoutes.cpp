#include "pch.h"
#include "MainWindow.xaml.h"
#include "Engine/Mcp/McpHttpServer.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/SourceNodeFactory.h"
#include "Rendering/IccProfileParser.h"
#include "Version.h"

// Helper: narrow string from wide string.
static std::string ToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), s.data(), len, nullptr, nullptr);
    return s;
}

// Base64 (standard alphabet, '=' padding, no line wrapping).
static std::string Base64Encode(const uint8_t* data, size_t len)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    if (len == 0) return out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len)
    {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
        i += 3;
    }
    if (i < len)
    {
        uint32_t v = uint32_t(data[i]) << 16;
        bool two = (i + 1 < len);
        if (two) v |= uint32_t(data[i + 1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(two ? kAlphabet[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}


namespace winrt::ShaderLab::implementation
{
    // Dispatch a lambda to the UI thread and block until completion.
    // Returns the result from the lambda. Must NOT be called from the UI thread.
    //
    // Key correctness properties:
    //   * The state (result/exception/event) lives in a shared_ptr captured by
    //     the lambda, so if we time out and the caller returns, the lambda can
    //     still safely write to it without dangling references.
    //   * We always check the wait result before reading the result so a timeout
    //     won't deref an empty optional. On timeout we throw so the calling
    //     route returns a 500 instead of producing garbage.
    //   * Non-void return only -- DispatchSync<void> isn't supported.
    template<typename F>
    auto MainWindow::DispatchSync(F&& fn) -> decltype(fn())
    {
        using R = decltype(fn());
        struct State
        {
            std::optional<R>      result;
            std::exception_ptr    ex;
            HANDLE                event{ nullptr };
            ~State() { if (event) CloseHandle(event); }
        };
        auto state = std::make_shared<State>();
        state->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!state->event)
            throw std::runtime_error("DispatchSync: CreateEventW failed");

        // Move the lambda into a shared_ptr so it stays alive even if the
        // DispatcherQueue holds the callback longer than this scope.
        auto fnPtr = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
        DispatcherQueue().TryEnqueue([state, fnPtr]()
        {
            try { state->result = (*fnPtr)(); }
            catch (...) { state->ex = std::current_exception(); }
            SetEvent(state->event);
        });

        // 30s timeout -- generous for stats/readback paths but still a
        // backstop against a wedged UI thread.
        DWORD wait = WaitForSingleObject(state->event, 30000);
        if (wait != WAIT_OBJECT_0)
            throw std::runtime_error("DispatchSync: UI thread did not respond within 30s");
        if (state->ex) std::rethrow_exception(state->ex);
        if (!state->result.has_value())
            throw std::runtime_error("DispatchSync: lambda completed without producing a result");
        return std::move(*state->result);
    }

    ::ShaderLab::McpHttpServer::Response MainWindow::GuiEngineCommandSink::Dispatch(
        std::function<::ShaderLab::McpHttpServer::Response(
            ::ShaderLab::Mcp::EngineContext&)> closure)
    {
        // Marshal the engine work to the render thread (single writer to
        // m_graph). Re-entrant calls from inside the consumer thread run
        // inline (RenderThreadDispatcher detects this).
        ::ShaderLab::McpHttpServer::Response resp;
        try
        {
            resp = window->m_renderDispatcher.DispatchSync(
                [this, &closure]() -> ::ShaderLab::McpHttpServer::Response {
                    ::ShaderLab::Mcp::EngineContext ctx{};
                    ctx.graph = &window->m_graph;
                    ctx.evaluator = &window->m_graphEvaluator;
                    ctx.displayMonitor = &window->m_displayMonitor;
                    ctx.sourceFactory = &window->m_sourceFactory;
                    // Closure runs on render thread now -- give it the render
                    // D2D context so source-prep / evaluator ops it performs
                    // share state with the per-frame render path.
                    ctx.dc = window->m_renderEngine.RenderD2DContext();
                    ctx.d3dDevice = window->m_renderEngine.D3DDevice();
                    ctx.d3dContext = window->m_renderEngine.D3DContext();
                    ctx.renderFrame = [this]() { window->RenderFrameToOffscreen(0.0); };
                    ctx.getPreviewNodeId = [this]() -> uint32_t { return window->m_previewNodeId; };
                    ctx.getLoadedIccProfile = [this]() -> std::optional<::ShaderLab::Rendering::DisplayProfile> {
                        return window->m_loadedIccProfile;
                    };
                    ctx.setLoadedIccProfile = [this](const ::ShaderLab::Rendering::DisplayProfile& p) {
                        window->m_loadedIccProfile = p;
                    };
                    return closure(ctx);
                });
        }
        catch (const std::exception& e)
        {
            ::ShaderLab::McpHttpServer::Response err;
            err.statusCode = 500;
            err.body = std::string(R"({"error":")") + e.what() + R"("})";
            err.contentType = "application/json";
            return err;
        }
        return resp;
    }

    // Event hooks fire from inside Dispatch closures, which run on the
    // render thread. Anything that touches XAML or UI-thread-only controllers
    // must be marshalled back to the UI thread via DispatcherQueue().TryEnqueue.
    //
    // P7 race protection: NodeGraphController::AutoLayout / RebuildLayout
    // iterate `m_graph.Nodes()` and each node's `properties` std::map. The
    // render worker mutates those maps every tick (analysisOutput updates,
    // clock advancement, source factory writes). To avoid use-after-free /
    // iterator invalidation in std::map traversal, we run those layout
    // calls THROUGH the render dispatcher with DispatchSync. The dispatcher
    // drains queued closures BEFORE the worker's per-tick body, so when our
    // closure runs the worker is implicitly paused -- m_graph is stable for
    // the duration of the layout, and m_visuals isn't being concurrently
    // read by the UI thread (which is blocked in DispatchSync).
    static void RunLayoutOnRenderThread(::ShaderLab::Rendering::RenderThreadDispatcher& dispatcher,
                                        std::function<void()> work)
    {
        dispatcher.DispatchSync([work = std::move(work)]() {
            work();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnNodeAdded(uint32_t nodeId)
    {
        window->m_graph.MarkAllDirty();
        window->m_forceRender = true;
        // Detect Output nodes added via /graph/apply or other engine-side
        // routes and auto-open a window for each. Engine-pure hosts don't
        // care about windows; this sink runs only in the GUI host.
        bool isOutput = false;
        if (auto* n = window->m_graph.FindNode(nodeId))
            isOutput = (n->type == ::ShaderLab::Graph::NodeType::Output);
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w, nodeId, isOutput]{
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
            w->PopulatePreviewNodeSelector();
            if (isOutput)
            {
                try { w->OpenOutputWindow(nodeId); } catch (...) {}
            }
        });
    }

    void MainWindow::GuiEngineCommandSink::OnNodeRemoved(uint32_t nodeId)
    {
        window->m_graphEvaluator.InvalidateNode(nodeId);
        window->m_graph.MarkAllDirty();
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w, nodeId]{
            w->CloseOutputWindow(nodeId);
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
            w->PopulatePreviewNodeSelector();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnNodeChanged(uint32_t nodeId)
    {
        window->m_forceRender = true;

        // If the changed node has any visibleWhen-conditional parameters,
        // a property change might flip a pin's visibility. Rebuild the
        // node-graph layout so new input pins materialize on the canvas.
        //
        // OnNodeChanged is already running inside a render-dispatcher
        // closure (the /graph/set-property route DispatchSync's into the
        // worker before invoking this hook), so it is safe to touch
        // m_graph and m_nodeGraphController here directly -- the worker
        // is paused for the duration. We just need to tell the controller
        // to repaint via m_needsRedraw (set by RebuildLayout itself).
        if (auto* n = window->m_graph.FindNode(nodeId))
        {
            if (n->customEffect.has_value())
            {
                for (const auto& p : n->customEffect->parameters)
                {
                    if (!p.visibleWhen.empty())
                    {
                        window->m_nodeGraphController.RebuildLayout();
                        break;
                    }
                }
            }
        }
    }

    void MainWindow::GuiEngineCommandSink::OnGraphCleared()
    {
        window->m_graphEvaluator.ReleaseCache();
        window->m_previewNodeId = 0;
        window->m_graph.MarkAllDirty();
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            w->m_outputWindows.clear();
            // Also drop sinks so the render worker doesn't keep churning on
            // closed windows. Without this the m_outputSinks vector grows
            // unbounded across graph clear/load cycles.
            {
                std::scoped_lock lock(w->m_outputSinksMutex);
                w->m_outputSinks.clear();
            }
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
            w->PopulatePreviewNodeSelector();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnGraphLoaded()
    {
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            w->ResetAfterGraphLoad(/*reopenOutputWindows=*/true);
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
        });
    }

    void MainWindow::GuiEngineCommandSink::OnGraphStructureChanged()
    {
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
        });
    }

    void MainWindow::GuiEngineCommandSink::OnCustomEffectRecompiled(uint32_t /*nodeId*/)
    {
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.RebuildLayout(); });
            w->PopulateAddNodeFlyout();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnDisplayProfileChanged()
    {
        window->m_graph.MarkAllDirty();
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{ w->UpdateStatusBar(); });
    }

    void MainWindow::SetupMcpRoutes()
    {
        if (!m_mcpServer)
            m_mcpServer = std::make_unique<::ShaderLab::McpHttpServer>();
        if (!m_engineSink)
            m_engineSink = std::make_unique<GuiEngineCommandSink>(this);

        // Activity callback: fires on the listener thread once per HTTP request.
        // We update atomic state + a small mutexed string snapshot; the UI render
        // tick polls these and refreshes the indicator dot + tooltip.  Keeping
        // this side cheap and non-blocking ensures we don't introduce lock-step
        // between MCP responses and the UI thread.
        m_mcpServer->SetActivityCallback(
            [this](const std::string& method,
                   const std::wstring& path,
                   uint16_t statusCode,
                   const std::string& peerAddress)
            {
                using clk = std::chrono::system_clock;
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    clk::now().time_since_epoch()).count();
                m_mcpLastActivityMs.store(nowMs, std::memory_order_relaxed);
                m_mcpRequestCount.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard lock(m_mcpLastReqMutex);
                    m_mcpLastReqMethod = method;
                    m_mcpLastReqPath = ToUtf8(path);
                    m_mcpLastReqPeer = peerAddress;
                    m_mcpLastReqStatus = statusCode;
                    if (!peerAddress.empty())
                        m_mcpKnownPeers.insert(peerAddress);
                }
                m_mcpUiUpdateSeq.fetch_add(1, std::memory_order_release);
            });

        // Register engine-pure routes (Phase 7 migration). Currently
        // empty; routes are migrated in batches with each commit.
        // The GUI app then registers UI-coupled routes below
        // (graph_snapshot, preview/graph view tools, etc).
        ::ShaderLab::Mcp::RegisterEngineRoutes(*m_mcpServer, *m_engineSink);

        // =====================================================================
        // GET /  — Health check / probe (some MCP clients GET / before POST).
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/", [](const std::wstring& path, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            // Only match exact "/" — longer GET paths fall through to other routes.
            if (path != L"/")
                return { 404, R"({"error":"Not found"})" };
            return { 200, R"({"name":"shaderlab","transport":"streamable-http","endpoint":"POST /"})" };
        });

        // =====================================================================
        // GET /context  — System prompt / onboarding for calling agents
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/context", [](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            std::string doc = R"JSON({
"name": "ShaderLab",
"description": "D2D shader effect development tool with node graph, HDR/WCG pipeline.",
"pipeline": {
    "format": "scRGB FP16 linear light",
    "whitePoint": "1.0 = 80 nits SDR white",
    "valuesAbove1": "HDR, super-white",
    "colorSpace": "Linear sRGB primaries, Rec.709"
},
"shaderConventions": {
    "texcoords": "D2D provides TEXCOORD0 in pixel/scene space, not normalized 0-1",
    "sampling": "Use Load int3 uv0.xy 0 for direct texel access; all inputs share TEXCOORD0",
    "filteredSampling": "Use SampleLevel with GetDimensions normalization for bilinear",
    "constantBuffer": "register b0, variables packed by D3DReflect offsets",
    "textures": "register t0..t7, one per input",
    "computeOutput": "RWTexture2D<float4> Output : register u0"
},
"analysisEffects": {
    "pattern": "Compute shaders can act as analysis effects that read an entire image and produce typed output fields",
    "outputTypes": "float, float2, float3, float4, floatarray, float2array, float3array, float4array",
    "outputConvention": "Write results to Output[int2(pixelOffset, 0)]. Each field occupies pixelCount() pixels sequentially",
    "readback": "The host reads the output row and unpacks typed fields based on analysisFields descriptors",
    "fieldDescriptors": "Define analysisFields array in custom effect definition with name, type, and length (for arrays)",
    "propertyBindings": "Analysis output fields can be bound to downstream node properties via propertyBindings",
    "builtInExample": "D2D Histogram effect: processes input, exposes 256-float histogram via GetValue",
    "customExample": "Gamut analysis: Output[0,0].x = maxLuminance (float), Output[1,0] = gamutBounds (float4), etc."
},
"nodeTypes": ["Source", "BuiltInEffect", "PixelShader", "ComputeShader", "Output"],
"outputNote": "PNG captures are tone-mapped SDR. Use /render/pixel/X/Y for true scRGB float values. Values above 1.0 are HDR.",
"endpoints": {
    "GET /context": "This document",
    "GET /graph": "Full graph state with nodes, edges, properties, custom effects",
    "GET /graph/node/{id}": "Single node detail including custom effect definition",
    "GET /registry/effects": "All built-in D2D effects",
    "GET /custom-effects": "All custom effects in graph with HLSL source",
    "GET /render/capture": "Output as base64 PNG, SDR tone-mapped",
    "GET /render/pixel/{x}/{y}": "scRGB float4 plus luminance at coordinates",
    "POST /graph/add-node": "Add a node, body: effectName string",
    "POST /graph/remove-node": "Remove node, body: nodeId number",
    "POST /graph/connect": "Connect pins, body: srcId srcPin dstId dstPin",
    "POST /graph/disconnect": "Disconnect, body: srcId srcPin dstId dstPin",
    "POST /graph/set-property": "Set property, body: nodeId key value",
    "POST /graph/load": "Load graph JSON, body: full graph JSON string",
    "GET /graph/save": "Get graph as JSON string",
    "POST /graph/clear": "Clear entire graph",
    "POST /effect/compile": "Compile HLSL, body: nodeId hlsl",
    "POST /render/preview-node": "Set preview node, body: nodeId number"
}
})JSON";
            return { 200, doc };
        });

        // =====================================================================
        // GET /graph, GET /graph/save, GET /graph/node/{id}
        // -- moved to Engine/Mcp/EngineMcpRoutes.cpp (Phase 7).
        // =====================================================================

        // =====================================================================
        // GET /registry -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Static D2D effect catalog, no Dispatch needed.
        // =====================================================================

        // =====================================================================
        // GET /custom-effects -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7).
        // =====================================================================

        // =====================================================================
        // POST /graph/add-node -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/remove-node -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/connect -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Same notes as /graph/disconnect.
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/disconnect -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). UI side effects (nodeLogs, AutoLayout) drop
        // out: the GUI render tick picks up dirty state and refreshes the
        // canvas next frame.
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/set-property -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Mutates m_graph through IEngineCommandSink.
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/load -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/clear -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // POST /render/preview-node
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/render/preview-node", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    m_previewNodeId = nodeId;
                    m_needsFitPreview = true;
                    m_forceRender = true;
                    m_graph.MarkAllDirty();
                    return { 200, R"({"ok":true})" };
                });
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // POST /effect/compile
        // =====================================================================
        // -- moved to Engine/Mcp/EngineMcpRoutes.cpp (Phase 7).
        //    Mirrors EffectDesignerWindow Update-in-Graph; fires
        //    OnCustomEffectRecompiled hook.
        // =====================================================================

        // =====================================================================
        // GET /render/pixel/{x}/{y}
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/render/pixel/", [this](const std::wstring& path, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            // Parse /render/pixel/{x}/{y}
            auto rest = path.substr(14); // after "/render/pixel/"
            auto slash = rest.find(L'/');
            if (slash == std::wstring::npos)
                return { 400, R"({"error":"Format: /render/pixel/{x}/{y}"})" };

            float x = std::stof(rest.substr(0, slash));
            float y = std::stof(rest.substr(slash + 1));

            auto* dc = m_renderEngine.D2DDeviceContext();
            if (!dc) return { 500, R"({"error":"No device context"})" };

            auto* image = ResolveDisplayImage(m_previewNodeId);
            if (!image) return { 404, R"({"error":"No output image"})" };

            // Read pixel value using a 1x1 bitmap copy.
            D2D1_POINT_2U srcPoint = { static_cast<UINT32>(x), static_cast<UINT32>(y) };
            D2D1_BITMAP_PROPERTIES1 bmpProps = {};
            bmpProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            bmpProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

            winrt::com_ptr<ID2D1Bitmap1> readBitmap;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, bmpProps, readBitmap.put());
            if (FAILED(hr)) return { 500, R"({"error":"Failed to create read bitmap"})" };

            D2D1_RECT_U srcRect = { srcPoint.x, srcPoint.y, srcPoint.x + 1, srcPoint.y + 1 };
            // Need to render the image to a target first, then copy.
            // For simplicity, report from the cached pixel inspector logic.
            // TODO: implement proper pixel readback

            return { 200, std::format("{{\"x\":{:.0f},\"y\":{:.0f},\"note\":\"Pixel readback via MCP coming soon\"}}", x, y) };
        });

        // =====================================================================
        // =====================================================================
        // GET /render/capture -- Save output PNG to temp file, return path
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/render/capture", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return m_renderDispatcher.DispatchSync(
                [this]() -> ::ShaderLab::McpHttpServer::Response {
                // Run on render thread (single writer to graph + owns the
                // engine D2D context). Force a full re-evaluation so the
                // capture reflects current state.
                m_graph.MarkAllDirty();
                RenderFrameToOffscreen(0.0);
                auto pngData = CapturePreviewAsPng();
                if (pngData.empty())
                    return { 404, R"({"error":"No output image"})" };

                // Write to temp file.
                wchar_t tempPath[MAX_PATH]{};
                GetTempPathW(MAX_PATH, tempPath);
                std::wstring filePath = std::wstring(tempPath) + L"shaderlab_capture.png";
                HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile == INVALID_HANDLE_VALUE)
                    return { 500, R"({"error":"Failed to create temp file"})" };
                DWORD written = 0;
                WriteFile(hFile, pngData.data(), static_cast<DWORD>(pngData.size()), &written, nullptr);
                CloseHandle(hFile);

                auto pathUtf8 = ToUtf8(filePath);
                // Escape backslashes for JSON.
                std::string escaped;
                for (char c : pathUtf8) { if (c == '\\') escaped += "\\\\"; else escaped += c; }

                return { 200, std::format("{{\"path\":\"{}\",\"size\":{}}}", escaped, pngData.size()) };
            });
        });

        // =====================================================================
        // GET /perf — Return per-frame performance timings
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/perf", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            auto& t = m_lastFrameTiming;
            if (t.framesSampled == 0)
                t = m_frameTiming;  // fallback to live if no snapshot yet
            double fps = (t.totalUs > 0) ? 1000000.0 / t.totalUs : 0;
            return { 200, std::format(
                "{{\"fps\":{:.1f},\"totalMs\":{:.2f},"
                "\"sourcesPrepMs\":{:.2f},\"evaluateMs\":{:.2f},"
                "\"deferredComputeMs\":{:.2f},\"drawMs\":{:.2f},"
                "\"endDrawFlushMs\":{:.2f},"
                "\"uiTickMs\":{:.2f},\"outputWindowsMs\":{:.2f},\"traceMs\":{:.2f},"
                "\"computeDispatches\":{},"
                "\"framesSampled\":{},\"endDrawFailed\":{}}}",
                fps, t.totalUs / 1000.0,
                t.sourcesPrepUs / 1000.0, t.evaluateUs / 1000.0,
                t.deferredComputeUs / 1000.0, t.drawUs / 1000.0,
                t.endDrawFlushUs / 1000.0,
                t.uiTickUs / 1000.0, t.outputWindowsUs / 1000.0, t.traceUs / 1000.0,
                t.computeDispatches,
                t.framesSampled, t.endDrawFailed) };
        });

        // =====================================================================
        // GET /node/{id}/logs — Return per-node log entries
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/node/", [this](const std::wstring& path, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                // Parse nodeId and optional /logs suffix from path.
                // Expected: /node/{id}/logs or /node/{id}/logs?since={seq}
                auto stripped = path.substr(6); // remove "/node/"
                uint32_t nodeId = 0;
                try { nodeId = static_cast<uint32_t>(std::stoi(stripped)); } catch (...) {
                    return { 400, R"({"error":"Invalid node ID"})" };
                }

                auto it = m_nodeLogs.find(nodeId);
                if (it == m_nodeLogs.end())
                    return { 200, R"({"logs":[]})" };

                auto& log = it->second;
                // Check for ?since= parameter.
                uint64_t sinceSeq = 0;
                auto qPos = path.find(L"since=");
                if (qPos != std::wstring::npos)
                {
                    try { sinceSeq = std::stoull(path.substr(qPos + 6)); } catch (...) {}
                }

                std::string json = "{\"logs\":[";
                bool first = true;
                for (const auto& entry : log.Entries())
                {
                    if (entry.sequence <= sinceSeq) continue;
                    if (!first) json += ",";
                    first = false;

                    auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        entry.timestamp.time_since_epoch()).count() % 1000;
                    struct tm tm_buf{};
                    localtime_s(&tm_buf, &tt);
                    char timeBuf[32]{};
                    sprintf_s(timeBuf, "%02d:%02d:%02d.%03d",
                        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms));

                    const char* levelStr = "Info";
                    if (entry.level == ::ShaderLab::Controls::LogLevel::Warning) levelStr = "Warning";
                    else if (entry.level == ::ShaderLab::Controls::LogLevel::Error) levelStr = "Error";

                    // JSON-escape the message.
                    std::string msg = ToUtf8(entry.message);
                    std::string escaped;
                    for (char c : msg) {
                        if (c == '"') escaped += "\\\"";
                        else if (c == '\\') escaped += "\\\\";
                        else if (c == '\n') escaped += "\\n";
                        else if (c == '\r') escaped += "\\r";
                        else if (c == '\t') escaped += "\\t";
                        else if (static_cast<unsigned char>(c) < 0x20)
                            escaped += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                        else escaped += c;
                    }

                    json += std::format("{{\"seq\":{},\"time\":\"{}\",\"level\":\"{}\",\"message\":\"{}\"}}",
                        entry.sequence, timeBuf, levelStr, escaped);
                }
                json += "]}";
                return { 200, json };
            });
        });

        // =====================================================================
        // POST /graph/bind-property -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // POST /graph/unbind-property -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // GET /analysis/{id} -- moved to Engine/Mcp/EngineMcpRoutes.cpp (Phase 7).
        // =====================================================================

        // =====================================================================
        // POST /render/pixel-trace — Run pixel trace at normalized coordinates
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/render/pixel-trace", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                float normX = static_cast<float>(jobj.GetNamedNumber(L"x"));
                float normY = static_cast<float>(jobj.GetNamedNumber(L"y"));

                return m_renderDispatcher.DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    // P13: pixel-trace runs on the render thread. m_graph is
                    // single-writer there; the render-side D2D context shares
                    // the engine's multi-threaded D2D device with the worker's
                    // BeginDraw session. UI's PopulatePixelTraceTree also
                    // routes its ReTrace through the render dispatcher, so
                    // there's no concurrent access to m_pixelTrace state.
                    auto* dc = m_renderEngine.RenderD2DContext();
                    if (!dc) return { 500, R"({"error":"No device context"})" };

                    // Compute preview image bounds inline on render context
                    // (can't call MainWindow::GetPreviewImageBounds, which
                    // reads from the UI D2D context).
                    auto* previewNode = m_graph.FindNode(m_previewNodeId);
                    auto* previewImage = previewNode ? previewNode->cachedOutput : nullptr;
                    if (!previewImage) return { 404, R"({"error":"No preview image"})" };
                    float oldDpiX, oldDpiY;
                    dc->GetDpi(&oldDpiX, &oldDpiY);
                    dc->SetDpi(96.0f, 96.0f);
                    D2D1_RECT_F bounds{};
                    HRESULT bhr = dc->GetImageLocalBounds(previewImage, &bounds);
                    dc->SetDpi(oldDpiX, oldDpiY);
                    if (FAILED(bhr)) return { 500, R"({"error":"GetImageLocalBounds failed"})" };

                    uint32_t imageW = static_cast<uint32_t>(bounds.right - bounds.left);
                    uint32_t imageH = static_cast<uint32_t>(bounds.bottom - bounds.top);
                    if (imageW == 0 || imageH == 0)
                        return { 404, R"({"error":"No preview image"})" };

                    if (!m_pixelTrace.BuildTrace(dc, m_graph, nodeId, normX, normY, imageW, imageH))
                        return { 500, R"({"error":"Pixel trace failed"})" };

                    const auto& root = m_pixelTrace.Root();
                    uint32_t pixelX = static_cast<uint32_t>(normX * imageW);
                    uint32_t pixelY = static_cast<uint32_t>(normY * imageH);

                    // Recursive lambda to serialize the trace tree.
                    std::function<std::string(const ::ShaderLab::Controls::PixelTraceNode&)> serializeNode;
                    serializeNode = [&](const ::ShaderLab::Controls::PixelTraceNode& tn) -> std::string
                    {
                        std::string j = "{";
                        j += std::format("\"nodeId\":{},\"name\":\"{}\",\"pin\":\"{}\"",
                            tn.nodeId, ToUtf8(tn.nodeName), ToUtf8(tn.pinName));

                        // Pixel values.
                        const auto& px = tn.pixel;
                        j += std::format(",\"pixel\":{{\"scRGB\":[{:.6f},{:.6f},{:.6f},{:.6f}]",
                            px.scR, px.scG, px.scB, px.scA);
                        j += std::format(",\"sRGB\":[{},{},{},{}]",
                            px.sR, px.sG, px.sB, px.sA);
                        j += std::format(",\"luminance\":{:.2f}}}", px.luminanceNits);

                        // Analysis fields (for compute/analysis nodes).
                        if (tn.hasAnalysisOutput && !tn.analysisFields.empty())
                        {
                            j += ",\"analysisFields\":[";
                            bool first = true;
                            for (const auto& fv : tn.analysisFields)
                            {
                                if (!first) j += ",";
                                j += "{\"name\":\"" + ToUtf8(fv.name) + "\"";

                                std::string typeTag;
                                switch (fv.type)
                                {
                                case ::ShaderLab::Graph::AnalysisFieldType::Float:       typeTag = "float"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float2:      typeTag = "float2"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float3:      typeTag = "float3"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float4:      typeTag = "float4"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::FloatArray:   typeTag = "floatarray"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float2Array:  typeTag = "float2array"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float3Array:  typeTag = "float3array"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float4Array:  typeTag = "float4array"; break;
                                }
                                j += ",\"type\":\"" + typeTag + "\"";

                                if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                                {
                                    uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                                    j += ",\"value\":[";
                                    for (uint32_t c = 0; c < cc; ++c)
                                    {
                                        if (c > 0) j += ",";
                                        j += std::format("{:.6f}", fv.components[c]);
                                    }
                                    j += "]";
                                }
                                else
                                {
                                    uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                                    uint32_t count = stride > 0 ? static_cast<uint32_t>(fv.arrayData.size()) / stride : 0;
                                    j += ",\"count\":" + std::to_string(count);
                                    j += ",\"value\":[";
                                    for (size_t i = 0; i < fv.arrayData.size(); ++i)
                                    {
                                        if (i > 0) j += ",";
                                        j += std::format("{:.6f}", fv.arrayData[i]);
                                    }
                                    j += "]";
                                }
                                j += "}";
                                first = false;
                            }
                            j += "]";
                        }
                        else
                        {
                            j += ",\"analysisFields\":[]";
                        }

                        // Recurse into inputs.
                        j += ",\"inputs\":[";
                        for (size_t i = 0; i < tn.inputs.size(); ++i)
                        {
                            if (i > 0) j += ",";
                            j += serializeNode(tn.inputs[i]);
                        }
                        j += "]";

                        j += "}";
                        return j;
                    };

                    std::string json = "{";
                    json += std::format("\"position\":{{\"x\":{:.6f},\"y\":{:.6f},\"pixelX\":{},\"pixelY\":{}}}",
                        normX, normY, pixelX, pixelY);
                    json += ",\"nodes\":[" + serializeNode(root) + "]";
                    json += "}";
                    return { 200, json };
                });
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // Graph editor view (snapshot + pan/zoom)
        // =====================================================================

        // POST /graph/snapshot — body: { "inline": bool? }
        // Captures the live node-graph view at the swap-chain panel size.
        // Always writes PNG to a unique %TEMP% file. When inline=true, also
        // returns base64-encoded bytes in the response.
        m_mcpServer->AddRoute(L"POST", L"/graph/snapshot", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                bool wantInline = false;
                if (!body.empty())
                {
                    winrt::Windows::Data::Json::JsonObject jo{ nullptr };
                    if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(body), jo))
                    {
                        if (jo.HasKey(L"inline"))
                        {
                            auto v = jo.GetNamedValue(L"inline");
                            if (v.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean)
                                wantInline = v.GetBoolean();
                        }
                    }
                }

                auto pngData = CaptureGraphAsPng();
                if (pngData.empty())
                    return { 500, R"({"error":"Snapshot failed"})" };

                // Unique temp filename to avoid concurrent-capture overwrites.
                static std::atomic<uint32_t> s_seq{ 0 };
                uint32_t seq = s_seq.fetch_add(1, std::memory_order_relaxed);
                wchar_t tempPath[MAX_PATH]{};
                GetTempPathW(MAX_PATH, tempPath);
                std::wstring filePath = std::format(L"{}shaderlab_graph_snapshot_{}_{}.png",
                    tempPath, GetCurrentProcessId(), seq);
                HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile == INVALID_HANDLE_VALUE)
                    return { 500, R"({"error":"Failed to create temp file"})" };
                DWORD written = 0;
                WriteFile(hFile, pngData.data(), static_cast<DWORD>(pngData.size()), &written, nullptr);
                CloseHandle(hFile);

                auto pathUtf8 = ToUtf8(filePath);
                std::string escapedPath;
                for (char c : pathUtf8) { if (c == '\\') escapedPath += "\\\\"; else escapedPath += c; }

                if (wantInline)
                {
                    auto b64 = Base64Encode(pngData.data(), pngData.size());
                    return { 200, std::format(
                        "{{\"path\":\"{}\",\"size\":{},\"width\":{},\"height\":{},"
                        "\"mimeType\":\"image/png\",\"base64\":\"{}\"}}",
                        escapedPath, pngData.size(),
                        m_graphPanelWidth, m_graphPanelHeight, b64) };
                }
                return { 200, std::format(
                    "{{\"path\":\"{}\",\"size\":{},\"width\":{},\"height\":{},\"mimeType\":\"image/png\"}}",
                    escapedPath, pngData.size(),
                    m_graphPanelWidth, m_graphPanelHeight) };
            });
        });

        // GET /graph/view — current pan/zoom + viewport + content bounds
        m_mcpServer->AddRoute(L"GET", L"/graph/view", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                m_nodeGraphController.RebuildLayout();
                auto pan = m_nodeGraphController.PanOffset();
                float zoom = m_nodeGraphController.Zoom();
                auto b = m_nodeGraphController.ContentBounds();
                float cw = (std::max)(0.0f, b.right - b.left);
                float ch = (std::max)(0.0f, b.bottom - b.top);
                return { 200, std::format(
                    "{{\"zoom\":{:.6f},\"panX\":{:.6f},\"panY\":{:.6f}"
                    ",\"viewportW\":{},\"viewportH\":{}"
                    ",\"contentBounds\":{{\"x\":{:.6f},\"y\":{:.6f},\"w\":{:.6f},\"h\":{:.6f}}}"
                    ",\"zoomLimits\":{{\"min\":0.1,\"max\":5.0}}"
                    "}}",
                    zoom, pan.x, pan.y,
                    m_graphPanelWidth, m_graphPanelHeight,
                    b.left, b.top, cw, ch) };
            });
        });

        // POST /graph/view — body: { zoom?, panX?, panY? }
        m_mcpServer->AddRoute(L"POST", L"/graph/view", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                winrt::Windows::Data::Json::JsonObject jo{ nullptr };
                if (!winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(body), jo))
                    return { 400, R"({"error":"Invalid JSON body"})" };

                auto pan = m_nodeGraphController.PanOffset();
                float zoom = m_nodeGraphController.Zoom();
                bool changed = false;

                if (jo.HasKey(L"zoom"))
                {
                    auto v = jo.GetNamedValue(L"zoom");
                    if (v.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                        return { 400, R"({"error":"'zoom' must be a number"})" };
                    zoom = static_cast<float>(v.GetNumber());
                    m_nodeGraphController.SetZoom(zoom);
                    changed = true;
                }
                if (jo.HasKey(L"panX"))
                {
                    auto v = jo.GetNamedValue(L"panX");
                    if (v.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                        return { 400, R"({"error":"'panX' must be a number"})" };
                    pan.x = static_cast<float>(v.GetNumber());
                    changed = true;
                }
                if (jo.HasKey(L"panY"))
                {
                    auto v = jo.GetNamedValue(L"panY");
                    if (v.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                        return { 400, R"({"error":"'panY' must be a number"})" };
                    pan.y = static_cast<float>(v.GetNumber());
                    changed = true;
                }
                if (jo.HasKey(L"panX") || jo.HasKey(L"panY"))
                    m_nodeGraphController.SetPanOffset(pan.x, pan.y);

                // Re-read post-clamp.
                auto p2 = m_nodeGraphController.PanOffset();
                float z2 = m_nodeGraphController.Zoom();
                return { 200, std::format(
                    "{{\"ok\":true,\"changed\":{},\"zoom\":{:.6f},\"panX\":{:.6f},\"panY\":{:.6f}}}",
                    changed ? "true" : "false", z2, p2.x, p2.y) };
            });
        });

        // POST /graph/view/fit — body: { padding?:number (DIPs, default 40) }
        m_mcpServer->AddRoute(L"POST", L"/graph/view/fit", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                float padding = 40.0f;
                if (!body.empty())
                {
                    winrt::Windows::Data::Json::JsonObject jo{ nullptr };
                    if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(body), jo)
                        && jo.HasKey(L"padding"))
                    {
                        auto v = jo.GetNamedValue(L"padding");
                        if (v.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
                            padding = static_cast<float>(v.GetNumber());
                    }
                }
                FitGraphView(padding);
                auto p = m_nodeGraphController.PanOffset();
                float z = m_nodeGraphController.Zoom();
                auto b = m_nodeGraphController.ContentBounds();
                bool empty = !(b.right > b.left && b.bottom > b.top);
                return { 200, std::format(
                    "{{\"ok\":true,\"empty\":{},\"zoom\":{:.6f},\"panX\":{:.6f},\"panY\":{:.6f}}}",
                    empty ? "true" : "false", z, p.x, p.y) };
            });
        });

        // =====================================================================
        // GET /gpu/list  — Enumerate GPU adapters + identify the active one
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/gpu/list", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                auto adapters = ::ShaderLab::Rendering::RenderEngine::EnumerateAdapters();
                std::string json = "{\"active\":{";
                json += "\"name\":\"" + ToUtf8(m_renderEngine.AdapterName()) + "\"";
                json += ",\"isWarp\":" + std::string(m_renderEngine.IsWarp() ? "true" : "false");
                json += "},\"adapters\":[";
                for (size_t i = 0; i < adapters.size(); ++i)
                {
                    if (i > 0) json += ",";
                    const auto& a = adapters[i];
                    json += "{";
                    json += "\"name\":\"" + ToUtf8(a.name) + "\"";
                    json += ",\"vendorId\":" + std::to_string(a.vendorId);
                    json += ",\"deviceId\":" + std::to_string(a.deviceId);
                    json += ",\"dedicatedVideoMemoryMB\":" + std::to_string(a.dedicatedVideoMemoryMB);
                    json += ",\"isWarp\":" + std::string(a.isWarp ? "true" : "false");
                    json += ",\"luid\":{";
                    json += "\"low\":" + std::to_string(static_cast<uint32_t>(a.luid.LowPart));
                    json += ",\"high\":" + std::to_string(static_cast<int32_t>(a.luid.HighPart));
                    json += "}";
                    json += ",\"isActive\":" + std::string(
                        (a.name == m_renderEngine.AdapterName() ||
                         (a.isWarp && m_renderEngine.IsWarp())) ? "true" : "false");
                    json += "}";
                }
                json += "]}";
                return { 200, json };
            });
        });

        // =====================================================================
        // POST /gpu/switch  — Switch the active GPU adapter
        // Body: one of
        //   {"mode":"warp"}            -> WARP software adapter
        //   {"mode":"default"}         -> let driver pick (typical: integrated)
        //   {"mode":"adapter","name":"NVIDIA GeForce ..."}
        //   {"mode":"adapter","luid":{"low":NUMBER,"high":NUMBER}}
        // Returns the new active adapter (or falls back to default if the
        // requested one fails to initialize). Triggers full
        // SwitchAdapter sequence: graph save -> device teardown -> new
        // device init -> graph reload.
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/gpu/switch", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                using namespace ::ShaderLab::Rendering;
                namespace WDJ = winrt::Windows::Data::Json;
                WDJ::JsonObject jobj{ nullptr };
                if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jobj))
                    return { 400, R"({"error":"Invalid JSON body"})" };

                std::wstring mode = jobj.HasKey(L"mode")
                    ? std::wstring(jobj.GetNamedString(L"mode")) : L"default";

                LUID luid{};
                DevicePreference pref = DevicePreference::Default;

                if (mode == L"warp")
                {
                    pref = DevicePreference::Warp;
                }
                else if (mode == L"default")
                {
                    pref = DevicePreference::Default;
                }
                else if (mode == L"adapter")
                {
                    pref = DevicePreference::Adapter;
                    if (jobj.HasKey(L"luid"))
                    {
                        auto luidObj = jobj.GetNamedObject(L"luid");
                        luid.LowPart  = static_cast<DWORD>(luidObj.GetNamedNumber(L"low"));
                        luid.HighPart = static_cast<LONG>(luidObj.GetNamedNumber(L"high"));
                    }
                    else if (jobj.HasKey(L"name"))
                    {
                        auto wantedName = std::wstring(jobj.GetNamedString(L"name"));
                        auto adapters = RenderEngine::EnumerateAdapters();
                        bool found = false;
                        for (const auto& a : adapters)
                        {
                            // Case-insensitive substring match so the
                            // caller doesnt have to know exact GPU naming.
                            auto lower = a.name;
                            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                            auto wantedLower = wantedName;
                            std::transform(wantedLower.begin(), wantedLower.end(), wantedLower.begin(), ::towlower);
                            if (lower.find(wantedLower) != std::wstring::npos)
                            { luid = a.luid; found = true; break; }
                        }
                        if (!found)
                            return { 404, R"({"error":"No adapter matches the supplied name"})" };
                    }
                    else
                    {
                        return { 400, R"({"error":"adapter mode requires luid or name"})" };
                    }
                }
                else
                {
                    return { 400, R"({"error":"mode must be 'warp', 'default', or 'adapter'"})" };
                }

                SwitchAdapter(pref, luid);

                std::string json = "{\"ok\":true";
                json += ",\"active\":{";
                json += "\"name\":\"" + ToUtf8(m_renderEngine.AdapterName()) + "\"";
                json += ",\"isWarp\":" + std::string(m_renderEngine.IsWarp() ? "true" : "false");
                json += "}}";
                return { 200, json };
            });
        });

        // =====================================================================
        // /display/profiles, /display/profile, /display/profile/clear
        // -- moved to Engine/Mcp/EngineMcpRoutes.cpp (Phase 7).
        //    Uses Rendering::UpdateWorkingSpaceNodes engine helper and
        //    fires OnDisplayProfileChanged so the GUI keeps the status
        //    bar / profile selector / dirty state in sync.
        // =====================================================================

        // =====================================================================
        // POST /render/capture-node -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7). Uses Rendering::CaptureNodeAsPng so the GUI host and
        // headless host share the same render-and-encode pipeline.
        // =====================================================================

        // =====================================================================
        // POST /render/image-stats -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Engine-pure GPU reduction over a node's output.
        // =====================================================================

        // =====================================================================
        // POST /render/pixel-region — moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Uses the existing Rendering::ReadPixelRegion
        // helper through IEngineCommandSink::Dispatch -- same UI-thread
        // serialization the MainWindow shim provided, but the route body
        // now lives engine-side and is registered for the headless host
        // too.
        // =====================================================================

        // =====================================================================
        // GET /preview/view  — Current preview pan/zoom + image bounds
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/preview/view", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                auto bounds = GetPreviewImageBounds();
                float imgW = (std::max)(0.0f, bounds.right - bounds.left);
                float imgH = (std::max)(0.0f, bounds.bottom - bounds.top);
                return { 200, std::format(
                    R"({{"zoom":{:.6f},"panX":{:.6f},"panY":{:.6f})"
                    R"(,"previewNodeId":{},"imageBounds":{{"x":{:.4f},"y":{:.4f},"w":{:.4f},"h":{:.4f}}})"
                    R"(,"zoomLimits":{{"min":0.01,"max":100.0}}}})",
                    m_previewZoom, m_previewPanX, m_previewPanY,
                    m_previewNodeId, bounds.left, bounds.top, imgW, imgH) };
            });
        });

        // =====================================================================
        // POST /preview/view  — Set preview pan/zoom (any subset).
        // Body: { zoom?, panX?, panY? }   zoom clamped to [0.01, 100.0]
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/preview/view", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                namespace WDJ = winrt::Windows::Data::Json;
                WDJ::JsonObject jo{ nullptr };
                if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                    return { 400, R"({"error":"Invalid JSON body"})" };

                bool changed = false;
                if (jo.HasKey(L"zoom"))
                {
                    auto v = jo.GetNamedValue(L"zoom");
                    if (v.ValueType() != WDJ::JsonValueType::Number)
                        return { 400, R"({"error":"'zoom' must be a number"})" };
                    float z = static_cast<float>(v.GetNumber());
                    z = (std::clamp)(z, 0.01f, 100.0f);
                    m_previewZoom = z;
                    changed = true;
                }
                if (jo.HasKey(L"panX"))
                {
                    auto v = jo.GetNamedValue(L"panX");
                    if (v.ValueType() != WDJ::JsonValueType::Number)
                        return { 400, R"({"error":"'panX' must be a number"})" };
                    m_previewPanX = static_cast<float>(v.GetNumber());
                    changed = true;
                }
                if (jo.HasKey(L"panY"))
                {
                    auto v = jo.GetNamedValue(L"panY");
                    if (v.ValueType() != WDJ::JsonValueType::Number)
                        return { 400, R"({"error":"'panY' must be a number"})" };
                    m_previewPanY = static_cast<float>(v.GetNumber());
                    changed = true;
                }
                if (changed) m_forceRender = true;

                return { 200, std::format(
                    R"({{"ok":true,"changed":{},"zoom":{:.6f},"panX":{:.6f},"panY":{:.6f}}})",
                    changed ? "true" : "false",
                    m_previewZoom, m_previewPanX, m_previewPanY) };
            });
        });

        // =====================================================================
        // POST /preview/view/fit  — Fit preview image to viewport.
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/preview/view/fit", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                FitPreviewToView();
                m_forceRender = true;
                return { 200, std::format(
                    R"({{"ok":true,"zoom":{:.6f},"panX":{:.6f},"panY":{:.6f}}})",
                    m_previewZoom, m_previewPanX, m_previewPanY) };
            });
        });

        // =====================================================================
        // GET /effect/hlsl/{nodeId} -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================
        // =====================================================================
        // POST /  — MCP JSON-RPC 2.0 endpoint (Streamable HTTP transport)
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                {
                    std::string preview = body.size() > 512 ? body.substr(0, 512) + "..." : body;
                    OutputDebugStringA(("[MCP] POST / body: " + preview + "\n").c_str());
                }
                winrt::Windows::Data::Json::JsonObject jobj{ nullptr };
                if (!winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(body), jobj))
                {
                    OutputDebugStringA(("[MCP] POST / parse error. bodySize=" + std::to_string(body.size())
                        + " raw=[" + body + "]\n").c_str());
                    return { 200, R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}})" };
                }
                if (!jobj.HasKey(L"method"))
                {
                    OutputDebugStringA("[MCP] POST / missing method field\n");
                    return { 200, R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Invalid Request"}})" };
                }
                auto method = ToUtf8(std::wstring(jobj.GetNamedString(L"method")));
                OutputDebugStringA(("[MCP] method=" + method + "\n").c_str());
                auto id = jobj.HasKey(L"id") ? jobj.GetNamedValue(L"id") : winrt::Windows::Data::Json::JsonValue::CreateNullValue();
                std::string idStr;
                if (id.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
                    idStr = std::format("{}", static_cast<int64_t>(id.GetNumber()));
                else if (id.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                    idStr = "\"" + ToUtf8(std::wstring(id.GetString())) + "\"";
                else
                    idStr = "null";

                auto wrapResult = [&](const std::string& result) -> std::string {
                    return std::format(R"JSON({{"jsonrpc":"2.0","id":{},"result":{}}})JSON", idStr, result);
                };

                // ---- initialize ----
                if (method == "initialize")
                {
                    auto verStr = ToUtf8(std::wstring(::ShaderLab::VersionString));
                    std::string result = R"JSON({
"protocolVersion": "2024-11-05",
"capabilities": {
    "tools": {},
    "resources": {}
},
"serverInfo": {
    "name": "shaderlab",
    "version": ")JSON" + verStr + R"JSON("
}
})JSON";
                    return { 200, wrapResult(result) };
                }

                // ---- notifications/initialized (no response needed but we ack) ----
                if (method == "notifications/initialized")
                {
                    return { 202, "" };
                }
                // Any other notification (no id, method starts with "notifications/")
                if (method.rfind("notifications/", 0) == 0)
                {
                    return { 202, "" };
                }

                // ---- tools/list ----
                if (method == "tools/list")
                {
                    std::string tools = R"JSON({"tools":[
{"name":"graph_add_node","description":"Add a node. Use effectName for built-in/ShaderLab effects. For sources use effectName='Video' or 'Image' with optional filePath.","inputSchema":{"type":"object","properties":{"effectName":{"type":"string","description":"Effect name, or 'Video'/'Image' for source nodes"},"filePath":{"type":"string","description":"File path for Video/Image source nodes (optional)"}},"required":["effectName"]}},
{"name":"graph_remove_node","description":"Remove a node by ID","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}},
{"name":"graph_connect","description":"Connect output pin to input pin","inputSchema":{"type":"object","properties":{"srcId":{"type":"number"},"srcPin":{"type":"number"},"dstId":{"type":"number"},"dstPin":{"type":"number"}},"required":["srcId","srcPin","dstId","dstPin"]}},
{"name":"graph_disconnect","description":"Disconnect an edge","inputSchema":{"type":"object","properties":{"srcId":{"type":"number"},"srcPin":{"type":"number"},"dstId":{"type":"number"},"dstPin":{"type":"number"}},"required":["srcId","srcPin","dstId","dstPin"]}},
{"name":"graph_set_property","description":"Set a node property. Value can be number, bool, string, or array for vectors.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"key":{"type":"string"},"value":{}},"required":["nodeId","key","value"]}},
{"name":"graph_get_node","description":"Get detailed info about a node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}},
{"name":"graph_save_json","description":"Serialize graph to JSON","inputSchema":{"type":"object","properties":{}}},
{"name":"graph_load_json","description":"Load graph from JSON string","inputSchema":{"type":"object","properties":{"json":{"type":"string"}},"required":["json"]}},
{"name":"graph_clear","description":"Clear the graph","inputSchema":{"type":"object","properties":{}}},
{"name":"graph_apply","description":"Apply a graph patch in one call: add nodes (using client refs), connect edges, set property bindings. Call /graph/clear first if you need a fresh graph. Body: { nodes:[{ref,effect,filePath?,properties?}], edges:[{from,to,fromPin?,toPin?}], bindings:[{node,property,from:'ref.field'|{node,field},component?}] }. 'from' and 'to' accept either a ref string or numeric nodeId. Returns refToId map + nodeIds in add order.","inputSchema":{"type":"object","properties":{"nodes":{"type":"array"},"edges":{"type":"array"},"bindings":{"type":"array"}}}},
{"name":"effect_compile","description":"Compile HLSL for a custom effect node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"hlsl":{"type":"string"}},"required":["nodeId","hlsl"]}},
{"name":"set_preview_node","description":"Set which node is previewed","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}},
{"name":"render_capture","description":"Capture preview as PNG. Note: HDR values clipped to SDR.","inputSchema":{"type":"object","properties":{}}},
{"name":"perf_timings","description":"Get per-frame performance timings (ms) for render pipeline phases","inputSchema":{"type":"object","properties":{}}},
{"name":"node_logs","description":"Get per-node log entries (timestamped info/warning/error). Use sinceSeq for incremental reads.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"sinceSeq":{"type":"number","description":"Only return entries after this sequence number"}},"required":["nodeId"]}},
{"name":"registry_get_effect","description":"Get metadata for a built-in effect","inputSchema":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}},
{"name":"graph_bind_property","description":"Bind a node property to an upstream analysis output field","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"propertyName":{"type":"string"},"sourceNodeId":{"type":"number"},"sourceFieldName":{"type":"string"},"sourceComponent":{"type":"number","description":"0-3 for .xyzw component (scalar dest only)"}},"required":["nodeId","propertyName","sourceNodeId","sourceFieldName"]}},
{"name":"graph_unbind_property","description":"Remove a property binding","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"propertyName":{"type":"string"}},"required":["nodeId","propertyName"]}},
{"name":"read_analysis_output","description":"Read typed analysis output fields from a compute/analysis node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}},
{"name":"read_pixel_trace","description":"Run pixel trace at normalized coordinates, returns per-node pixel values and analysis outputs","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"x":{"type":"number","description":"Normalized X (0-1)"},"y":{"type":"number","description":"Normalized Y (0-1)"}},"required":["nodeId","x","y"]}},
{"name":"list_effects","description":"List all available effects (Built-in D2D + ShaderLab) with categories","inputSchema":{"type":"object","properties":{}}},
{"name":"graph_overview","description":"Compact graph summary: nodes (id, name, type, error), edges, preview node","inputSchema":{"type":"object","properties":{}}},
{"name":"get_display_info","description":"Current display capabilities, active profile, pipeline format, app version","inputSchema":{"type":"object","properties":{}}},
{"name":"graph_rename_node","description":"Rename a node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"name":{"type":"string"}},"required":["nodeId","name"]}},
{"name":"graph_snapshot","description":"Capture a PNG snapshot of the live node-graph editor view at the current pan/zoom and panel size. With inline=true returns the image as MCP image content (base64). Without inline, returns the temp file path only.","inputSchema":{"type":"object","properties":{"inline":{"type":"boolean","description":"If true, return the PNG bytes inline as MCP image content"}}}},
{"name":"graph_get_view","description":"Get the node-graph view's current zoom, pan offset, viewport size, and the bounding box of all nodes (in canvas space).","inputSchema":{"type":"object","properties":{}}},
{"name":"graph_set_view","description":"Pan and/or zoom the node-graph editor view. Any subset of {zoom, panX, panY} may be supplied. Changes apply immediately to the live UI. zoom is clamped to [0.1, 5.0]; pan has no clamp. Coordinate convention: screen = zoom * canvas + pan.","inputSchema":{"type":"object","properties":{"zoom":{"type":"number"},"panX":{"type":"number"},"panY":{"type":"number"}}}},
{"name":"graph_fit_view","description":"Fit the node-graph view to show all nodes with the given viewport-space padding (DIPs, default 40). No-op when the graph is empty.","inputSchema":{"type":"object","properties":{"padding":{"type":"number"}}}},
{"name":"list_display_profiles","description":"List all built-in display profile presets and the currently active simulated/live profile. Returns full caps (HDR, peak nits, SDR white) and CIE primaries.","inputSchema":{"type":"object","properties":{}}},
{"name":"set_display_profile","description":"Apply a simulated display profile (overrides OS-reported caps until cleared). Specify exactly ONE of: preset (factory or display name), presetIndex (0-based), iccPath (.icc/.icm file), custom (full chroma + nits spec).","inputSchema":{"type":"object","properties":{"preset":{"type":"string"},"presetIndex":{"type":"number"},"iccPath":{"type":"string"},"custom":{"type":"object","properties":{"name":{"type":"string"},"hdrEnabled":{"type":"boolean"},"sdrWhiteNits":{"type":"number"},"peakNits":{"type":"number"},"minNits":{"type":"number"},"maxFullFrameNits":{"type":"number"},"primaryRed":{"type":"array","items":{"type":"number"}},"primaryGreen":{"type":"array","items":{"type":"number"}},"primaryBlue":{"type":"array","items":{"type":"number"}},"whitePoint":{"type":"array","items":{"type":"number"}},"gamut":{"type":"string"}},"required":["peakNits"]}}}},
{"name":"clear_simulated_profile","description":"Revert to the live OS-reported display profile (clears any simulated/preset/ICC override).","inputSchema":{"type":"object","properties":{}}},
{"name":"render_capture_node","description":"Capture any node's resolved output as PNG (FORCES a render frame so dirty nodes evaluate). With inline=true returns the image as MCP image content (base64). 404 if node missing; 409 with notReady=true if the node is dirty / has unconnected inputs.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"inline":{"type":"boolean"}},"required":["nodeId"]}},
{"name":"preview_get_view","description":"Get the preview pane's current zoom + pan + image bounds + zoom limits.","inputSchema":{"type":"object","properties":{}}},
{"name":"preview_set_view","description":"Set the preview pane's zoom and/or pan. zoom clamped to [0.01, 100.0]. Returns post-clamp values.","inputSchema":{"type":"object","properties":{"zoom":{"type":"number"},"panX":{"type":"number"},"panY":{"type":"number"}}}},
{"name":"preview_fit_view","description":"Fit the preview image to the preview viewport (auto zoom + center).","inputSchema":{"type":"object","properties":{}}},
{"name":"image_stats","description":"GPU-accelerated per-channel image statistics (min/max/mean/median/p95/sum + nonzero counts). Forces a render frame first so the target node is fresh. Channels default to luminance+R+G+B+A; pass channels:[\"luminance\"] to skip the others. nonzeroOnly excludes zero pixels from min/max/mean/sum.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"nonzeroOnly":{"type":"boolean"},"channels":{"type":"array","items":{"type":"string","enum":["luminance","r","g","b","a"]}}},"required":["nodeId"]}},
{"name":"read_pixel_region","description":"Read a small w x h region of FP32 RGBA pixels from a node's output (scRGB linear-light). Region is capped at 32x32 (1024 pixels) and per-axis at 64. Pixels are returned row-major as a flat float array (RGBARGBA...).","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"x":{"type":"number"},"y":{"type":"number"},"w":{"type":"number"},"h":{"type":"number"}},"required":["nodeId","x","y","w","h"]}},
{"name":"effect_get_hlsl","description":"Read a node's custom-effect HLSL source, parameter list, compile state, and last runtime error. For non-custom nodes returns hasCustomEffect=false (200, not 404). For ShaderLab library effects, also includes isLibraryEffect=true + shaderLabEffectId/Version.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}},
{"name":"list_gpus","description":"Enumerate available GPU adapters (DXGI). Returns the active adapter and a list of all installed adapters with name, vendorId, deviceId, dedicated VRAM (MB), LUID, and isWarp flag.","inputSchema":{"type":"object","properties":{}}},
{"name":"switch_gpu","description":"Switch the active GPU adapter. Triggers a full graph-save, device-teardown, and graph-reload cycle. Use mode='warp' for the WARP software adapter, 'default' to let the driver pick, or 'adapter' with either {luid:{low,high}} or {name:'partial-match'}. Falls back to default if the requested adapter fails to initialize.","inputSchema":{"type":"object","properties":{"mode":{"type":"string","enum":["warp","default","adapter"]},"name":{"type":"string","description":"Substring match against adapter name (used when mode='adapter')"},"luid":{"type":"object","properties":{"low":{"type":"number"},"high":{"type":"number"}}}},"required":["mode"]}}
]})JSON";
                    return { 200, wrapResult(tools) };
                }

                // ---- tools/call ----
                if (method == "tools/call")
                {
                    auto params = jobj.GetNamedObject(L"params");
                    auto toolName = ToUtf8(std::wstring(params.GetNamedString(L"name")));
                    auto args = params.HasKey(L"arguments") ? params.GetNamedObject(L"arguments") : winrt::Windows::Data::Json::JsonObject();
                    auto argsStr = ToUtf8(std::wstring(args.Stringify()));

                    // Route to existing REST handlers.
                    ::ShaderLab::McpHttpServer::Response restResp = { 404, "" };

                    if (toolName == "graph_add_node")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/add-node", argsStr);
                    else if (toolName == "graph_remove_node")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/remove-node", argsStr);
                    else if (toolName == "graph_connect")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/connect", argsStr);
                    else if (toolName == "graph_disconnect")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/disconnect", argsStr);
                    else if (toolName == "graph_set_property")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/set-property", argsStr);
                    else if (toolName == "graph_get_node")
                    {
                        auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                        restResp = m_mcpServer->RouteRequest(L"GET", std::format(L"/graph/node/{}", nodeId), "");
                    }
                    else if (toolName == "graph_save_json")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/graph/save", "");
                    else if (toolName == "graph_load_json")
                    {
                        auto jsonStr = ToUtf8(std::wstring(args.GetNamedString(L"json")));
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/load", jsonStr);
                    }
                    else if (toolName == "graph_clear")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/clear", "");
                    else if (toolName == "graph_apply")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/apply", argsStr);
                    else if (toolName == "effect_compile")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/effect/compile", argsStr);
                    else if (toolName == "set_preview_node")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/preview-node", argsStr);
                    else if (toolName == "render_capture")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/render/capture", "");
                    else if (toolName == "perf_timings")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/perf", "");
                    else if (toolName == "node_logs")
                    {
                        auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                        uint64_t sinceSeq = 0;
                        if (args.HasKey(L"sinceSeq"))
                            sinceSeq = static_cast<uint64_t>(args.GetNamedNumber(L"sinceSeq"));
                        restResp = m_mcpServer->RouteRequest(L"GET",
                            std::format(L"/node/{}/logs?since={}", nodeId, sinceSeq), "");
                    }
                    else if (toolName == "registry_get_effect")
                    {
                        auto name = std::wstring(args.GetNamedString(L"name"));
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/registry/effect/" + name, "");
                    }
                    else if (toolName == "graph_bind_property")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/bind-property", argsStr);
                    else if (toolName == "graph_unbind_property")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/unbind-property", argsStr);
                    else if (toolName == "read_analysis_output")
                    {
                        auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                        restResp = m_mcpServer->RouteRequest(L"GET", std::format(L"/analysis/{}", nodeId), "");
                    }
                    else if (toolName == "read_pixel_trace")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/pixel-trace", argsStr);
                    else if (toolName == "list_effects")
                    {
                        restResp = DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                            std::string json = "{\"builtIn\":{";
                            auto& reg = ::ShaderLab::Effects::EffectRegistry::Instance();
                            auto cats = reg.Categories();
                            bool firstCat = true;
                            for (const auto& cat : cats)
                            {
                                if (cat == L"Analysis") continue;
                                if (!firstCat) json += ",";
                                json += "\"" + ToUtf8(cat) + "\":[";
                                auto effects = reg.ByCategory(cat);
                                bool firstFx = true;
                                for (const auto* e : effects)
                                {
                                    if (!firstFx) json += ",";
                                    json += "\"" + ToUtf8(e->name) + "\"";
                                    firstFx = false;
                                }
                                json += "]";
                                firstCat = false;
                            }
                            json += "},\"shaderLab\":{";
                            auto& sl = ::ShaderLab::Effects::ShaderLabEffects::Instance();
                            auto slCats = sl.Categories();
                            firstCat = true;
                            for (const auto& cat : slCats)
                            {
                                if (!firstCat) json += ",";
                                json += "\"" + ToUtf8(cat) + "\":[";
                                auto effects = sl.ByCategory(cat);
                                bool firstFx = true;
                                for (const auto* e : effects)
                                {
                                    if (!firstFx) json += ",";
                                    json += "\"" + ToUtf8(e->name) + "\"";
                                    firstFx = false;
                                }
                                json += "]";
                                firstCat = false;
                            }
                            json += "}}";
                            return { 200, json };
                        });
                    }
                    else if (toolName == "graph_overview")
                    {
                        restResp = DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                            std::string json = "{\"previewNodeId\":" + std::to_string(m_previewNodeId) + ",\"nodes\":[";
                            bool first = true;
                            for (const auto& n : m_graph.Nodes())
                            {
                                if (!first) json += ",";
                                std::string typeStr;
                                switch (n.type)
                                {
                                case ::ShaderLab::Graph::NodeType::Source:        typeStr = "Source"; break;
                                case ::ShaderLab::Graph::NodeType::BuiltInEffect: typeStr = "BuiltIn"; break;
                                case ::ShaderLab::Graph::NodeType::PixelShader:   typeStr = "PixelShader"; break;
                                case ::ShaderLab::Graph::NodeType::ComputeShader: typeStr = "ComputeShader"; break;
                                case ::ShaderLab::Graph::NodeType::Output:        typeStr = "Output"; break;
                                }
                                json += std::format("{{\"id\":{},\"name\":\"{}\",\"type\":\"{}\"",
                                    n.id, ToUtf8(n.name), typeStr);
                                if (!n.runtimeError.empty())
                                    json += ",\"error\":\"" + ToUtf8(n.runtimeError) + "\"";
                                json += std::format(",\"inputs\":{},\"outputs\":{}}}", n.inputPins.size(), n.outputPins.size());
                                first = false;
                            }
                            json += "],\"edges\":[";
                            first = true;
                            for (const auto& e : m_graph.Edges())
                            {
                                if (!first) json += ",";
                                json += std::format("[{},{},{},{}]", e.sourceNodeId, e.sourcePin, e.destNodeId, e.destPin);
                                first = false;
                            }
                            json += "]}";
                            return { 200, json };
                        });
                    }
                    else if (toolName == "get_display_info")
                    {
                        restResp = DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                            auto profile = m_displayMonitor.ActiveProfile();
                            auto live = m_displayMonitor.LiveProfile();
                            auto caps = m_displayMonitor.CachedCapabilities();
                            auto verStr = ToUtf8(std::wstring(::ShaderLab::VersionString));
                            std::string json = std::format(
                                "{{\"appVersion\":\"{}\",\"graphFormatVersion\":{}"
                                ",\"pipeline\":\"{}\""
                                ",\"display\":{{\"hdr\":{},\"maxNits\":{:.0f},\"sdrWhiteNits\":{:.0f}"
                                ",\"simulated\":{},\"profileName\":\"{}\""
                                ",\"activeGamut\":{{\"red\":[{:.4f},{:.4f}],\"green\":[{:.4f},{:.4f}],\"blue\":[{:.4f},{:.4f}]}}"
                                ",\"monitorGamut\":{{\"red\":[{:.4f},{:.4f}],\"green\":[{:.4f},{:.4f}],\"blue\":[{:.4f},{:.4f}]}}"
                                "}}}}",
                                verStr, ::ShaderLab::GraphFormatVersion,
                                ToUtf8(std::wstring(m_renderEngine.ActiveFormat().name)),
                                caps.hdrEnabled ? "true" : "false",
                                caps.maxLuminanceNits, caps.sdrWhiteLevelNits,
                                profile.isSimulated ? "true" : "false",
                                ToUtf8(profile.profileName),
                                profile.primaryRed.x, profile.primaryRed.y,
                                profile.primaryGreen.x, profile.primaryGreen.y,
                                profile.primaryBlue.x, profile.primaryBlue.y,
                                live.primaryRed.x, live.primaryRed.y,
                                live.primaryGreen.x, live.primaryGreen.y,
                                live.primaryBlue.x, live.primaryBlue.y);
                            return { 200, json };
                        });
                    }
                    else if (toolName == "graph_rename_node")
                    {
                        restResp = DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                            auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                            auto newName = std::wstring(args.GetNamedString(L"name"));
                            auto* node = m_graph.FindNode(nodeId);
                            if (!node) return { 404, R"({"error":"Node not found"})" };
                            node->name = newName;
                            m_nodeGraphController.RebuildLayout();
                            PopulatePreviewNodeSelector();
                            PopulateAddNodeFlyout();
                            return { 200, R"({"ok":true})" };
                        });
                    }
                    else if (toolName == "graph_snapshot")
                    {
                        // Forward to REST handler. Re-serialize args to JSON so
                        // the route gets a proper body containing {inline:bool}.
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/snapshot", argsStr);

                        // When the agent requested inline image bytes, repack
                        // the response as MCP-native image content (not text).
                        bool wantInline = false;
                        if (args.HasKey(L"inline"))
                        {
                            auto v = args.GetNamedValue(L"inline");
                            if (v.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean)
                                wantInline = v.GetBoolean();
                        }
                        if (wantInline && restResp.statusCode == 200)
                        {
                            // Parse base64 + mimeType out of the REST response
                            // and emit MCP image content directly so we skip
                            // the text-escape wrapping below.
                            winrt::Windows::Data::Json::JsonObject ro{ nullptr };
                            if (winrt::Windows::Data::Json::JsonObject::TryParse(
                                    winrt::to_hstring(restResp.body), ro)
                                && ro.HasKey(L"base64") && ro.HasKey(L"mimeType"))
                            {
                                auto b64 = ToUtf8(std::wstring(ro.GetNamedString(L"base64")));
                                auto mime = ToUtf8(std::wstring(ro.GetNamedString(L"mimeType")));
                                std::string content = std::format(
                                    R"JSON({{"content":[{{"type":"image","data":"{}","mimeType":"{}"}}],"isError":false}})JSON",
                                    b64, mime);
                                return { 200, wrapResult(content) };
                            }
                        }
                    }
                    else if (toolName == "graph_get_view")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/graph/view", "");
                    else if (toolName == "graph_set_view")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/view", argsStr);
                    else if (toolName == "graph_fit_view")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/view/fit", argsStr);
                    else if (toolName == "list_display_profiles")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/display/profiles", "");
                    else if (toolName == "set_display_profile")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/display/profile", argsStr);
                    else if (toolName == "clear_simulated_profile")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/display/profile/clear", "");
                    else if (toolName == "preview_get_view")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/preview/view", "");
                    else if (toolName == "preview_set_view")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/preview/view", argsStr);
                    else if (toolName == "preview_fit_view")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/preview/view/fit", "");
                    else if (toolName == "image_stats")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/image-stats", argsStr);
                    else if (toolName == "read_pixel_region")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/pixel-region", argsStr);
                    else if (toolName == "effect_get_hlsl")
                    {
                        auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                        restResp = m_mcpServer->RouteRequest(L"GET",
                            std::format(L"/effect/hlsl/{}", nodeId), "");
                    }
                    else if (toolName == "list_gpus")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/gpu/list", "");
                    else if (toolName == "switch_gpu")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/gpu/switch", argsStr);
                    else if (toolName == "render_capture_node")
                    {
                        // Forward to REST handler; if inline=true was requested
                        // and we got a successful PNG back, repack as MCP-native
                        // image content (mirroring the graph_snapshot flow).
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/capture-node", argsStr);
                        bool wantInline = args.HasKey(L"inline")
                            && args.GetNamedValue(L"inline").ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean
                            && args.GetNamedBoolean(L"inline");
                        if (wantInline && restResp.statusCode == 200)
                        {
                            winrt::Windows::Data::Json::JsonObject ro{ nullptr };
                            if (winrt::Windows::Data::Json::JsonObject::TryParse(
                                    winrt::to_hstring(restResp.body), ro)
                                && ro.HasKey(L"base64") && ro.HasKey(L"mimeType"))
                            {
                                auto b64 = ToUtf8(std::wstring(ro.GetNamedString(L"base64")));
                                auto mime = ToUtf8(std::wstring(ro.GetNamedString(L"mimeType")));
                                std::string content = std::format(
                                    R"JSON({{"content":[{{"type":"image","data":"{}","mimeType":"{}"}}],"isError":false}})JSON",
                                    b64, mime);
                                return { 200, wrapResult(content) };
                            }
                        }
                    }

                    bool isError = restResp.statusCode >= 400;

                    // MCP requires content[].text to be a STRING, not raw JSON.
                    // Escape the body for embedding in a JSON string value.
                    std::string escaped;
                    for (char c : restResp.body)
                    {
                        if (c == '"') escaped += "\\\"";
                        else if (c == '\\') escaped += "\\\\";
                        else if (c == '\n') escaped += "\\n";
                        else if (c == '\r') escaped += "\\r";
                        else if (c == '\t') escaped += "\\t";
                        else escaped += c;
                    }

                    std::string content = std::format(
                        R"JSON({{"content":[{{"type":"text","text":"{}"}}],"isError":{}}})JSON",
                        escaped.empty() ? "" : escaped,
                        isError ? "true" : "false");

                    return { 200, wrapResult(content) };
                }

                // ---- resources/list ----
                if (method == "resources/list")
                {
                    std::string resources = R"JSON({"resources":[
{"uri":"shaderlab://context","name":"ShaderLab Context","description":"System prompt: pipeline format, shader conventions, API reference","mimeType":"application/json"},
{"uri":"shaderlab://graph","name":"Effect Graph","description":"Full graph state with nodes, edges, properties, custom effect definitions","mimeType":"application/json"},
{"uri":"shaderlab://registry/effects","name":"Built-in Effects","description":"All 48+ built-in D2D effects with property metadata","mimeType":"application/json"},
{"uri":"shaderlab://custom-effects","name":"Custom Effects","description":"Custom effects in graph with HLSL source and compile status","mimeType":"application/json"}
]})JSON";
                    return { 200, wrapResult(resources) };
                }

                // ---- resources/read ----
                if (method == "resources/read")
                {
                    auto params2 = jobj.GetNamedObject(L"params");
                    auto uri = ToUtf8(std::wstring(params2.GetNamedString(L"uri")));

                    std::string restPath;
                    if (uri == "shaderlab://context") restPath = "/context";
                    else if (uri == "shaderlab://graph") restPath = "/graph";
                    else if (uri == "shaderlab://registry/effects") restPath = "/registry/effects";
                    else if (uri == "shaderlab://custom-effects") restPath = "/custom-effects";
                    else
                        return { 200, wrapResult(R"JSON({"contents":[]})JSON") };

                    auto restResp = m_mcpServer->RouteRequest(L"GET", std::wstring(restPath.begin(), restPath.end()), "");

                    // Escape the JSON body for embedding in the text field.
                    std::string escaped;
                    for (char c : restResp.body)
                    {
                        if (c == '"') escaped += "\\\"";
                        else if (c == '\\') escaped += "\\\\";
                        else if (c == '\n') escaped += "\\n";
                        else if (c == '\r') escaped += "\\r";
                        else escaped += c;
                    }

                    std::string result = std::format(
                        R"JSON({{"contents":[{{"uri":"{}","mimeType":"application/json","text":"{}"}}]}})JSON",
                        uri, escaped);
                    return { 200, wrapResult(result) };
                }

                // ---- ping ----
                if (method == "ping")
                    return { 200, wrapResult("{}") };

                // Unknown method.
                return { 200, std::format(
                    R"JSON({{"jsonrpc":"2.0","id":{},"error":{{"code":-32601,"message":"Method not found: {}"}}}})JSON",
                    idStr, method) };
            }
            catch (const std::exception& ex)
            {
                return { 200, std::format(
                    R"JSON({{"jsonrpc":"2.0","id":null,"error":{{"code":-32700,"message":"Parse error: {}"}}}})JSON",
                    ex.what()) };
            }
        });
    }
}
