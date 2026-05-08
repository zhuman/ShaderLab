#pragma once

#include "MainWindow.g.h"
#include "Rendering/RenderEngine.h"
#include "Rendering/DisplayMonitor.h"
#include "Rendering/GraphEvaluator.h"
#include "Graph/EffectGraph.h"
#include "Effects/EffectRegistry.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Controls/ShaderEditorController.h"
#include "Controls/NodeGraphController.h"
#include "Controls/PixelInspectorController.h"
#include "Controls/PixelTraceController.h"
#include "Controls/OutputWindow.h"
#include "Controls/LogWindow.h"
#include "Controls/NodeLog.h"
#include "EffectDesignerWindow.xaml.h"
#include "Engine/Mcp/McpHttpServer.h"
#include "Engine/Mcp/EngineMcpRoutes.h"

namespace winrt::ShaderLab::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        // Auto-start MCP server (set from --mcp command-line flag).
        void SetAutoStartMcp(bool autoStart) { m_autoStartMcp = autoStart; }

        // Device preference (set from --gpu / --warp command-line flags).
        void SetDevicePreference(::ShaderLab::Rendering::DevicePreference pref) { m_devicePref = pref; }

        // File path passed on the command line via Explorer FTA.
        // Loaded after rendering is initialized.
        void SetPendingOpenPath(std::wstring path) { m_pendingOpenPath = std::move(path); }

        // XAML-bound event handlers (must be public for generated code).
        void OnColumnSplitterPointerPressed(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnColumnSplitterPointerMoved(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnColumnSplitterPointerReleased(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnGpuInfoTapped(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& args);

        // Status-bar broom button (Phase 8 p8-status-bar-button).
        winrt::fire_and_forget OnReaperBroomClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnNodeGraphDragOver(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        winrt::fire_and_forget OnNodeGraphDrop(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        void OnSaveAccelerator(
            winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);
        void OnSaveAsAccelerator(
            winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);

    private:
        HWND GetWindowHandle();
        void InitializeRendering();
        void SwitchAdapter(::ShaderLab::Rendering::DevicePreference pref, LUID adapterLuid);
        void OnPreviewPanelLoaded();
        void RegisterCustomEffects();
        void UpdateStatusBar();
        void PopulatePreviewNodeSelector();
        void UpdateOutdatedEffectsButton();
        void UpdatePreviewOverlay();
        void PopulateDisplayProfileSelector();
        ID2D1Image* GetPreviewImage();
        ID2D1Image* ResolveDisplayImage(uint32_t nodeId);

        // Returns the preview viewport size in DIPs (not physical pixels).
        D2D1_SIZE_F PreviewViewportDips() const;
        void ApplyDisplayProfile(const ::ShaderLab::Rendering::DisplayProfile& profile);
        void RevertToLiveDisplay();

        // Mirror the active display profile (live or simulated) into the
        // properties of every "Working Space" parameter node in the graph.
        // Called from the few hot paths where the profile changes
        // (ApplyDisplayProfile, RevertToLiveDisplay, display-change
        // callback) and once per render tick so newly-added Working Space
        // nodes pick up live values immediately. Cheap no-op when the graph
        // contains no Working Space nodes.
        void UpdateWorkingSpaceNodes();

        void ResetAfterGraphLoad(bool reopenOutputWindows = true);

        // Pixel trace helpers.
        D2D1_RECT_F GetPreviewImageBounds();
        bool PointerToImageCoords(
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args,
            float& outNormX, float& outNormY);
        void PopulatePixelTraceTree();
        void UpdatePixelTraceValues();
        winrt::Microsoft::UI::Xaml::Controls::Grid CreateTraceRow(
            const ::ShaderLab::Controls::PixelTraceNode& traceNode);

        // Event handlers.
        void OnPreviewSizeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
        void OnPreviewPointerMoved(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPreviewKeyDown(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
        void OnDisplayProfileSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        winrt::fire_and_forget LoadIccProfileAsync();
        void OnPreviewPointerPressed(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnTraceUnitSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnSaveGraphClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnLoadGraphClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        // Save the current graph; if no file path is known, prompts via picker.
        winrt::fire_and_forget SaveGraphAsync();
        // Always prompts via picker, even if a path is known.
        winrt::Windows::Foundation::IAsyncAction SaveGraphAsAsync();
        winrt::fire_and_forget LoadGraphAsync();
        // Quietly write the graph to disk at the previously-picked path.
        // Returns true if the write succeeded; false if no path or the
        // write failed. Public-ish so the close-confirmation dialog can
        // call it on the "Save" branch.
        bool SaveGraphToCurrentPath();
        // Async wrapper used by the close-confirmation flow so the
        // dialog can co_await the embed-progress UI.
        winrt::Windows::Foundation::IAsyncAction SaveGraphToCurrentPathAsync();
        // Run a save with a modal progress dialog. Reuses
        // m_currentFilePath / m_embedMedia.
        winrt::Windows::Foundation::IAsyncAction RunSaveWithProgressAsync();
        // Mark the current graph as having unsaved edits. Cheap; call it
        // from any user-driven mutation site. Resets on save / load / new.
        void MarkUnsaved();
        // Async confirmation dialog used by AppWindow Closing handler.
        // Returns 0 = save (then proceed), 1 = discard, 2 = cancel.
        winrt::Windows::Foundation::IAsyncOperation<int32_t> PromptUnsavedChangesAsync();
        void RefreshTitleBar();
        // Load a .effectgraph (or legacy .json) from a known path. Used by
        // file activation (double-click in Explorer) and by the picker.
        winrt::Windows::Foundation::IAsyncAction LoadGraphFromPathAsync(winrt::hstring path);

        // Path of the most recently saved or loaded graph (empty until
        // the user picks a destination via Save As / open).
        std::wstring m_currentFilePath;
        bool m_unsavedChanges{ false };

        // User preference: embed referenced media inside the .effectgraph
        // zip. Default true; reset by checkbox in the save flow. Sticky
        // across saves of the same window.
        bool m_embedMedia{ true };

        // Directories holding files extracted from the most recently
        // loaded .effectgraph archives. Cleaned up at shutdown so the
        // user's %TEMP% doesn't accumulate stale graph media.
        std::vector<std::wstring> m_extractedMediaDirs;

        // Heartbeat: every HeartbeatIntervalSec we touch a sentinel
        // file inside each extracted dir so a future instance can
        // tell our dirs from orphans left behind by a crash.
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_heartbeatTimer{ nullptr };
        static constexpr uint32_t HeartbeatIntervalSec = 60;
        static constexpr uint32_t HeartbeatStaleSec = 150; // > 2x interval
        void StartHeartbeatTimer();
        void TouchHeartbeats();
        // Scan %TEMP% for ShaderLab-* dirs whose heartbeat is older
        // than HeartbeatStaleSec. If any are found, prompt the user
        // once at startup to clean them up.
        winrt::fire_and_forget ReapStaleMediaDirsAsync();
        void PopulateAddNodeFlyout();
        void OnAddEffectNode(const ::ShaderLab::Effects::EffectDescriptor& desc);
        void OnAddImageSourceClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget AddImageSourceAsync();
        void OnAddFloodSourceClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget AddWindowsGraphicsCaptureSourceAsync();
        void OnNodeAdded(uint32_t nodeId);
        void OnPreviewPointerDragged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPreviewPointerReleased(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);

        // Render loop.
        void OnRenderTick(
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& sender,
            winrt::Windows::Foundation::IInspectable const& args);
        void RenderFrame(double deltaSeconds = 0.0);

        // Device stack.
        ::ShaderLab::Rendering::RenderEngine       m_renderEngine;
        ::ShaderLab::Rendering::DisplayMonitor     m_displayMonitor;
        ::ShaderLab::Rendering::GraphEvaluator     m_graphEvaluator;

        // Effect graph.
        ::ShaderLab::Graph::EffectGraph         m_graph;
        ::ShaderLab::Effects::SourceNodeFactory m_sourceFactory;

        // Controllers.
        ::ShaderLab::Controls::ShaderEditorController    m_shaderEditor;
        ::ShaderLab::Controls::NodeGraphController       m_nodeGraphController;
        ::ShaderLab::Controls::PixelInspectorController  m_pixelInspector;
        ::ShaderLab::Controls::PixelTraceController      m_pixelTrace;

        // Output windows (one per additional Output node).
        std::vector<std::unique_ptr<::ShaderLab::Controls::OutputWindow>> m_outputWindows;
        void OpenOutputWindow(uint32_t nodeId);
        void CloseOutputWindow(uint32_t nodeId);
        void PresentOutputWindows();

        // Per-node log system.
        std::unordered_map<uint32_t, ::ShaderLab::Controls::NodeLog> m_nodeLogs;
        std::vector<std::unique_ptr<::ShaderLab::Controls::LogWindow>> m_logWindows;
        void OpenLogWindow(uint32_t nodeId);
        void UpdateLogWindows();
        ::ShaderLab::Controls::NodeLog& GetNodeLog(uint32_t nodeId) { return m_nodeLogs[nodeId]; }

        // Render loop timer.
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_renderTimer{ nullptr };
        uint32_t m_frameCount{ 0 };
        uint64_t m_lastVideoUploadCount{ 0 };
        float    m_lastFps{ 0.0f };
        float    m_lastVideoFps{ 0.0f };
        std::chrono::steady_clock::time_point m_fpsTimePoint;
        std::chrono::steady_clock::time_point m_lastRenderTick;

        // Render cadence: target 1 / monitor refresh, clamped to [60, 240] Hz.
        // Refreshed on init and on every display change so a 120/144/240 Hz
        // panel actually drives the render loop at its native rate. Going
        // higher than display refresh wastes work; going lower than 60 Hz
        // makes interactions feel laggy on unusual modes (e.g. 30 Hz TV out).
        uint32_t m_targetRefreshHz{ 60 };
        uint32_t QueryDisplayRefreshHz() const noexcept;
        void UpdateRenderTimerInterval();

        // Per-frame performance timings (microseconds, rolling averages).
        struct FrameTimings {
            double totalUs{};            // wall-clock between consecutive timer ticks (true frame interval)
            double videoTickUs{};        // TickAndUploadLiveCaptures + TickAndUploadVideos + dirty propagation
            double sourcesPrepUs{};      // PrepareSourceNode loop inside RenderFrame
            double evaluateUs{};         // GraphEvaluator::Evaluate (passes 1 + 2)
            double deferredComputeUs{};  // ProcessDeferredCompute (D3D11 compute dispatches) + post-PDC eval
            double drawUs{};             // swap-chain DrawImage (CPU command queueing)
            double presentUs{};          // RenderEngine::Present (back-buffer swap, blocks on VSync/GPU)
            double nodeGraphUs{};        // RenderNodeGraph + overlays (canvas redraw)
            double outputWindowsUs{};    // PresentOutputWindows (peeled-off output panes)
            double traceUs{};            // PopulatePixelTraceTree + RenderTraceSwatches
            uint32_t computeDispatches{};
            uint32_t framesSampled{};
        };
        FrameTimings m_frameTiming;
        FrameTimings m_lastFrameTiming;  // snapshot for MCP read

        HWND m_hwnd{ nullptr };
        bool m_customEffectsRegistered{ false };
        bool m_isShuttingDown{ false };

        // Video seek slider / position label (updated per-tick while playing).
        winrt::Microsoft::UI::Xaml::Controls::Slider m_videoSeekSlider{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_videoPositionLabel{ nullptr };
        uint32_t m_videoSeekNodeId{ 0 };
        bool m_videoSeekSuppressEvents{ false };

        // Per-node preview.
        uint32_t m_previewNodeId{ 0 };       // Tracks selected node for inline viewport
        std::vector<uint32_t> m_topoOrder;   // cached for [ ] navigation

        // Node clipboard for copy/paste.
        struct ClipboardEntry {
            ::ShaderLab::Graph::EffectNode node;
            uint32_t originalId;
        };
        std::vector<ClipboardEntry> m_nodeClipboard;
        std::vector<::ShaderLab::Graph::EffectEdge> m_edgeClipboard;

        // Display profile selection.
        std::vector<::ShaderLab::Rendering::DisplayProfile> m_displayPresets;
        std::optional<::ShaderLab::Rendering::DisplayProfile> m_loadedIccProfile;
        int32_t m_committedProfileIndex{ 0 };
        bool m_suppressProfileEvent{ false };

        // Pixel trace.
        bool m_traceActive{ false };
        uint32_t m_traceUnit{ 0 };          // 0=scRGB, 1=sRGB, 2=Nits, 3=PQ
        uint32_t m_lastTraceTopologyHash{ 0 };
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Grid> m_traceRowCache;

        // Node graph editor rendering.
        winrt::com_ptr<IDXGISwapChain1>     m_graphSwapChain;
        winrt::com_ptr<ID2D1Bitmap1>        m_graphRenderTarget;
        winrt::com_ptr<ID2D1SolidColorBrush> m_graphGridBrush;
        uint32_t m_graphPanelWidth{ 0 };
        uint32_t m_graphPanelHeight{ 0 };
        float m_graphPanelDipsWidth{ 0.0f };
        float m_graphPanelDipsHeight{ 0.0f };

        void InitializeGraphPanel();
        void ResizeGraphPanel(float widthDips, float heightDips);
        void UpdateGraphPanelScale();
        void RenderNodeGraph();
        D2D1_POINT_2F GraphPanelPointerToCanvas(
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnGraphPanelPointerPressed(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnGraphPanelPointerMoved(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnGraphPanelPointerReleased(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnGraphPanelPointerWheel(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);

        bool m_isGraphPanning{ false };
        D2D1_POINT_2F m_graphPanStart{};
        D2D1_POINT_2F m_graphPanOrigin{};
        void UpdatePropertiesPanel();
        // True when any descendant of PropertiesPanel currently has keyboard
        // focus (TextBox cursor, NumberBox edit, dropdown open). Used by the
        // 4 Hz binding-value refresh path to avoid clobbering an in-progress
        // edit by Clear() + recreate on the panel.
        bool IsPropertiesPanelInteracting();
        void ShowCurveEditorDialog(uint32_t nodeId, const std::wstring& propertyKey, std::function<void()> markDirty);
        void AddMathExpressionInput(uint32_t nodeId);
        void RemoveMathExpressionInput(uint32_t nodeId, const std::wstring& paramName);
        winrt::fire_and_forget BrowseImageForSourceNode(uint32_t nodeId);
        winrt::fire_and_forget BrowseVideoForSourceNode();
        winrt::fire_and_forget BrowseVideoForExistingNode(uint32_t nodeId);
        void OnSaveImageClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget SaveImageAsync();

        // Capture the current preview as a PNG byte buffer.
        // Returns empty vector on failure.
        std::vector<uint8_t> CapturePreviewAsPng();

        // Encode a D2D image as a PNG byte buffer (BGRA8 via WIC).  Used by
        // CapturePreviewAsPng and CaptureNodeAsPng to share the encoder path.
        // Caps each axis at maxDim pixels to keep responses bounded.
        std::vector<uint8_t> CaptureImageAsPng(ID2D1Image* image, uint32_t maxDim = 2048);

        // Capture an arbitrary node's resolved output as a PNG byte buffer.
        // Forces a render frame first so dirty downstream nodes evaluate.
        // Returns:
        //   - empty + outNotFound=true  when the node ID doesn't exist.
        //   - empty + outNotReady=true  when the node exists but isn't yet ready.
        //   - empty + neither flag set  on encode failure.
        std::vector<uint8_t> CaptureNodeAsPng(uint32_t nodeId,
                                              bool& outNotFound,
                                              bool& outNotReady);

        // Read a w x h pixel region from a node's resolved output as scRGB
        // FP32 RGBA values (one float4 per pixel, row-major from top-left).
        // Forces a render frame first.  Region is clipped to image bounds.
        // Returns true on success and populates `outPixels` with w*h*4 floats.
        // outActualW/H reflect the (clipped) region actually read.
        bool ReadPixelRegion(uint32_t nodeId,
                             int32_t x, int32_t y, uint32_t w, uint32_t h,
                             std::vector<float>& outPixels,
                             uint32_t& outActualW, uint32_t& outActualH,
                             bool& outNotFound, bool& outNotReady);

        // Capture the live node-graph view (current pan/zoom, sized to the
        // graph swap-chain panel) as a PNG byte buffer.  Renders into an
        // off-screen bitmap so it doesn't disturb the live render tick.
        // Returns empty vector on failure.
        std::vector<uint8_t> CaptureGraphAsPng();

        // Pan/zoom the node-graph view to fit all nodes on screen, with the
        // given viewport-space padding (DIPs).  No-op when the graph is empty.
        void FitGraphView(float padding = 40.0f);

        // Shared draw routine for the node-graph scene (clear + dot grid +
        // controller render).  Used by both the live render tick and the
        // off-screen snapshot capture so the two never drift.
        void RenderGraphScene(ID2D1DeviceContext5* dc, D2D1_SIZE_F viewSize);
        void OpenEffectDesigner();
        void EnforceCustomEffectNameUniqueness(uint32_t modifiedNodeId);

        uint32_t m_selectedNodeId{ 0 };
        bool m_isDraggingNode{ false };
        bool m_isDraggingConnection{ false };
        bool m_isDraggingSlider{ false };
        uint32_t m_sliderDragNodeId{ 0 };

        // Effect Designer window.
        winrt::ShaderLab::EffectDesignerWindow m_designerWindow{ nullptr };

        // MCP HTTP server for AI agent integration.
        std::unique_ptr<::ShaderLab::McpHttpServer> m_mcpServer;
        // TEMP (Phase 8 perf debugging): default ON so the MCP-driven
        // graph-building loop doesn't require a manual toggle every
        // restart. Revert to false once the crash repro is sorted.
        bool m_autoStartMcp{ true };
        ::ShaderLab::Rendering::DevicePreference m_devicePref{ ::ShaderLab::Rendering::DevicePreference::Default };
        std::wstring m_pendingOpenPath; // file path from Explorer FTA, loaded after init
        void SetupMcpRoutes();
        template<typename F> auto DispatchSync(F&& fn) -> decltype(fn());

        // Phase 7: MainWindow as IEngineCommandSink, hands engine-side
        // routes a closure that runs on the UI thread (via DispatchSync).
        // Engine-pure routes that don't need UI thread coordination call
        // sink.Dispatch with a closure that just reads engine state.
        // Mutating routes use it to ensure m_graph mutations don't race
        // with the render tick on the UI thread.
        struct GuiEngineCommandSink : public ::ShaderLab::Mcp::IEngineCommandSink
        {
            MainWindow* window{ nullptr };
            explicit GuiEngineCommandSink(MainWindow* w) : window(w) {}
            ::ShaderLab::McpHttpServer::Response Dispatch(
                std::function<::ShaderLab::McpHttpServer::Response(
                    ::ShaderLab::Mcp::EngineContext&)> closure) override;

            // ---- Event hooks ---------------------------------------------
            // Wired to the same UI methods that fire on native user
            // interactions (toolbar add-node, drag-edge, etc) so MCP-driven
            // mutations and user-driven mutations take the same code path
            // through the GUI.
            void OnNodeAdded(uint32_t /*nodeId*/) override;
            void OnNodeRemoved(uint32_t nodeId) override;
            void OnNodeChanged(uint32_t /*nodeId*/) override;
            void OnGraphCleared() override;
            void OnGraphLoaded() override;
            void OnGraphStructureChanged() override;
            void OnCustomEffectRecompiled(uint32_t nodeId) override;
            void OnDisplayProfileChanged() override;
        };
        std::unique_ptr<GuiEngineCommandSink> m_engineSink;

        // MCP activity indicator state.
        // Updated from the MCP listener thread via the activity callback;
        // polled from the UI render tick to drive the dot color + tooltip.
        std::atomic<int64_t>  m_mcpLastActivityMs{ 0 };
        std::atomic<uint64_t> m_mcpRequestCount{ 0 };
        std::atomic<uint64_t> m_mcpUiUpdateSeq{ 0 };  // bumped every callback; UI compares
        uint64_t              m_mcpLastUiUpdateSeq{ 0 };
        std::mutex            m_mcpLastReqMutex;
        std::string           m_mcpLastReqMethod;
        std::string           m_mcpLastReqPath;
        std::string           m_mcpLastReqPeer;
        uint16_t              m_mcpLastReqStatus{ 0 };
        std::set<std::string> m_mcpKnownPeers;  // distinct peer addresses seen since server start
        void UpdateMcpActivityIndicator();
        void ResetMcpActivityState();
        void UpdateFpsTooltip();

        // Column splitter drag state.
        bool m_isDraggingSplitter{ false };
        double m_splitterDragStartX{ 0 };
        double m_splitterStartCol0Width{ 0 };
        double m_splitterStartCol2Width{ 0 };

        // Preview pan/zoom.
        float m_previewPanX{ 0.0f };
        float m_previewPanY{ 0.0f };
        float m_previewZoom{ 1.0f };
        bool  m_needsFitPreview{ false };
        bool  m_forceRender{ true }; // Force first render + after pan/zoom changes
        bool m_isPreviewPanning{ false };
        float m_previewPanStartX{ 0.0f };
        float m_previewPanStartY{ 0.0f };
        float m_previewPanOriginX{ 0.0f };
        float m_previewPanOriginY{ 0.0f };
        bool m_previewDragMoved{ false };
        float m_traceClickDipX{ 0.0f };
        float m_traceClickDipY{ 0.0f };
        float m_traceClickPanX{ 0.0f };
        float m_traceClickPanY{ 0.0f };
        float m_traceClickZoom{ 1.0f };
        bool m_traceOutOfBounds{ false };
        void UpdateCrosshairOverlay();
        void FitPreviewToView();

        // Trace swatch HDR swap chain.
        winrt::com_ptr<IDXGISwapChain1>   m_traceSwapChain;
        winrt::com_ptr<ID2D1Bitmap1>      m_traceSwatchTarget;
        uint32_t m_traceSwatchHeight{ 0 };
        void InitializeTraceSwatchPanel();
        void ResizeTraceSwatchPanel();
        void UpdateTraceSwatchPanelScale();
        void RenderTraceSwatches();
    };
}

namespace winrt::ShaderLab::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
