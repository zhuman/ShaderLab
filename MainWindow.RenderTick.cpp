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
    // -----------------------------------------------------------------------

    void MainWindow::OnRenderTick(
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& /*sender*/,
        winrt::Windows::Foundation::IInspectable const& /*args*/)
    {
        if (m_isShuttingDown) return;
        if (!m_renderEngine.IsInitialized()) return;

        try
        {
        // Compute frame delta time.
        auto now = std::chrono::steady_clock::now();
        double deltaSec = std::chrono::duration<double>(now - m_lastRenderTick).count();
        // tTickStart marks the start of this entire timer-tick processing so
        // we can attribute time to phases that arent inside RenderFrame
        // (video decode + upload, dirty propagation, node-graph editor
        // redraw, output-window present, pixel-trace updates). The frames
        // wall-clock interval is the gap between consecutive m_lastRenderTick
        // captures (= deltaSec).
        auto tTickStart = now;
        m_lastRenderTick = now;
        // Clamp to avoid huge jumps (e.g., after breakpoint or sleep).
        if (deltaSec > 0.1) deltaSec = 0.016;

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

                // Disable AutoDuration if StopTime has an explicit binding.
                if (autoDuration && node.propertyBindings.count(L"StopTime"))
                {
                    autoDuration = false;
                    node.properties[L"AutoDuration"] = 0.0f;
                }

                // Auto-detect StopTime from downstream video durations.
                // Data bindings don't create graph edges — scan all nodes
                // for propertyBindings that reference this Clock.
                if (autoDuration)
                {
                    float maxDur = 0.0f;
                    for (const auto& other : m_graph.Nodes())
                    {
                        if (other.id == node.id) continue;
                        // Check if this node has any property bound to our Clock.
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

        // Only re-evaluate the graph when something changed.
        // Always render if output windows are open (they need continuous present).
        bool wasForceRender = m_forceRender;
        bool hasDirty = m_graph.HasDirtyNodes();
        bool hasOutputWindows = !m_outputWindows.empty();
        bool needsEval = hasDirty || m_needsFitPreview || m_forceRender || hasOutputWindows;
        // Mark the boundary between "video tick / dirty propagation" and
        // "render frame" so we can attribute time to each.
        auto tVideoTickEnd = std::chrono::high_resolution_clock::now();
        if (needsEval)
        {
            RenderFrame(deltaSec);
            m_forceRender = false;
            m_frameCount++;
            // Rebuild layout only on user-initiated changes (not animation ticks)
            // to update analysis display sizing without killing performance.
            if (wasForceRender)
                m_nodeGraphController.RebuildLayout();
        }
        auto tRenderFrameEnd = std::chrono::high_resolution_clock::now();

        RenderNodeGraph();
        auto tNodeGraphEnd = std::chrono::high_resolution_clock::now();

        // Accumulate per-tick wall-clock + tick-only phases. `totalUs` is
        // tick-to-tick wall clock so it matches the displayed FPS exactly:
        // sub-phase counters below sum to <= totalUs and the difference
        // is dispatcher idle / message-pump / OS overhead between ticks.
        {
            auto usec = [](auto a, auto b) {
                return std::chrono::duration<double, std::micro>(b - a).count();
            };
            const double a = 0.1;
            auto& t = m_frameTiming;
            t.totalUs       = t.totalUs * (1-a) + (deltaSec * 1'000'000.0) * a;
            t.videoTickUs   = t.videoTickUs * (1-a) + usec(tTickStart, tVideoTickEnd) * a;
            t.nodeGraphUs   = t.nodeGraphUs * (1-a) + usec(tRenderFrameEnd, tNodeGraphEnd) * a;
            t.framesSampled++;
            // Snapshot every 30 frames for MCP reads.
            if (t.framesSampled % 30 == 0)
                m_lastFrameTiming = t;
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

        // Update FPS counter every second (counts output frames only).
        // Also update log windows at ~4Hz.
        auto fpsNow = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(fpsNow - m_fpsTimePoint).count();
        if (elapsed >= 250)
        {
            if (!m_logWindows.empty())
                UpdateLogWindows();
            // Refresh properties panel only when the selected node actually has
            // a property binding whose live value can change frame to frame.
            // Without this guard, video playback (which dirties graph nodes
            // every frame) caused a 4 Hz rebuild of the entire properties
            // panel -- visibly jittering Slider/NumberBox widths as auto-sized
            // controls re-laid out.
            //
            // Additional guard: skip the rebuild when any descendant of the
            // PropertiesPanel currently has keyboard focus (i.e. the user
            // is mid-edit in a TextBox / NumberBox / dropdown). The clear()
            // + recreate cycle was destroying the focused control on every
            // 250 ms tick, making any property whose owner has a bound
            // sibling effectively un-editable in a Clock-driven graph.
            if (m_selectedNodeId != 0 && m_graph.HasDirtyNodes())
            {
                auto* selNode = m_graph.FindNode(m_selectedNodeId);
                if (selNode && !selNode->propertyBindings.empty() &&
                    !IsPropertiesPanelInteracting())
                    UpdatePropertiesPanel();
            }
            // Refresh MCP activity indicator (dot color fade + tooltip "Xs ago"
            // counter).  Cheap when no activity has occurred.
            UpdateMcpActivityIndicator();

            // Refresh the FPS tooltip's breakdown at 250 ms cadence so that
            // hovering the FPS counter shows live phase costs without
            // waiting for the once-per-second FPS-text update. The TextBlock
            // inside the ToolTip is data-bound so updating its Text
            // refreshes the open tooltip in place.
            UpdateFpsTooltip();
        }
        if (elapsed >= 1000)
        {
            float fps = static_cast<float>(m_frameCount) * 1000.0f / static_cast<float>(elapsed);

            // Video decode FPS (kept for tooltip).
            uint64_t currentVideoUploads = m_sourceFactory.TotalVideoUploads();
            float videoFps = static_cast<float>(currentVideoUploads - m_lastVideoUploadCount) * 1000.0f / static_cast<float>(elapsed);
            m_lastVideoUploadCount = currentVideoUploads;
            m_lastVideoFps = videoFps;
            m_lastFps = fps;

            FpsText().Text(std::format(L"{:.0f} fps | {:.1f} ms", fps, m_frameTiming.totalUs / 1000.0));
            UpdateFpsTooltip();
            m_frameCount = 0;
            m_fpsTimePoint = fpsNow;
        }

        } // end try
        catch (const winrt::hresult_error& ex)
        {
            OutputDebugStringW(std::format(L"[RenderTick] Exception: 0x{:08X}\n",
                static_cast<uint32_t>(ex.code())).c_str());
        }
        catch (const std::exception& ex)
        {
            OutputDebugStringW(std::format(L"[RenderTick] std::exception: {}\n",
                std::wstring(ex.what(), ex.what() + strlen(ex.what()))).c_str());
        }
        catch (...)
        {
            OutputDebugStringW(L"[RenderTick] Unknown exception\n");
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
            t.presentUs      = t.presentUs * (1-a) + usec(tDrawEnd, tPresentEnd) * a;
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

}
