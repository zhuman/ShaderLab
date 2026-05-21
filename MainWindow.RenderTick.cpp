// MainWindow partial (Phase 4 split): the OnRenderTick / RenderFrame
// render loop, including the dirty-propagation pre-pass, video tick,
// and output-window present. All methods are members of
// `winrt::ShaderLab::implementation::MainWindow`. Extracted from
// MainWindow.xaml.cpp at commit c177770.

#include "pch.h"
#include "MainWindow.xaml.h"

#include "Rendering/PipelineFormat.h"

namespace winrt::ShaderLab::implementation
{
    // -----------------------------------------------------------------------
    // Render loop
    //
    // The render loop is split across two threads:
    //   - UI thread (m_renderTimer DispatcherQueueTimer): runs OnRenderTick.
    //     Handles XAML-touching work only -- editor-canvas redraw on the
    //     UI-side D2D context, FPS panel text update, video seek slider,
    //     properties panel refresh, MCP indicator, log windows.
    //   - Render-worker thread (m_renderWorker): runs RenderWorkerLoop ->
    //     RenderTickBody. Handles all graph + GPU work -- working space
    //     sync, capture/clock/video upload, dirty propagation, RenderFrame
    //     (which evaluates the graph and presents the main swap chain),
    //     and snapshot publication.
    //
    // The two threads communicate through:
    //   - m_renderDispatcher: closures from UI/MCP land on the render thread
    //   - m_uiGraphSnapshot: render thread publishes per-frame; UI reads
    //
    // This split exists so that a slow GPU evaluation (heavy graph, expensive
    // synchronous compute readbacks) cannot block UI input handling. The
    // user-visible win is that buttons / flyouts / canvas pan-zoom stay
    // responsive even when the render side is at ~2 fps on a heavy graph.
    // -----------------------------------------------------------------------

    void MainWindow::OnRenderTick(
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& /*sender*/,
        winrt::Windows::Foundation::IInspectable const& /*args*/)
    {
        if (m_isShuttingDown) return;
        if (!m_renderEngine.IsInitialized()) return;

        try
        {
        // Compute frame delta time (used by the render-tick body for clock
        // node advancement and frame timing).
        auto now = std::chrono::steady_clock::now();
        double deltaSec = std::chrono::duration<double>(now - m_lastRenderTick).count();
        m_lastRenderTick = now;
        if (deltaSec > 0.1) deltaSec = 0.016;

        auto tTickStart = std::chrono::high_resolution_clock::now();

        // Drain pending dispatcher closures: NO-OP from the UI side post-P7.
        // The worker thread is the registered consumer and drains its own
        // queue. UI thread reading the queue would race graph mutations and
        // process closures slowly because OnRenderTick also does blit /
        // canvas redraw / event handling. Leaving Drain() here is the
        // primary cause of UI dropdown / hover input lag under load:
        // long-running MCP closures end up running on the UI thread,
        // blocking input event delivery for the duration. Worker calls
        // m_renderDispatcher.Drain() in RenderWorkerLoop -- that's the only
        // path closures should run on.
        // m_renderDispatcher.Drain();   // <-- removed in 7fa7021+

        // Blit the most recently published offscreen frame into the
        // SwapChainPanel-bound swap chain and Present.
        BlitOffscreenToSwapChain();

        // Editor canvas redraw (UI-side D2D context, P4).
        // Trigger a redraw whenever the worker has published a new snapshot
        // since our last UI tick -- the canvas reads runtime fields like
        // clockTime / analysisOutput from the live graph, but doesn't
        // self-invalidate when those change. Without this, the canvas only
        // redraws on UI-side interaction, so a playing Clock or Video looks
        // frozen even though the worker is ticking and republishing.
        if (m_frameGeneration != m_lastSeenFrameGeneration)
        {
            m_nodeGraphController.SetNeedsRedraw();
            m_lastSeenFrameGeneration = m_frameGeneration;
        }
        RenderNodeGraph();

        // P7: present any open output windows on the UI thread. Render
        // worker has already drawn into each sink's offscreen pair (see
        // MainWindow::RenderOutputSinks called from RenderFrameToOffscreen);
        // here we just sync UI view state + blit + Present1 per window.
        PresentOutputWindows();
        auto tNodeGraphEnd = std::chrono::high_resolution_clock::now();

        // Accumulate UI-tick timing.
        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_frameTiming;
            t.uiTickUs      = t.uiTickUs * (1-a) + usec(tTickStart, tNodeGraphEnd) * a;
        }

        // Update video seek slider and position label while playing.
        if (m_videoSeekSlider && m_videoSeekNodeId != 0)
        {
            auto* vp = m_sourceFactory.GetVideoProvider(m_videoSeekNodeId);
            if (vp && vp->IsOpen())
            {
                double pos = vp->CurrentPosition();
                m_videoSeekSuppressEvents = true;
                m_videoSeekSlider.Value(pos);
                m_videoSeekSuppressEvents = false;
                if (m_videoPositionLabel)
                    m_videoPositionLabel.Text(std::format(L"Position: {:.1f}s / {:.1f}s", pos, vp->Duration()));
            }
        }

        // Periodic UI updates at 250 ms (log windows, properties panel,
        // MCP activity indicator, FPS tooltip) and 1 s (FPS counter).
        auto fpsNow = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(fpsNow - m_fpsTimePoint).count();
        if (elapsed >= 250)
        {
            if (!m_logWindows.empty())
                UpdateLogWindows();
            if (m_selectedNodeId != 0 && m_graph.HasDirtyNodes())
            {
                auto* selNode = m_graph.FindNode(m_selectedNodeId);
                if (selNode && !selNode->propertyBindings.empty() &&
                    !IsPropertiesPanelInteracting())
                    UpdatePropertiesPanel();
            }
            UpdateMcpActivityIndicator();
            UpdateFpsTooltip();
        }
        if (elapsed >= 1000)
        {
            uint64_t framesSeen = m_frameCount.exchange(0, std::memory_order_relaxed);
            float fps = static_cast<float>(framesSeen) * 1000.0f / static_cast<float>(elapsed);

            uint64_t currentVideoUploads = m_sourceFactory.TotalVideoUploads();
            float videoFps = static_cast<float>(currentVideoUploads - m_lastVideoUploadCount) * 1000.0f / static_cast<float>(elapsed);
            m_lastVideoUploadCount = currentVideoUploads;
            m_lastVideoFps = videoFps;
            m_lastFps = fps;

            FpsText().Text(std::format(L"{:.0f} fps | {:.1f} ms", fps, m_frameTiming.totalUs / 1000.0));
            UpdateFpsTooltip();
            m_fpsTimePoint = fpsNow;
        }

        } // end try
        catch (const winrt::hresult_error& ex)
        {
            OutputDebugStringW(std::format(L"[OnRenderTick] Exception: 0x{:08X}\n",
                static_cast<uint32_t>(ex.code())).c_str());
        }
        catch (const std::exception& ex)
        {
            OutputDebugStringW(std::format(L"[OnRenderTick] std::exception: {}\n",
                std::wstring(ex.what(), ex.what() + strlen(ex.what()))).c_str());
        }
        catch (...)
        {
            OutputDebugStringW(L"[OnRenderTick] Unknown exception\n");
        }
    }

    // ---------------------------------------------------------------------
    // RenderWorkerLoop -- render thread entry point.
    //
    // Runs the offscreen render path: each iteration evaluates the graph
    // and draws the preview image into a double-buffered offscreen target,
    // then publishes the buffer index for UI thread to blit.
    // ---------------------------------------------------------------------
    void MainWindow::RenderWorkerLoop(std::stop_token stop)
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        m_renderDispatcher.RegisterConsumer();

        auto last = std::chrono::steady_clock::now();
        while (!stop.stop_requested() && !m_renderShouldStop.load(std::memory_order_acquire))
        {
            m_renderDispatcher.WaitFor(std::chrono::milliseconds(16));
            m_renderDispatcher.Drain();
            if (stop.stop_requested() || m_renderShouldStop.load(std::memory_order_acquire))
                break;
            if (m_isShuttingDown) break;
            if (!m_renderEngine.IsInitialized()) continue;

            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            if (dt > 0.1) dt = 0.016;

            try
            {
                // Per-tick non-GPU work that previously lived in OnRenderTick:
                // working space sync, capture/clock tick, video upload, dirty
                // propagation. Then the offscreen render itself.
                UpdateWorkingSpaceNodes();

                // Use the render-thread D2D context for source uploads. They
                // create D2D bitmaps that the evaluator (also using the
                // render context) will draw -- everything stays on one
                // context to avoid cross-context state races.
                if (auto* dc5 = static_cast<ID2D1DeviceContext5*>(m_renderEngine.RenderD2DContext()))
                {
                    auto& nodes = const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes());
                    if (m_sourceFactory.TickAndUploadLiveCaptures(nodes, dc5))
                        m_forceRender = true;
                }

                // Tick clock nodes: advance time. (Same code as OnRenderTick's
                // body uses; safe to call from render thread because m_graph
                // is single-writer in this design.)
                for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
                {
                    if (!node.isClock) continue;
                    auto getF = [&](const std::wstring& k, float def) {
                        auto it = node.properties.find(k);
                        if (it != node.properties.end())
                            if (auto* f = std::get_if<float>(&it->second)) return *f;
                        return def;
                    };
                    bool autoDuration = getF(L"AutoDuration", 1.0f) > 0.5f;
                    if (autoDuration && node.propertyBindings.count(L"StopTime"))
                    {
                        autoDuration = false;
                        node.properties[L"AutoDuration"] = 0.0f;
                    }
                    if (autoDuration)
                    {
                        float maxDur = 0.0f;
                        for (const auto& other : m_graph.Nodes())
                        {
                            if (other.id == node.id) continue;
                            bool boundToThisClock = false;
                            for (const auto& [propName, binding] : other.propertyBindings)
                            {
                                for (const auto& src : binding.sources)
                                {
                                    if (src && src->sourceNodeId == node.id)
                                    { boundToThisClock = true; break; }
                                }
                                if (boundToThisClock) break;
                            }
                            if (!boundToThisClock) continue;
                            for (const auto& field : other.analysisOutput.fields)
                            {
                                if (field.name == L"Duration" && field.components[0] > 0.0f)
                                    if (field.components[0] > maxDur) maxDur = field.components[0];
                            }
                        }
                        if (maxDur > 0.0f)
                            node.properties[L"StopTime"] = maxDur;
                    }
                    if (node.isPlaying)
                    {
                        float startTime = getF(L"StartTime", 0.0f);
                        float stopTime = getF(L"StopTime", 10.0f);
                        float speed = getF(L"Speed", 1.0f);
                        bool loop = getF(L"Loop", 1.0f) > 0.5f;
                        double duration = static_cast<double>(stopTime - startTime);
                        if (duration <= 0.0) duration = 1.0;
                        node.clockTime += dt * speed;
                        if (loop)
                        {
                            while (node.clockTime >= duration) node.clockTime -= duration;
                            while (node.clockTime < 0.0) node.clockTime += duration;
                        }
                        else
                        {
                            node.clockTime = std::clamp(node.clockTime, 0.0, duration);
                            if (node.clockTime >= duration) node.isPlaying = false;
                        }
                        node.dirty = true;
                    }
                }

                m_graphEvaluator.ResolveSourceBindings(m_graph);

                if (auto* dc = m_renderEngine.RenderD2DContext())
                {
                    try {
                        m_sourceFactory.TickAndUploadVideos(
                            const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()),
                            dc, dt);
                    } catch (...) {}
                }

                // Dirty propagation downstream.
                {
                    std::vector<uint32_t> queue;
                    for (const auto& node : m_graph.Nodes())
                        if (node.dirty) queue.push_back(node.id);
                    for (size_t i = 0; i < queue.size(); ++i)
                    {
                        for (const auto* edge : m_graph.GetOutputEdges(queue[i]))
                        {
                            auto* dn = m_graph.FindNode(edge->destNodeId);
                            if (dn && !dn->dirty)
                            {
                                dn->dirty = true;
                                queue.push_back(edge->destNodeId);
                            }
                        }
                    }
                }

                bool wasForceRender = m_forceRender;
                bool hasDirty = m_graph.HasDirtyNodes();
                // P7: when output windows are open, force eval every frame
                // so the worker keeps producing frames into their offscreen
                // pairs even if no graph node is dirty (e.g. static Gamut
                // Source feeding an Output node). Same gate as the legacy
                // RenderFrame had.
                bool hasOutputWindows = false;
                {
                    std::scoped_lock lk(m_outputSinksMutex);
                    hasOutputWindows = !m_outputSinks.empty();
                }
                bool needsEval = hasDirty || m_needsFitPreview || m_forceRender || hasOutputWindows;
                if (needsEval)
                {
                    RenderFrameToOffscreen(dt);
                    m_forceRender = false;
                    m_frameCount.fetch_add(1, std::memory_order_relaxed);
                    if (hasDirty || wasForceRender)
                        ++m_graphGeneration;
                }

                // Publish snapshot.
                ++m_frameGeneration;
                auto snap = ::ShaderLab::Graph::BuildGraphUiSnapshot(
                    m_graph, m_previewNodeId, m_graphGeneration, m_frameGeneration);
                std::atomic_store(&m_uiGraphSnapshot,
                    std::shared_ptr<const ::ShaderLab::Graph::GraphUiSnapshot>(snap));
            }
            catch (const winrt::hresult_error& ex)
            {
                OutputDebugStringW(std::format(L"[RenderWorker] hresult: 0x{:08X}\n",
                    static_cast<uint32_t>(ex.code())).c_str());
            }
            catch (...)
            {
                OutputDebugStringW(L"[RenderWorker] tick exception\n");
            }
        }

        m_renderDispatcher.Drain();
    }

    // ---------------------------------------------------------------------
    // RenderTickBody -- body of the render-thread tick. All graph + GPU work.
    // Equivalent to the old OnRenderTick body before the split.
    // ---------------------------------------------------------------------
    void MainWindow::RenderTickBody(double deltaSec)
    {
        if (m_isShuttingDown) return;
        if (!m_renderEngine.IsInitialized()) return;

        auto tTickStart = std::chrono::high_resolution_clock::now();

        // Mirror the active display profile into Working Space parameter
        // nodes. This is a cheap node-list walk that no-ops when no
        // Working Space nodes are present and only marks dirty when at
        // least one field actually changed, so freshly-added nodes pick
        // up live values immediately without hooking every AddNode site.
        UpdateWorkingSpaceNodes();

        // Tick live capture providers (DXGI Desktop Duplication, Windows
        // Graphics Capture). These don't go through the dirty/video-
        // provider path the rest of the source-prep loop uses, so call
        // their dedicated tick here. A captured frame marks the source
        // node dirty so the existing needsEval gate triggers a re-eval
        // and present.
        if (auto* dc5 = static_cast<ID2D1DeviceContext5*>(m_renderEngine.D2DDeviceContext()))
        {
            auto& nodes = const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes());
            if (m_sourceFactory.TickAndUploadLiveCaptures(nodes, dc5))
                m_forceRender = true;
        }

        // Tick clock nodes: advance time.
        for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
        {
            if (node.isClock)
            {
                auto getF = [&](const std::wstring& k, float def) {
                    auto it = node.properties.find(k);
                    if (it != node.properties.end())
                        if (auto* f = std::get_if<float>(&it->second)) return *f;
                    return def;
                };

                bool autoDuration = getF(L"AutoDuration", 1.0f) > 0.5f;
                if (autoDuration && node.propertyBindings.count(L"StopTime"))
                {
                    autoDuration = false;
                    node.properties[L"AutoDuration"] = 0.0f;
                }
                if (autoDuration)
                {
                    float maxDur = 0.0f;
                    for (const auto& other : m_graph.Nodes())
                    {
                        if (other.id == node.id) continue;
                        bool boundToThisClock = false;
                        for (const auto& [propName, binding] : other.propertyBindings)
                        {
                            for (const auto& src : binding.sources)
                            {
                                if (src && src->sourceNodeId == node.id)
                                { boundToThisClock = true; break; }
                            }
                            if (boundToThisClock) break;
                        }
                        if (!boundToThisClock) continue;
                        for (const auto& field : other.analysisOutput.fields)
                        {
                            if (field.name == L"Duration" && field.components[0] > 0.0f)
                                if (field.components[0] > maxDur) maxDur = field.components[0];
                        }
                    }
                    if (maxDur > 0.0f)
                        node.properties[L"StopTime"] = maxDur;
                }

                if (node.isPlaying)
                {
                    float startTime = getF(L"StartTime", 0.0f);
                    float stopTime = getF(L"StopTime", 10.0f);
                    float speed = getF(L"Speed", 1.0f);
                    bool loop = getF(L"Loop", 1.0f) > 0.5f;

                    double duration = static_cast<double>(stopTime - startTime);
                    if (duration <= 0.0) duration = 1.0;

                    node.clockTime += deltaSec * speed;

                    if (loop)
                    {
                        while (node.clockTime >= duration) node.clockTime -= duration;
                        while (node.clockTime < 0.0) node.clockTime += duration;
                    }
                    else
                    {
                        node.clockTime = std::clamp(node.clockTime, 0.0, duration);
                        if (node.clockTime >= duration) node.isPlaying = false;
                    }

                    node.dirty = true;
                    m_nodeGraphController.SetNeedsRedraw();
                }
            }
        }

        // Resolve source node property bindings (e.g., Clock.Time → Video.Time)
        // BEFORE ticking video sources, so they see the updated time values.
        m_graphEvaluator.ResolveSourceBindings(m_graph);

        // Tick video sources and upload new frames.
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (dc)
        {
            try {
                m_sourceFactory.TickAndUploadVideos(
                    const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()),
                    dc, deltaSec);
            } catch (...) {}
        }

        // Propagate dirty flags downstream so D3D11 compute effects
        // re-dispatch when upstream sources change (video frames, animation).
        // Runs AFTER video tick so new-frame dirty flags reach compute nodes.
        {
            std::vector<uint32_t> queue;
            for (const auto& node : m_graph.Nodes())
                if (node.dirty) queue.push_back(node.id);
            for (size_t i = 0; i < queue.size(); ++i)
            {
                for (const auto* edge : m_graph.GetOutputEdges(queue[i]))
                {
                    auto* dn = m_graph.FindNode(edge->destNodeId);
                    if (dn && !dn->dirty)
                    {
                        dn->dirty = true;
                        queue.push_back(edge->destNodeId);
                    }
                }
            }
        }

        // Only re-evaluate the graph when something changed. Always render
        // if output windows are open (they need continuous present).
        bool wasForceRender = m_forceRender;
        bool hasDirty = m_graph.HasDirtyNodes();
        bool hasOutputWindows = !m_outputWindows.empty();
        bool needsEval = hasDirty || m_needsFitPreview || m_forceRender || hasOutputWindows;
        auto tVideoTickEnd = std::chrono::high_resolution_clock::now();
        if (needsEval)
        {
            RenderFrame(deltaSec);
            m_forceRender = false;
            m_frameCount.fetch_add(1, std::memory_order_relaxed);
            if (hasDirty || wasForceRender)
                ++m_graphGeneration;
            // Layout rebuild touches m_visuals which is owned by UI thread's
            // controller. Marshal to UI dispatcher.
            if (wasForceRender)
            {
                DispatcherQueue().TryEnqueue([this]{
                    m_nodeGraphController.RebuildLayout();
                });
            }
        }
        auto tRenderFrameEnd = std::chrono::high_resolution_clock::now();

        // Publish a fresh GraphUiSnapshot so UI / MCP consumers see the latest
        // state.
        ++m_frameGeneration;
        auto snap = ::ShaderLab::Graph::BuildGraphUiSnapshot(
            m_graph, m_previewNodeId, m_graphGeneration, m_frameGeneration);
        std::atomic_store(&m_uiGraphSnapshot,
            std::shared_ptr<const ::ShaderLab::Graph::GraphUiSnapshot>(snap));

        // Frame-timing accumulation.
        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_frameTiming;
            t.totalUs       = t.totalUs * (1-a) + (deltaSec * 1'000'000.0) * a;
            t.videoTickUs   = t.videoTickUs * (1-a) + usec(tTickStart, tVideoTickEnd) * a;
            t.framesSampled++;
            if (t.framesSampled % 30 == 0)
                m_lastFrameTiming = t;
        }
    }


    void MainWindow::RenderFrame(double deltaSeconds)
    {
        if (!m_renderEngine.IsInitialized())
            return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        auto tFrameStart = std::chrono::high_resolution_clock::now();

        // Re-prepare dirty source nodes (e.g., Flood color changed, video frame advance).
        for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
        {
            if (node.type == ::ShaderLab::Graph::NodeType::Source &&
                (node.dirty || m_sourceFactory.GetVideoProvider(node.id)))
            {
                try {
                    m_sourceFactory.PrepareSourceNode(node, dc, deltaSeconds, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
                } catch (...) {
                    node.runtimeError = L"Source preparation failed";
                    node.dirty = false;
                }
            }
        }

        // Compute which nodes are needed (feed a visible output).
        // Start by marking all nodes unneeded, then mark roots and propagate upstream.
        {
            for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
                node.needed = false;

            // Roots: Output nodes, preview node, output window nodes.
            // Data-only/analysis nodes are NOT automatic roots — they only
            // evaluate when dirty or when something downstream needs them.
            std::vector<uint32_t> roots;
            for (const auto& node : m_graph.Nodes())
            {
                if (node.type == ::ShaderLab::Graph::NodeType::Output)
                    roots.push_back(node.id);
                // Only include data-only analysis nodes if they're dirty
                // (need initial computation or property changed).
                if (node.dirty && node.customEffect.has_value() &&
                    node.customEffect->analysisOutputType == ::ShaderLab::Graph::AnalysisOutputType::Typed)
                    roots.push_back(node.id);
            }
            if (m_previewNodeId != 0)
                roots.push_back(m_previewNodeId);
            for (const auto& window : m_outputWindows)
                roots.push_back(window->NodeId());

            // BFS upstream from roots.
            std::unordered_set<uint32_t> visited;
            std::vector<uint32_t> queue = roots;
            while (!queue.empty())
            {
                uint32_t id = queue.back();
                queue.pop_back();
                if (visited.count(id)) continue;
                visited.insert(id);
                auto* node = m_graph.FindNode(id);
                if (node) node->needed = true;
                // Add all upstream nodes (via both image and data edges).
                for (const auto* edge : m_graph.GetInputEdges(id))
                    queue.push_back(edge->sourceNodeId);
                // Add property binding sources.
                if (node)
                {
                    for (const auto& [propName, binding] : node->propertyBindings)
                    {
                        if (binding.wholeArray)
                            queue.push_back(binding.wholeArraySourceNodeId);
                        for (const auto& src : binding.sources)
                        {
                            if (src.has_value())
                                queue.push_back(src->sourceNodeId);
                        }
                    }
                }
            }
        }

        auto tSourcesEnd = std::chrono::high_resolution_clock::now();

        // Evaluate the effect graph.
        m_graphEvaluator.Evaluate(m_graph, dc);

        // If any effects were newly created this frame, evaluate again immediately.
        // D2D needs the first pass to initialize transform pipeline; the second
        // pass produces correct output with the proper cbuffer values.
        if (m_graph.HasDirtyNodes())
            m_graphEvaluator.Evaluate(m_graph, dc);

        auto tEvalEnd = std::chrono::high_resolution_clock::now();

        // Deferred fit: after first evaluation with valid output, fit the preview.
        if (m_needsFitPreview && GetPreviewImage())
        {
            m_needsFitPreview = false;
            FitPreviewToView();
        }

        // Begin draw to swap chain.
        auto* drawDc = m_renderEngine.BeginDraw();
        if (!drawDc)
            return;

        // Process deferred D3D11 compute dispatches inside the active D2D
        // draw session, where all effect chains are fully materialized.
        // Phase 8c: install the per-frame CPU-analysis interest set so
        // ProcessDeferredCompute knows which compute nodes need to read
        // their structured buffer back to CPU. Currently:
        //   * the selected node so the Properties panel + canvas value
        //     labels stay live for the user's focus;
        //   * every upstream source that the selected node is bound to,
        //     so the selected node's bound *parameter* values (not just
        //     its analysis fields) stay live in the Properties panel
        //     for nodes that consume an upstream stats output (e.g.
        //     ICtCp Tone Map showing TargetPeakNits = LumStats.Mean
        //     should display the changing Mean even though TargetPeakNits
        //     is served via the GPU SRV).
        // Every other compute node whose downstream consumers are
        // entirely GPU-routed will skip its CopyResource + Map round-trip.
        // Throttling at Performance::CpuAnalysisHintThrottleMs (default
        // 2 s = 0.5 Hz) keeps the readback rate human-readable while
        // playing video.
        {
            std::unordered_set<uint32_t> interest;
            if (m_selectedNodeId != 0)
            {
                interest.insert(m_selectedNodeId);
                if (auto* sel = m_graph.FindNode(m_selectedNodeId))
                {
                    for (const auto& [propName, binding] : sel->propertyBindings)
                    {
                        if (binding.wholeArray)
                            interest.insert(binding.wholeArraySourceNodeId);
                        for (const auto& srcOpt : binding.sources)
                        {
                            if (srcOpt.has_value())
                                interest.insert(srcOpt->sourceNodeId);
                        }
                    }
                }
            }
            m_graphEvaluator.SetCpuAnalysisInterest(std::move(interest));
        }
        uint32_t computeCount = static_cast<uint32_t>(m_graphEvaluator.DeferredComputeCount());
        auto tComputeStart = std::chrono::high_resolution_clock::now();
        if (m_graphEvaluator.ProcessDeferredCompute(m_graph, drawDc))
        {
            m_nodeGraphController.SetNeedsRedraw();
            if (m_graph.HasDirtyNodes())
            {
                // Post-PDC re-evaluate: re-apply properties on D2D effects
                // downstream of the just-dispatched compute bridges so
                // their internal intermediate caches invalidate. We do
                // NOT want compute nodes to re-add themselves to
                // m_deferredCompute here -- those entries would leak
                // into next frame's PDC and cause a duplicate dispatch
                // (stale-source then current-source overwriting the
                // same UAV in alternation, which manifests as visible
                // two-frame flicker).
                m_graphEvaluator.SetDeferredComputeFrozen(true);
                m_graphEvaluator.Evaluate(m_graph, drawDc);
                m_graphEvaluator.SetDeferredComputeFrozen(false);
            }
        }

        auto tComputeEnd = std::chrono::high_resolution_clock::now();

        // Log compute dispatch timing if it was slow (>10ms).
        if (computeCount > 0)
        {
            double computeMs = std::chrono::duration<double, std::milli>(tComputeEnd - tComputeStart).count();
            if (computeMs > 10.0)
            {
                // Log to each compute node that dispatched.
                for (const auto& node : m_graph.Nodes())
                {
                    if (node.customEffect.has_value() &&
                        node.customEffect->shaderType == ::ShaderLab::Graph::CustomShaderType::D3D11ComputeShader &&
                        !node.outputPins.empty() && node.cachedOutput)
                    {
                        m_nodeLogs[node.id].Warning(
                            std::format(L"Slow compute dispatch: {:.1f}ms ({} dispatches)", computeMs, computeCount));
                    }
                }
            }
        }

        // Log per-node state changes (errors) — only on transitions.
        for (const auto& node : m_graph.Nodes())
        {
            auto& log = m_nodeLogs[node.id];
            // Log runtime errors when they change.
            static std::unordered_map<uint32_t, std::wstring> s_lastError;
            if (node.runtimeError != s_lastError[node.id])
            {
                s_lastError[node.id] = node.runtimeError;
                if (!node.runtimeError.empty())
                    log.Error(node.runtimeError);
                else
                    log.Info(L"Error cleared");
            }
        }

        // Set DPI to 96 so D2D coordinates match WinUI DIPs exactly.
        // The XAML compositor handles physical pixel scaling.
        // This ensures the preview transform, crosshair overlay, and pixel
        // trace coordinates all use the same coordinate space.
        float oldDpiX, oldDpiY;
        drawDc->GetDpi(&oldDpiX, &oldDpiY);
        drawDc->SetDpi(96.0f, 96.0f);

        drawDc->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        // Apply preview pan/zoom transform.
        D2D1_MATRIX_3X2_F previewTransform =
            D2D1::Matrix3x2F::Scale(m_previewZoom, m_previewZoom) *
            D2D1::Matrix3x2F::Translation(m_previewPanX, m_previewPanY);
        drawDc->SetTransform(previewTransform);

        auto* previewImage = ResolveDisplayImage(m_previewNodeId);
        if (previewImage)
        {
            drawDc->SetTransform(previewTransform);
            drawDc->DrawImage(previewImage, m_previewZoom < 1.0 ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC : D2D1_INTERPOLATION_MODE_LINEAR);
        }
        else if (m_previewNodeId != 0)
        {
            // Draw "No Input" when previewing a node with broken upstream.
            winrt::com_ptr<IDWriteFactory> dwFactory;
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), dwFactory.as<IUnknown>().put());
            if (dwFactory)
            {
                winrt::com_ptr<IDWriteTextFormat> fmt;
                dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", fmt.put());
                if (fmt)
                {
                    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    winrt::com_ptr<ID2D1SolidColorBrush> brush;
                    drawDc->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.8f), brush.put());
                    if (brush)
                    {
                        D2D1_SIZE_F sz = drawDc->GetSize();
                        drawDc->DrawText(L"No Input", 8, fmt.get(),
                            D2D1::RectF(0, 0, sz.width, sz.height), brush.get());
                    }
                }
            }
        }

        drawDc->SetTransform(D2D1::Matrix3x2F::Identity());
        drawDc->SetDpi(oldDpiX, oldDpiY);

        auto tDrawEnd = std::chrono::high_resolution_clock::now();

        m_renderEngine.EndDraw();
        m_renderEngine.Present();

        auto tPresentEnd = std::chrono::high_resolution_clock::now();

        // Accumulate per-frame timing (exponential moving average, alpha=0.1).
        // Note: `totalUs` is set by OnRenderTick to the wall-clock tick-to-tick
        // interval -- thats the only number that matches the displayed FPS.
        // Everything below is a sub-phase of that interval.
        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_frameTiming;
            t.sourcesPrepUs  = t.sourcesPrepUs * (1-a) + usec(tFrameStart, tSourcesEnd) * a;
            t.evaluateUs     = t.evaluateUs * (1-a) + usec(tSourcesEnd, tEvalEnd) * a;
            t.deferredComputeUs = t.deferredComputeUs * (1-a) + usec(tEvalEnd, tComputeEnd) * a;
            t.drawUs         = t.drawUs * (1-a) + usec(tComputeEnd, tDrawEnd) * a;
            t.endDrawFlushUs = t.endDrawFlushUs * (1-a) + usec(tDrawEnd, tPresentEnd) * a;
            t.computeDispatches = computeCount;
        }

        auto tOutWinsStart = std::chrono::high_resolution_clock::now();
        // Present to any open output windows.
        PresentOutputWindows();
        auto tOutWinsEnd = std::chrono::high_resolution_clock::now();

        // Refresh pixel trace after graph evaluation (before next frame).
        if (m_traceActive)
        {
            PopulatePixelTraceTree();
            RenderTraceSwatches();
        }
        auto tTraceEnd = std::chrono::high_resolution_clock::now();
        // Update crosshair position each frame (tracks with pan/zoom).
        UpdateCrosshairOverlay();

        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_frameTiming;
            t.outputWindowsUs = t.outputWindowsUs * (1-a) + usec(tOutWinsStart, tOutWinsEnd) * a;
            t.traceUs         = t.traceUs * (1-a) + usec(tOutWinsEnd, tTraceEnd) * a;
        }
    }

    // -------------------------------------------------------------------------
    // RenderFrameToOffscreen / BlitOffscreenToSwapChain
    //
    // Phase 7 split: render thread renders the preview image into a double-
    // buffered offscreen D2D bitmap (no swap-chain Present); UI thread later
    // blits the most recently published buffer into the SwapChainPanel-bound
    // swap chain and Presents it.
    //
    // The two-buffer publish protocol uses m_offscreenPublishedIdx (atomic
    // int32 with -1 = nothing published yet) and m_offscreenPublishedVersion
    // (atomic uint64 monotonic). Render thread writes index N (where N is
    // the buffer it just rendered to), then UI thread reads that index and
    // blits. Render thread then writes the OTHER index next time.
    // -------------------------------------------------------------------------

    bool MainWindow::EnsureOffscreenUiWrappers()
    {
        // UI thread only: rebuild m_offscreenSourceBitmapUi[0,1] when the
        // render engine's offscreen size changes (or when context is
        // recreated, e.g. adapter switch).
        EnsureUiD2dContext();
        if (!m_uiD2dContext) return false;

        uint32_t w = m_renderEngine.OffscreenWidth();
        uint32_t h = m_renderEngine.OffscreenHeight();
        if (w == 0 || h == 0) return false;

        if (w == m_offscreenWrapperWidth && h == m_offscreenWrapperHeight &&
            m_offscreenSourceBitmapUi[0] && m_offscreenSourceBitmapUi[1])
        {
            return true;
        }

        for (uint32_t i = 0; i < 2; ++i)
        {
            m_offscreenSourceBitmapUi[i] = nullptr;
            auto* tex = m_renderEngine.OffscreenTexture(i);
            if (!tex) return false;
            winrt::com_ptr<IDXGISurface> surface;
            if (FAILED(tex->QueryInterface(IID_PPV_ARGS(surface.put()))))
                return false;

            // Source-side wrapper: no TARGET option, no CANNOT_DRAW (UI uses
            // it as DrawImage source). Format must match what RenderEngine
            // created the textures with (scRGB FP16 by default).
            const auto& fmt = m_renderEngine.ActiveFormat();
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(fmt.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            if (FAILED(m_uiD2dContext->CreateBitmapFromDxgiSurface(
                    surface.get(), bp, m_offscreenSourceBitmapUi[i].put())))
                return false;
        }
        m_offscreenWrapperWidth = w;
        m_offscreenWrapperHeight = h;
        return true;
    }

    void MainWindow::RenderFrameToOffscreen(double deltaSec)
    {
        // Runs on render thread once that path is enabled. Currently still
        // safe to call from UI thread for the inline-fallback case (the
        // synchronous dispatcher mode preserves today's behaviour).
        if (m_isShuttingDown) return;
        if (!m_renderEngine.IsInitialized()) return;

        // Pick offscreen size = swap chain back buffer size for now.
        uint32_t w = m_renderEngine.BackBufferWidth();
        uint32_t h = m_renderEngine.BackBufferHeight();
        if (w == 0 || h == 0) return;

        if (!m_renderEngine.EnsureOffscreenTargets(w, h))
            return;

        auto tFrameStart = std::chrono::high_resolution_clock::now();

        // Pick the buffer to write to. We use the OPPOSITE of whatever was
        // just published, so UI thread can keep reading the other one
        // concurrently without contention.
        int32_t lastPub = m_offscreenPublishedIdx.load(std::memory_order_acquire);
        int32_t writeIdx = (lastPub == 0) ? 1 : 0;

        // Use the render-thread-dedicated D2D context (not the default one
        // -- that one is shared with capture / pixel-inspector paths that
        // run on UI thread, and concurrent BeginDraw on it would put it
        // into a wrong-state error mid-tick).
        auto* dc = m_renderEngine.RenderD2DContext();
        if (!dc) return;
        auto* targetBitmap = m_renderEngine.OffscreenRenderBitmap(writeIdx);
        if (!targetBitmap) return;

        // ---- Source preparation + graph evaluation (same as RenderFrame) ----
        for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
        {
            if (node.type == ::ShaderLab::Graph::NodeType::Source &&
                (node.dirty || m_sourceFactory.GetVideoProvider(node.id)))
            {
                try {
                    m_sourceFactory.PrepareSourceNode(node, dc, deltaSec,
                        m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
                } catch (...) {
                    node.runtimeError = L"Source preparation failed";
                    node.dirty = false;
                }
            }
        }

        auto tSourcesEnd = std::chrono::high_resolution_clock::now();

        // Compute which nodes are needed (mark roots + propagate upstream).
        {
            for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
                node.needed = false;
            std::vector<uint32_t> roots;
            for (const auto& node : m_graph.Nodes())
            {
                if (node.type == ::ShaderLab::Graph::NodeType::Output)
                    roots.push_back(node.id);
                if (node.dirty && node.customEffect.has_value() &&
                    node.customEffect->analysisOutputType == ::ShaderLab::Graph::AnalysisOutputType::Typed)
                    roots.push_back(node.id);
            }
            if (m_previewNodeId != 0)
                roots.push_back(m_previewNodeId);
            for (const auto& window : m_outputWindows)
                roots.push_back(window->NodeId());
            std::unordered_set<uint32_t> visited;
            std::vector<uint32_t> queue = roots;
            while (!queue.empty())
            {
                uint32_t id = queue.back();
                queue.pop_back();
                if (visited.count(id)) continue;
                visited.insert(id);
                auto* node = m_graph.FindNode(id);
                if (node) node->needed = true;
                for (const auto* edge : m_graph.GetInputEdges(id))
                    queue.push_back(edge->sourceNodeId);
                if (node)
                {
                    for (const auto& [propName, binding] : node->propertyBindings)
                    {
                        if (binding.wholeArray)
                            queue.push_back(binding.wholeArraySourceNodeId);
                        for (const auto& src : binding.sources)
                            if (src.has_value()) queue.push_back(src->sourceNodeId);
                    }
                }
            }
        }

        m_graphEvaluator.Evaluate(m_graph, dc);
        if (m_graph.HasDirtyNodes())
            m_graphEvaluator.Evaluate(m_graph, dc); // second pass for new effects

        auto tEvalEnd = std::chrono::high_resolution_clock::now();

        // ---- BeginDraw on offscreen + ProcessDeferredCompute + draw preview --
        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());
        dc->SetTarget(targetBitmap);
        dc->BeginDraw();

        // CPU-analysis interest set (same as old RenderFrame).
        {
            std::unordered_set<uint32_t> interest;
            if (m_selectedNodeId != 0)
            {
                interest.insert(m_selectedNodeId);
                if (auto* sel = m_graph.FindNode(m_selectedNodeId))
                {
                    for (const auto& [propName, binding] : sel->propertyBindings)
                    {
                        if (binding.wholeArray)
                            interest.insert(binding.wholeArraySourceNodeId);
                        for (const auto& srcOpt : binding.sources)
                            if (srcOpt.has_value())
                                interest.insert(srcOpt->sourceNodeId);
                    }
                }
            }
            m_graphEvaluator.SetCpuAnalysisInterest(std::move(interest));
        }

        if (m_graphEvaluator.ProcessDeferredCompute(m_graph, dc))
        {
            m_nodeGraphController.SetNeedsRedraw();
            if (m_graph.HasDirtyNodes())
            {
                m_graphEvaluator.SetDeferredComputeFrozen(true);
                m_graphEvaluator.Evaluate(m_graph, dc);
                m_graphEvaluator.SetDeferredComputeFrozen(false);
            }
        }

        auto tComputeEnd = std::chrono::high_resolution_clock::now();
        uint32_t computeCount = static_cast<uint32_t>(m_graphEvaluator.DeferredComputeCount());

        // Set DPI to 96 to match WinUI DIPs.
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        D2D1_MATRIX_3X2_F previewTransform =
            D2D1::Matrix3x2F::Scale(m_previewZoom, m_previewZoom) *
            D2D1::Matrix3x2F::Translation(m_previewPanX, m_previewPanY);
        dc->SetTransform(previewTransform);

        auto* previewImage = ResolveDisplayImage(m_previewNodeId);
        if (previewImage)
            dc->DrawImage(previewImage);

        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->SetDpi(oldDpiX, oldDpiY);

        auto tDrawEnd = std::chrono::high_resolution_clock::now();

        HRESULT hrEnd = dc->EndDraw();
        dc->SetTarget(oldTarget.get());

        auto tEndDraw = std::chrono::high_resolution_clock::now();

        // Always bump framesSampled BEFORE deciding whether to publish, so
        // /perf accurately reflects worker activity even when EndDraw fails
        // (device-lost, transient state, etc.). Without this, a single
        // EndDraw glitch would freeze the displayed FPS at a stale value.
        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_frameTiming;
            t.sourcesPrepUs     = t.sourcesPrepUs     * (1-a) + usec(tFrameStart,  tSourcesEnd) * a;
            t.evaluateUs        = t.evaluateUs        * (1-a) + usec(tSourcesEnd,  tEvalEnd)    * a;
            t.deferredComputeUs = t.deferredComputeUs * (1-a) + usec(tEvalEnd,     tComputeEnd) * a;
            t.drawUs            = t.drawUs            * (1-a) + usec(tComputeEnd,  tDrawEnd)    * a;
            t.endDrawFlushUs    = t.endDrawFlushUs    * (1-a) + usec(tDrawEnd,     tEndDraw)    * a;
            t.computeDispatches = computeCount;
            t.totalUs           = t.totalUs           * (1-a) + usec(tFrameStart,  tEndDraw)    * a;
            t.framesSampled++;
            t.endDrawFailed     = FAILED(hrEnd) ? (t.endDrawFailed + 1) : t.endDrawFailed;
            if (t.framesSampled % 30 == 0)
                m_lastFrameTiming = t;
        }

        if (FAILED(hrEnd))
            return;

        // Publish: store this buffer's index with release semantics so the
        // UI thread sees a fully-rendered frame before reading.
        m_offscreenPublishedIdx.store(writeIdx, std::memory_order_release);
        m_offscreenPublishedVersion.fetch_add(1, std::memory_order_release);

        // P7: render any open output windows into THEIR offscreen pairs.
        // Each sink has its own double-buffered offscreen managed by this
        // method. UI thread blits the published buffer in BlitAndPresent.
        RenderOutputSinks();
    }

    // ---------------------------------------------------------------------
    // RenderOutputSinks -- render-thread output-window rendering. Iterates
    // a snapshot of m_outputSinks (so we don't hold m_outputSinksMutex while
    // doing GPU work), and for each non-closed sink:
    //   - reads view state under sink->viewMutex
    //   - ensures buffers exist at requested size (creates D3D textures +
    //     render-side D2D bitmap targets, bumps bufferGen on size change)
    //   - resolves the node's cachedOutput
    //   - picks the write idx (opposite of publishedIdx)
    //   - BeginDraw on render-side bitmap, Clear, apply pan/zoom transform,
    //     DrawImage, EndDraw
    //   - publishes the new idx + version
    // The actual blit-to-swap-chain + Present1 happens on the UI thread.
    // ---------------------------------------------------------------------
    void MainWindow::RenderOutputSinks()
    {
        std::vector<std::shared_ptr<::ShaderLab::Controls::OutputSinkRenderState>> snapshot;
        {
            std::scoped_lock lock(m_outputSinksMutex);
            snapshot = m_outputSinks;
        }
        if (snapshot.empty()) return;

        auto* dc = m_renderEngine.RenderD2DContext();
        auto* d3dDevice = m_renderEngine.D3DDevice();
        if (!dc || !d3dDevice) return;
        const auto& fmt = m_renderEngine.ActiveFormat();

        for (auto& sink : snapshot)
        {
            if (!sink) continue;

            bool closed = false;
            {
                std::scoped_lock lock(sink->viewMutex);
                closed = sink->closed;
            }
            if (closed) continue;

            // Resolve the source image (the node's cachedOutput).
            auto* image = ResolveDisplayImage(sink->nodeId);
            if (!image) continue;

            // Get the image's natural bounds in DIPs at 96 DPI.
            float oldDpiX, oldDpiY;
            dc->GetDpi(&oldDpiX, &oldDpiY);
            dc->SetDpi(96.0f, 96.0f);
            D2D1_RECT_F bounds{};
            HRESULT bhr = dc->GetImageLocalBounds(image, &bounds);
            if (FAILED(bhr))
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                continue;
            }
            uint32_t imgW = static_cast<uint32_t>(bounds.right - bounds.left);
            uint32_t imgH = static_cast<uint32_t>(bounds.bottom - bounds.top);
            if (imgW == 0 || imgH == 0)
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                continue;
            }
            // Cap to a sensible max so a runaway-bounds source can't OOM
            // us. 8192 matches the per-effect cap in PreRenderInputBitmap.
            imgW = (std::min)(imgW, 8192u);
            imgH = (std::min)(imgH, 8192u);

            // Allocate the offscreen at the IMAGE's native size. This
            // keeps the render path stateless about the panel's display
            // size / DPI -- UI's BlitAndPresent does the fit when blitting
            // into the swap chain back buffer (which it knows the size of
            // first-hand). Avoids the cross-thread DPI/scale math that
            // produced misaligned output earlier.
            if (sink->bufW != imgW || sink->bufH != imgH ||
                !sink->textures[0] || !sink->textures[1])
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = imgW;
                td.Height = imgH;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = fmt.dxgiFormat;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

                bool ok = true;
                for (uint32_t i = 0; i < 2 && ok; ++i)
                {
                    sink->renderTargets[i] = nullptr;
                    sink->textures[i] = nullptr;
                    if (FAILED(d3dDevice->CreateTexture2D(&td, nullptr,
                            sink->textures[i].put())))
                    { ok = false; break; }
                    winrt::com_ptr<IDXGISurface> surface;
                    if (FAILED(sink->textures[i]->QueryInterface(
                            IID_PPV_ARGS(surface.put()))))
                    { ok = false; break; }
                    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                        D2D1::PixelFormat(fmt.dxgiFormat,
                            D2D1_ALPHA_MODE_PREMULTIPLIED),
                        96.0f, 96.0f);
                    if (FAILED(dc->CreateBitmapFromDxgiSurface(
                            surface.get(), bp, sink->renderTargets[i].put())))
                    { ok = false; break; }
                }
                if (!ok)
                {
                    for (uint32_t i = 0; i < 2; ++i)
                    {
                        sink->renderTargets[i] = nullptr;
                        sink->textures[i] = nullptr;
                    }
                    sink->bufW = sink->bufH = 0;
                    dc->SetDpi(oldDpiX, oldDpiY);
                    continue;
                }
                sink->bufW = imgW;
                sink->bufH = imgH;
                sink->bufferGen.fetch_add(1, std::memory_order_release);
            }

            // Pick write idx = opposite of last published.
            int32_t lastPub = sink->publishedIdx.load(std::memory_order_acquire);
            int32_t writeIdx = (lastPub == 0) ? 1 : 0;
            auto* target = sink->renderTargets[writeIdx].get();
            if (!target)
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                continue;
            }

            winrt::com_ptr<ID2D1Image> prevTarget;
            dc->GetTarget(prevTarget.put());
            dc->SetTarget(target);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(-bounds.left, -bounds.top));
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));
            dc->DrawImage(image);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            HRESULT hr = dc->EndDraw();
            dc->SetTarget(prevTarget.get());
            dc->SetDpi(oldDpiX, oldDpiY);
            if (FAILED(hr)) continue;

            sink->publishedIdx.store(writeIdx, std::memory_order_release);
            sink->publishedVersion.fetch_add(1, std::memory_order_release);
        }
    }

    void MainWindow::BlitOffscreenToSwapChain()
    {
        // UI thread only. Blits the latest published offscreen buffer to the
        // SwapChainPanel-bound swap chain via a UI-side D2D context that shares
        // the engine's multi-threaded D2D device.
        if (m_isShuttingDown) return;
        if (!m_renderEngine.IsInitialized()) return;
        if (!EnsureOffscreenUiWrappers()) return;

        int32_t idx = m_offscreenPublishedIdx.load(std::memory_order_acquire);
        if (idx < 0 || idx > 1) return;

        // Skip blit when there's no new published frame since last UI tick.
        // Without this the UI thread vsync-waits in Present1(1, 0) on EVERY
        // 16 ms tick (60 Hz) even when the worker is producing frames at,
        // say, 10 Hz. Each Present1 wait blocks input event delivery, so
        // dropdowns / hover highlights get starved during heavy graph eval.
        // The compositor keeps showing the last presented frame on its own;
        // we only need to re-Present when the worker has actually published
        // a new offscreen.
        uint64_t version = m_offscreenPublishedVersion.load(std::memory_order_acquire);
        if (version == m_lastBlittedVersion) return;
        m_lastBlittedVersion = version;

        auto* sourceBitmap = m_offscreenSourceBitmapUi[idx].get();
        if (!sourceBitmap) return;

        auto* swap = m_renderEngine.SwapChain();
        if (!swap) return;

        winrt::com_ptr<IDXGISurface> backBuffer;
        HRESULT hr = swap->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
        if (FAILED(hr)) return;

        const auto& fmt = m_renderEngine.ActiveFormat();
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(fmt.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        winrt::com_ptr<ID2D1Bitmap1> backBufferBitmap;
        hr = m_uiD2dContext->CreateBitmapFromDxgiSurface(
                backBuffer.get(), bp, backBufferBitmap.put());
        if (FAILED(hr)) return;

        m_uiD2dContext->SetTarget(backBufferBitmap.get());
        m_uiD2dContext->BeginDraw();
        m_uiD2dContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        m_uiD2dContext->DrawImage(sourceBitmap);
        hr = m_uiD2dContext->EndDraw();
        m_uiD2dContext->SetTarget(nullptr);
        if (FAILED(hr)) return;

        DXGI_PRESENT_PARAMETERS params{};
        swap->Present1(1, 0, &params);
    }

}
