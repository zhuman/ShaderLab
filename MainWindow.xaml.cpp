#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "Rendering/PipelineFormat.h"
#include "Rendering/IccProfileParser.h"
#include "Rendering/EffectGraphFile.h"
#include "Rendering/PixelReadback.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/DxgiDuplicationSourceProvider.h"
#include "Effects/Performance.h"
#include "Version.h"
#include "EngineExport.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <shlobj.h>
#include <KnownFolders.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::ShaderLab::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();

        // Engine ABI compatibility check. Mismatch means the loaded
        // ShaderLabEngine.dll is from a different build than the headers
        // we compiled against -- abort early with a friendly message
        // instead of failing later in an obscure way.
        if (::ShaderLab_GetAbiVersion() != SHADERLAB_ENGINE_ABI_VERSION)
        {
            wchar_t msg[256];
            swprintf_s(msg, L"ShaderLab engine ABI mismatch (header %u, DLL %u). Rebuild and redeploy.",
                static_cast<unsigned>(SHADERLAB_ENGINE_ABI_VERSION),
                static_cast<unsigned>(::ShaderLab_GetAbiVersion()));
            ::MessageBoxW(nullptr, msg, L"ShaderLab", MB_OK | MB_ICONERROR);
            ::ExitProcess(1);
        }

        Title(std::wstring(L"ShaderLab v") + ::ShaderLab::VersionString + L" \u2014 HDR Shader Effect Development");
        // After Title() we'll refresh from RefreshTitleBar() once a graph
        // is loaded; the initial state is "Untitled".

        m_hwnd = GetWindowHandle();

        // Close all output windows and exit the process when the main window closes.
        this->Closed([this](auto&&, auto&&)
        {
            m_isShuttingDown = true;
            m_outputWindows.clear();
            // Best-effort: remove temp directories we extracted media
            // archives into so the user's %TEMP% doesn't accumulate
            // stale .effectgraph payloads.
            std::error_code ec;
            for (const auto& d : m_extractedMediaDirs)
                std::filesystem::remove_all(d, ec);
            // WinUI 3 keeps the process alive while any Window exists.
            // Force exit so output windows don't keep us running.
            ::PostQuitMessage(0);
        });

        // Cancellable close: prompt the user when there are unsaved
        // changes. AppWindow.Closing is the WinUI 3 hook that can take
        // a deferral (so we can show an async dialog and decide whether
        // to allow the close after the user picks an option).
        this->AppWindow().Closing(
            [this](auto&&, winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
            {
                if (m_isShuttingDown || !m_unsavedChanges) return;
                args.Cancel(true);

                [](MainWindow* self) -> winrt::fire_and_forget
                {
                    auto strong = self->get_strong();
                    int32_t choice = co_await self->PromptUnsavedChangesAsync();
                    if (choice == 2) co_return; // Cancel -> stay open
                    if (choice == 0)            // Save then close
                    {
                        if (!self->m_currentFilePath.empty())
                            co_await self->RunSaveWithProgressAsync();
                        else
                            co_await self->SaveGraphAsAsync();
                        // If the user backed out of the picker, don't close.
                        if (self->m_unsavedChanges) co_return;
                    }
                    self->m_unsavedChanges = false;
                    self->Close();
                }(this);
            });

        // Wire up event handlers (safe before panel is loaded).
        PreviewPanel().SizeChanged({ this, &MainWindow::OnPreviewSizeChanged });
        PreviewPanel().PointerMoved({ this, &MainWindow::OnPreviewPointerMoved });
        PreviewPanel().KeyDown({ this, &MainWindow::OnPreviewKeyDown });
        PreviewPanel().IsTabStop(true);

        PopulateDisplayProfileSelector();
        DisplayProfileSelector().SelectionChanged({ this, &MainWindow::OnDisplayProfileSelectionChanged });

        PreviewPanel().PointerPressed({ this, &MainWindow::OnPreviewPointerPressed });
        PreviewPanel().PointerReleased({ this, &MainWindow::OnPreviewPointerReleased });
        PreviewPanel().PointerWheelChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            auto point = args.GetCurrentPoint(PreviewPanel());
            int delta = point.Properties().MouseWheelDelta();
            float cursorX = static_cast<float>(point.Position().X);
            float cursorY = static_cast<float>(point.Position().Y);

            float zoomFactor = (delta > 0) ? 1.1f : (1.0f / 1.1f);
            float newZoom = std::clamp(m_previewZoom * zoomFactor, 0.01f, 100.0f);

            // Zoom centered on cursor position (DIP coordinates).
            m_previewPanX = cursorX - (cursorX - m_previewPanX) * (newZoom / m_previewZoom);
            m_previewPanY = cursorY - (cursorY - m_previewPanY) * (newZoom / m_previewZoom);
            m_previewZoom = newZoom;
            m_forceRender = true;
            args.Handled(true);
        });
        TraceUnitSelector().SelectedIndex(0);
        TraceUnitSelector().SelectionChanged({ this, &MainWindow::OnTraceUnitSelectionChanged });

        SaveGraphButton().Click({ this, &MainWindow::OnSaveGraphClicked });
        LoadGraphButton().Click({ this, &MainWindow::OnLoadGraphClicked });
        AutoArrangeButton().Click([this](auto&&, auto&&)
        {
            // Reset viewport so the laid-out nodes are visible even if the
            // user had zoomed/panned the canvas off-screen.
            m_nodeGraphController.SetZoom(1.0f);
            m_nodeGraphController.SetPanOffset(0.0f, 0.0f);
            m_nodeGraphController.AutoLayout();
            m_nodeGraphController.SetNeedsRedraw();
            m_forceRender = true;
        });
        UpdateAllEffectsButton().Click([this](auto&&, auto&&)
        {
            auto& registry = ::ShaderLab::Effects::ShaderLabEffects::Instance();
            for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
            {
                if (!node.customEffect.has_value() || node.customEffect->shaderLabEffectId.empty())
                    continue;
                auto* desc = registry.FindById(node.customEffect->shaderLabEffectId);
                if (!desc || desc->effectVersion <= node.customEffect->shaderLabEffectVersion)
                    continue;

                auto savedProps = node.properties;
                auto freshNode = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
                if (!freshNode.customEffect.has_value()) continue;

                node.customEffect = std::move(freshNode.customEffect.value());
                node.inputPins = std::move(freshNode.inputPins);
                node.outputPins = std::move(freshNode.outputPins);
                node.isClock = freshNode.isClock;
                node.properties = std::move(freshNode.properties);
                for (const auto& [key, val] : savedProps)
                {
                    auto it = node.properties.find(key);
                    if (it != node.properties.end())
                        it->second = val;
                }
                node.dirty = true;
                node.cachedOutput = nullptr; // raw pointer is now dangling
                m_graphEvaluator.InvalidateNode(node.id);
            }
            m_graph.MarkAllDirty();
            m_nodeGraphController.RebuildLayout();
            PopulatePreviewNodeSelector();
            UpdatePropertiesPanel();
            UpdateOutdatedEffectsButton();
        });
        // Populate the Add Node flyout with effects from the registry.
        PopulateAddNodeFlyout();

        BottomTabView().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isShuttingDown) return;
            UpdateCrosshairOverlay();
        });

        // Defer GPU initialization until the SwapChainPanel is in the visual tree.
        PreviewPanel().Loaded([this](auto&&, auto&&) { OnPreviewPanelLoaded(); });
        NodeGraphPanel().SizeChanged([this](auto&&, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& e)
        {
            ResizeGraphPanel(
                static_cast<float>(e.NewSize().Width),
                static_cast<float>(e.NewSize().Height));
        });
        NodeGraphPanel().CompositionScaleChanged([this](auto&&, auto&&)
        {
            UpdateGraphPanelScale();
            ResizeGraphPanel(
                static_cast<float>(NodeGraphPanel().ActualWidth()),
                static_cast<float>(NodeGraphPanel().ActualHeight()));
        });
        PreviewPanel().CompositionScaleChanged([this](auto&&, auto&&)
        {
            OnPreviewSizeChanged(PreviewPanel(), nullptr);
        });
        TraceSwatchPanel().SizeChanged([this](auto&&, auto&&)
        {
            ResizeTraceSwatchPanel();
        });
        TraceSwatchPanel().CompositionScaleChanged([this](auto&&, auto&&)
        {
            UpdateTraceSwatchPanelScale();
            ResizeTraceSwatchPanel();
        });
        NodeGraphContainer().PointerPressed({ this, &MainWindow::OnGraphPanelPointerPressed });
        NodeGraphContainer().PointerMoved({ this, &MainWindow::OnGraphPanelPointerMoved });
        NodeGraphContainer().PointerReleased({ this, &MainWindow::OnGraphPanelPointerReleased });
        NodeGraphContainer().PointerWheelChanged({ this, &MainWindow::OnGraphPanelPointerWheel });
        NodeGraphContainer().KeyDown([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
        {
            auto key = args.Key();
            auto ctrlState = winrt::Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(
                winrt::Windows::System::VirtualKey::Control);
            bool ctrlDown = (static_cast<uint32_t>(ctrlState) & static_cast<uint32_t>(winrt::Windows::UI::Core::CoreVirtualKeyStates::Down)) != 0;

            // Ctrl+A: select all nodes.
            if (ctrlDown && key == winrt::Windows::System::VirtualKey::A)
            {
                m_nodeGraphController.SelectAll();
                args.Handled(true);
                return;
            }

            // Ctrl+C: copy selected nodes.
            if (ctrlDown && key == winrt::Windows::System::VirtualKey::C)
            {
                auto& sel = m_nodeGraphController.SelectedNodes();
                if (sel.empty() && m_selectedNodeId != 0)
                {
                    m_nodeGraphController.SelectNode(m_selectedNodeId);
                }
                m_nodeClipboard.clear();
                m_edgeClipboard.clear();
                for (uint32_t nodeId : m_nodeGraphController.SelectedNodes())
                {
                    auto* node = m_graph.FindNode(nodeId);
                    if (node)
                        m_nodeClipboard.push_back({ *node, nodeId });
                }
                // Copy internal edges (edges where both src and dst are in the selection).
                for (const auto& edge : m_graph.Edges())
                {
                    bool srcIn = m_nodeGraphController.SelectedNodes().count(edge.sourceNodeId) > 0;
                    bool dstIn = m_nodeGraphController.SelectedNodes().count(edge.destNodeId) > 0;
                    if (srcIn && dstIn)
                        m_edgeClipboard.push_back(edge);
                }
                args.Handled(true);
                return;
            }

            // Ctrl+V: paste copied nodes.
            if (ctrlDown && key == winrt::Windows::System::VirtualKey::V && !m_nodeClipboard.empty())
            {
                // Map old IDs to new IDs.
                std::unordered_map<uint32_t, uint32_t> idMap;
                float offsetX = 40.0f, offsetY = 40.0f;

                for (auto& entry : m_nodeClipboard)
                {
                    auto newNode = entry.node;
                    newNode.position.x += offsetX;
                    newNode.position.y += offsetY;
                    newNode.dirty = true;
                    newNode.cachedOutput = nullptr;
                    newNode.runtimeError.clear();
                    // Fresh GUID for custom effects to avoid D2D shader ID collisions.
                    if (newNode.customEffect.has_value())
                        CoCreateGuid(&newNode.customEffect->shaderGuid);

                    uint32_t newId = m_nodeGraphController.AddNode(std::move(newNode), { 0.0f, 0.0f });
                    idMap[entry.originalId] = newId;
                }

                // Reconnect internal edges with new IDs.
                for (const auto& edge : m_edgeClipboard)
                {
                    auto srcIt = idMap.find(edge.sourceNodeId);
                    auto dstIt = idMap.find(edge.destNodeId);
                    if (srcIt != idMap.end() && dstIt != idMap.end())
                        m_graph.Connect(srcIt->second, edge.sourcePin, dstIt->second, edge.destPin);
                }

                m_graph.MarkAllDirty();
                m_nodeGraphController.RebuildLayout();
                PopulatePreviewNodeSelector();
                args.Handled(true);
                return;
            }

            // Ctrl+L: auto-arrange graph layout.
            if (ctrlDown && key == winrt::Windows::System::VirtualKey::L)
            {
                m_nodeGraphController.AutoLayout();
                m_forceRender = true;
                args.Handled(true);
                return;
            }

            // Delete: remove all selected nodes (or single selected node).
            if (key == winrt::Windows::System::VirtualKey::Delete &&
                (!m_nodeGraphController.SelectedNodes().empty() || m_selectedNodeId != 0))
            {
                // If only a single node is selected via click (not multi-select), select it for deletion.
                if (m_nodeGraphController.SelectedNodes().empty() && m_selectedNodeId != 0)
                {
                    const auto* node = m_graph.FindNode(m_selectedNodeId);
                    if (node && node->type == ::ShaderLab::Graph::NodeType::Output)
                        return;
                    m_nodeGraphController.SelectNode(m_selectedNodeId);
                }

                // Close output windows for nodes being deleted.
                for (uint32_t nodeId : m_nodeGraphController.SelectedNodes())
                    CloseOutputWindow(nodeId);

                m_nodeGraphController.DeleteSelected();
                m_selectedNodeId = 0;
                m_graph.MarkAllDirty();
                m_nodeGraphController.RebuildLayout();
                PopulatePreviewNodeSelector();
                UpdatePropertiesPanel();
                MarkUnsaved();

                // Reset preview to Output node.
                for (const auto& n : m_graph.Nodes())
                {
                    if (n.type == ::ShaderLab::Graph::NodeType::Output)
                    {
                        m_previewNodeId = n.id;
                        break;
                    }
                }
                PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                args.Handled(true);
            }
        });
        NodeGraphContainer().IsTabStop(true);

        SaveImageButton().Click({ this, &MainWindow::OnSaveImageClicked });
        EffectDesignerButton().Click([this](auto&&, auto&&) { OpenEffectDesigner(); });

        // MCP server toggle.
        McpServerToggle().Click([this](auto&&, auto&&)
        {
            bool checked = McpServerToggle().IsChecked().GetBoolean();
            if (checked)
            {
                if (!m_mcpServer)
                    SetupMcpRoutes();
                if (m_mcpServer && !m_mcpServer->IsRunning())
                    m_mcpServer->Start(47808);
                // Wait briefly for the listener thread to bind and set the port.
                Sleep(100);
                uint16_t actualPort = m_mcpServer ? m_mcpServer->Port() : 47808;
                McpServerLabel().Text(std::format(L"MCP Server :{}", actualPort));
                McpExportConfigButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                ResetMcpActivityState();
                UpdateMcpActivityIndicator();
            }
            else
            {
                if (m_mcpServer)
                    m_mcpServer->Stop();
                McpServerLabel().Text(L"MCP Server");
                McpExportConfigButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                ResetMcpActivityState();
                UpdateMcpActivityIndicator();
            }
        });

        McpExportConfigButton().Click([this](auto&&, auto&&)
        {
            uint16_t port = m_mcpServer ? m_mcpServer->Port() : 47808;
            namespace DP = winrt::Windows::ApplicationModel::DataTransfer;
            auto pkg = DP::DataPackage();
            std::wstring config = std::format(
                L"{{\n"
                L"  \"mcpServers\": {{\n"
                L"    \"shaderlab\": {{\n"
                L"      \"url\": \"http://localhost:{}/\"\n"
                L"    }}\n"
                L"  }}\n"
                L"}}", port);
            pkg.SetText(config);
            DP::Clipboard::SetContent(pkg);
            PipelineFormatText().Text(std::format(L"MCP config copied to clipboard (http://localhost:{})", port));
        });
    }

    MainWindow::~MainWindow()
    {
        m_isShuttingDown = true;

        // Stop MCP server before tearing down resources.
        if (m_mcpServer)
            m_mcpServer->Stop();

        if (m_renderTimer)
        {
            m_renderTimer.Stop();
            m_renderTimer = nullptr;
        }

        m_traceSwapChain = nullptr;
        m_traceSwatchTarget = nullptr;

        // Close all output windows.
        m_outputWindows.clear();

        m_graphEvaluator.ReleaseCache(m_graph);
        m_displayMonitor.Shutdown();
        m_renderEngine.Shutdown();
    }

    HWND MainWindow::GetWindowHandle()
    {
        HWND hwnd{ nullptr };
        auto windowNative = this->try_as<::IWindowNative>();
        if (windowNative)
        {
            windowNative->get_WindowHandle(&hwnd);
        }
        return hwnd;
    }

    void MainWindow::InitializeRendering()
    {
        // Query display capabilities and pick a default pipeline format.
        m_displayMonitor.Initialize(m_hwnd);
        auto caps = m_displayMonitor.CachedCapabilities();
        auto format = ::ShaderLab::Rendering::RecommendedFormat(caps);

        // Create D3D11/D2D1 device stack and swap chain on the PreviewPanel.
        m_renderEngine.Initialize(m_hwnd, PreviewPanel(), format, m_devicePref);

        // Phase 8c: enable skip-unneeded-CPU-readback. Every frame the
        // RenderTick installs `{m_selectedNodeId}` as the CPU-analysis
        // interest set, so the selected node's Properties panel stays
        // live while every other compute analysis dispatch's Map() is
        // skipped when its consumers are entirely GPU-routed.
        // MCP /analysis/{id} reads temporarily disable the flag so they
        // always return fresh values regardless of selection.
        ::ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);

        // Now that we have a DXGI factory, register adapter-change monitoring.
        if (m_renderEngine.DXGIFactory())
        {
            m_displayMonitor.Shutdown();
            m_displayMonitor.Initialize(m_hwnd, m_renderEngine.DXGIFactory());
        }

        // Subscribe to display changes so we can update the status bar.
        m_displayMonitor.SetCallback([this](const ::ShaderLab::Rendering::DisplayCapabilities& /*newCaps*/)
        {
            this->DispatcherQueue().TryEnqueue([this]()
            {
                // Re-evaluate graph so effects using monitor gamut
                // pick up the new primaries.
                m_graph.MarkAllDirty();
                m_forceRender = true;
                // Pick up new refresh rate (e.g. user changed displays
                // or switched modes from 60 Hz to 144 Hz).
                UpdateRenderTimerInterval();
                // Push the new capabilities into any Working Space nodes
                // so downstream binders see the live values immediately.
                UpdateWorkingSpaceNodes();
                UpdateStatusBar();
            });
        });
    }

    void MainWindow::RegisterCustomEffects()
    {
        if (m_customEffectsRegistered) return;

        auto* factory = m_renderEngine.D2DFactory();
        if (!factory) return;

        winrt::com_ptr<ID2D1Factory1> factory1;
        factory->QueryInterface(IID_PPV_ARGS(factory1.put()));
        if (!factory1) return;

        ::ShaderLab::Effects::RegisterEngineD2DEffects(factory1.get());
        OutputDebugStringW(L"[CustomFX] Registered engine D2D effects\n");

        // Phase 8: enable on-disk bytecode cache under %LOCALAPPDATA%.
        // Background-reap entries older than 90 days on engine init.
        wchar_t* localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData)
        {
            std::wstring cacheRoot = std::wstring(localAppData) + L"\\ShaderLab\\bytecode";
            ::CoTaskMemFree(localAppData);
            ::ShaderLab::Effects::ConfigureBytecodeCache(cacheRoot);
            OutputDebugStringW((L"[CustomFX] BytecodeCache root: " + cacheRoot + L"\n").c_str());
        }

        m_customEffectsRegistered = true;
    }

    void MainWindow::OnPreviewPanelLoaded()
    {
        if (!m_hwnd || m_renderEngine.IsInitialized()) return;

        InitializeRendering();
        RegisterCustomEffects();

        // Pre-setup MCP routes (server starts when user toggles the button or --mcp flag).
        SetupMcpRoutes();
        if (m_autoStartMcp && m_mcpServer)
        {
            m_mcpServer->Start(47808);
            McpServerToggle().IsChecked(true);
            // Delay slightly to let the listener thread bind.
            DispatcherQueue().TryEnqueue([this]()
            {
                uint16_t actualPort = m_mcpServer ? m_mcpServer->Port() : 47808;
                McpServerLabel().Text(std::format(L"MCP Server :{}", actualPort));
                McpExportConfigButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                ResetMcpActivityState();
                UpdateMcpActivityIndicator();
            });
        }

        UpdateStatusBar();

        // Refresh title bar now that the effect library is loaded so the
        // title shows both the app and effect-library version.
        RefreshTitleBar();

        m_nodeGraphController.SetGraph(&m_graph);
        m_nodeGraphController.SetConnectionCallback(
            [this](uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin, bool isData) {
                auto* srcNode = m_graph.FindNode(srcId);
                auto* dstNode = m_graph.FindNode(dstId);
                std::wstring srcName = srcNode ? srcNode->name : std::format(L"Node {}", srcId);
                std::wstring dstName = dstNode ? dstNode->name : std::format(L"Node {}", dstId);
                if (isData)
                {
                    m_nodeLogs[dstId].Info(std::format(L"Property bound from {} (data pin)", srcName));
                    m_nodeLogs[srcId].Info(std::format(L"Data output bound to {}", dstName));
                }
                else
                {
                    m_nodeLogs[dstId].Info(std::format(L"Input {} connected from {}", dstPin, srcName));
                    m_nodeLogs[srcId].Info(std::format(L"Output {} connected to {}", srcPin, dstName));
                }
                MarkUnsaved();
            });

        if (m_renderEngine.D3DDevice())
        {
            m_pixelInspector.Initialize(m_renderEngine.D3DDevice());
            m_pixelTrace.Initialize(m_renderEngine.D3DDevice());
        }

        // Start render loop. The interval tracks the monitor's refresh
        // rate (clamped to 60..240 Hz), so 120 Hz / 144 Hz / 240 Hz panels
        // and high-FPS video sources actually run at their native cadence.
        m_fpsTimePoint = std::chrono::steady_clock::now();
        m_lastRenderTick = m_fpsTimePoint;
        m_renderTimer = DispatcherQueue().CreateTimer();
        m_renderTimer.Tick({ this, &MainWindow::OnRenderTick });
        UpdateRenderTimerInterval();
        m_renderTimer.Start();

        // Initialize the node graph editor panel.
        InitializeGraphPanel();

        // Sweep %TEMP% for ShaderLab-* media dirs left behind by a
        // crashed prior instance and offer to delete them. Runs after
        // panel init so the dialog has a XamlRoot to attach to.
        ReapStaleMediaDirsAsync();

        // If the app was launched via file double-click, load that
        // graph now that rendering is wired up.
        if (!m_pendingOpenPath.empty())
        {
            auto path = std::move(m_pendingOpenPath);
            m_pendingOpenPath.clear();
            LoadGraphFromPathAsync(winrt::hstring(path));
        }
        else
        {
            RefreshTitleBar();
        }
    }

    void MainWindow::UpdateStatusBar()
    {
        auto caps = m_displayMonitor.CachedCapabilities();

        if (m_displayMonitor.IsSimulated())
        {
            auto profile = m_displayMonitor.ActiveProfile();
            DisplayModeText().Text(L"Display: Simulated \u2014 " + winrt::hstring(profile.profileName));
        }
        else
        {
            DisplayModeText().Text(L"Display: " + caps.ModeString());
        }

        DisplayLuminanceText().Text(L"Max Luminance: " + caps.LuminanceString());
        PipelineFormatText().Text(L"Pipeline: " + m_renderEngine.ActiveFormat().name);

        // GPU info.
        if (m_renderEngine.IsInitialized())
        {
            if (m_renderEngine.IsWarp())
                GpuInfoText().Text(L"GPU: Software (WARP)");
            else
                GpuInfoText().Text(L"GPU: " + m_renderEngine.AdapterName());
        }
    }

    void MainWindow::ResetMcpActivityState()
    {
        m_mcpLastActivityMs.store(0, std::memory_order_relaxed);
        m_mcpRequestCount.store(0, std::memory_order_relaxed);
        m_mcpUiUpdateSeq.store(0, std::memory_order_relaxed);
        m_mcpLastUiUpdateSeq = 0;
        std::lock_guard lock(m_mcpLastReqMutex);
        m_mcpLastReqMethod.clear();
        m_mcpLastReqPath.clear();
        m_mcpLastReqPeer.clear();
        m_mcpLastReqStatus = 0;
        m_mcpKnownPeers.clear();
    }

    void MainWindow::UpdateMcpActivityIndicator()
    {
        // Hide the dot entirely when the server is off.
        if (!m_mcpServer || !m_mcpServer->IsRunning())
        {
            McpActivityDot().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            Controls::ToolTipService::SetToolTip(McpServerToggle(),
                winrt::box_value(winrt::hstring(L"Start/stop MCP server for AI assistant integration")));
            return;
        }

        McpActivityDot().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);

        const int64_t lastMs = m_mcpLastActivityMs.load(std::memory_order_relaxed);
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const int64_t ageMs = (lastMs == 0) ? -1 : (nowMs - lastMs);
        const uint64_t totalCount = m_mcpRequestCount.load(std::memory_order_relaxed);

        // Color ramp (chosen for high contrast against the accent-blue
        // checked-toggle background AND the neutral unchecked background):
        //   no activity yet  -> dark slate (#FF303030)
        //   <250ms           -> amber (#FFFFC107) -- "live" pulse
        //   250-1500ms       -> linear fade amber -> dark slate
        //   >=1500ms         -> dark slate (#FF303030)
        winrt::Windows::UI::Color color{};
        color.A = 0xFF;
        if (ageMs < 0)
        {
            color.R = 0x30; color.G = 0x30; color.B = 0x30;
        }
        else if (ageMs < 250)
        {
            color.R = 0xFF; color.G = 0xC1; color.B = 0x07;
        }
        else if (ageMs < 1500)
        {
            const float t = static_cast<float>(ageMs - 250) / 1250.0f;
            const float inv = 1.0f - t;
            const float r = 0xFF * inv + 0x30 * t;
            const float g = 0xC1 * inv + 0x30 * t;
            const float b = 0x07 * inv + 0x30 * t;
            color.R = static_cast<uint8_t>(r);
            color.G = static_cast<uint8_t>(g);
            color.B = static_cast<uint8_t>(b);
        }
        else
        {
            color.R = 0x30; color.G = 0x30; color.B = 0x30;
        }
        McpActivityDotBrush().Color(color);

        // Tooltip: only rebuild on a callback-driven change OR while we're
        // still in the active fade window (so "Xs ago" stays current).
        const uint64_t seq = m_mcpUiUpdateSeq.load(std::memory_order_acquire);
        const bool inFade = (ageMs >= 0 && ageMs < 1500);
        if (seq == m_mcpLastUiUpdateSeq && !inFade)
            return;
        m_mcpLastUiUpdateSeq = seq;
        std::wstring tooltip;
        uint16_t port = m_mcpServer->Port();
        if (totalCount == 0)
        {
            tooltip = std::format(L"MCP Server :{} \u2014 listening (no requests yet)", port);
        }
        else
        {
            std::string method, path, peer;
            uint16_t status = 0;
            size_t peerCount = 0;
            {
                std::lock_guard lock(m_mcpLastReqMutex);
                method = m_mcpLastReqMethod;
                path   = m_mcpLastReqPath;
                peer   = m_mcpLastReqPeer;
                status = m_mcpLastReqStatus;
                peerCount = m_mcpKnownPeers.size();
            }
            std::wstring methodW(method.begin(), method.end());
            int needed = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                static_cast<int>(path.size()), nullptr, 0);
            std::wstring pathW(static_cast<size_t>(needed), L'\0');
            if (needed > 0)
            {
                MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                    static_cast<int>(path.size()), pathW.data(), needed);
            }
            std::wstring peerW(peer.begin(), peer.end());
            std::wstring ageStr;
            if (ageMs < 1000)        ageStr = L"just now";
            else if (ageMs < 60000)  ageStr = std::format(L"{}s ago", ageMs / 1000);
            else                     ageStr = std::format(L"{}m ago", ageMs / 60000);

            tooltip = std::format(
                L"MCP Server :{} \u2014 {} request{}\n"
                L"Last: {} {} \u2192 {} ({})\n"
                L"From: {}{}",
                port, totalCount, (totalCount == 1 ? L"" : L"s"),
                methodW, pathW, status, ageStr,
                peerW.empty() ? L"(unknown)" : peerW.c_str(),
                peerCount > 1 ? std::format(L"  (\u00d7{} distinct clients)", peerCount).c_str() : L"");
        }
        Controls::ToolTipService::SetToolTip(McpServerToggle(),
            winrt::box_value(winrt::hstring(tooltip)));
    }

    uint32_t MainWindow::QueryDisplayRefreshHz() const noexcept
    {
        // Use EnumDisplaySettings on the monitor that contains the app HWND.
        // dmDisplayFrequency is reported in whole Hz; modern panels round to
        // 60/120/144/165/240 etc. Returning 0 here is the "unknown" signal.
        if (!m_hwnd)
            return 0;

        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
        if (!mon)
            return 0;

        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(mon, &mi))
            return 0;

        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!::EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            return 0;

        return dm.dmDisplayFrequency;
    }

    void MainWindow::UpdateRenderTimerInterval()
    {
        // Pick a target Hz from the active monitor, clamped to [60, 240].
        // - 60 Hz floor: avoid laggy interactions on 30 Hz TV-out modes.
        // - 240 Hz ceiling: dispatcher tick scheduling becomes noisy below
        //   ~4 ms, and rendering faster than panel refresh is wasted work.
        uint32_t hz = QueryDisplayRefreshHz();
        if (hz < 60) hz = 60;
        if (hz > 240) hz = 240;

        if (hz == m_targetRefreshHz && m_renderTimer)
            return;

        m_targetRefreshHz = hz;

        // Use microseconds so non-integer-ms periods (e.g. 144 Hz ~6.944 ms)
        // don't get rounded to a slower or faster cadence than the panel.
        const auto interval = std::chrono::microseconds(1'000'000 / hz);
        if (m_renderTimer)
            m_renderTimer.Interval(interval);
    }

    void MainWindow::OnGpuInfoTapped(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& /*args*/)
    {
        namespace Controls = winrt::Microsoft::UI::Xaml::Controls;
        using namespace ::ShaderLab::Rendering;

        auto adapters = RenderEngine::EnumerateAdapters();
        if (adapters.empty()) return;

        auto flyout = Controls::MenuFlyout();
        for (size_t i = 0; i < adapters.size(); ++i)
        {
            auto& a = adapters[i];
            auto item = Controls::MenuFlyoutItem();
            std::wstring label = a.name;
            if (a.dedicatedVideoMemoryMB > 0)
                label += std::format(L" ({} MB)", a.dedicatedVideoMemoryMB);
            if (a.isWarp)
                label += L" [Software]";

            // Mark current adapter.
            if (a.name == m_renderEngine.AdapterName() ||
                (a.isWarp && m_renderEngine.IsWarp()))
                label = L"\u2713 " + label;

            item.Text(winrt::hstring(label));
            auto luid = a.luid;
            bool isWarp = a.isWarp;
            item.Click([this, luid, isWarp](auto&&, auto&&) {
                SwitchAdapter(
                    isWarp ? DevicePreference::Warp : DevicePreference::Adapter,
                    luid);
            });
            flyout.Items().Append(item);
        }

        flyout.ShowAt(sender.as<winrt::Microsoft::UI::Xaml::FrameworkElement>());
    }

    void MainWindow::SwitchAdapter(
        ::ShaderLab::Rendering::DevicePreference pref, LUID adapterLuid)
    {
        // Stop render timer.
        if (m_renderTimer) m_renderTimer.Stop();

        // Save graph + view state.
        auto graphJson = m_graph.ToJson();
        uint32_t savedPreviewId = m_previewNodeId;
        uint32_t savedSelectedId = m_selectedNodeId;
        auto savedPan = m_nodeGraphController.PanOffset();
        float savedZoom = m_nodeGraphController.Zoom();
        float savedPreviewZoom = m_previewZoom;
        float savedPreviewPanX = m_previewPanX;
        float savedPreviewPanY = m_previewPanY;

        // ---- NUKE EVERYTHING ----
        m_graphEvaluator.ReleaseCache(m_graph);
        m_sourceFactory.ReleaseCache();
        m_nodeGraphController.ReleaseDeviceResources();
        // Clear swap chain references from XAML panels before destroying them.
        try {
            auto panelNative = NodeGraphPanel().as<ISwapChainPanelNative>();
            if (panelNative) panelNative->SetSwapChain(nullptr);
        } catch (...) {}
        m_graphRenderTarget = nullptr;
        m_graphSwapChain = nullptr;
        m_graphGridBrush = nullptr;
        m_traceSwatchTarget = nullptr;
        m_traceSwapChain = nullptr;
        for (auto& w : m_outputWindows) w->Close();
        m_outputWindows.clear();
        for (auto& w : m_logWindows) w->Close();
        m_logWindows.clear();
        m_graph.Clear();
        m_displayMonitor.SetCallback(nullptr);
        m_displayMonitor.Shutdown();
        m_renderEngine.Shutdown();

        // ---- REBUILD FROM SCRATCH ----
        m_devicePref = pref;
        if (pref == ::ShaderLab::Rendering::DevicePreference::Adapter)
            m_renderEngine.SetPreferredAdapterLuid(adapterLuid);

        try
        {
            InitializeRendering();
        }
        catch (...)
        {
            // Requested adapter failed — fall back to default.
            OutputDebugStringW(L"[GPU Switch] InitializeRendering failed, falling back to Default\n");
            m_devicePref = ::ShaderLab::Rendering::DevicePreference::Default;
            try { InitializeRendering(); }
            catch (...) {
                // Total failure — restart timer and bail.
                if (m_renderTimer) m_renderTimer.Start();
                return;
            }
        }
        m_customEffectsRegistered = false;
        RegisterCustomEffects();
        InitializeGraphPanel();

        if (m_renderEngine.D3DDevice())
        {
            m_pixelInspector.Initialize(m_renderEngine.D3DDevice());
            m_pixelTrace.Initialize(m_renderEngine.D3DDevice());
        }

        // ---- RELOAD GRAPH ----
        try { m_graph = ::ShaderLab::Graph::EffectGraph::FromJson(graphJson); }
        catch (...) {}
        ResetAfterGraphLoad(false);
        m_nodeGraphController.RebuildLayout();

        // Restore view state.
        if (savedPreviewId != 0 && m_graph.FindNode(savedPreviewId))
            m_previewNodeId = savedPreviewId;
        m_nodeGraphController.SetPanOffset(savedPan.x, savedPan.y);
        m_nodeGraphController.SetZoom(savedZoom);
        m_previewZoom = savedPreviewZoom;
        m_previewPanX = savedPreviewPanX;
        m_previewPanY = savedPreviewPanY;
        m_needsFitPreview = false;
        if (savedSelectedId != 0 && m_graph.FindNode(savedSelectedId))
        {
            m_selectedNodeId = savedSelectedId;
            m_nodeGraphController.SelectNode(savedSelectedId);
            UpdatePropertiesPanel();
        }

        // Re-prepare source nodes on the new device.
        // Skip video sources — they can be reopened by the user via file picker.
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (dc)
        {
            for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
            {
                if (node.type == ::ShaderLab::Graph::NodeType::Source)
                {
                    bool isVideo = false;
                    auto it = node.properties.find(L"IsVideo");
                    if (it != node.properties.end())
                        if (auto* b = std::get_if<bool>(&it->second)) isVideo = *b;

                    if (isVideo)
                    {
                        // Defer video reload — give the new device time to settle.
                        uint32_t videoNodeId = node.id;
                        node.dirty = true;
                        DispatcherQueue().TryEnqueue([this, videoNodeId]() {
                            auto* vn = m_graph.FindNode(videoNodeId);
                            auto* vdc = m_renderEngine.D2DDeviceContext();
                            if (vn && vdc)
                            {
                                try {
                                    m_sourceFactory.PrepareSourceNode(*vn, vdc, 0.0,
                                        m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
                                    vn->runtimeError.clear();
                                } catch (...) {
                                    vn->runtimeError = L"Video reload failed after GPU switch";
                                }
                            }
                        });
                        continue;
                    }

                    node.dirty = true;
                    try {
                        m_sourceFactory.PrepareSourceNode(node, dc, 0.0,
                            m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
                    } catch (...) {
                        node.dirty = false;
                    }
                }
            }
        }

        m_forceRender = true;
        UpdateStatusBar();
        // Reopen output windows.
        auto outputIds = m_graph.GetOutputNodeIds();
        for (uint32_t id : outputIds)
        {
            try { OpenOutputWindow(id); } catch (...) {}
        }
        // Restart render timer.
        if (m_renderTimer) m_renderTimer.Start();

        // Deferred video reload: post to the dispatcher queue so it runs
        // after the switch is fully complete and the first frames render.
        DispatcherQueue().TryEnqueue(
            winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
            [this]() {
                for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
                {
                    if (node.type == ::ShaderLab::Graph::NodeType::Source &&
                        !node.runtimeError.empty())
                    {
                        node.runtimeError.clear();
                        node.dirty = true;
                    }
                }
                m_forceRender = true;
            });
    }

    // -----------------------------------------------------------------------
    // Drag and drop — create source nodes from dropped files
    // -----------------------------------------------------------------------

    void MainWindow::OnNodeGraphDragOver(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::DragEventArgs const& args)
    {
        if (args.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
            args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
    }

    winrt::fire_and_forget MainWindow::OnNodeGraphDrop(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::DragEventArgs const& args)
    {
        auto strong = get_strong();
        if (!args.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
            co_return;

        auto items = co_await args.DataView().GetStorageItemsAsync();
        auto* dc = m_renderEngine.D2DDeviceContext();

        for (const auto& item : items)
        {
            auto file = item.try_as<winrt::Windows::Storage::StorageFile>();
            if (!file) continue;

            std::wstring path(file.Path());
            bool isVideo = ::ShaderLab::Effects::SourceNodeFactory::IsVideoFile(path);

            ::ShaderLab::Graph::EffectNode node;
            if (isVideo)
            {
                node = ::ShaderLab::Effects::SourceNodeFactory::CreateVideoSourceNode(path);
            }
            else
            {
                node = ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(path);
            }

            auto id = m_graph.AddNode(std::move(node));
            m_nodeLogs[id].Info(std::format(L"Dropped: {}", std::filesystem::path(path).filename().wstring()));

            // Prepare the source on the current device.
            auto* addedNode = m_graph.FindNode(id);
            if (addedNode && dc)
            {
                try {
                    m_sourceFactory.PrepareSourceNode(*addedNode, dc, 0.0,
                        m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
                } catch (...) {}
            }
        }

        m_graph.MarkAllDirty();
        m_nodeGraphController.AutoLayout();
        PopulatePreviewNodeSelector();
        m_forceRender = true;
    }

    void MainWindow::OnPreviewSizeChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
    {
        if (!m_renderEngine.IsInitialized())
            return;

        auto panel = PreviewPanel();
        double width = args ? args.NewSize().Width : panel.ActualWidth();
        double height = args ? args.NewSize().Height : panel.ActualHeight();
        auto scaleX = static_cast<float>(panel.CompositionScaleX());
        auto scaleY = static_cast<float>(panel.CompositionScaleY());
        auto w = static_cast<uint32_t>((std::max)(1.0f, static_cast<float>(std::ceil(width * scaleX))));
        auto h = static_cast<uint32_t>((std::max)(1.0f, static_cast<float>(std::ceil(height * scaleY))));
        m_renderEngine.Resize(w, h);
    }

    // -----------------------------------------------------------------------
    // Per-node preview
    // -----------------------------------------------------------------------

    void MainWindow::PopulatePreviewNodeSelector()
    {
        // Cache the topological order (used by graph evaluation and navigation).
        try { m_topoOrder = m_graph.TopologicalSort(); }
        catch (...) { m_topoOrder.clear(); }

        UpdateOutdatedEffectsButton();
    }

    void MainWindow::UpdateOutdatedEffectsButton()
    {
        auto& registry = ::ShaderLab::Effects::ShaderLabEffects::Instance();
        uint32_t outdatedCount = 0;
        for (const auto& node : m_graph.Nodes())
        {
            if (!node.customEffect.has_value() || node.customEffect->shaderLabEffectId.empty())
                continue;
            auto* desc = registry.FindById(node.customEffect->shaderLabEffectId);
            if (desc && desc->effectVersion > node.customEffect->shaderLabEffectVersion)
                outdatedCount++;
        }
        if (outdatedCount > 0)
        {
            UpdateAllEffectsText().Text(std::format(L"Update Effects ({})", outdatedCount));
            UpdateAllEffectsButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
        else
        {
            UpdateAllEffectsButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
    }

    void MainWindow::OnPreviewKeyDown(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
    {
        if (m_topoOrder.empty()) return;

        auto key = args.Key();
        constexpr auto vkOpenBracket = static_cast<winrt::Windows::System::VirtualKey>(219);   // [
        constexpr auto vkCloseBracket = static_cast<winrt::Windows::System::VirtualKey>(221);  // ]
        if (key == vkOpenBracket || key == vkCloseBracket)
        {
            // Find current index in topo order.
            int32_t curIdx = -1;
            for (int32_t i = 0; i < static_cast<int32_t>(m_topoOrder.size()); ++i)
            {
                if (m_topoOrder[i] == m_previewNodeId)
                { curIdx = i; break; }
            }

            int32_t count = static_cast<int32_t>(m_topoOrder.size());
            if (key == vkOpenBracket && curIdx > 0)
                m_previewNodeId = m_topoOrder[curIdx - 1];
            else if (key == vkCloseBracket && curIdx < count - 1)
                m_previewNodeId = m_topoOrder[curIdx + 1];

            UpdatePreviewOverlay();
            m_forceRender = true;
            FitPreviewToView();
            args.Handled(true);
        }
    }

    void MainWindow::UpdatePreviewOverlay()
    {
        const auto* node = m_graph.FindNode(m_previewNodeId);
        if (node)
        {
            PreviewOverlayText().Text(winrt::hstring(node->name));
            PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
        else
        {
            PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
    }

    ID2D1Image* MainWindow::GetPreviewImage()
    {
        const auto* node = m_graph.FindNode(m_previewNodeId);
        if (node && node->cachedOutput)
            return node->cachedOutput;
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Display profile selection -- methods moved to MainWindow.WorkingSpace.cpp
    // (Phase 4 split). PopulateDisplayProfileSelector / ApplyDisplayProfile /
    // RevertToLiveDisplay / UpdateWorkingSpaceNodes /
    // OnDisplayProfileSelectionChanged / LoadIccProfileAsync are members of
    // the same class -- see MainWindow.xaml.h.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Graph save/load + embedded-media archive + heartbeat / stale-temp-dir
    // reaper -- methods moved to MainWindow.GraphFileIo.cpp (Phase 4 split).
    // The methods are members of the same class -- see MainWindow.xaml.h.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Add node
    // -----------------------------------------------------------------------

    void MainWindow::PopulateAddNodeFlyout()
    {
        auto flyout = AddNodeFlyout();
        flyout.Items().Clear();

        namespace MUX = winrt::Microsoft::UI::Xaml::Controls;

        // ---- Built-in D2D effects ----
        auto builtInGroup = MUX::MenuFlyoutSubItem();
        builtInGroup.Text(L"Built-in D2D");

        auto& registry = ::ShaderLab::Effects::EffectRegistry::Instance();
        auto categories = registry.Categories();

        MUX::MenuFlyoutSubItem sourceSubItem{ nullptr };

        for (const auto& cat : categories)
        {
            // Skip the old Analysis category — replaced by ShaderLab analysis effects.
            if (cat == L"Analysis") continue;

            auto subItem = MUX::MenuFlyoutSubItem();
            subItem.Text(winrt::hstring(cat));

            auto effects = registry.ByCategory(cat);
            for (const auto* desc : effects)
            {
                auto menuItem = MUX::MenuFlyoutItem();
                menuItem.Text(winrt::hstring(desc->name));

                auto capturedDesc = *desc;
                menuItem.Click([this, capturedDesc](auto&&, auto&&)
                {
                    OnAddEffectNode(capturedDesc);
                });

                subItem.Items().Append(menuItem);
            }

            builtInGroup.Items().Append(subItem);

            if (cat == L"Source")
                sourceSubItem = subItem;
        }

        // Append Image and Flood sources to the Source category.
        if (!sourceSubItem)
        {
            sourceSubItem = MUX::MenuFlyoutSubItem();
            sourceSubItem.Text(L"Source");
            builtInGroup.Items().Append(sourceSubItem);
        }

        auto imageSourceItem = MUX::MenuFlyoutItem();
        imageSourceItem.Text(L"Image Source");
        imageSourceItem.Click([this](auto&&, auto&&)
        {
            auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Image Source");
            auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
            OnNodeAdded(nodeId);
        });
        sourceSubItem.Items().Append(imageSourceItem);

        auto floodSourceItem = MUX::MenuFlyoutItem();
        floodSourceItem.Text(L"Flood Fill (Solid Color)");
        floodSourceItem.Click([this](auto&&, auto&&)
        {
            OnAddFloodSourceClicked(nullptr, nullptr);
        });
        sourceSubItem.Items().Append(floodSourceItem);

        flyout.Items().Append(builtInGroup);

        // ---- ShaderLab effects ----
        auto& slRegistry = ::ShaderLab::Effects::ShaderLabEffects::Instance();
        auto slCategories = slRegistry.Categories();

        if (!slCategories.empty())
        {
            auto slGroup = MUX::MenuFlyoutSubItem();
            slGroup.Text(L"ShaderLab");

            for (const auto& cat : slCategories)
            {
                auto subItem = MUX::MenuFlyoutSubItem();
                subItem.Text(winrt::hstring(cat));

                auto effects = slRegistry.ByCategory(cat);

                // Group effects by their optional `subcategory` field. The
                // analysis category in particular has 16+ entries; folding
                // them into Highlights / Scopes / Statistics / Gamut Mapping
                // / Comparison sub-trees makes the flyout navigable. Effects
                // without a subcategory remain at the top level.
                std::map<std::wstring, std::vector<const ::ShaderLab::Effects::ShaderLabEffectDescriptor*>> grouped;
                std::vector<const ::ShaderLab::Effects::ShaderLabEffectDescriptor*> ungrouped;
                for (const auto* desc : effects)
                {
                    if (desc->subcategory.empty()) ungrouped.push_back(desc);
                    else grouped[desc->subcategory].push_back(desc);
                }

                auto addEffectItem = [this](MUX::MenuFlyoutSubItem& parent,
                                            const ::ShaderLab::Effects::ShaderLabEffectDescriptor* desc)
                {
                    auto menuItem = MUX::MenuFlyoutItem();
                    menuItem.Text(winrt::hstring(desc->name));

                    auto capturedName = desc->name;
                    menuItem.Click([this, capturedName](auto&&, auto&&)
                    {
                        auto* slDesc = ::ShaderLab::Effects::ShaderLabEffects::Instance().FindByName(capturedName);
                        if (!slDesc) return;
                        auto node = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*slDesc);
                        m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
                        m_graph.MarkAllDirty();
                        m_nodeGraphController.RebuildLayout();
                        PopulatePreviewNodeSelector();
                        // Force a render tick that triggers the post-eval
                        // RebuildLayout, which is what reveals on-node UI
                        // (Clock play/pause + slider) for newly-added
                        // parameter nodes. Without this the user has to
                        // click the node before the controls appear.
                        m_forceRender = true;
                    });
                    parent.Items().Append(menuItem);
                };

                // Sub-trees first (sorted by name for stable ordering).
                for (auto& [subName, subList] : grouped)
                {
                    auto subSub = MUX::MenuFlyoutSubItem();
                    subSub.Text(winrt::hstring(subName));
                    for (const auto* desc : subList)
                        addEffectItem(subSub, desc);
                    subItem.Items().Append(subSub);
                }
                // Then ungrouped effects directly under the category.
                for (const auto* desc : ungrouped)
                    addEffectItem(subItem, desc);

                slGroup.Items().Append(subItem);

                // Append Video Source to the Source subcategory.
                if (cat == L"Source")
                {
                    auto videoSourceItem = MUX::MenuFlyoutItem();
                    videoSourceItem.Text(L"Video Source");
                    videoSourceItem.Click([this](auto&&, auto&&)
                    {
                        BrowseVideoForSourceNode();
                    });
                    subItem.Items().Append(videoSourceItem);

                    auto dxgiCaptureGroup = MUX::MenuFlyoutSubItem();
                    dxgiCaptureGroup.Text(L"DXGI DuplicateOutput");
                    auto outputs = ::ShaderLab::Effects::DxgiDuplicationSourceProvider::EnumerateOutputs();
                    for (const auto& output : outputs)
                    {
                        auto item = MUX::MenuFlyoutItem();
                        std::wstring text = output.deviceName.empty()
                            ? std::format(L"Adapter {} Output {}", output.adapterIndex, output.outputIndex)
                            : std::format(L"{} ({},{})", output.deviceName, output.adapterIndex, output.outputIndex);
                        item.Text(winrt::hstring(text));
                        item.Click([this, output, text](auto&&, auto&&)
                        {
                            auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateDxgiDuplicateOutputSourceNode(
                                output.adapterIndex,
                                output.outputIndex,
                                L"DXGI " + text);
                            auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
                            OnNodeAdded(nodeId);
                        });
                        dxgiCaptureGroup.Items().Append(item);
                    }
                    if (dxgiCaptureGroup.Items().Size() == 0)
                    {
                        auto unavailable = MUX::MenuFlyoutItem();
                        unavailable.Text(L"No outputs available");
                        unavailable.IsEnabled(false);
                        dxgiCaptureGroup.Items().Append(unavailable);
                    }
                    subItem.Items().Append(dxgiCaptureGroup);

                    auto wgcSourceItem = MUX::MenuFlyoutItem();
                    wgcSourceItem.Text(L"Windows Graphics Capture...");
                    wgcSourceItem.Click([this](auto&&, auto&&)
                    {
                        AddWindowsGraphicsCaptureSourceAsync();
                    });
                    subItem.Items().Append(wgcSourceItem);
                }
            }

            flyout.Items().Append(slGroup);
        }

        // ---- Add Output Node ----
        {
            auto sep = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator();
            flyout.Items().Append(sep);

            auto outputItem = MUX::MenuFlyoutItem();
            outputItem.Text(L"Output Window");
            outputItem.Click([this](auto&&, auto&&)
            {
                // Create a new Output node with auto-incrementing name.
                auto outputIds = m_graph.GetOutputNodeIds();
                std::wstring name = L"Output";
                if (!outputIds.empty())
                    name = L"Output " + std::to_wstring(outputIds.size() + 1);

                ::ShaderLab::Graph::EffectNode outputNode;
                outputNode.name = name;
                outputNode.type = ::ShaderLab::Graph::NodeType::Output;
                outputNode.inputPins = { { L"Input", 0 } };
                auto nodeId = m_nodeGraphController.AddNode(std::move(outputNode), { 0.0f, 0.0f });

                m_graph.MarkAllDirty();
                m_nodeGraphController.RebuildLayout();
                PopulatePreviewNodeSelector();

                // Auto-open an output window for the new node.
                OpenOutputWindow(nodeId);
            });
            flyout.Items().Append(outputItem);
        }

        // ---- Custom Effects (duplicate existing) ----
        // Collect unique custom effect names from graph nodes.
        struct CustomEffectTemplate
        {
            std::wstring name;
            uint32_t sourceNodeId;
        };
        std::vector<CustomEffectTemplate> customTemplates;
        for (const auto& node : m_graph.Nodes())
        {
            if ((node.type == ::ShaderLab::Graph::NodeType::PixelShader ||
                 node.type == ::ShaderLab::Graph::NodeType::ComputeShader) &&
                node.customEffect.has_value() &&
                node.customEffect->isCompiled())
            {
                // Only add if this name isn't already in the list.
                bool found = false;
                for (const auto& t : customTemplates)
                    if (t.name == node.name) { found = true; break; }
                if (!found)
                    customTemplates.push_back({ node.name, node.id });
            }
        }

        if (!customTemplates.empty())
        {
            auto customGroup = MUX::MenuFlyoutSubItem();
            customGroup.Text(L"Custom Effects");

            for (const auto& tmpl : customTemplates)
            {
                auto menuItem = MUX::MenuFlyoutItem();
                menuItem.Text(winrt::hstring(tmpl.name));

                auto capturedNodeId = tmpl.sourceNodeId;
                menuItem.Click([this, capturedNodeId](auto&&, auto&&)
                {
                    // Deep-copy the custom effect from the source node.
                    auto* srcNode = m_graph.FindNode(capturedNodeId);
                    if (!srcNode || !srcNode->customEffect.has_value()) return;

                    ::ShaderLab::Graph::EffectNode newNode;
                    newNode.name = srcNode->name;
                    newNode.type = srcNode->type;
                    newNode.inputPins = srcNode->inputPins;
                    newNode.outputPins = srcNode->outputPins;
                    newNode.properties = srcNode->properties;

                    auto def = srcNode->customEffect.value();
                    // New instance gets a fresh GUID so D2D treats it as a separate effect.
                    CoCreateGuid(&def.shaderGuid);
                    newNode.customEffect = std::move(def);

                    m_nodeGraphController.AddNode(std::move(newNode), { 0.0f, 0.0f });
                    m_graph.MarkAllDirty();
                    m_nodeGraphController.RebuildLayout();
                    PopulatePreviewNodeSelector();
                    PopulateAddNodeFlyout(); // refresh custom effects list
                });

                customGroup.Items().Append(menuItem);
            }

            flyout.Items().Append(customGroup);
        }
    }

    void MainWindow::OnAddEffectNode(const ::ShaderLab::Effects::EffectDescriptor& desc)
    {
        auto node = ::ShaderLab::Effects::EffectRegistry::CreateNode(desc);
        // Pass {0,0} to use the controller's auto-layout which avoids overlaps.
        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
        OnNodeAdded(nodeId);
    }

    void MainWindow::OnAddImageSourceClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        AddImageSourceAsync();
    }

    winrt::fire_and_forget MainWindow::AddImageSourceAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);
        picker.FileTypeFilter().Append(L".png");
        picker.FileTypeFilter().Append(L".jpg");
        picker.FileTypeFilter().Append(L".jpeg");
        picker.FileTypeFilter().Append(L".bmp");
        picker.FileTypeFilter().Append(L".tif");
        picker.FileTypeFilter().Append(L".tiff");
        picker.FileTypeFilter().Append(L".hdr");
        picker.FileTypeFilter().Append(L".exr");
        picker.FileTypeFilter().Append(L".jxr");

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        auto filePath = std::wstring(file.Path().c_str());
        auto fileName = std::wstring(file.Name().c_str());

        auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(filePath, fileName);

        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });

        // Prepare the source (load the image bitmap).
        auto* graphNode = m_graph.FindNode(nodeId);
        if (graphNode && m_renderEngine.D2DDeviceContext())
        {
            m_sourceFactory.PrepareSourceNode(*graphNode, m_renderEngine.D2DDeviceContext(), 0.0, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
        }

        OnNodeAdded(nodeId);
    }

    void MainWindow::OnAddFloodSourceClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateFloodSourceNode(
            { 1.0f, 1.0f, 1.0f, 1.0f }, L"White");

        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });

        auto* graphNode = m_graph.FindNode(nodeId);
        if (graphNode && m_renderEngine.D2DDeviceContext())
        {
            m_sourceFactory.PrepareSourceNode(*graphNode, m_renderEngine.D2DDeviceContext(), 0.0, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
        }

        OnNodeAdded(nodeId);
    }

    winrt::fire_and_forget MainWindow::AddWindowsGraphicsCaptureSourceAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Graphics::Capture::GraphicsCapturePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);

        auto item = co_await picker.PickSingleItemAsync();
        if (!item) co_return;

        std::wstring name = item.DisplayName().empty()
            ? L"Windows Graphics Capture"
            : std::wstring(item.DisplayName().c_str());
        auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateWindowsGraphicsCaptureSourceNode(name);
        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
        m_sourceFactory.RegisterGraphicsCaptureItem(nodeId, item);

        if (auto* graphNode = m_graph.FindNode(nodeId); graphNode && m_renderEngine.D2DDeviceContext())
        {
            m_sourceFactory.PrepareSourceNode(*graphNode, m_renderEngine.D2DDeviceContext(), 0.0, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
        }

        OnNodeAdded(nodeId);
    }

    void MainWindow::OnNodeAdded(uint32_t /*nodeId*/)
    {
        m_graph.MarkAllDirty();
        m_nodeGraphController.RebuildLayout();
        PopulatePreviewNodeSelector();
        MarkUnsaved();
    }

    // -----------------------------------------------------------------------
    // Split comparison
    // -----------------------------------------------------------------------

    ID2D1Image* MainWindow::ResolveDisplayImage(uint32_t nodeId)
    {
        const auto* node = m_graph.FindNode(nodeId);
        if (!node || !node->cachedOutput) return nullptr;

        // Safety: cachedOutput is a non-owning raw pointer whose lifetime is
        // owned by m_graphEvaluator's cache. If the node is dirty, the
        // evaluator has not yet repopulated it this frame and the pointer
        // may be dangling (released by InvalidateNode/ReleaseCache).
        // The D2D debug layer (d2d1debug3.dll) traps the use-after-release.
        if (node->dirty) return nullptr;

        // Don't render effects that have required inputs but none connected.
        if (!node->inputPins.empty())
        {
            auto inputs = m_graph.GetInputEdges(nodeId);
            if (inputs.empty())
                return nullptr;
        }

        ID2D1Image* image = node->cachedOutput;
        return image;
    }

    void MainWindow::OnPreviewPointerDragged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& /*args*/)
    {
        // Previously used for split comparison dragging — now a no-op.
    }

    void MainWindow::OnPreviewPointerReleased(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isPreviewPanning)
        {
            bool wasClick = !m_previewDragMoved;
            m_isPreviewPanning = false;
            PreviewPanel().ReleasePointerCapture(args.Pointer());

            // If it was a click (no drag), trigger pixel trace.
            if (wasClick)
            {
                auto point = args.GetCurrentPoint(PreviewPanel());
                m_traceClickDipX = static_cast<float>(point.Position().X);
                m_traceClickDipY = static_cast<float>(point.Position().Y);
                m_traceClickPanX = m_previewPanX;
                m_traceClickPanY = m_previewPanY;
                m_traceClickZoom = m_previewZoom;

                float normX = 0, normY = 0;
                if (PointerToImageCoords(args, normX, normY))
                {
                    m_pixelTrace.SetTrackPosition(normX, normY);
                    m_traceActive = true;
                    m_lastTraceTopologyHash = 0;

                    auto imgBounds = GetPreviewImageBounds();
                    float dpiScale = (std::max)(1.0f, PreviewPanel().CompositionScaleX());
                    float iW = imgBounds.right - imgBounds.left;
                    float iH = imgBounds.bottom - imgBounds.top;
                    if (iW <= 0 || iW > 100000.0f) { auto vp = PreviewViewportDips(); iW = vp.width; }
                    if (iH <= 0 || iH > 100000.0f) { auto vp = PreviewViewportDips(); iH = vp.height; }
                    // Display position in actual image pixels (context DIPs * DPI scale).
                    uint32_t px = static_cast<uint32_t>(normX * iW * dpiScale);
                    uint32_t py = static_cast<uint32_t>(normY * iH * dpiScale);
                    TracePositionText().Text(std::format(L"Position: ({}, {})", px, py));

                    PopulatePixelTraceTree();
                    UpdateCrosshairOverlay();
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Pixel trace
    // -----------------------------------------------------------------------

    D2D1_SIZE_F MainWindow::PreviewViewportDips() const
    {
        // The preview renders at 96 DPI, so the D2D viewport matches WinUI DIPs.
        // Use the panel's actual size (which is in WinUI DIPs).
        auto panel = const_cast<MainWindow*>(this)->PreviewPanel();
        float w = static_cast<float>(panel.ActualWidth());
        float h = static_cast<float>(panel.ActualHeight());
        if (w <= 0) w = static_cast<float>(m_renderEngine.BackBufferWidth());
        if (h <= 0) h = static_cast<float>(m_renderEngine.BackBufferHeight());
        return { w, h };
    }

    D2D1_RECT_F MainWindow::GetPreviewImageBounds()
    {
        auto* previewImage = GetPreviewImage();
        if (!previewImage) return {};

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return {};

        // Get bounds at 96 DPI to match the preview rendering DPI.
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        D2D1_RECT_F bounds{};
        HRESULT hr = dc->GetImageLocalBounds(previewImage, &bounds);

        dc->SetDpi(oldDpiX, oldDpiY);
        if (FAILED(hr)) return {};
        return bounds;
    }

    bool MainWindow::PointerToImageCoords(
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args,
        float& outNormX, float& outNormY)
    {
        auto point = args.GetCurrentPoint(PreviewPanel());
        float px = static_cast<float>(point.Position().X);
        float py = static_cast<float>(point.Position().Y);

        // Invert the preview pan/zoom to get D2D image-space coordinates (context DIPs).
        float imgDipX = (px - m_previewPanX) / m_previewZoom;
        float imgDipY = (py - m_previewPanY) / m_previewZoom;

        // The image is drawn at identity in D2D's DIP space via the preview transform.
        // GetImageLocalBounds returns bounds in context DIPs (accounting for image DPI
        // vs context DPI). The source rect in ReadPixel also operates in context DIPs.
        // So we normalize by context DIP bounds.
        auto bounds = GetPreviewImageBounds();
        float imgW = bounds.right - bounds.left;
        float imgH = bounds.bottom - bounds.top;

        // For infinite/huge images, fall back to viewport dimensions.
        if (imgW <= 0 || imgH <= 0 || imgW > 100000.0f || imgH > 100000.0f)
        {
            auto vp = PreviewViewportDips();
            imgW = vp.width;
            imgH = vp.height;
            bounds = { 0, 0, imgW, imgH };
        }

        if (imgW <= 0 || imgH <= 0) return false;

        // Check if the click is outside the image bounds before clamping.
        float rawNormX = (imgDipX - bounds.left) / imgW;
        float rawNormY = (imgDipY - bounds.top) / imgH;
        m_traceOutOfBounds = (rawNormX < 0.0f || rawNormX > 1.0f ||
                              rawNormY < 0.0f || rawNormY > 1.0f);

        outNormX = std::clamp(rawNormX, 0.0f, 1.0f);
        outNormY = std::clamp(rawNormY, 0.0f, 1.0f);
        return true;
    }

    void MainWindow::OnPreviewPointerPressed(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        auto point = args.GetCurrentPoint(PreviewPanel());

        // Left or middle button → start pan
        if (point.Properties().IsMiddleButtonPressed() || point.Properties().IsLeftButtonPressed())
        {
            m_isPreviewPanning = true;
            m_previewPanStartX = static_cast<float>(point.Position().X);
            m_previewPanStartY = static_cast<float>(point.Position().Y);
            m_previewPanOriginX = m_previewPanX;
            m_previewPanOriginY = m_previewPanY;
            m_previewDragMoved = false;
            PreviewPanel().CapturePointer(args.Pointer());
            args.Handled(true);
        }
    }

    void MainWindow::OnTraceUnitSelectionChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /*args*/)
    {
        m_traceUnit = static_cast<uint32_t>(TraceUnitSelector().SelectedIndex());
        if (m_traceActive && m_pixelTrace.HasTrace())
            UpdatePixelTraceValues();
    }

    namespace
    {
        // Format pixel values based on the selected unit mode.
        std::wstring FormatPixelValues(
            const ::ShaderLab::Controls::InspectedPixel& p,
            uint32_t unit)
        {
            switch (unit)
            {
            case 0: // scRGB
                return std::format(L"R:{:.4f}  G:{:.4f}  B:{:.4f}  A:{:.4f}",
                    p.scR, p.scG, p.scB, p.scA);
            case 1: // sRGB
                return std::format(L"R:{}  G:{}  B:{}  A:{}",
                    p.sR, p.sG, p.sB, p.sA);
            case 2: // Nits (per-channel × 80)
                return std::format(L"R:{:.1f}  G:{:.1f}  B:{:.1f}",
                    p.scR * 80.0f, p.scG * 80.0f, p.scB * 80.0f);
            case 3: // PQ
                return std::format(L"R:{:.4f}  G:{:.4f}  B:{:.4f}",
                    p.pqR, p.pqG, p.pqB);
            default:
                return p.ScRGBString();
            }
        }

        // Simple topology hash from trace tree structure.
        uint32_t HashTraceTopology(const ::ShaderLab::Controls::PixelTraceNode& node)
        {
            uint32_t h = node.nodeId * 31 + node.inputPin;
            for (const auto& child : node.inputs)
                h = h * 37 + HashTraceTopology(child);
            return h;
        }
    }

    winrt::Microsoft::UI::Xaml::Controls::Grid MainWindow::CreateTraceRow(
        const ::ShaderLab::Controls::PixelTraceNode& traceNode)
    {
        auto row = winrt::Microsoft::UI::Xaml::Controls::Grid();
        row.ColumnDefinitions().Append(winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition());
        row.ColumnDefinitions().GetAt(0).Width({ 1, winrt::Microsoft::UI::Xaml::GridUnitType::Star });
        auto lumCol = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition();
        lumCol.Width({ 0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto });
        row.ColumnDefinitions().Append(lumCol);

        // Node label: "[pin] Name"
        auto label = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
        std::wstring labelText = traceNode.nodeName;
        if (!traceNode.pinName.empty())
            labelText = L"[" + traceNode.pinName + L"] " + labelText;

        // For analysis nodes: show field values instead of pixel values.
        std::wstring valuesText;
        if (traceNode.hasAnalysisOutput && !traceNode.analysisFields.empty())
        {
            valuesText = L"\n";
            for (const auto& fv : traceNode.analysisFields)
            {
                valuesText += fv.name + L": ";
                if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                {
                    uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    for (uint32_t c = 0; c < cc; ++c)
                    {
                        if (c > 0) valuesText += L"  ";
                        valuesText += std::format(L"{:.4f}", fv.components[c]);
                    }
                }
                else
                {
                    uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    uint32_t count = stride > 0 ? static_cast<uint32_t>(fv.arrayData.size()) / stride : 0;
                    valuesText += std::format(L"[{} elements]", count);
                }
                valuesText += L"\n";
            }
        }
        else
        {
            valuesText = L"\n" + FormatPixelValues(traceNode.pixel, m_traceUnit);
        }

        label.Text(labelText + valuesText);
        label.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Cascadia Mono, Consolas, Courier New"));
        label.FontSize(12);
        label.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::NoWrap);
        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(label, 0);
        row.Children().Append(label);

        // Luminance column (skip for analysis nodes).
        auto lumText = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
        if (traceNode.hasAnalysisOutput)
            lumText.Text(L"");
        else
            lumText.Text(std::format(L"{:.1f} cd/m\u00B2", traceNode.pixel.luminanceNits));
        lumText.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Cascadia Mono, Consolas, Courier New"));
        lumText.FontSize(12);
        lumText.Margin({ 8, 0, 0, 0 });
        lumText.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(lumText, 1);
        row.Children().Append(lumText);

        return row;
    }

    void MainWindow::UpdateCrosshairOverlay()
    {
        auto canvas = CrosshairCanvas();
        canvas.Children().Clear();

        // Clip the canvas to the preview panel bounds.
        auto clipGeom = winrt::Microsoft::UI::Xaml::Media::RectangleGeometry();
        clipGeom.Rect({ 0, 0,
            static_cast<float>(canvas.ActualWidth()),
            static_cast<float>(canvas.ActualHeight()) });
        canvas.Clip(clipGeom);

        // Only show crosshair when Pixel Trace tab is active.
        if (!m_traceActive || BottomTabView().SelectedIndex() != 1)
            return;

        // Compute the image-space point from the click position and the transform at click time,
        // then project it back to screen space using the CURRENT transform.
        // This makes the crosshair track 1:1 with the image during pan/zoom.
        float imgSpaceX = (m_traceClickDipX - m_traceClickPanX) / m_traceClickZoom;
        float imgSpaceY = (m_traceClickDipY - m_traceClickPanY) / m_traceClickZoom;
        float cx = imgSpaceX * m_previewZoom + m_previewPanX;
        float cy = imgSpaceY * m_previewZoom + m_previewPanY;
        float arm = 16.0;
        float gap = 4.0;

        auto makeLine = [&](float x1, float y1, float x2, float y2,
            winrt::Windows::UI::Color color, double thickness)
        {
            auto line = winrt::Microsoft::UI::Xaml::Shapes::Line();
            line.X1(x1); line.Y1(y1); line.X2(x2); line.Y2(y2);
            line.Stroke(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(color));
            line.StrokeThickness(thickness);
            canvas.Children().Append(line);
        };

        winrt::Windows::UI::Color black{ 230, 0, 0, 0 };
        winrt::Windows::UI::Color white{ 230, 255, 255, 255 };

        // Black outline.
        makeLine(cx - arm, cy, cx - gap, cy, black, 3.0);
        makeLine(cx + gap, cy, cx + arm, cy, black, 3.0);
        makeLine(cx, cy - arm, cx, cy - gap, black, 3.0);
        makeLine(cx, cy + gap, cx, cy + arm, black, 3.0);
        // White fill.
        makeLine(cx - arm, cy, cx - gap, cy, white, 1.5);
        makeLine(cx + gap, cy, cx + arm, cy, white, 1.5);
        makeLine(cx, cy - arm, cx, cy - gap, white, 1.5);
        makeLine(cx, cy + gap, cx, cy + arm, white, 1.5);
    }

    void MainWindow::PopulatePixelTraceTree()
    {
        if (!m_traceActive) return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        // Use actual image dimensions in context DIPs for pixel coordinate mapping.
        // DrawImage's sourceRectangle is in the same coordinate space as the D2D context.
        auto bounds = GetPreviewImageBounds();
        float imgW = bounds.right - bounds.left;
        float imgH = bounds.bottom - bounds.top;
        if (imgW <= 0 || imgH <= 0 || imgW > 100000.0f || imgH > 100000.0f)
        {
            auto vp = PreviewViewportDips();
            imgW = vp.width;
            imgH = vp.height;
        }
        uint32_t traceW = static_cast<uint32_t>(imgW);
        uint32_t traceH = static_cast<uint32_t>(imgH);
        if (traceW == 0 || traceH == 0) return;

        if (!m_pixelTrace.ReTrace(dc, m_graph, m_previewNodeId, traceW, traceH))
            return;

        auto topoHash = HashTraceTopology(m_pixelTrace.Root());
        if (topoHash == m_lastTraceTopologyHash)
        {
            // Topology unchanged — just update text values in place.
            UpdatePixelTraceValues();
            return;
        }

        m_lastTraceTopologyHash = topoHash;
        m_traceRowCache.clear();

        auto panel = PixelTracePanel();
        panel.Children().Clear();

        // Recursive lambda to build indented rows in a flat StackPanel.
        std::function<void(const ::ShaderLab::Controls::PixelTraceNode&, uint32_t)> buildRows;

        buildRows = [&](const ::ShaderLab::Controls::PixelTraceNode& traceNode, uint32_t depth)
        {
            auto rowGrid = CreateTraceRow(traceNode);
            rowGrid.Margin({ static_cast<double>(depth) * 20.0, 0, 0, 0 });
            m_traceRowCache.push_back(rowGrid);
            panel.Children().Append(rowGrid);

            for (const auto& child : traceNode.inputs)
                buildRows(child, depth + 1);
        };

        buildRows(m_pixelTrace.Root(), 0);

        // Show out-of-bounds warning if the selected pixel is outside the image.
        if (m_traceOutOfBounds)
        {
            auto warning = winrt::Microsoft::UI::Xaml::Controls::InfoBar();
            warning.IsOpen(true);
            warning.IsClosable(false);
            warning.Severity(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
            warning.Title(L"Outside image bounds");
            warning.Message(L"The selected pixel is outside the image area. Values shown are from the nearest edge pixel.");
            warning.Margin({ 0, 8, 0, 0 });
            panel.Children().Append(warning);
        }
    }

    void MainWindow::UpdatePixelTraceValues()
    {
        if (!m_pixelTrace.HasTrace()) return;

        // Walk the trace tree in the same order as the cache was built (pre-order).
        uint32_t idx = 0;
        std::function<void(const ::ShaderLab::Controls::PixelTraceNode&)> update;

        update = [&](const ::ShaderLab::Controls::PixelTraceNode& traceNode)
        {
            if (idx >= m_traceRowCache.size()) return;
            auto row = m_traceRowCache[idx++];

            // Update label text (child 0).
            auto label = row.Children().GetAt(0).as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>();
            std::wstring labelText = traceNode.nodeName;
            if (!traceNode.pinName.empty())
                labelText = L"[" + traceNode.pinName + L"] " + labelText;

            if (traceNode.hasAnalysisOutput && !traceNode.analysisFields.empty())
            {
                std::wstring valText = L"\n";
                for (const auto& fv : traceNode.analysisFields)
                {
                    valText += fv.name + L": ";
                    if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                    {
                        uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                        for (uint32_t c = 0; c < cc; ++c)
                        {
                            if (c > 0) valText += L"  ";
                            valText += std::format(L"{:.4f}", fv.components[c]);
                        }
                    }
                    else
                    {
                        uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                        uint32_t count = stride > 0 ? static_cast<uint32_t>(fv.arrayData.size()) / stride : 0;
                        valText += std::format(L"[{} elements]", count);
                    }
                    valText += L"\n";
                }
                label.Text(labelText + valText);
            }
            else
            {
                label.Text(labelText + L"\n" + FormatPixelValues(traceNode.pixel, m_traceUnit));
            }

            // Update luminance (child 1).
            auto lumText = row.Children().GetAt(1).as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>();
            if (traceNode.hasAnalysisOutput)
                lumText.Text(L"");
            else
                lumText.Text(std::format(L"{:.1f} cd/m\u00B2", traceNode.pixel.luminanceNits));

            for (const auto& child : traceNode.inputs)
                update(child);
        };

        update(m_pixelTrace.Root());
    }

    // -----------------------------------------------------------------------
    // Cursor readout
    // -----------------------------------------------------------------------

    void MainWindow::OnPreviewPointerMoved(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!m_renderEngine.IsInitialized()) return;

        // Handle preview pan.
        if (m_isPreviewPanning)
        {
            auto point = args.GetCurrentPoint(PreviewPanel());
            float px = static_cast<float>(point.Position().X);
            float py = static_cast<float>(point.Position().Y);
            float dx = px - m_previewPanStartX;
            float dy = py - m_previewPanStartY;
            if (std::abs(dx) > 5.0f || std::abs(dy) > 5.0f)
                m_previewDragMoved = true;
            m_previewPanX = m_previewPanOriginX + dx;
            m_previewPanY = m_previewPanOriginY + dy;
            m_forceRender = true;
            args.Handled(true);
            return;
        }

        auto point = args.GetCurrentPoint(PreviewPanel());
        // Status-bar pixel-readout was removed (it forced a per-mouse-move
        // GPU readback for a single-pixel display that crowded out the FPS
        // counter at low frame rates). The Pixel Trace tab is the
        // canonical place for cursor-coordinate inspection now.
        (void)point;
    }

    // -----------------------------------------------------------------------
    // Node graph editor rendering
    // -----------------------------------------------------------------------

    void MainWindow::InitializeGraphPanel()
    {
        if (!m_renderEngine.DXGIFactory() || !m_renderEngine.D3DDevice())
            return;

        auto panel = NodeGraphPanel();
        m_graphPanelDipsWidth = (std::max)(1.0f, static_cast<float>(panel.ActualWidth()));
        m_graphPanelDipsHeight = (std::max)(1.0f, static_cast<float>(panel.ActualHeight()));
        if (m_graphPanelDipsWidth <= 1.0f) m_graphPanelDipsWidth = 400.0f;
        if (m_graphPanelDipsHeight <= 1.0f) m_graphPanelDipsHeight = 300.0f;

        float scaleX = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleY()));
        m_graphPanelWidth = static_cast<uint32_t>((std::max)(1.0f, std::ceil(m_graphPanelDipsWidth * scaleX)));
        m_graphPanelHeight = static_cast<uint32_t>((std::max)(1.0f, std::ceil(m_graphPanelDipsHeight * scaleY)));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = m_graphPanelWidth;
        desc.Height = m_graphPanelHeight;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        auto factory = m_renderEngine.DXGIFactory();
        auto* d3dDevice = m_renderEngine.D3DDevice();

        winrt::com_ptr<IDXGIDevice1> dxgiDevice;
        d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));

        HRESULT hr = factory->CreateSwapChainForComposition(
            dxgiDevice.get(), &desc, nullptr, m_graphSwapChain.put());

        if (FAILED(hr)) return;

        // Bind swap chain to the panel.
        auto panelNative = panel.as<ISwapChainPanelNative>();
        panelNative->SetSwapChain(m_graphSwapChain.get());
        UpdateGraphPanelScale();

        // Create render target.
        auto* dc = m_renderEngine.D2DDeviceContext();
        winrt::com_ptr<IDXGISurface> surface;
        m_graphSwapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_graphRenderTarget.put());

        m_nodeGraphController.SetViewportSize(
            m_graphPanelDipsWidth,
            m_graphPanelDipsHeight);
    }

    void MainWindow::ResizeGraphPanel(float widthDips, float heightDips)
    {
        auto panel = NodeGraphPanel();
        m_graphPanelDipsWidth = (std::max)(1.0f, widthDips);
        m_graphPanelDipsHeight = (std::max)(1.0f, heightDips);

        float scaleX = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleY()));
        auto w = static_cast<uint32_t>((std::max)(1.0f, std::ceil(m_graphPanelDipsWidth * scaleX)));
        auto h = static_cast<uint32_t>((std::max)(1.0f, std::ceil(m_graphPanelDipsHeight * scaleY)));

        m_nodeGraphController.SetViewportSize(m_graphPanelDipsWidth, m_graphPanelDipsHeight);

        if (!m_graphSwapChain || (w == m_graphPanelWidth && h == m_graphPanelHeight))
        {
            m_nodeGraphController.SetNeedsRedraw();
            return;
        }

        m_graphPanelWidth = w;
        m_graphPanelHeight = h;

        m_graphRenderTarget = nullptr;
        m_graphSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        winrt::com_ptr<IDXGISurface> surface;
        m_graphSwapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_graphRenderTarget.put());

        m_nodeGraphController.SetNeedsRedraw();
    }

    void MainWindow::UpdateGraphPanelScale()
    {
        if (!m_graphSwapChain)
            return;

        winrt::com_ptr<IDXGISwapChain2> swapChain2;
        if (FAILED(m_graphSwapChain->QueryInterface(IID_PPV_ARGS(swapChain2.put()))))
            return;

        auto panel = NodeGraphPanel();
        float scaleX = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleY()));

        DXGI_MATRIX_3X2_F matrix{};
        matrix._11 = 1.0f / scaleX;
        matrix._22 = 1.0f / scaleY;
        swapChain2->SetMatrixTransform(&matrix);
    }

    void MainWindow::InitializeTraceSwatchPanel()
    {
        if (m_traceSwapChain || !m_renderEngine.DXGIFactory() || !m_renderEngine.D3DDevice())
            return;

        auto panel = TraceSwatchPanel();
        float scaleX = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleY()));
        auto width = static_cast<uint32_t>((std::max)(1.0f, std::ceil(static_cast<float>(panel.ActualWidth()) * scaleX)));
        auto height = static_cast<uint32_t>((std::max)(1.0f, std::ceil(static_cast<float>(panel.ActualHeight()) * scaleY)));
        if (width == 0) width = static_cast<uint32_t>(std::ceil(28.0f * scaleX));
        if (height == 0) height = static_cast<uint32_t>(std::ceil(600.0f * scaleY));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        winrt::com_ptr<IDXGIDevice1> dxgiDevice;
        m_renderEngine.D3DDevice()->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));

        HRESULT hr = m_renderEngine.DXGIFactory()->CreateSwapChainForComposition(
            dxgiDevice.get(), &desc, nullptr, m_traceSwapChain.put());
        if (FAILED(hr)) return;

        auto panelNative = panel.as<ISwapChainPanelNative>();
        panelNative->SetSwapChain(m_traceSwapChain.get());
        UpdateTraceSwatchPanelScale();

        // Set scRGB color space for HDR rendering.
        winrt::com_ptr<IDXGISwapChain3> sc3;
        m_traceSwapChain.as(sc3);
        if (sc3)
            sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);

        m_traceSwatchHeight = height;

        auto* dc = m_renderEngine.D2DDeviceContext();
        winrt::com_ptr<IDXGISurface> surface;
        m_traceSwapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_traceSwatchTarget.put());
    }

    void MainWindow::ResizeTraceSwatchPanel()
    {
        if (!m_traceSwapChain)
            return;

        auto panel = TraceSwatchPanel();
        float scaleX = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleY()));
        auto width = static_cast<uint32_t>((std::max)(1.0f, std::ceil(static_cast<float>(panel.ActualWidth()) * scaleX)));
        auto height = static_cast<uint32_t>((std::max)(1.0f, std::ceil(static_cast<float>(panel.ActualHeight()) * scaleY)));

        if (height == m_traceSwatchHeight)
            return;

        m_traceSwatchHeight = height;
        m_traceSwatchTarget = nullptr;
        m_traceSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        winrt::com_ptr<IDXGISurface> surface;
        m_traceSwapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_traceSwatchTarget.put());
    }

    void MainWindow::UpdateTraceSwatchPanelScale()
    {
        if (!m_traceSwapChain)
            return;

        winrt::com_ptr<IDXGISwapChain2> swapChain2;
        if (FAILED(m_traceSwapChain->QueryInterface(IID_PPV_ARGS(swapChain2.put()))))
            return;

        auto panel = TraceSwatchPanel();
        float scaleX = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleY()));

        DXGI_MATRIX_3X2_F matrix{};
        matrix._11 = 1.0f / scaleX;
        matrix._22 = 1.0f / scaleY;
        swapChain2->SetMatrixTransform(&matrix);
    }

    void MainWindow::RenderTraceSwatches()
    {
        if (!m_traceActive || !m_pixelTrace.HasTrace())
            return;

        if (!m_traceSwapChain)
            InitializeTraceSwatchPanel();
        if (!m_traceSwapChain || !m_traceSwatchTarget)
            return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());

        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        auto traceDpiX = 96.0f * (std::max)(1.0f, static_cast<float>(TraceSwatchPanel().CompositionScaleX()));
        auto traceDpiY = 96.0f * (std::max)(1.0f, static_cast<float>(TraceSwatchPanel().CompositionScaleY()));
        dc->SetDpi(traceDpiX, traceDpiY);

        dc->SetTarget(m_traceSwatchTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        // Walk the trace tree and render swatches at approximate row positions.
        float y = 4.0f;
        constexpr float swatchSize = 20.0f;
        constexpr float rowHeight = 40.0f;

        std::function<void(const ::ShaderLab::Controls::PixelTraceNode&, int)> drawSwatches;
        drawSwatches = [&](const ::ShaderLab::Controls::PixelTraceNode& node, int depth)
        {
            // scRGB color: 1.0 = 80 nits SDR white. Values > 1.0 = HDR.
            D2D1_COLOR_F color = {
                node.pixel.scR, node.pixel.scG, node.pixel.scB, 1.0f
            };

            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(color, brush.put());
            if (brush)
            {
                D2D1_ROUNDED_RECT rr = {
                    { 4.0f, y, 4.0f + swatchSize, y + swatchSize },
                    3.0f, 3.0f
                };
                dc->FillRoundedRectangle(rr, brush.get());
            }
            y += rowHeight;

            for (const auto& child : node.inputs)
                drawSwatches(child, depth + 1);
        };

        drawSwatches(m_pixelTrace.Root(), 0);

        dc->EndDraw();
        dc->SetTarget(oldTarget.get());
        dc->SetDpi(oldDpiX, oldDpiY);

        m_traceSwapChain->Present(0, 0);
    }

    void MainWindow::RenderGraphScene(ID2D1DeviceContext5* dc, D2D1_SIZE_F viewSize)
    {
        if (!dc) return;

        dc->Clear(D2D1::ColorF(0x1A1A1E));

        // Draw subtle dot grid for spatial reference.
        {
            auto pan = m_nodeGraphController.PanOffset();
            float zoom = m_nodeGraphController.Zoom();
            float gridSpacing = 24.0f * zoom;
            if (gridSpacing > 8.0f)
            {
                if (!m_graphGridBrush)
                    dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.06f), m_graphGridBrush.put());
                if (m_graphGridBrush)
                {
                    float startX = fmodf(pan.x, gridSpacing);
                    float startY = fmodf(pan.y, gridSpacing);
                    if (startX < 0) startX += gridSpacing;
                    if (startY < 0) startY += gridSpacing;
                    for (float y = startY; y < viewSize.height; y += gridSpacing)
                        for (float x = startX; x < viewSize.width; x += gridSpacing)
                            dc->FillEllipse({ { x, y }, 1.0f, 1.0f }, m_graphGridBrush.get());
                }
            }
        }

        m_nodeGraphController.Render(dc, viewSize);
    }

    void MainWindow::RenderNodeGraph()
    {
        if (!m_graphSwapChain || !m_graphRenderTarget)
            return;

        // Skip redraw if nothing changed in the graph canvas.
        if (!m_nodeGraphController.NeedsRedraw())
            return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());

        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        float graphDpiX = 96.0f * (std::max)(1.0f, static_cast<float>(NodeGraphPanel().CompositionScaleX()));
        float graphDpiY = 96.0f * (std::max)(1.0f, static_cast<float>(NodeGraphPanel().CompositionScaleY()));
        dc->SetDpi(graphDpiX, graphDpiY);

        dc->SetTarget(m_graphRenderTarget.get());
        dc->BeginDraw();

        D2D1_SIZE_F viewSize = { m_graphPanelDipsWidth, m_graphPanelDipsHeight };
        RenderGraphScene(dc, viewSize);

        dc->EndDraw();
        dc->SetTarget(oldTarget.get());

        dc->SetDpi(oldDpiX, oldDpiY);

        m_graphSwapChain->Present(0, 0);
    }

    D2D1_POINT_2F MainWindow::GraphPanelPointerToCanvas(
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        // Convert pointer DIP position to canvas coordinates
        // by inverting the pan/zoom transform.
        auto point = args.GetCurrentPoint(NodeGraphContainer());
        float sx = static_cast<float>(point.Position().X);
        float sy = static_cast<float>(point.Position().Y);
        auto pan = m_nodeGraphController.PanOffset();
        float zoom = m_nodeGraphController.Zoom();
        return { (sx - pan.x) / zoom, (sy - pan.y) / zoom };
    }

    void MainWindow::OnGraphPanelPointerPressed(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        auto canvasPoint = GraphPanelPointerToCanvas(args);
        auto point = args.GetCurrentPoint(NodeGraphContainer());

        // Right-click or middle-click: start graph canvas panning.
        if (point.Properties().IsRightButtonPressed() || point.Properties().IsMiddleButtonPressed())
        {
            m_isGraphPanning = true;
            m_graphPanStart = { static_cast<float>(point.Position().X),
                                static_cast<float>(point.Position().Y) };
            m_graphPanOrigin = m_nodeGraphController.PanOffset();
            NodeGraphContainer().CapturePointer(args.Pointer());
            args.Handled(true);
            return;
        }

        OutputDebugStringW(std::format(L"[GraphClick] canvas=({:.1f},{:.1f}) visuals={}\n",
            canvasPoint.x, canvasPoint.y, m_nodeGraphController.SelectedNodes().size()).c_str());

        // Debug: dump all node positions
        for (const auto& node : m_graph.Nodes())
        {
            OutputDebugStringW(std::format(L"  node {} '{}' pos=({:.0f},{:.0f})\n",
                node.id, node.name, node.position.x, node.position.y).c_str());
        }

        // Check for play button hit on clock nodes.
        uint32_t playNodeId = m_nodeGraphController.HitTestPlayButton(canvasPoint);
        if (playNodeId != 0)
        {
            auto* node = m_graph.FindNode(playNodeId);
            if (node)
            {
                node->isPlaying = !node->isPlaying;
                m_nodeGraphController.SetNeedsRedraw();
                m_nodeLogs[playNodeId].Info(node->isPlaying ? L"Clock started" : L"Clock paused");
            }
            args.Handled(true);
            return;
        }

        // Check for inline slider hit on parameter nodes.
        uint32_t sliderNodeId = m_nodeGraphController.HitTestSlider(canvasPoint);
        if (sliderNodeId != 0)
        {
            m_isDraggingSlider = true;
            m_sliderDragNodeId = sliderNodeId;
            m_nodeGraphController.UpdateSliderDrag(sliderNodeId, canvasPoint);
            m_selectedNodeId = sliderNodeId;
            m_graph.MarkAllDirty();
            m_forceRender = true;
            UpdatePropertiesPanel();
            NodeGraphContainer().CapturePointer(args.Pointer());
            args.Handled(true);
            return;
        }

        // Check for pin hit first (start connection drag).
        uint32_t pinNodeId = 0;
        uint32_t pinIndex = 0;
        bool isOutput = false;
        bool isDataPin = false;
        if (m_nodeGraphController.HitTestPin(canvasPoint, pinNodeId, pinIndex, isOutput, isDataPin))
        {
            m_nodeGraphController.BeginConnection(pinNodeId, pinIndex, isOutput, isDataPin);
            m_isDraggingConnection = true;
            NodeGraphContainer().CapturePointer(args.Pointer());
            args.Handled(true);
            return;
        }

        // Alt+Click on an edge (image or data binding) removes that edge.
        {
            auto altState = winrt::Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(
                winrt::Windows::System::VirtualKey::Menu); // Menu == Alt
            bool altDown = (static_cast<uint32_t>(altState) &
                static_cast<uint32_t>(winrt::Windows::UI::Core::CoreVirtualKeyStates::Down)) != 0;
            if (altDown)
            {
                auto hit = m_nodeGraphController.HitTestEdge(canvasPoint, 8.0f);
                if (hit.found && m_nodeGraphController.RemoveEdge(hit))
                {
                    m_graph.MarkAllDirty();
                    m_nodeGraphController.RebuildLayout();
                    m_forceRender = true;
                    UpdatePropertiesPanel();
                    MarkUnsaved();
                    args.Handled(true);
                    return;
                }
            }
        }

        // Check for node hit (select + start drag).
        uint32_t hitNodeId = m_nodeGraphController.HitTestNode(canvasPoint);
        if (hitNodeId != 0)
        {
            // Check if Shift is held for multi-select.
            auto shiftState = winrt::Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(
                winrt::Windows::System::VirtualKey::Shift);
            bool shiftDown = (static_cast<uint32_t>(shiftState) & static_cast<uint32_t>(winrt::Windows::UI::Core::CoreVirtualKeyStates::Down)) != 0;

            if (shiftDown)
            {
                auto& sel = m_nodeGraphController.SelectedNodes();
                if (sel.count(hitNodeId))
                {
                    // Already selected — just start dragging the group, don't toggle.
                }
                else
                {
                    // Add to selection.
                    m_nodeGraphController.SelectNode(hitNodeId, /*addToSelection*/ true);
                }
            }
            else
            {
                // Without Shift: clicking a node in an existing multi-selection
                // keeps the group (for dragging). Clicking outside the selection
                // replaces it with the clicked node.
                auto& sel = m_nodeGraphController.SelectedNodes();
                if (!sel.count(hitNodeId))
                    m_nodeGraphController.SelectNode(hitNodeId);
                // else: already selected, keep group intact for drag.
            }

            m_selectedNodeId = hitNodeId;
            m_nodeGraphController.BeginDragNodes(canvasPoint);
            m_isDraggingNode = true;
            NodeGraphContainer().CapturePointer(args.Pointer());
            UpdatePropertiesPanel();
            BottomTabView().SelectedIndex(0);
            NodeGraphContainer().Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);

            // Preview the clicked node (skip parameter, data-only, and histogram effects).
            const auto* clickedNode = m_graph.FindNode(hitNodeId);
            bool isAnalysisEffect = clickedNode &&
                clickedNode->effectClsid.has_value() &&
                IsEqualGUID(clickedNode->effectClsid.value(), CLSID_D2D1Histogram);
            bool isParamNode = m_nodeGraphController.IsParameterNode(hitNodeId);
            bool isDataOnly = clickedNode && clickedNode->outputPins.empty();

            if (!isAnalysisEffect && !isParamNode && !isDataOnly)
                m_previewNodeId = hitNodeId;

            m_forceRender = true;
            // Defer the fit until the next eval populates cachedOutput. On
            // the very first selection of a node (before its first eval),
            // GetPreviewImageBounds() returns an empty rect, so an immediate
            // FitPreviewToView() lands on the wrong zoom. The deferred path
            // in OnRenderTick re-fits once bounds are available.
            m_needsFitPreview = true;
            UpdatePreviewOverlay();
        }
        else
        {
            // Clicked empty space — deselect, keep current preview.
            m_nodeGraphController.DeselectAll();
            m_selectedNodeId = 0;
            UpdatePropertiesPanel();
        }
        args.Handled(true);
    }

    void MainWindow::OnGraphPanelPointerMoved(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isGraphPanning)
        {
            auto point = args.GetCurrentPoint(NodeGraphContainer());
            float dx = static_cast<float>(point.Position().X) - m_graphPanStart.x;
            float dy = static_cast<float>(point.Position().Y) - m_graphPanStart.y;
            m_nodeGraphController.SetPanOffset(m_graphPanOrigin.x + dx, m_graphPanOrigin.y + dy);
            args.Handled(true);
            return;
        }

        auto canvasPoint = GraphPanelPointerToCanvas(args);

        if (m_isDraggingSlider)
        {
            if (m_nodeGraphController.UpdateSliderDrag(m_sliderDragNodeId, canvasPoint))
            {
                m_graph.MarkAllDirty();
                m_forceRender = true;
                UpdatePropertiesPanel();
            }
            args.Handled(true);
        }
        else if (m_isDraggingNode)
        {
            m_nodeGraphController.UpdateDragNodes(canvasPoint);
            args.Handled(true);
        }
        else if (m_isDraggingConnection)
        {
            m_nodeGraphController.UpdateConnection(canvasPoint);
            args.Handled(true);
        }
    }

    void MainWindow::OnGraphPanelPointerReleased(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isGraphPanning)
        {
            m_isGraphPanning = false;
            NodeGraphContainer().ReleasePointerCapture(args.Pointer());
            args.Handled(true);
            return;
        }

        if (m_isDraggingSlider)
        {
            m_isDraggingSlider = false;
            m_sliderDragNodeId = 0;
            NodeGraphContainer().ReleasePointerCapture(args.Pointer());
            args.Handled(true);
            return;
        }

        if (m_isDraggingNode)
        {
            m_nodeGraphController.EndDragNodes();
            m_isDraggingNode = false;
            NodeGraphContainer().ReleasePointerCapture(args.Pointer());
        }
        else if (m_isDraggingConnection)
        {
            auto canvasPoint = GraphPanelPointerToCanvas(args);
            bool connected = m_nodeGraphController.EndConnection(canvasPoint);
            m_isDraggingConnection = false;
            NodeGraphContainer().ReleasePointerCapture(args.Pointer());
            if (connected)
            {
                m_graph.MarkAllDirty();
                PopulatePreviewNodeSelector();
            }
        }
        args.Handled(true);
    }

    void MainWindow::OnGraphPanelPointerWheel(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        auto point = args.GetCurrentPoint(NodeGraphContainer());
        int32_t delta = point.Properties().MouseWheelDelta();

        float oldZoom = m_nodeGraphController.Zoom();
        float factor = (delta > 0) ? 1.1f : 0.9f;
        float newZoom = (std::clamp)(oldZoom * factor, 0.2f, 3.0f);

        // Zoom toward the pointer position.
        float px = static_cast<float>(point.Position().X);
        float py = static_cast<float>(point.Position().Y);
        auto pan = m_nodeGraphController.PanOffset();
        float newPanX = px - (px - pan.x) * (newZoom / oldZoom);
        float newPanY = py - (py - pan.y) * (newZoom / oldZoom);

        m_nodeGraphController.SetZoom(newZoom);
        m_nodeGraphController.SetPanOffset(newPanX, newPanY);
        args.Handled(true);
    }

    bool MainWindow::IsPropertiesPanelInteracting()
    {
        // Walks the focused element's parent chain looking for the
        // PropertiesPanel StackPanel. If found, the user is mid-edit on
        // some control inside the panel and a Clear() + recreate would
        // destroy their in-progress input. Used by the 4 Hz binding-value
        // refresh on Clock-driven graphs.
        try
        {
            auto root = this->Content().XamlRoot();
            if (!root) return false;
            auto focused = winrt::Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(root);
            auto cur = focused.try_as<winrt::Microsoft::UI::Xaml::DependencyObject>();
            auto target = PropertiesPanel().try_as<winrt::Microsoft::UI::Xaml::DependencyObject>();
            if (!cur || !target) return false;
            for (int hops = 0; hops < 64 && cur; ++hops)
            {
                if (cur == target) return true;
                cur = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(cur);
            }
        }
        catch (...) {}
        return false;
    }

    void MainWindow::UpdatePropertiesPanel()
    {
        namespace Controls = winrt::Microsoft::UI::Xaml::Controls;
        namespace Media = winrt::Microsoft::UI::Xaml::Media;
        using ::ShaderLab::Effects::PropertyUIHint;
        using ::ShaderLab::Effects::PropertyMetadata;

        auto panel = PropertiesPanel();
        panel.Children().Clear();

        // Clear per-tick video slider references (will be re-set if video node selected).
        m_videoSeekSlider = nullptr;
        m_videoPositionLabel = nullptr;
        m_videoSeekNodeId = 0;

        if (m_selectedNodeId == 0)
        {
            auto placeholder = Controls::TextBlock();
            placeholder.Text(L"Select a node to view its properties.");
            placeholder.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
            panel.Children().Append(placeholder);
            return;
        }

        auto* node = m_graph.FindNode(m_selectedNodeId);
        if (!node) return;

        uint32_t capturedId = m_selectedNodeId;

        // ---- Node name (editable) ----
        auto nameLabel = Controls::TextBlock();
        nameLabel.Text(L"Name");
        nameLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        panel.Children().Append(nameLabel);

        auto nameBox = Controls::TextBox();
        nameBox.Text(winrt::hstring(node->name));
        nameBox.Margin({ 0, 2, 0, 8 });
        nameBox.LostFocus([this, capturedId](auto&&, auto&&)
        {
            auto* n = m_graph.FindNode(capturedId);
            if (!n) return;
            PopulatePreviewNodeSelector();
            m_nodeGraphController.RebuildLayout();
        });
        nameBox.TextChanged([this, capturedId](auto&&, auto&&)
        {
            auto* n = m_graph.FindNode(capturedId);
            if (!n) return;
            auto p = PropertiesPanel();
            if (p.Children().Size() > 1)
            {
                auto tb = p.Children().GetAt(1).try_as<Controls::TextBox>();
                if (tb) n->name = std::wstring(tb.Text().c_str());
            }
        });
        panel.Children().Append(nameBox);

        // ---- Node type (read-only) ----
        auto typeText = Controls::TextBlock();
        std::wstring typeStr;
        switch (node->type)
        {
        case ::ShaderLab::Graph::NodeType::Source:         typeStr = L"Source"; break;
        case ::ShaderLab::Graph::NodeType::BuiltInEffect:  typeStr = L"Built-in Effect"; break;
        case ::ShaderLab::Graph::NodeType::PixelShader:    typeStr = L"Custom Pixel Shader"; break;
        case ::ShaderLab::Graph::NodeType::ComputeShader:  typeStr = L"Custom Compute Shader"; break;
        case ::ShaderLab::Graph::NodeType::Output:         typeStr = L"Output"; break;
        }
        typeText.Text(L"Type: " + typeStr);
        typeText.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
        typeText.Margin({ 0, 0, 0, 8 });
        panel.Children().Append(typeText);

        // ---- Custom effect: "Edit in Designer" button ----
        // Hide for parameter nodes (no HLSL) and output nodes.
        if ((node->type == ::ShaderLab::Graph::NodeType::PixelShader ||
             node->type == ::ShaderLab::Graph::NodeType::ComputeShader) &&
            node->customEffect.has_value() &&
            !node->customEffect->hlslSource.empty())
        {
            auto editBtn = Controls::Button();
            editBtn.Content(winrt::box_value(L"\xE70F  Edit in Effect Designer"));
            editBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            editBtn.Margin({ 0, 0, 0, 8 });
            editBtn.Click([this, capturedId](auto&&, auto&&)
            {
                auto* n = m_graph.FindNode(capturedId);
                if (!n || !n->customEffect.has_value()) return;

                OpenEffectDesigner();

                auto designerImpl = winrt::get_self<
                    winrt::ShaderLab::implementation::EffectDesignerWindow>(m_designerWindow);
                designerImpl->LoadDefinition(capturedId, n->customEffect.value(), n->name);
            });
            panel.Children().Append(editBtn);
        }

        // ---- Runtime error display ----
        if (!node->runtimeError.empty())
        {
            auto errorBorder = Controls::Border();
            errorBorder.Background(Media::SolidColorBrush(winrt::Windows::UI::Color{ 255, 180, 30, 30 }));
            errorBorder.CornerRadius({ 4, 4, 4, 4 });
            errorBorder.Padding({ 8, 6, 8, 6 });
            errorBorder.Margin({ 0, 0, 0, 8 });

            auto errorText = Controls::TextBlock();
            errorText.Text(winrt::hstring(node->runtimeError));
            errorText.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::White()));
            errorText.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
            errorText.FontSize(12);
            errorBorder.Child(errorText);
            panel.Children().Append(errorBorder);

            // Add "Reload" button for video sources with errors.
            {
                auto isVideoIt = node->properties.find(L"IsVideo");
                bool hasVideo = false;
                if (isVideoIt != node->properties.end())
                    if (auto* bv = std::get_if<bool>(&isVideoIt->second)) hasVideo = *bv;
                if (hasVideo && node->shaderPath.has_value())
                {
                    auto reloadBtn = Controls::Button();
                    reloadBtn.Content(winrt::box_value(L"\xE72C  Reload Video"));
                    reloadBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                    reloadBtn.Margin({ 0, 0, 0, 8 });
                    uint32_t reloadNodeId = capturedId;
                    reloadBtn.Click([this, reloadNodeId](auto&&, auto&&) {
                        auto* n = m_graph.FindNode(reloadNodeId);
                        if (!n) return;
                        n->runtimeError.clear();
                        n->dirty = true;
                        m_forceRender = true;
                        UpdatePropertiesPanel();
                    });
                    panel.Children().Append(reloadBtn);
                }
            }
        }

        // ---- Image source: file path + Browse button ----
        bool isVideoSource = false;
        bool isLiveCaptureSource = false;
        {
            auto isVideoIt = node->properties.find(L"IsVideo");
            if (isVideoIt != node->properties.end())
            {
                auto* bv = std::get_if<bool>(&isVideoIt->second);
                if (bv && *bv) isVideoSource = true;
            }
            // DXGI Desktop Duplication / Windows Graphics Capture sources
            // own their bitmap from a live capture provider; they have no
            // file path to browse to and the "Image Path" UI is misleading.
            for (const auto* key : { L"IsDxgiDuplicateOutput", L"IsWindowsGraphicsCapture" })
            {
                auto it = node->properties.find(key);
                if (it != node->properties.end())
                {
                    auto* bv = std::get_if<bool>(&it->second);
                    if (bv && *bv) { isLiveCaptureSource = true; break; }
                }
            }
        }

        if (node->type == ::ShaderLab::Graph::NodeType::Source &&
            !(node->effectClsid.has_value()) &&
            !isVideoSource && !isLiveCaptureSource)
        {
            auto pathLabel = Controls::TextBlock();
            pathLabel.Text(L"Image Path");
            pathLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            pathLabel.Margin({ 0, 4, 0, 2 });
            panel.Children().Append(pathLabel);

            auto pathText = Controls::TextBlock();
            pathText.Text(node->shaderPath.has_value() && !node->shaderPath.value().empty()
                ? winrt::hstring(node->shaderPath.value())
                : L"(no file selected)");
            pathText.FontSize(12);
            pathText.IsTextSelectionEnabled(true);
            pathText.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
            pathText.Foreground(Media::SolidColorBrush(
                node->shaderPath.has_value() && !node->shaderPath.value().empty()
                    ? winrt::Microsoft::UI::Colors::White()
                    : winrt::Microsoft::UI::Colors::Gray()));
            pathText.Margin({ 0, 0, 0, 4 });
            panel.Children().Append(pathText);

            auto browseBtn = Controls::Button();
            browseBtn.Content(winrt::box_value(L"Browse..."));
            browseBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            browseBtn.Margin({ 0, 0, 0, 8 });
            browseBtn.Click([this, capturedId](auto&&, auto&&)
            {
                BrowseImageForSourceNode(capturedId);
            });
            panel.Children().Append(browseBtn);
        }

        // ---- Video source: playback controls ----
        if (isVideoSource)
        {
            auto* videoProvider = m_sourceFactory.GetVideoProvider(capturedId);

            auto pathLabel = Controls::TextBlock();
            pathLabel.Text(L"Video File");
            pathLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            pathLabel.Margin({ 0, 4, 0, 2 });
            panel.Children().Append(pathLabel);

            auto pathText = Controls::TextBlock();
            pathText.Text(node->shaderPath.has_value() && !node->shaderPath.value().empty()
                ? winrt::hstring(std::filesystem::path(node->shaderPath.value()).filename().wstring())
                : L"(no file)");
            pathText.FontSize(12);
            pathText.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
            pathText.Margin({ 0, 0, 0, 2 });
            panel.Children().Append(pathText);

            // Browse button to change video file.
            auto browseBtn = Controls::Button();
            browseBtn.Content(winrt::box_value(L"Browse..."));
            browseBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            browseBtn.Margin({ 0, 0, 0, 6 });
            browseBtn.Click([this, capturedId](auto&&, auto&&)
            {
                BrowseVideoForExistingNode(capturedId);
            });
            panel.Children().Append(browseBtn);

            if (videoProvider && videoProvider->IsOpen())
            {
                // Video info line.
                auto infoText = Controls::TextBlock();
                infoText.Text(std::format(L"{}x{} - {:.1f} fps - {:.1f}s",
                    videoProvider->FrameWidth(), videoProvider->FrameHeight(),
                    videoProvider->FrameRate(), videoProvider->Duration()));
                infoText.FontSize(11);
                infoText.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
                infoText.Margin({ 0, 0, 0, 6 });
                panel.Children().Append(infoText);

                // Loop toggle.
                auto loopToggle = Controls::ToggleSwitch();
                loopToggle.Header(winrt::box_value(L"Loop"));
                loopToggle.IsOn(videoProvider->IsLooping());
                loopToggle.Margin({ 0, 4, 0, 8 });
                loopToggle.Toggled([this, capturedId](auto&& sender, auto&&)
                {
                    auto toggle = sender.as<Controls::ToggleSwitch>();
                    auto* node = m_graph.FindNode(capturedId);
                    if (node)
                        node->properties[L"Loop"] = toggle.IsOn();
                });
                panel.Children().Append(loopToggle);
            }
        }

        // ---- Look up metadata from registry ----
        const ::ShaderLab::Effects::EffectDescriptor* desc = nullptr;
        if (node->type == ::ShaderLab::Graph::NodeType::BuiltInEffect && node->effectClsid.has_value())
            desc = ::ShaderLab::Effects::EffectRegistry::Instance().FindByClsid(node->effectClsid.value());

        // Build metadata map for custom effect parameters (min/max/step from ParameterDefinition).
        std::map<std::wstring, PropertyMetadata> customMeta;
        if (node->customEffect.has_value())
        {
            for (const auto& p : node->customEffect->parameters)
            {
                PropertyMetadata pm;
                if (!p.enumLabels.empty())
                {
                    pm.uiHint = PropertyUIHint::ComboBox;
                    pm.enumLabels = p.enumLabels;
                }
                else
                {
                    pm.uiHint = PropertyUIHint::Slider;
                }
                pm.minValue = p.minValue;
                pm.maxValue = p.maxValue;
                pm.step = p.step;
                customMeta[p.name] = pm;
            }
        }

        // ---- Editable properties ----
        if (node->properties.empty())
        {
            auto noProps = Controls::TextBlock();
            noProps.Text(L"No editable properties.");
            noProps.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
            noProps.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            noProps.Margin({ 0, 4, 0, 0 });
            panel.Children().Append(noProps);
        }
        else
        {
            auto propsHeader = Controls::TextBlock();
            propsHeader.Text(L"Properties");
            propsHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            propsHeader.Margin({ 0, 4, 0, 4 });
            panel.Children().Append(propsHeader);

            // ---- Numeric Expression node: dedicated Expression editor + dynamic inputs ----
            const bool isMathExpression = node->customEffect.has_value() &&
                node->customEffect->shaderLabEffectId == L"Math Expression";
            if (isMathExpression)
            {
                auto exprLabel = Controls::TextBlock();
                exprLabel.Text(L"Expression");
                exprLabel.FontSize(12);
                exprLabel.Margin({ 0, 6, 0, 2 });
                panel.Children().Append(exprLabel);

                std::wstring exprText;
                auto eIt = node->properties.find(L"Expression");
                if (eIt != node->properties.end())
                    if (auto* s = std::get_if<std::wstring>(&eIt->second)) exprText = *s;

                auto exprBox = Controls::TextBox();
                exprBox.Text(winrt::hstring(exprText));
                exprBox.AcceptsReturn(false);
                exprBox.FontFamily(Media::FontFamily(L"Consolas"));
                exprBox.Margin({ 0, 0, 0, 8 });
                uint32_t mathId = capturedId;
                exprBox.TextChanged([this, mathId](auto&& sender, auto&&)
                {
                    auto* n = m_graph.FindNode(mathId);
                    if (!n) return;
                    auto box = sender.template as<Controls::TextBox>();
                    n->properties[L"Expression"] = std::wstring(box.Text().c_str());
                    n->dirty = true;
                    m_graph.MarkAllDirty();
                    m_forceRender = true;
                    m_nodeGraphController.SetNeedsRedraw();
                });
                panel.Children().Append(exprBox);
            }

            for (const auto& [key, value] : node->properties)
            {
                // Skip internal metadata that shouldn't appear as UI properties.
                if (key == L"analysisFields" || key == L"propertyBindings")
                    continue;
                // Math Expression: Expression is rendered above as a dedicated control.
                if (isMathExpression && key == L"Expression")
                    continue;
                // For nodes with a customEffect, only render properties that
                // correspond to a declared parameter. Properties that exist
                // solely as host-driven bootstrap values (e.g. Working Space,
                // fed by hiddenDefaults + UpdateWorkingSpaceNodes) must NOT
                // appear in the Properties panel — they are sink-only
                // analysis backing storage.
                if (node->customEffect.has_value())
                {
                    bool isDeclaredParam = false;
                    for (const auto& p : node->customEffect->parameters)
                    {
                        if (p.name == key) { isDeclaredParam = true; break; }
                    }
                    if (!isDeclaredParam) continue;
                }
                // Skip parameters hidden by conditional visibility.
                if (node->customEffect.has_value())
                {
                    bool condHidden = false;
                    for (const auto& p : node->customEffect->parameters)
                    {
                        if (p.name == key && !p.visibleWhen.empty())
                        {
                            condHidden = !::ShaderLab::Graph::EvaluateVisibleWhen(p.visibleWhen, node->properties);
                            break;
                        }
                    }
                    if (condHidden) continue;
                }
                // Skip video source internal properties (managed by video UI controls).
                if (key == L"IsVideo" || key == L"IsPlaying" || key == L"PlaybackSpeed" || key == L"Loop" || key == L"shaderPath")
                    continue;
                // Resolve metadata for this property.
                const PropertyMetadata* meta = nullptr;
                if (desc)
                {
                    auto it = desc->propertyMetadata.find(key);
                    if (it != desc->propertyMetadata.end())
                        meta = &it->second;
                }
                if (!meta)
                {
                    auto cmIt = customMeta.find(key);
                    if (cmIt != customMeta.end())
                        meta = &cmIt->second;
                }

                auto propLabel = Controls::TextBlock();
                propLabel.Text(winrt::hstring(key));
                propLabel.FontSize(12);
                propLabel.Margin({ 0, 6, 0, 2 });

                auto capturedKey = key;

                // Check if this property has an active binding.
                auto bindIt = node->propertyBindings.find(key);
                bool isBound = (bindIt != node->propertyBindings.end());

                // Build a row: [label] [bind/unbind button]
                auto labelRow = Controls::StackPanel();
                labelRow.Orientation(Controls::Orientation::Horizontal);
                labelRow.Spacing(6);
                labelRow.Children().Append(propLabel);

                // Math Expression: per-input "X" button to remove this variable.
                // Only show on float input parameters (A, B, ...), and never if
                // it is the last remaining input.
                if (isMathExpression && std::holds_alternative<float>(value))
                {
                    int floatInputCount = 0;
                    if (node->customEffect.has_value())
                    {
                        for (const auto& p : node->customEffect->parameters)
                            if (p.typeName == L"float") ++floatInputCount;
                    }
                    if (floatInputCount > 1)
                    {
                        auto removeBtn = Controls::Button();
                        removeBtn.Content(winrt::box_value(L"\u2715"));
                        removeBtn.FontSize(10);
                        removeBtn.MinWidth(24);
                        removeBtn.Padding({ 4, 0, 4, 0 });
                        std::wstring removeKey = capturedKey;
                        uint32_t removeNodeId = capturedId;
                        removeBtn.Click([this, removeNodeId, removeKey](auto&&, auto&&)
                        {
                            RemoveMathExpressionInput(removeNodeId, removeKey);
                        });
                        labelRow.Children().Append(removeBtn);
                    }
                }

                if (isBound)
                {
                    // Show binding info: "← NodeName.FieldName"
                    auto& binding = bindIt->second;

                    // Extract primary source info from the new per-component format.
                    uint32_t primarySrcNodeId = 0;
                    std::wstring primarySrcFieldName;
                    uint32_t primarySrcComponent = 0;
                    if (binding.wholeArray)
                    {
                        primarySrcNodeId = binding.wholeArraySourceNodeId;
                        primarySrcFieldName = binding.wholeArraySourceFieldName;
                    }
                    else
                    {
                        for (const auto& src : binding.sources)
                        {
                            if (src.has_value())
                            {
                                primarySrcNodeId = src->sourceNodeId;
                                primarySrcFieldName = src->sourceFieldName;
                                primarySrcComponent = src->sourceComponent;
                                break;
                            }
                        }
                    }

                    auto* srcNode = m_graph.FindNode(primarySrcNodeId);

                    // Look up source field type to determine component count.
                    uint32_t srcComponents = 1;
                    if (srcNode && srcNode->customEffect.has_value())
                    {
                        for (const auto& fd : srcNode->customEffect->analysisFields)
                        {
                            if (fd.name == primarySrcFieldName)
                            {
                                srcComponents = ::ShaderLab::Graph::AnalysisFieldComponentCount(fd.type);
                                break;
                            }
                        }
                    }

                    // Determine dest component count.
                    uint32_t destComponents = std::visit([](auto&& v) -> uint32_t
                    {
                        using VT = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<VT, float>) return 1;
                        else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float2>) return 2;
                        else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float3>) return 3;
                        else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float4>) return 4;
                        else return 1;
                    }, value);

                    std::wstring bindLabel = L"\u2190 ";
                    if (srcNode) bindLabel += srcNode->name + L".";
                    bindLabel += primarySrcFieldName;

                    auto bindInfo = Controls::TextBlock();
                    bindInfo.Text(winrt::hstring(bindLabel));
                    bindInfo.FontSize(11);
                    bindInfo.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
                    labelRow.Children().Append(bindInfo);

                    // Show component picker when source has more components than dest needs.
                    if (srcComponents > 1 && destComponents == 1)
                    {
                        auto compPicker = Controls::ComboBox();
                        compPicker.FontSize(11);
                        compPicker.MinWidth(55);
                        compPicker.Padding({ 4, 0, 4, 0 });
                        const wchar_t* compLabels[] = { L".x", L".y", L".z", L".w" };
                        for (uint32_t c = 0; c < srcComponents && c < 4; ++c)
                            compPicker.Items().Append(winrt::box_value(winrt::hstring(compLabels[c])));
                        compPicker.SelectedIndex(static_cast<int32_t>(
                            (std::min)(primarySrcComponent, srcComponents - 1)));
                        compPicker.SelectionChanged([this, capturedId, capturedKey](auto&&, auto&&)
                        {
                            auto* n = m_graph.FindNode(capturedId);
                            if (!n) return;
                            auto bit = n->propertyBindings.find(capturedKey);
                            if (bit == n->propertyBindings.end()) return;
                        });
                        // Use a simpler click-based approach: rebuild panel on change.
                        compPicker.SelectionChanged([this, capturedId, capturedKey](
                            winrt::Windows::Foundation::IInspectable const& sender, auto&&)
                        {
                            auto combo = sender.as<Controls::ComboBox>();
                            auto* n = m_graph.FindNode(capturedId);
                            if (!n) return;
                            auto bit = n->propertyBindings.find(capturedKey);
                            if (bit == n->propertyBindings.end()) return;
                            // Update the first component source's sourceComponent.
                            auto& b = bit->second;
                            if (!b.sources.empty() && b.sources[0].has_value())
                                b.sources[0]->sourceComponent = static_cast<uint32_t>(combo.SelectedIndex());
                            n->dirty = true;
                            m_graph.MarkAllDirty();
                        });
                        labelRow.Children().Append(compPicker);
                    }
                    else if (destComponents == 1 && srcComponents == 1)
                    {
                        // Both scalar — show .x
                        auto compText = Controls::TextBlock();
                        compText.Text(L".x");
                        compText.FontSize(11);
                        compText.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
                        labelRow.Children().Append(compText);
                    }

                    // Unbind button.
                    auto unbindBtn = Controls::HyperlinkButton();
                    unbindBtn.Content(winrt::box_value(L"Unbind"));
                    unbindBtn.FontSize(11);
                    unbindBtn.Padding({ 2, 0, 2, 0 });
                    unbindBtn.Click([this, capturedId, capturedKey](auto&&, auto&&)
                    {
                        auto* n = m_graph.FindNode(capturedId);
                        if (n) {
                            m_graph.UnbindProperty(capturedId, capturedKey);
                            m_nodeGraphController.RebuildLayout();
                            UpdatePropertiesPanel();
                        }
                    });
                    labelRow.Children().Append(unbindBtn);
                }
                else
                {
                    // Bind button — only show if there are analysis sources available.
                    bool hasAnalysisSources = false;
                    for (const auto& n : m_graph.Nodes())
                    {
                        if (n.id != capturedId &&
                            n.analysisOutput.type == ::ShaderLab::Graph::AnalysisOutputType::Typed &&
                            !n.analysisOutput.fields.empty())
                        {
                            hasAnalysisSources = true;
                            break;
                        }
                    }

                    if (hasAnalysisSources)
                    {
                        // Build a flyout with available analysis fields.
                        auto bindBtn = Controls::DropDownButton();
                        bindBtn.Content(winrt::box_value(L"\U0001F517"));  // 🔗
                        bindBtn.FontSize(10);
                        bindBtn.Padding({ 4, 0, 4, 0 });
                        bindBtn.MinWidth(0);
                        bindBtn.MinHeight(0);

                        auto flyout = Controls::MenuFlyout();
                        for (const auto& srcNode : m_graph.Nodes())
                        {
                            if (srcNode.id == capturedId) continue;
                            if (srcNode.analysisOutput.type != ::ShaderLab::Graph::AnalysisOutputType::Typed) continue;
                            if (srcNode.analysisOutput.fields.empty()) continue;

                            auto subItem = Controls::MenuFlyoutSubItem();
                            subItem.Text(winrt::hstring(srcNode.name));

                            for (const auto& fv : srcNode.analysisOutput.fields)
                            {
                                uint32_t srcNodeId = srcNode.id;
                                auto fieldName = fv.name;
                                bool isArray = ::ShaderLab::Graph::AnalysisFieldIsArray(fv.type);
                                uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);

                                if (isArray || cc == 1)
                                {
                                    // Single item: bind directly.
                                    auto item = Controls::MenuFlyoutItem();
                                    item.Text(winrt::hstring(fieldName));
                                    item.Click([this, capturedId, capturedKey, srcNodeId, fieldName](auto&&, auto&&)
                                    {
                                        m_graph.BindProperty(capturedId, capturedKey, srcNodeId, fieldName, 0);
                                        m_nodeGraphController.RebuildLayout();
                                        UpdatePropertiesPanel();
                                    });
                                    subItem.Items().Append(item);
                                }
                                else
                                {
                                    // Multi-component: show sub-items for .x/.y/.z/.w + whole.
                                    auto fieldSub = Controls::MenuFlyoutSubItem();
                                    fieldSub.Text(winrt::hstring(fieldName));

                                    // Whole vector binding.
                                    auto wholeItem = Controls::MenuFlyoutItem();
                                    wholeItem.Text(L"(all)");
                                    wholeItem.Click([this, capturedId, capturedKey, srcNodeId, fieldName](auto&&, auto&&)
                                    {
                                        m_graph.BindProperty(capturedId, capturedKey, srcNodeId, fieldName, 0);
                                        m_nodeGraphController.RebuildLayout();
                                        UpdatePropertiesPanel();
                                    });
                                    fieldSub.Items().Append(wholeItem);

                                    const wchar_t* compNames[] = { L".x", L".y", L".z", L".w" };
                                    for (uint32_t c = 0; c < cc; ++c)
                                    {
                                        auto compItem = Controls::MenuFlyoutItem();
                                        compItem.Text(winrt::hstring(fieldName + compNames[c]));
                                        auto capturedComp = c;
                                        compItem.Click([this, capturedId, capturedKey, srcNodeId, fieldName, capturedComp](auto&&, auto&&)
                                        {
                                            m_graph.BindProperty(capturedId, capturedKey, srcNodeId, fieldName, capturedComp);
                                            m_nodeGraphController.RebuildLayout();
                                            UpdatePropertiesPanel();
                                        });
                                        fieldSub.Items().Append(compItem);
                                    }
                                    subItem.Items().Append(fieldSub);
                                }
                            }
                            flyout.Items().Append(subItem);
                        }
                        bindBtn.Flyout(flyout);
                        labelRow.Children().Append(bindBtn);
                    }
                }

                panel.Children().Append(labelRow);

                // Lambda to mark the node dirty after a property change.
                auto markDirty = [this, capturedId, capturedKey]()
                {
                    auto* n = m_graph.FindNode(capturedId);
                    if (n) { n->dirty = true; m_graph.MarkAllDirty(); }

                    // Log the property change.
                    if (n)
                    {
                        auto propIt = n->properties.find(capturedKey);
                        if (propIt != n->properties.end())
                        {
                            std::wstring valStr;
                            std::visit([&](auto&& val) {
                                using V = std::decay_t<decltype(val)>;
                                if constexpr (std::is_same_v<V, float>)
                                    valStr = std::format(L"{:.3f}", val);
                                else if constexpr (std::is_same_v<V, int32_t>)
                                    valStr = std::to_wstring(val);
                                else if constexpr (std::is_same_v<V, uint32_t>)
                                    valStr = std::to_wstring(val);
                                else if constexpr (std::is_same_v<V, bool>)
                                    valStr = val ? L"true" : L"false";
                                else if constexpr (std::is_same_v<V, std::wstring>)
                                    valStr = val;
                                else
                                    valStr = L"(complex)";
                            }, propIt->second);
                            m_nodeLogs[capturedId].Info(
                                std::format(L"Property '{}' = {}", capturedKey, valStr));
                        }
                    }

                    // Rebuild properties panel and node layout when a property change
                    // might affect conditional visibility of other parameters.
                    bool hasVisibleWhen = false;
                    if (n && n->customEffect.has_value())
                    {
                        for (const auto& p : n->customEffect->parameters)
                            if (!p.visibleWhen.empty()) { hasVisibleWhen = true; break; }
                    }

                    // Also rebuild for parameter nodes when Min/Max changes
                    // (need to update Value slider range).
                    bool isParameterNode = n && n->customEffect.has_value() &&
                        n->customEffect->hlslSource.empty() &&
                        n->customEffect->analysisOutputType == ::ShaderLab::Graph::AnalysisOutputType::Typed;

                    if (hasVisibleWhen || isParameterNode || (n && n->effectClsid.has_value() &&
                        IsEqualGUID(n->effectClsid.value(), CLSID_D2D1Histogram)))
                    {
                        this->DispatcherQueue().TryEnqueue(
                            winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                            [this]() {
                                if (!m_isShuttingDown)
                                {
                                    UpdatePropertiesPanel();
                                    m_nodeGraphController.RebuildLayout();
                                }
                            });
                    }
                };

                // If bound, skip creating the editable control — show the live value instead.
                if (isBound)
                {
                    auto boundVal = Controls::TextBlock();
                    // Show the current effective value.
                    std::wstring valStr;
                    std::visit([&](const auto& v)
                    {
                        using VT = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<VT, float>)
                            valStr = std::format(L"{:.4f}", v);
                        else if constexpr (std::is_same_v<VT, int32_t> || std::is_same_v<VT, uint32_t>)
                            valStr = std::to_wstring(v);
                        else if constexpr (std::is_same_v<VT, bool>)
                            valStr = v ? L"true" : L"false";
                        else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float2>)
                            valStr = std::format(L"{:.4f}, {:.4f}", v.x, v.y);
                        else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float3>)
                            valStr = std::format(L"{:.4f}, {:.4f}, {:.4f}", v.x, v.y, v.z);
                        else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float4>)
                            valStr = std::format(L"{:.4f}, {:.4f}, {:.4f}, {:.4f}", v.x, v.y, v.z, v.w);
                        else if constexpr (std::is_same_v<VT, std::vector<float>>)
                            valStr = std::format(L"[{} floats]", v.size());
                        else
                            valStr = L"(bound)";
                    }, value);
                    boundVal.Text(winrt::hstring(valStr));
                    boundVal.FontSize(12);
                    boundVal.FontFamily(Media::FontFamily(L"Consolas"));
                    boundVal.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
                    boundVal.Margin({ 0, 0, 0, 4 });
                    panel.Children().Append(boundVal);
                    continue;  // Skip the normal control creation.
                }

                // ---- Dispatch by variant type + metadata hint ----
                std::visit([&](const auto& v)
                {
                    using T = std::decay_t<decltype(v)>;

                    if constexpr (std::is_same_v<T, bool>)
                    {
                        // Checkbox / ToggleSwitch
                        auto toggle = Controls::ToggleSwitch();
                        toggle.IsOn(v);
                        toggle.OnContent(winrt::box_value(L"True"));
                        toggle.OffContent(winrt::box_value(L"False"));
                        toggle.Margin({ 0, 0, 0, 4 });
                        toggle.Toggled([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                        {
                            auto* n = m_graph.FindNode(capturedId);
                            if (!n) return;
                            auto it = n->properties.find(capturedKey);
                            if (it == n->properties.end()) return;
                            auto ts = sender.template as<Controls::ToggleSwitch>();
                            it->second = ts.IsOn();
                            markDirty();
                        });
                        panel.Children().Append(toggle);
                    }
                    else if constexpr (std::is_same_v<T, uint32_t>)
                    {
                        if (meta && meta->uiHint == PropertyUIHint::ComboBox && !meta->enumLabels.empty())
                        {
                            // ComboBox for enum values.
                            auto combo = Controls::ComboBox();
                            for (const auto& label : meta->enumLabels)
                                combo.Items().Append(winrt::box_value(winrt::hstring(label)));
                            if (v < meta->enumLabels.size())
                                combo.SelectedIndex(static_cast<int32_t>(v));
                            combo.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                            combo.Margin({ 0, 0, 0, 4 });
                            combo.SelectionChanged([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                auto* n = m_graph.FindNode(capturedId);
                                if (!n) return;
                                auto it = n->properties.find(capturedKey);
                                if (it == n->properties.end()) return;
                                auto cb = sender.template as<Controls::ComboBox>();
                                int32_t idx = cb.SelectedIndex();
                                if (idx >= 0)
                                {
                                    it->second = static_cast<uint32_t>(idx);
                                    markDirty();
                                }
                            });
                            panel.Children().Append(combo);
                        }
                        else
                        {
                            // Slider for uint32 with range, or NumberBox fallback.
                            float minV = meta ? meta->minValue : 0.0f;
                            float maxV = meta ? meta->maxValue : 100.0f;
                            float stepV = meta ? meta->step : 1.0f;
                            auto nb = Controls::NumberBox();
                            nb.Value(static_cast<double>(v));
                            nb.Minimum(minV);
                            nb.Maximum(maxV);
                            nb.SmallChange(stepV);
                            nb.LargeChange(stepV * 10.0);
                            nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
                            nb.Margin({ 0, 0, 0, 4 });
                            nb.ValueChanged([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                auto* n = m_graph.FindNode(capturedId);
                                if (!n) return;
                                auto it = n->properties.find(capturedKey);
                                if (it == n->properties.end()) return;
                                auto box = sender.template as<Controls::NumberBox>();
                                double val = box.Value();
                                if (!std::isnan(val))
                                {
                                    it->second = static_cast<uint32_t>(val);
                                    markDirty();
                                }
                            });
                            panel.Children().Append(nb);
                        }
                    }
                    else if constexpr (std::is_same_v<T, float>)
                    {
                        bool useSlider = meta && meta->uiHint == PropertyUIHint::Slider;
                        bool useCombo = meta && meta->uiHint == PropertyUIHint::ComboBox && !meta->enumLabels.empty();
                        float minV = meta ? meta->minValue : -FLT_MAX;
                        float maxV = meta ? meta->maxValue : FLT_MAX;
                        float stepV = meta ? meta->step : 0.01f;

                        // For parameter nodes, the "Value" slider range uses
                        // the current "Min"/"Max" property values dynamically.
                        if (key == L"Value" && node->customEffect.has_value() &&
                            node->customEffect->hlslSource.empty())
                        {
                            auto minIt = node->properties.find(L"Min");
                            auto maxIt = node->properties.find(L"Max");
                            if (minIt != node->properties.end())
                                if (auto* f = std::get_if<float>(&minIt->second)) minV = *f;
                            if (maxIt != node->properties.end())
                                if (auto* f = std::get_if<float>(&maxIt->second)) maxV = *f;
                        }

                        if (useCombo)
                        {
                            // ComboBox for float-backed enum values.
                            auto combo = Controls::ComboBox();
                            for (const auto& label : meta->enumLabels)
                                combo.Items().Append(winrt::box_value(winrt::hstring(label)));
                            uint32_t idx = static_cast<uint32_t>(v + 0.5f);
                            if (idx < meta->enumLabels.size())
                                combo.SelectedIndex(static_cast<int32_t>(idx));
                            combo.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                            combo.Margin({ 0, 0, 0, 4 });
                            combo.SelectionChanged([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                auto* n = m_graph.FindNode(capturedId);
                                if (!n) return;
                                auto it = n->properties.find(capturedKey);
                                if (it == n->properties.end()) return;
                                auto cb = sender.template as<Controls::ComboBox>();
                                int32_t idx = cb.SelectedIndex();
                                if (idx >= 0)
                                {
                                    it->second = static_cast<float>(idx);
                                    markDirty();
                                }
                            });
                            panel.Children().Append(combo);
                        }
                        else if (useSlider)
                        {
                            // Slider + NumberBox pair for ranged floats.
                            // Shared flag prevents re-entrant sync loops.
                            auto syncing = std::make_shared<bool>(false);

                            auto slider = Controls::Slider();
                            slider.Minimum(minV);
                            slider.Maximum(maxV);
                            slider.StepFrequency(stepV);
                            slider.Value(static_cast<double>(v));
                            slider.Margin({ 0, 0, 0, 0 });

                            auto nb = Controls::NumberBox();
                            nb.Value(static_cast<double>(v));
                            nb.Minimum(minV);
                            nb.Maximum(maxV);
                            nb.SmallChange(stepV);
                            nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
                            nb.Margin({ 0, 0, 0, 4 });

                            // Slider → NumberBox sync.
                            slider.ValueChanged([nb, syncing, this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                if (*syncing) return;
                                *syncing = true;
                                auto sl = sender.template as<Controls::Slider>();
                                nb.Value(sl.Value());
                                auto* n = m_graph.FindNode(capturedId);
                                if (n)
                                {
                                    auto it = n->properties.find(capturedKey);
                                    if (it != n->properties.end())
                                    {
                                        it->second = static_cast<float>(sl.Value());
                                        markDirty();
                                    }
                                }
                                *syncing = false;
                            });

                            // NumberBox → Slider sync.
                            nb.ValueChanged([slider, syncing, this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                if (*syncing) return;
                                *syncing = true;
                                auto box = sender.template as<Controls::NumberBox>();
                                double val = box.Value();
                                if (!std::isnan(val))
                                {
                                    slider.Value(val);
                                    auto* n = m_graph.FindNode(capturedId);
                                    if (n)
                                    {
                                        auto it = n->properties.find(capturedKey);
                                        if (it != n->properties.end())
                                        {
                                            it->second = static_cast<float>(val);
                                            markDirty();
                                        }
                                    }
                                }
                                *syncing = false;
                            });

                            panel.Children().Append(slider);
                            panel.Children().Append(nb);
                        }
                        else
                        {
                            // NumberBox only.
                            auto nb = Controls::NumberBox();
                            nb.Value(static_cast<double>(v));
                            nb.SmallChange(stepV);
                            nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
                            nb.Margin({ 0, 0, 0, 4 });
                            nb.ValueChanged([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                auto* n = m_graph.FindNode(capturedId);
                                if (!n) return;
                                auto it = n->properties.find(capturedKey);
                                if (it == n->properties.end()) return;
                                auto box = sender.template as<Controls::NumberBox>();
                                double val = box.Value();
                                if (!std::isnan(val))
                                {
                                    it->second = static_cast<float>(val);
                                    markDirty();
                                }
                            });
                            panel.Children().Append(nb);
                        }
                    }
                    else if constexpr (std::is_same_v<T, int32_t>)
                    {
                        float minV = meta ? meta->minValue : static_cast<float>(INT_MIN);
                        float maxV = meta ? meta->maxValue : static_cast<float>(INT_MAX);
                        float stepV = meta ? meta->step : 1.0f;
                        auto nb = Controls::NumberBox();
                        nb.Value(static_cast<double>(v));
                        nb.Minimum(minV);
                        nb.Maximum(maxV);
                        nb.SmallChange(stepV);
                        nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
                        nb.Margin({ 0, 0, 0, 4 });
                        nb.ValueChanged([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                        {
                            auto* n = m_graph.FindNode(capturedId);
                            if (!n) return;
                            auto it = n->properties.find(capturedKey);
                            if (it == n->properties.end()) return;
                            auto box = sender.template as<Controls::NumberBox>();
                            double val = box.Value();
                            if (!std::isnan(val))
                            {
                                it->second = static_cast<int32_t>(val);
                                markDirty();
                            }
                        });
                        panel.Children().Append(nb);
                    }
                    else if constexpr (std::is_same_v<T, std::wstring>)
                    {
                        auto tb = Controls::TextBox();
                        tb.Text(winrt::hstring(v));
                        tb.Margin({ 0, 0, 0, 4 });
                        tb.LostFocus([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                        {
                            auto* n = m_graph.FindNode(capturedId);
                            if (!n) return;
                            auto it = n->properties.find(capturedKey);
                            if (it == n->properties.end()) return;
                            auto box = sender.template as<Controls::TextBox>();
                            it->second = std::wstring(box.Text().c_str());
                            markDirty();
                        });
                        panel.Children().Append(tb);
                    }
                    else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                    {
                        // 5×4 grid of NumberBoxes for the color matrix.
                        static const std::wstring rowLabels[] = { L"R", L"G", L"B", L"A", L"Ofs" };
                        static const std::wstring colLabels[] = { L"R", L"G", L"B", L"A" };

                        auto grid = Controls::StackPanel();
                        grid.Orientation(Controls::Orientation::Vertical);
                        grid.Margin({ 0, 0, 0, 4 });

                        // Column header row.
                        auto headerRow = Controls::StackPanel();
                        headerRow.Orientation(Controls::Orientation::Horizontal);
                        auto spacer = Controls::TextBlock();
                        spacer.Width(32);
                        headerRow.Children().Append(spacer);
                        for (int c = 0; c < 4; ++c)
                        {
                            auto colHdr = Controls::TextBlock();
                            colHdr.Text(winrt::hstring(colLabels[c]));
                            colHdr.Width(70);
                            colHdr.FontSize(11);
                            colHdr.HorizontalTextAlignment(winrt::Microsoft::UI::Xaml::TextAlignment::Center);
                            headerRow.Children().Append(colHdr);
                        }
                        grid.Children().Append(headerRow);

                        for (int r = 0; r < 5; ++r)
                        {
                            auto row = Controls::StackPanel();
                            row.Orientation(Controls::Orientation::Horizontal);
                            row.Margin({ 0, 1, 0, 1 });

                            auto rowLbl = Controls::TextBlock();
                            rowLbl.Text(winrt::hstring(rowLabels[r]));
                            rowLbl.Width(32);
                            rowLbl.FontSize(11);
                            rowLbl.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
                            row.Children().Append(rowLbl);

                            for (int c = 0; c < 4; ++c)
                            {
                                int cellIdx = r * 4 + c;
                                const float* p = &v._11;
                                auto nb = Controls::NumberBox();
                                nb.Value(static_cast<double>(p[cellIdx]));
                                nb.SmallChange(meta ? meta->step : 0.01);
                                nb.Width(70);
                                nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);

                                nb.ValueChanged([this, capturedId, capturedKey, cellIdx, markDirty](auto&& sender, auto&&)
                                {
                                    auto* n = m_graph.FindNode(capturedId);
                                    if (!n) return;
                                    auto it = n->properties.find(capturedKey);
                                    if (it == n->properties.end()) return;
                                    auto box = sender.template as<Controls::NumberBox>();
                                    double val = box.Value();
                                    if (std::isnan(val)) return;
                                    auto* mat = std::get_if<D2D1_MATRIX_5X4_F>(&it->second);
                                    if (mat)
                                    {
                                        float* mp = &mat->_11;
                                        mp[cellIdx] = static_cast<float>(val);
                                        markDirty();
                                    }
                                });
                                row.Children().Append(nb);
                            }
                            grid.Children().Append(row);
                        }
                        panel.Children().Append(grid);
                    }
                    else if constexpr (std::is_same_v<T, std::vector<float>>)
                    {
                        // Curve editor: show a button to open a flyout, plus a small preview.
                        auto curveContainer = Controls::StackPanel();
                        curveContainer.Orientation(Controls::Orientation::Vertical);
                        curveContainer.Margin({ 0, 0, 0, 4 });

                        // Miniature curve preview using a Polyline.
                        auto previewCanvas = winrt::Microsoft::UI::Xaml::Controls::Canvas();
                        previewCanvas.Width(200);
                        previewCanvas.Height(60);
                        previewCanvas.Background(Media::SolidColorBrush(
                            winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));

                        // Draw curve preview as a Polyline.
                        auto polyline = winrt::Microsoft::UI::Xaml::Shapes::Polyline();
                        polyline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
                        polyline.StrokeThickness(1.5);
                        auto points = winrt::Microsoft::UI::Xaml::Media::PointCollection();
                        if (!v.empty())
                        {
                            uint32_t step = (std::max)(1u, static_cast<uint32_t>(v.size()) / 50u);
                            for (uint32_t i = 0; i < v.size(); i += step)
                            {
                                float x = static_cast<float>(i) / static_cast<float>(v.size() - 1) * 200.0f;
                                float y = 60.0f - std::clamp(v[i], 0.0f, 1.0f) * 60.0f;
                                points.Append(winrt::Windows::Foundation::Point(x, y));
                            }
                        }
                        polyline.Points(points);
                        previewCanvas.Children().Append(polyline);
                        curveContainer.Children().Append(previewCanvas);

                        // "Edit Curve" button opens a ContentDialog with full curve editor.
                        auto editBtn = Controls::Button();
                        editBtn.Content(winrt::box_value(L"Edit Curve..."));
                        editBtn.Margin({ 0, 4, 0, 0 });
                        editBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                        editBtn.Click([this, capturedId, capturedKey, markDirty](auto&&, auto&&)
                        {
                            ShowCurveEditorDialog(capturedId, capturedKey, markDirty);
                        });
                        curveContainer.Children().Append(editBtn);
                        panel.Children().Append(curveContainer);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>
                                    || std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>
                                    || std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                    {
                        // Vector editor: horizontal row of labeled NumberBoxes.
                        constexpr int N = std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2> ? 2
                                        : std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3> ? 3 : 4;

                        std::vector<std::wstring> labels;
                        if (meta && meta->componentLabels.size() >= static_cast<size_t>(N))
                            labels = { meta->componentLabels.begin(), meta->componentLabels.begin() + N };
                        else
                        {
                            // Heuristic: if the property name contains "Color", use R/G/B/A labels.
                            bool isColor = (capturedKey.find(L"Color") != std::wstring::npos)
                                        || (capturedKey.find(L"color") != std::wstring::npos);
                            if (isColor)
                            {
                                if constexpr (N == 2) labels = { L"R", L"G" };
                                else if constexpr (N == 3) labels = { L"R", L"G", L"B" };
                                else                       labels = { L"R", L"G", L"B", L"A" };
                            }
                            else
                            {
                                if constexpr (N == 2) labels = { L"X", L"Y" };
                                else if constexpr (N == 3) labels = { L"X", L"Y", L"Z" };
                                else                       labels = { L"X", L"Y", L"Z", L"W" };
                            }
                        }

                        float components[4] = {};
                        if constexpr (N >= 2) { components[0] = v.x; components[1] = v.y; }
                        if constexpr (N >= 3) { components[2] = v.z; }
                        if constexpr (N >= 4) { components[3] = v.w; }

                        float minV = meta ? meta->minValue : -FLT_MAX;
                        float maxV = meta ? meta->maxValue : FLT_MAX;
                        float stepV = meta ? meta->step : 0.01f;

                        auto row = Controls::StackPanel();
                        row.Orientation(Controls::Orientation::Vertical);
                        row.Margin({ 0, 0, 0, 4 });

                        for (int i = 0; i < N; ++i)
                        {
                            auto compRow = Controls::StackPanel();
                            compRow.Orientation(Controls::Orientation::Horizontal);
                            compRow.Margin({ 0, 1, 0, 1 });

                            auto lbl = Controls::TextBlock();
                            lbl.Text(winrt::hstring(labels[i]));
                            lbl.FontSize(11);
                            lbl.Width(20);
                            lbl.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
                            compRow.Children().Append(lbl);

                            float compMin = (meta && static_cast<int>(meta->componentMin.size()) > i)
                                ? meta->componentMin[i] : minV;
                            float compMax = (meta && static_cast<int>(meta->componentMax.size()) > i)
                                ? meta->componentMax[i] : maxV;

                            auto nb = Controls::NumberBox();
                            nb.Value(static_cast<double>(components[i]));
                            nb.Minimum(compMin);
                            nb.Maximum(compMax);
                            nb.SmallChange(stepV);
                            nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
                            nb.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);

                            int compIdx = i;
                            nb.ValueChanged([this, capturedId, capturedKey, compIdx, markDirty](auto&& sender, auto&&)
                            {
                                auto* n = m_graph.FindNode(capturedId);
                                if (!n) return;
                                auto it = n->properties.find(capturedKey);
                                if (it == n->properties.end()) return;
                                auto box = sender.template as<Controls::NumberBox>();
                                double val = box.Value();
                                if (std::isnan(val)) return;
                                float fval = static_cast<float>(val);

                                std::visit([compIdx, fval](auto& vec)
                                {
                                    using VT = std::decay_t<decltype(vec)>;
                                    if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float2>)
                                    {
                                        if (compIdx == 0) vec.x = fval;
                                        else if (compIdx == 1) vec.y = fval;
                                    }
                                    else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float3>)
                                    {
                                        if (compIdx == 0) vec.x = fval;
                                        else if (compIdx == 1) vec.y = fval;
                                        else if (compIdx == 2) vec.z = fval;
                                    }
                                    else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float4>)
                                    {
                                        if (compIdx == 0) vec.x = fval;
                                        else if (compIdx == 1) vec.y = fval;
                                        else if (compIdx == 2) vec.z = fval;
                                        else if (compIdx == 3) vec.w = fval;
                                    }
                                }, it->second);
                                markDirty();
                            });
                            compRow.Children().Append(nb);
                            row.Children().Append(compRow);
                        }
                        panel.Children().Append(row);
                    }
                }, value);
            }
        }

        // ---- Gamma Transfer curve preview ----
        if (node->type == ::ShaderLab::Graph::NodeType::BuiltInEffect &&
            node->effectClsid.has_value() && IsEqualGUID(node->effectClsid.value(), CLSID_D2D1GammaTransfer))
        {
            auto previewLabel = Controls::TextBlock();
            previewLabel.Text(L"Curve Preview");
            previewLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            previewLabel.Margin({ 0, 8, 0, 4 });
            panel.Children().Append(previewLabel);

            auto canvas = Controls::Canvas();
            canvas.Width(200);
            canvas.Height(80);
            canvas.Background(Media::SolidColorBrush(
                winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));

            // Draw per-channel gamma curves: output = amplitude * input^exponent + offset
            struct ChannelInfo { std::wstring prefix; winrt::Windows::UI::Color lineColor; };
            ChannelInfo channels[] = {
                { L"Red",   { 200, 255, 80, 80 } },
                { L"Green", { 200, 80, 255, 80 } },
                { L"Blue",  { 200, 80, 80, 255 } },
            };

            for (const auto& ch : channels)
            {
                float amp = 1.0f, exp = 1.0f, ofs = 0.0f;
                bool disabled = false;
                auto ampIt = node->properties.find(ch.prefix + L"Amplitude");
                auto expIt = node->properties.find(ch.prefix + L"Exponent");
                auto ofsIt = node->properties.find(ch.prefix + L"Offset");
                auto disIt = node->properties.find(ch.prefix + L"Disable");
                if (ampIt != node->properties.end()) if (auto* pf = std::get_if<float>(&ampIt->second)) amp = *pf;
                if (expIt != node->properties.end()) if (auto* pf = std::get_if<float>(&expIt->second)) exp = *pf;
                if (ofsIt != node->properties.end()) if (auto* pf = std::get_if<float>(&ofsIt->second)) ofs = *pf;
                if (disIt != node->properties.end()) if (auto* pb = std::get_if<bool>(&disIt->second)) disabled = *pb;

                if (disabled) continue;

                auto poly = winrt::Microsoft::UI::Xaml::Shapes::Polyline();
                poly.Stroke(Media::SolidColorBrush(ch.lineColor));
                poly.StrokeThickness(1.5);
                auto pts = winrt::Microsoft::UI::Xaml::Media::PointCollection();
                for (int i = 0; i <= 50; ++i)
                {
                    float input = static_cast<float>(i) / 50.0f;
                    float output = amp * std::pow(input, exp) + ofs;
                    output = std::clamp(output, 0.0f, 1.0f);
                    pts.Append({ input * 200.0f, 80.0f - output * 80.0f });
                }
                poly.Points(pts);
                canvas.Children().Append(poly);
            }
            panel.Children().Append(canvas);
        }

        // ---- Analysis output visualization ----
        if (node->analysisOutput.type == ::ShaderLab::Graph::AnalysisOutputType::Histogram &&
            !node->analysisOutput.data.empty())
        {
            auto histHeader = Controls::TextBlock();
            histHeader.Text(winrt::hstring(node->analysisOutput.label));
            histHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            histHeader.Margin({ 0, 8, 0, 4 });
            panel.Children().Append(histHeader);

            // Draw histogram as a bar chart using XAML Canvas.
            constexpr float chartW = 300.0f;
            constexpr float chartH = 120.0f;
            auto canvas = Controls::Canvas();
            canvas.Width(chartW);
            canvas.Height(chartH);
            canvas.Background(Media::SolidColorBrush(
                winrt::Windows::UI::Color{ 255, 30, 30, 30 }));

            const auto& bins = node->analysisOutput.data;
            uint32_t numBins = static_cast<uint32_t>(bins.size());
            if (numBins > 0)
            {
                // Find max bin value for normalization.
                float maxVal = *std::max_element(bins.begin(), bins.end());
                if (maxVal <= 0.0f) maxVal = 1.0f;

                // Choose bar color based on channel.
                winrt::Windows::UI::Color barColor;
                switch (node->analysisOutput.channelIndex)
                {
                case 0: barColor = { 200, 255, 60, 60 }; break;   // Red
                case 1: barColor = { 200, 60, 255, 60 }; break;   // Green
                case 2: barColor = { 200, 60, 100, 255 }; break;  // Blue
                default: barColor = { 200, 200, 200, 200 }; break; // Alpha / other
                }
                auto barBrush = Media::SolidColorBrush(barColor);

                float barWidth = chartW / static_cast<float>(numBins);
                for (uint32_t i = 0; i < numBins; ++i)
                {
                    float normalized = bins[i] / maxVal;
                    float barH = normalized * chartH;
                    if (barH < 0.5f) continue; // skip empty bins

                    auto rect = winrt::Microsoft::UI::Xaml::Shapes::Rectangle();
                    rect.Width((std::max)(1.0, static_cast<double>(barWidth)));
                    rect.Height(static_cast<double>(barH));
                    rect.Fill(barBrush);
                    Controls::Canvas::SetLeft(rect, static_cast<double>(i * barWidth));
                    Controls::Canvas::SetTop(rect, static_cast<double>(chartH - barH));
                    canvas.Children().Append(rect);
                }
            }
            panel.Children().Append(canvas);
        }

        // ---- Typed analysis output ----
        if (node->analysisOutput.type == ::ShaderLab::Graph::AnalysisOutputType::Typed &&
            !node->analysisOutput.fields.empty())
        {
            auto analysisHeader = Controls::TextBlock();
            analysisHeader.Text(L"Analysis Results");
            analysisHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            analysisHeader.Margin({ 0, 8, 0, 4 });
            panel.Children().Append(analysisHeader);

            for (const auto& fv : node->analysisOutput.fields)
            {
                auto row = Controls::StackPanel();
                row.Orientation(Controls::Orientation::Horizontal);
                row.Spacing(8);
                row.Margin({ 0, 2, 0, 2 });

                auto labelText = Controls::TextBlock();
                labelText.Text(winrt::hstring(fv.name));
                labelText.FontSize(12);
                labelText.Width(140);
                labelText.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
                row.Children().Append(labelText);

                auto valueText = Controls::TextBlock();
                std::wstring valStr;
                if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                {
                    uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    for (uint32_t c = 0; c < cc; ++c)
                    {
                        if (c > 0) valStr += L"  ";
                        valStr += std::format(L"{:.4f}", fv.components[c]);
                    }
                }
                else
                {
                    uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    uint32_t count = stride > 0
                        ? static_cast<uint32_t>(fv.arrayData.size()) / stride : 0;
                    valStr = std::format(L"[{} elements]\n", count);
                    // Show elements (up to 32, then truncate).
                    uint32_t showCount = (std::min)(count, 32u);
                    for (uint32_t i = 0; i < showCount; ++i)
                    {
                        if (stride == 1)
                        {
                            valStr += std::format(L"  [{}] {:.4f}\n", i, fv.arrayData[i]);
                        }
                        else
                        {
                            valStr += std::format(L"  [{}] ", i);
                            for (uint32_t c = 0; c < stride; ++c)
                            {
                                if (c > 0) valStr += L"  ";
                                valStr += std::format(L"{:.4f}", fv.arrayData[i * stride + c]);
                            }
                            valStr += L"\n";
                        }
                    }
                    if (count > showCount)
                        valStr += std::format(L"  ... ({} more)\n", count - showCount);
                }
                valueText.Text(winrt::hstring(valStr));
                valueText.FontSize(12);
                valueText.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
                valueText.IsTextSelectionEnabled(true);
                valueText.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::NoWrap);
                row.Children().Append(valueText);

                panel.Children().Append(row);
            }
        }

        // ---- Input pins info ----
        if (!node->inputPins.empty())
        {
            auto pinsHeader = Controls::TextBlock();
            pinsHeader.Text(L"Inputs: " + std::to_wstring(node->inputPins.size()));
            pinsHeader.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
            pinsHeader.Margin({ 0, 8, 0, 0 });
            panel.Children().Append(pinsHeader);
        }

        // ---- ShaderLab effect version upgrade button ----
        if (node->customEffect.has_value() && !node->customEffect->shaderLabEffectId.empty())
        {
            auto& registry = ::ShaderLab::Effects::ShaderLabEffects::Instance();
            auto* latest = registry.FindById(node->customEffect->shaderLabEffectId);
            if (latest && latest->effectVersion > node->customEffect->shaderLabEffectVersion)
            {
                auto upgradeBorder = Controls::Border();
                upgradeBorder.Background(Media::SolidColorBrush(winrt::Windows::UI::Color{ 255, 50, 80, 140 }));
                upgradeBorder.CornerRadius({ 4, 4, 4, 4 });
                upgradeBorder.Padding({ 8, 6, 8, 6 });
                upgradeBorder.Margin({ 0, 8, 0, 0 });

                auto upgradeStack = Controls::StackPanel();
                upgradeStack.Spacing(6);

                auto upgradeText = Controls::TextBlock();
                upgradeText.Text(std::format(L"Newer version available (v{} -> v{})",
                    node->customEffect->shaderLabEffectVersion, latest->effectVersion));
                upgradeText.FontSize(12);
                upgradeText.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::White()));
                upgradeStack.Children().Append(upgradeText);

                auto upgradeBtn = Controls::Button();
                upgradeBtn.Content(winrt::box_value(L"\xE777  Update Effect"));
                upgradeBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                upgradeBtn.Click([this, capturedId](auto&&, auto&&)
                {
                    auto* n = m_graph.FindNode(capturedId);
                    if (!n || !n->customEffect.has_value()) return;

                    auto& reg = ::ShaderLab::Effects::ShaderLabEffects::Instance();
                    auto* desc = reg.FindById(n->customEffect->shaderLabEffectId);
                    if (!desc) return;

                    // Save current property values.
                    auto savedProps = n->properties;

                    // Create a fresh node from the latest descriptor.
                    auto freshNode = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
                    if (!freshNode.customEffect.has_value()) return;

                    // Replace definition with fresh version.
                    n->customEffect = std::move(freshNode.customEffect.value());
                    n->inputPins = std::move(freshNode.inputPins);
                    n->outputPins = std::move(freshNode.outputPins);
                    n->isClock = freshNode.isClock;

                    // Start with fresh default properties, then restore matching saved values.
                    n->properties = std::move(freshNode.properties);
                    for (const auto& [key, val] : savedProps)
                    {
                        auto it = n->properties.find(key);
                        if (it != n->properties.end())
                            it->second = val;
                    }

                    n->dirty = true;
                    n->cachedOutput = nullptr; // raw pointer is now dangling
                    m_graphEvaluator.InvalidateNode(capturedId);
                    m_graph.MarkAllDirty();
                    PopulatePreviewNodeSelector();
                    UpdatePropertiesPanel();
                });
                upgradeStack.Children().Append(upgradeBtn);
                upgradeBorder.Child(upgradeStack);
                panel.Children().Append(upgradeBorder);
            }
        }

        // "Show Logs" button at the bottom of properties panel.
        if (node)
        {
            // Math Expression: "+ Add Input" button just above Show Logs.
            if (node->customEffect.has_value() &&
                node->customEffect->shaderLabEffectId == L"Math Expression")
            {
                auto addBtn = Controls::Button();
                addBtn.Content(winrt::box_value(L"\u2795  Add Input"));
                addBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                addBtn.Margin({ 0, 12, 0, 0 });
                uint32_t addNodeId = node->id;
                addBtn.Click([this, addNodeId](auto&&, auto&&)
                {
                    AddMathExpressionInput(addNodeId);
                });
                panel.Children().Append(addBtn);
            }

            auto logBtn = Controls::Button();
            logBtn.Content(winrt::box_value(L"\xE7BA  Show Logs"));
            logBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            logBtn.Margin({ 0, 12, 0, 0 });
            uint32_t logNodeId = node->id;
            logBtn.Click([this, logNodeId](auto&&, auto&&) {
                OpenLogWindow(logNodeId);
            });
            panel.Children().Append(logBtn);
        }
    }

    void MainWindow::AddMathExpressionInput(uint32_t nodeId)
    {
        auto* node = m_graph.FindNode(nodeId);
        if (!node || !node->customEffect.has_value()) return;
        if (node->customEffect->shaderLabEffectId != L"Math Expression") return;

        // Find the next unused single-letter name A..Z.
        std::wstring nextName;
        for (wchar_t ch = L'A'; ch <= L'Z'; ++ch)
        {
            std::wstring candidate(1, ch);
            if (node->properties.find(candidate) == node->properties.end())
            {
                nextName = candidate;
                break;
            }
        }
        if (nextName.empty()) return;  // 26 inputs already; bail out silently.

        ::ShaderLab::Graph::ParameterDefinition pd;
        pd.name = nextName;
        pd.typeName = L"float";
        pd.defaultValue = 0.0f;
        pd.minValue = -100000.0f;
        pd.maxValue = 100000.0f;
        pd.step = 0.1f;
        node->customEffect->parameters.push_back(std::move(pd));
        node->properties[nextName] = 0.0f;

        node->dirty = true;
        m_graph.MarkAllDirty();
        m_nodeGraphController.RebuildLayout();
        m_forceRender = true;
        UpdatePropertiesPanel();
    }

    void MainWindow::RemoveMathExpressionInput(uint32_t nodeId, const std::wstring& paramName)
    {
        auto* node = m_graph.FindNode(nodeId);
        if (!node || !node->customEffect.has_value()) return;
        if (node->customEffect->shaderLabEffectId != L"Math Expression") return;
        if (paramName == L"Expression") return;  // never remove the formula.

        // Refuse to remove the last remaining input.
        int floatInputs = 0;
        for (const auto& p : node->customEffect->parameters)
            if (p.typeName == L"float") ++floatInputs;
        if (floatInputs <= 1) return;

        // Drop the parameter definition + property + any binding on this slot.
        std::erase_if(node->customEffect->parameters,
            [&paramName](const auto& p) { return p.name == paramName; });
        node->properties.erase(paramName);
        m_graph.UnbindProperty(nodeId, paramName);

        node->dirty = true;
        m_graph.MarkAllDirty();
        m_nodeGraphController.RebuildLayout();
        m_forceRender = true;
        UpdatePropertiesPanel();
    }

    void MainWindow::ShowCurveEditorDialog(
        uint32_t nodeId, const std::wstring& propertyKey, std::function<void()> markDirty)
    {
        namespace Controls = winrt::Microsoft::UI::Xaml::Controls;
        namespace Media = winrt::Microsoft::UI::Xaml::Media;

        auto* node = m_graph.FindNode(nodeId);
        if (!node) return;
        auto propIt = node->properties.find(propertyKey);
        if (propIt == node->properties.end()) return;
        auto* lutPtr = std::get_if<std::vector<float>>(&propIt->second);
        if (!lutPtr) return;

        constexpr float canvasW = 400.0f;
        constexpr float canvasH = 300.0f;
        constexpr float pointRadius = 6.0f;

        // Build a set of editable control points from the LUT.
        // Sample ~16 evenly-spaced points for dragging; the full LUT is interpolated from them.
        auto controlPoints = std::make_shared<std::vector<winrt::Windows::Foundation::Point>>();
        uint32_t lutSize = static_cast<uint32_t>(lutPtr->size());
        uint32_t numCtrlPts = (std::min)(16u, lutSize);
        if (numCtrlPts < 2) numCtrlPts = 2;
        for (uint32_t i = 0; i < numCtrlPts; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(numCtrlPts - 1);
            uint32_t idx = (std::min)(static_cast<uint32_t>(t * (lutSize - 1)), lutSize - 1);
            float x = t * canvasW;
            float y = canvasH - std::clamp((*lutPtr)[idx], 0.0f, 1.0f) * canvasH;
            controlPoints->push_back({ x, y });
        }

        // Canvas for the curve.
        auto canvas = Controls::Canvas();
        canvas.Width(canvasW);
        canvas.Height(canvasH);
        canvas.Background(Media::SolidColorBrush(
            winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));

        // Draw grid lines.
        for (int i = 1; i < 4; ++i)
        {
            float pos = canvasH * i / 4.0f;
            auto hline = winrt::Microsoft::UI::Xaml::Shapes::Line();
            hline.X1(0); hline.X2(canvasW); hline.Y1(pos); hline.Y2(pos);
            hline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 60, 60, 60)));
            hline.StrokeThickness(0.5);
            canvas.Children().Append(hline);

            float posX = canvasW * i / 4.0f;
            auto vline = winrt::Microsoft::UI::Xaml::Shapes::Line();
            vline.X1(posX); vline.X2(posX); vline.Y1(0); vline.Y2(canvasH);
            vline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 60, 60, 60)));
            vline.StrokeThickness(0.5);
            canvas.Children().Append(vline);
        }

        // Curve polyline.
        auto polyline = winrt::Microsoft::UI::Xaml::Shapes::Polyline();
        polyline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
        polyline.StrokeThickness(2.0);
        canvas.Children().Append(polyline);

        // Helper to rebuild the polyline from control points.
        auto rebuildCurve = [polyline, controlPoints, canvasW, canvasH]()
        {
            auto pts = winrt::Microsoft::UI::Xaml::Media::PointCollection();
            for (const auto& cp : *controlPoints)
                pts.Append(cp);
            polyline.Points(pts);
        };
        rebuildCurve();

        // Add draggable point ellipses.
        auto draggingIdx = std::make_shared<int>(-1);
        std::vector<winrt::Microsoft::UI::Xaml::Shapes::Ellipse> pointEllipses;

        for (uint32_t i = 0; i < controlPoints->size(); ++i)
        {
            auto ellipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse();
            ellipse.Width(pointRadius * 2);
            ellipse.Height(pointRadius * 2);
            ellipse.Fill(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::White()));
            Controls::Canvas::SetLeft(ellipse, (*controlPoints)[i].X - pointRadius);
            Controls::Canvas::SetTop(ellipse, (*controlPoints)[i].Y - pointRadius);
            canvas.Children().Append(ellipse);
            pointEllipses.push_back(ellipse);
        }

        // Pointer handlers for dragging control points.
        canvas.PointerPressed([draggingIdx, controlPoints, pointRadius](auto&&,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            auto pos = args.GetCurrentPoint(nullptr).Position();
            for (int i = static_cast<int>(controlPoints->size()) - 1; i >= 0; --i)
            {
                float dx = pos.X - (*controlPoints)[i].X;
                float dy = pos.Y - (*controlPoints)[i].Y;
                if (dx * dx + dy * dy < (pointRadius * 3) * (pointRadius * 3))
                {
                    *draggingIdx = i;
                    break;
                }
            }
        });

        canvas.PointerMoved([draggingIdx, controlPoints, &pointEllipses, rebuildCurve,
            canvasW, canvasH, pointRadius](auto&&,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            if (*draggingIdx < 0) return;
            auto pos = args.GetCurrentPoint(nullptr).Position();
            int idx = *draggingIdx;

            // Clamp X to maintain ordering (first/last points locked to edges).
            float x = pos.X;
            float y = std::clamp(pos.Y, 0.0f, canvasH);
            if (idx == 0)
                x = 0.0f;
            else if (idx == static_cast<int>(controlPoints->size()) - 1)
                x = canvasW;
            else
            {
                float minX = (*controlPoints)[idx - 1].X + 1.0f;
                float maxX = (*controlPoints)[idx + 1].X - 1.0f;
                x = std::clamp(x, minX, maxX);
            }

            (*controlPoints)[idx] = { x, y };
            Controls::Canvas::SetLeft(pointEllipses[idx], x - pointRadius);
            Controls::Canvas::SetTop(pointEllipses[idx], y - pointRadius);
            rebuildCurve();
        });

        canvas.PointerReleased([draggingIdx](auto&&, auto&&) { *draggingIdx = -1; });

        // Build dialog.
        auto dialog = Controls::ContentDialog();
        dialog.Title(winrt::box_value(L"Curve Editor — " + propertyKey));
        dialog.PrimaryButtonText(L"Apply");
        dialog.CloseButtonText(L"Cancel");
        dialog.XamlRoot(this->Content().XamlRoot());

        auto dialogContent = Controls::StackPanel();
        dialogContent.Children().Append(canvas);

        // "Reset to Identity" button.
        auto resetBtn = Controls::Button();
        resetBtn.Content(winrt::box_value(L"Reset to Identity"));
        resetBtn.Margin({ 0, 8, 0, 0 });
        resetBtn.Click([controlPoints, &pointEllipses, rebuildCurve, canvasW, canvasH, pointRadius](auto&&, auto&&)
        {
            for (uint32_t i = 0; i < controlPoints->size(); ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(controlPoints->size() - 1);
                (*controlPoints)[i] = { t * canvasW, canvasH - t * canvasH };
                Controls::Canvas::SetLeft(pointEllipses[i], t * canvasW - pointRadius);
                Controls::Canvas::SetTop(pointEllipses[i], (canvasH - t * canvasH) - pointRadius);
            }
            rebuildCurve();
        });
        dialogContent.Children().Append(resetBtn);
        dialog.Content(dialogContent);

        // Show dialog and apply result.
        auto capturedNodeId = nodeId;
        auto capturedKey = propertyKey;
        auto capturedMarkDirty = markDirty;
        auto capturedCtrlPts = controlPoints;

        dialog.PrimaryButtonClick([this, capturedNodeId, capturedKey, capturedMarkDirty,
            capturedCtrlPts, canvasW, canvasH](auto&&, auto&&)
        {
            auto* n = m_graph.FindNode(capturedNodeId);
            if (!n) return;
            auto it = n->properties.find(capturedKey);
            if (it == n->properties.end()) return;
            auto* lut = std::get_if<std::vector<float>>(&it->second);
            if (!lut) return;

            // Interpolate control points into the full LUT array.
            uint32_t lutSize = static_cast<uint32_t>(lut->size());
            for (uint32_t i = 0; i < lutSize; ++i)
            {
                float x = static_cast<float>(i) / static_cast<float>(lutSize - 1) * canvasW;

                // Find the two surrounding control points.
                int cpIdx = 0;
                for (int j = 1; j < static_cast<int>(capturedCtrlPts->size()); ++j)
                {
                    if ((*capturedCtrlPts)[j].X >= x)
                    {
                        cpIdx = j - 1;
                        break;
                    }
                    cpIdx = j;
                }

                float val;
                if (cpIdx >= static_cast<int>(capturedCtrlPts->size()) - 1)
                {
                    val = 1.0f - (*capturedCtrlPts).back().Y / canvasH;
                }
                else
                {
                    float x0 = (*capturedCtrlPts)[cpIdx].X;
                    float x1 = (*capturedCtrlPts)[cpIdx + 1].X;
                    float y0 = (*capturedCtrlPts)[cpIdx].Y;
                    float y1 = (*capturedCtrlPts)[cpIdx + 1].Y;
                    float t = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.0f;
                    float yInterp = y0 + t * (y1 - y0);
                    val = 1.0f - yInterp / canvasH;
                }
                (*lut)[i] = std::clamp(val, 0.0f, 1.0f);
            }
            capturedMarkDirty();
            UpdatePropertiesPanel();
        });

        dialog.ShowAsync();
    }

    winrt::fire_and_forget MainWindow::BrowseImageForSourceNode(uint32_t nodeId)
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);
        picker.FileTypeFilter().Append(L".png");
        picker.FileTypeFilter().Append(L".jpg");
        picker.FileTypeFilter().Append(L".jpeg");
        picker.FileTypeFilter().Append(L".bmp");
        picker.FileTypeFilter().Append(L".tif");
        picker.FileTypeFilter().Append(L".tiff");
        picker.FileTypeFilter().Append(L".hdr");
        picker.FileTypeFilter().Append(L".exr");
        picker.FileTypeFilter().Append(L".jxr");

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        auto* node = m_graph.FindNode(nodeId);
        if (!node) co_return;

        node->shaderPath = std::wstring(file.Path().c_str());
        node->name = std::wstring(file.Name().c_str());
        node->dirty = true;

        // Prepare the source (load the image bitmap).
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (dc)
            m_sourceFactory.PrepareSourceNode(*node, dc, 0.0, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());

        m_graph.MarkAllDirty();
        m_nodeGraphController.RebuildLayout();
        PopulatePreviewNodeSelector();
        FitPreviewToView();
        UpdatePropertiesPanel();
    }

    // -----------------------------------------------------------------------
    // Video source file picker
    // -----------------------------------------------------------------------

    winrt::fire_and_forget MainWindow::BrowseVideoForSourceNode()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::VideosLibrary);
        picker.FileTypeFilter().Append(L".mp4");
        picker.FileTypeFilter().Append(L".mkv");
        picker.FileTypeFilter().Append(L".mov");
        picker.FileTypeFilter().Append(L".avi");
        picker.FileTypeFilter().Append(L".wmv");
        picker.FileTypeFilter().Append(L".webm");
        picker.FileTypeFilter().Append(L".m4v");
        picker.FileTypeFilter().Append(L".ts");

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        auto filePath = std::wstring(file.Path().c_str());
        auto fileName = std::wstring(file.Name().c_str());

        auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateVideoSourceNode(filePath, fileName);
        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
        OnNodeAdded(nodeId);

        // Prepare immediately so first frame is decoded.
        auto* graphNode = m_graph.FindNode(nodeId);
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (graphNode && dc)
            m_sourceFactory.PrepareSourceNode(*graphNode, dc, 0.0, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());

        m_graph.MarkAllDirty();
        m_nodeGraphController.RebuildLayout();
        PopulatePreviewNodeSelector();
        FitPreviewToView();
    }

    winrt::fire_and_forget MainWindow::BrowseVideoForExistingNode(uint32_t nodeId)
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::VideosLibrary);
        picker.FileTypeFilter().Append(L".mp4");
        picker.FileTypeFilter().Append(L".mkv");
        picker.FileTypeFilter().Append(L".mov");
        picker.FileTypeFilter().Append(L".avi");
        picker.FileTypeFilter().Append(L".wmv");
        picker.FileTypeFilter().Append(L".webm");
        picker.FileTypeFilter().Append(L".m4v");
        picker.FileTypeFilter().Append(L".ts");

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        auto filePath = std::wstring(file.Path().c_str());
        auto fileName = std::wstring(file.Name().c_str());

        auto* node = m_graph.FindNode(nodeId);
        if (!node) co_return;

        // Update the node's path and name.
        node->shaderPath = filePath;
        node->properties[L"shaderPath"] = filePath;
        node->name = fileName;
        node->dirty = true;

        // Re-prepare to open the new video file.
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (dc)
            m_sourceFactory.PrepareSourceNode(*node, dc, 0.0, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());

        m_graph.MarkAllDirty();
        m_nodeGraphController.RebuildLayout();
        PopulatePreviewNodeSelector();
        UpdatePropertiesPanel();
    }

    // -----------------------------------------------------------------------
    // Column splitter
    // -----------------------------------------------------------------------

    void MainWindow::OnColumnSplitterPointerPressed(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        m_isDraggingSplitter = true;
        auto point = args.GetCurrentPoint(this->Content());
        m_splitterDragStartX = point.Position().X;

        auto col0 = Content().as<winrt::Microsoft::UI::Xaml::Controls::Grid>().ColumnDefinitions().GetAt(0);
        auto col2 = Content().as<winrt::Microsoft::UI::Xaml::Controls::Grid>().ColumnDefinitions().GetAt(2);
        m_splitterStartCol0Width = col0.ActualWidth();
        m_splitterStartCol2Width = col2.ActualWidth();

        sender.as<winrt::Microsoft::UI::Xaml::UIElement>().CapturePointer(args.Pointer());
        args.Handled(true);
    }

    void MainWindow::OnColumnSplitterPointerMoved(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!m_isDraggingSplitter) return;

        auto point = args.GetCurrentPoint(this->Content());
        double delta = point.Position().X - m_splitterDragStartX;

        double newCol0 = (std::max)(100.0, m_splitterStartCol0Width + delta);
        double newCol2 = (std::max)(100.0, m_splitterStartCol2Width - delta);

        auto grid = Content().as<winrt::Microsoft::UI::Xaml::Controls::Grid>();
        grid.ColumnDefinitions().GetAt(0).Width(
            winrt::Microsoft::UI::Xaml::GridLengthHelper::FromPixels(newCol0));
        grid.ColumnDefinitions().GetAt(2).Width(
            winrt::Microsoft::UI::Xaml::GridLengthHelper::FromPixels(newCol2));

        args.Handled(true);
    }

    void MainWindow::OnColumnSplitterPointerReleased(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isDraggingSplitter)
        {
            m_isDraggingSplitter = false;
            sender.as<winrt::Microsoft::UI::Xaml::UIElement>().ReleasePointerCapture(args.Pointer());
        }
    }

    void MainWindow::OpenEffectDesigner()
    {
        if (!m_designerWindow)
        {
            m_designerWindow = winrt::make<winrt::ShaderLab::implementation::EffectDesignerWindow>();

            // Set up callbacks from the designer to the main window.
            auto designerImpl = winrt::get_self<winrt::ShaderLab::implementation::EffectDesignerWindow>(m_designerWindow);
            designerImpl->SetAddToGraphCallback([this](::ShaderLab::Graph::EffectNode node) -> uint32_t
            {
                auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
                m_graph.MarkAllDirty();
                m_nodeGraphController.RebuildLayout();
                PopulatePreviewNodeSelector();
                PopulateAddNodeFlyout(); // refresh custom effects list
                return nodeId;
            });
            designerImpl->SetUpdateInGraphCallback([this](uint32_t nodeId, ::ShaderLab::Graph::CustomEffectDefinition def)
            {
                auto* node = m_graph.FindNode(nodeId);
                if (node)
                {
                    // Name enforcement: if HLSL changed and other nodes share
                    // this name with the OLD HLSL, this node must be renamed.
                    bool hlslChanged = !node->customEffect.has_value() ||
                        node->customEffect->hlslSource != def.hlslSource;

                    node->customEffect = std::move(def);
                    node->dirty = true;
                    node->cachedOutput = nullptr; // shader changed -> output may be released
                    m_graph.MarkAllDirty();
                    m_graphEvaluator.UpdateNodeShader(nodeId, *node);

                    if (hlslChanged)
                    {
                        EnforceCustomEffectNameUniqueness(nodeId);
                        PopulateAddNodeFlyout();
                    }
                }
            });

            m_designerWindow.Closed([this](auto&&, auto&&) { m_designerWindow = nullptr; });
        }
        m_designerWindow.Activate();
    }

    void MainWindow::EnforceCustomEffectNameUniqueness(uint32_t modifiedNodeId)
    {
        auto* modNode = m_graph.FindNode(modifiedNodeId);
        if (!modNode || !modNode->customEffect.has_value()) return;

        const auto& modHlsl = modNode->customEffect->hlslSource;
        const auto& modName = modNode->name;

        // Check if any other node has the same name but different HLSL.
        bool conflict = false;
        for (const auto& other : m_graph.Nodes())
        {
            if (other.id == modifiedNodeId) continue;
            if (other.name != modName) continue;
            if (!other.customEffect.has_value()) continue;
            if (other.customEffect->hlslSource != modHlsl)
            {
                conflict = true;
                break;
            }
        }

        if (!conflict) return;

        // Auto-rename: append a suffix until unique.
        std::wstring baseName = modName;
        // Strip existing " (N)" suffix if present.
        auto parenPos = baseName.rfind(L" (");
        if (parenPos != std::wstring::npos && baseName.back() == L')')
            baseName = baseName.substr(0, parenPos);

        for (int suffix = 2; suffix < 100; ++suffix)
        {
            std::wstring candidate = baseName + L" (" + std::to_wstring(suffix) + L")";
            bool taken = false;
            for (const auto& other : m_graph.Nodes())
            {
                if (other.id == modifiedNodeId) continue;
                if (other.name == candidate)
                {
                    // Same name is OK only if same HLSL.
                    if (other.customEffect.has_value() &&
                        other.customEffect->hlslSource != modHlsl)
                    {
                        taken = true;
                        break;
                    }
                }
            }
            if (!taken)
            {
                modNode->name = candidate;
                m_nodeGraphController.RebuildLayout();
                break;
            }
        }
    }

    void MainWindow::OnSaveImageClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        SaveImageAsync();
    }

    std::vector<uint8_t> MainWindow::CapturePreviewAsPng()
    {
        auto* image = ResolveDisplayImage(m_previewNodeId);
        if (!image) return {};
        return CaptureImageAsPng(image);
    }

    std::vector<uint8_t> MainWindow::CaptureImageAsPng(ID2D1Image* image, uint32_t maxDim)
    {
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc || !image) return {};

        // Use 96 DPI so GetImageLocalBounds returns pixel coordinates.
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(image, &bounds);
        uint32_t w = static_cast<uint32_t>(bounds.right - bounds.left);
        uint32_t h = static_cast<uint32_t>(bounds.bottom - bounds.top);

        dc->SetDpi(oldDpiX, oldDpiY);

        if (w == 0 || h == 0) return {};
        w = (std::min)(w, maxDim);
        h = (std::min)(h, maxDim);

        try
        {
            winrt::com_ptr<ID2D1Bitmap1> renderBitmap;
            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bmpProps, renderBitmap.put()));

            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            dc->SetTarget(renderBitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->DrawImage(image);
            dc->EndDraw();
            dc->SetTarget(oldTarget.get());

            winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
            D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, cpuProps, cpuBitmap.put()));
            D2D1_POINT_2U destPt = { 0, 0 };
            D2D1_RECT_U srcRc = { 0, 0, w, h };
            winrt::check_hresult(cpuBitmap->CopyFromBitmap(&destPt, renderBitmap.get(), &srcRc));

            D2D1_MAPPED_RECT mapped{};
            winrt::check_hresult(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped));

            winrt::com_ptr<IWICImagingFactory> wicFactory;
            winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wicFactory.put())));

            winrt::com_ptr<IStream> memStream;
            CreateStreamOnHGlobal(nullptr, TRUE, memStream.put());

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(memStream.get(), WICBitmapEncoderNoCache));

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
            winrt::check_hresult(frame->Initialize(nullptr));
            winrt::check_hresult(frame->SetSize(w, h));
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            winrt::check_hresult(frame->SetPixelFormat(&fmt));
            winrt::check_hresult(frame->WritePixels(h, mapped.pitch, mapped.pitch * h, mapped.bits));
            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());

            cpuBitmap->Unmap();

            STATSTG stat{};
            memStream->Stat(&stat, STATFLAG_NONAME);
            ULONG pngSize = static_cast<ULONG>(stat.cbSize.QuadPart);
            std::vector<uint8_t> result(pngSize);
            LARGE_INTEGER zero{};
            memStream->Seek(zero, STREAM_SEEK_SET, nullptr);
            memStream->Read(result.data(), pngSize, nullptr);
            return result;
        }
        catch (...) { return {}; }
    }

    std::vector<uint8_t> MainWindow::CaptureNodeAsPng(uint32_t nodeId,
                                                     bool& outNotFound,
                                                     bool& outNotReady)
    {
        outNotFound = false;
        outNotReady = false;

        // Force a render frame so dirty downstream nodes evaluate before we
        // try to resolve the output.  Same convention as /render/capture.
        RenderFrame();

        auto* image = ResolveDisplayImage(nodeId);
        if (!image)
        {
            // Disambiguate "no such node" vs "node exists but isn't ready".
            auto* node = m_graph.FindNode(nodeId);
            if (!node) { outNotFound = true; return {}; }
            outNotReady = true;
            return {};
        }
        return CaptureImageAsPng(image);
    }

    bool MainWindow::ReadPixelRegion(uint32_t nodeId,
                                     int32_t x, int32_t y, uint32_t w, uint32_t h,
                                     std::vector<float>& outPixels,
                                     uint32_t& outActualW, uint32_t& outActualH,
                                     bool& outNotFound, bool& outNotReady)
    {
        outPixels.clear();
        outActualW = 0;
        outActualH = 0;
        outNotFound = false;
        outNotReady = false;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return false;

        // Force a fresh frame so dirty nodes evaluate before readback.
        // The engine helper (Rendering::ReadPixelRegion) is otherwise
        // pure -- doesn't drive eval -- so the host has to ensure the
        // graph is up-to-date.
        RenderFrame();

        auto result = ::ShaderLab::Rendering::ReadPixelRegion(
            m_graph, nodeId, x, y, w, h, dc);

        switch (result.status)
        {
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::Success:
            outPixels = std::move(result.pixels);
            outActualW = result.actualWidth;
            outActualH = result.actualHeight;
            return true;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::NotFound:
            outNotFound = true;
            return false;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::NotReady:
            outNotReady = true;
            return false;
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::InvalidRegion:
        case ::ShaderLab::Rendering::ReadPixelRegionStatus::D2DError:
        default:
            return false;
        }
    }

    std::vector<uint8_t> MainWindow::CaptureGraphAsPng()
    {
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return {};

        // Layout may be stale (e.g., after MCP set-property) — rebuild before
        // measuring or drawing.
        m_nodeGraphController.RebuildLayout();

        const uint32_t w = (std::max)(uint32_t{ 1 }, m_graphPanelWidth);
        const uint32_t h = (std::max)(uint32_t{ 1 }, m_graphPanelHeight);

        // Save & restore the controller's dirty flag.  RenderGraphScene calls
        // controller.Render, which clears the flag on completion — that would
        // suppress the next live render tick.  Capturing is a *side query* and
        // must not change live render scheduling.
        const bool wasDirty = m_nodeGraphController.NeedsRedraw();

        // Save full DC state.
        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());
        float oldDpiX = 96.0f, oldDpiY = 96.0f;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        D2D1_MATRIX_3X2_F oldTransform = D2D1::Matrix3x2F::Identity();
        dc->GetTransform(&oldTransform);

        winrt::com_ptr<ID2D1Bitmap1> renderBitmap;
        winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
        D2D1_MAPPED_RECT mapped{};
        bool isMapped = false;

        // RAII restore for the DC state and CPU map. Runs in reverse order.
        struct ScopeGuard {
            std::function<void()> fn;
            ~ScopeGuard() { if (fn) fn(); }
        };
        ScopeGuard restoreState{ [&]
        {
            if (isMapped && cpuBitmap) { cpuBitmap->Unmap(); }
            dc->SetTransform(oldTransform);
            dc->SetDpi(oldDpiX, oldDpiY);
            dc->SetTarget(oldTarget.get());
            if (wasDirty) m_nodeGraphController.SetNeedsRedraw();
        } };

        try
        {
            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
                bmpProps, renderBitmap.put()));

            dc->SetDpi(96.0f, 96.0f);
            dc->SetTarget(renderBitmap.get());
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->BeginDraw();

            D2D1_SIZE_F viewSize = { static_cast<float>(w), static_cast<float>(h) };
            RenderGraphScene(dc, viewSize);

            HRESULT hrEnd = dc->EndDraw();
            if (FAILED(hrEnd)) return {};

            D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
                cpuProps, cpuBitmap.put()));
            D2D1_POINT_2U destPt = { 0, 0 };
            D2D1_RECT_U srcRc = { 0, 0, w, h };
            winrt::check_hresult(cpuBitmap->CopyFromBitmap(&destPt, renderBitmap.get(), &srcRc));

            winrt::check_hresult(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped));
            isMapped = true;

            winrt::com_ptr<IWICImagingFactory> wicFactory;
            winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wicFactory.put())));

            winrt::com_ptr<IStream> memStream;
            winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, memStream.put()));

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(memStream.get(), WICBitmapEncoderNoCache));

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
            winrt::check_hresult(frame->Initialize(nullptr));
            winrt::check_hresult(frame->SetSize(w, h));
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            winrt::check_hresult(frame->SetPixelFormat(&fmt));
            winrt::check_hresult(frame->WritePixels(h, mapped.pitch, mapped.pitch * h, mapped.bits));
            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());

            STATSTG stat{};
            memStream->Stat(&stat, STATFLAG_NONAME);
            ULONG pngSize = static_cast<ULONG>(stat.cbSize.QuadPart);
            std::vector<uint8_t> result(pngSize);
            LARGE_INTEGER zero{};
            memStream->Seek(zero, STREAM_SEEK_SET, nullptr);
            memStream->Read(result.data(), pngSize, nullptr);
            return result;
        }
        catch (...) { return {}; }
    }

    void MainWindow::FitGraphView(float padding)
    {
        // Layout may be stale (e.g., after MCP set-property) — rebuild before
        // measuring bounds.
        m_nodeGraphController.RebuildLayout();

        D2D1_RECT_F b = m_nodeGraphController.ContentBounds();
        float contentW = b.right - b.left;
        float contentH = b.bottom - b.top;
        if (contentW <= 0.0f || contentH <= 0.0f) return; // Empty graph.

        float vpW = static_cast<float>(m_graphPanelWidth);
        float vpH = static_cast<float>(m_graphPanelHeight);
        if (vpW <= 1.0f || vpH <= 1.0f) return;

        // Padding is in viewport (screen) space.  Guard against viewport too
        // small for the requested padding.
        padding = (std::max)(0.0f, padding);
        if (padding * 2.0f > vpW * 0.5f) padding = vpW * 0.25f;
        if (padding * 2.0f > vpH * 0.5f) padding = (std::min)(padding, vpH * 0.25f);

        float availW = (std::max)(1.0f, vpW - 2.0f * padding);
        float availH = (std::max)(1.0f, vpH - 2.0f * padding);

        float zoom = (std::min)(availW / contentW, availH / contentH);
        // Controller will clamp to [0.1, 5.0] internally; mirror so pan is
        // computed against the actual zoom that will take effect.
        zoom = (std::max)(0.1f, (std::min)(5.0f, zoom));

        float contentCx = (b.left + b.right) * 0.5f;
        float contentCy = (b.top + b.bottom) * 0.5f;

        // screen = zoom * canvas + pan, so pan = screenCenter - zoom * canvasCenter.
        float panX = vpW * 0.5f - zoom * contentCx;
        float panY = vpH * 0.5f - zoom * contentCy;

        m_nodeGraphController.SetZoom(zoom);
        m_nodeGraphController.SetPanOffset(panX, panY);
    }

    winrt::fire_and_forget MainWindow::SaveImageAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileSavePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);

        // Use the previewed node's name as suggested filename.
        auto* previewNode = m_graph.FindNode(m_previewNodeId);
        std::wstring suggestedName = previewNode ? previewNode->name : L"output";
        // Sanitize for filename
        for (auto& ch : suggestedName)
            if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|')
                ch = L'_';
        picker.SuggestedFileName(winrt::hstring(suggestedName));
        picker.FileTypeChoices().Insert(L"JPEG XR (HDR)", winrt::single_threaded_vector<winrt::hstring>({ L".jxr" }));
        picker.FileTypeChoices().Insert(L"PNG Image (SDR)", winrt::single_threaded_vector<winrt::hstring>({ L".png" }));

        auto file = co_await picker.PickSaveFileAsync();
        if (!file) co_return;

        auto* previewImage = ResolveDisplayImage(m_previewNodeId);
        if (!previewImage)
        {
            PipelineFormatText().Text(L"Error: No image to save");
            co_return;
        }

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) co_return;

        try
        {
            // Reset DPI/transform to ensure clean bounds measurement.
            float oldDpiX, oldDpiY;
            dc->GetDpi(&oldDpiX, &oldDpiY);
            dc->SetDpi(96.0f, 96.0f);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());

            // Get image bounds to determine size.
            D2D1_RECT_F bounds{};
            dc->GetImageLocalBounds(previewImage, &bounds);
            uint32_t w = static_cast<uint32_t>(bounds.right - bounds.left);
            uint32_t h = static_cast<uint32_t>(bounds.bottom - bounds.top);

            dc->SetDpi(oldDpiX, oldDpiY);
            if (w == 0 || h == 0) co_return;

            auto fileExt = std::wstring(file.FileType().c_str());
            bool isJxr = (fileExt == L".jxr" || fileExt == L".wdp");

            // JXR: render in FP16 scRGB for full HDR fidelity.
            // PNG: render in 8-bit BGRA (SDR clamp).
            DXGI_FORMAT renderFormat = isJxr
                ? DXGI_FORMAT_R16G16B16A16_FLOAT
                : DXGI_FORMAT_B8G8R8A8_UNORM;
            D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

            winrt::com_ptr<ID2D1Bitmap1> renderBitmap;
            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(renderFormat, alphaMode));
            dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bmpProps, renderBitmap.put());

            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            dc->SetTarget(renderBitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0, 0, 1.0f));
            dc->DrawImage(previewImage);
            dc->EndDraw();
            dc->SetTarget(oldTarget.get());

            // WIC encode.
            winrt::com_ptr<IWICImagingFactory> wicFactory;
            winrt::check_hresult(CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wicFactory.put())));

            auto filePath = std::wstring(file.Path().c_str());

            GUID containerFormat = isJxr ? GUID_ContainerFormatWmp : GUID_ContainerFormatPng;

            winrt::com_ptr<IWICStream> stream;
            winrt::check_hresult(wicFactory->CreateStream(stream.put()));
            winrt::check_hresult(stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE));

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(wicFactory->CreateEncoder(containerFormat, nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::com_ptr<IPropertyBag2> encoderOptions;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), encoderOptions.put()));

            if (isJxr && encoderOptions)
            {
                // Set lossless compression for JXR.
                PROPBAG2 option{};
                option.pstrName = const_cast<LPOLESTR>(L"Lossless");
                VARIANT val{};
                val.vt = VT_BOOL;
                val.boolVal = VARIANT_TRUE;
                encoderOptions->Write(1, &option, &val);
            }

            winrt::check_hresult(frame->Initialize(encoderOptions.get()));
            winrt::check_hresult(frame->SetSize(w, h));

            WICPixelFormatGUID pixelFormat = isJxr
                ? GUID_WICPixelFormat64bppRGBAHalf
                : GUID_WICPixelFormat32bppBGRA;
            winrt::check_hresult(frame->SetPixelFormat(&pixelFormat));

            // Set scRGB color context for JXR so readers know the color space.
            if (isJxr)
            {
                winrt::com_ptr<IWICColorContext> colorCtx;
                winrt::check_hresult(wicFactory->CreateColorContext(colorCtx.put()));
                winrt::check_hresult(colorCtx->InitializeFromExifColorSpace(1)); // sRGB family
                IWICColorContext* ctxArray[] = { colorCtx.get() };
                frame->SetColorContexts(1, ctxArray);
            }

            // Read back pixels from GPU.
            winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
            D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(renderFormat, alphaMode));
            dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, cpuProps, cpuBitmap.put());
            D2D1_POINT_2U destPoint = { 0, 0 };
            D2D1_RECT_U srcRect = { 0, 0, w, h };
            cpuBitmap->CopyFromBitmap(&destPoint, renderBitmap.get(), &srcRect);

            D2D1_MAPPED_RECT mapped{};
            winrt::check_hresult(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped));
            winrt::check_hresult(frame->WritePixels(h, mapped.pitch, mapped.pitch * h, mapped.bits));
            cpuBitmap->Unmap();

            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());

            auto sizeKB = std::filesystem::file_size(filePath) / 1024;
            auto msg = isJxr
                ? L"Saved scRGB FP16 JXR (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L", " + std::to_wstring(sizeKB) + L" KB): " + file.Name()
                : L"Saved PNG (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L"): " + file.Name();
            PipelineFormatText().Text(msg);
        }
        catch (...)
        {
            PipelineFormatText().Text(L"Error: Failed to save image");
        }
    }

    // -----------------------------------------------------------------------
    // Preview pan/zoom
    // -----------------------------------------------------------------------

    void MainWindow::FitPreviewToView()
    {
        auto vp = PreviewViewportDips();
        float vpW = vp.width;
        float vpH = vp.height;
        if (vpW <= 0 || vpH <= 0)
        {
            m_previewZoom = 1.0f;
            m_previewPanX = 0.0f;
            m_previewPanY = 0.0f;
            return;
        }

        auto bounds = GetPreviewImageBounds();
        float imgW = bounds.right - bounds.left;
        float imgH = bounds.bottom - bounds.top;

        // For infinite or very large images (e.g., Flood), use a default view.
        if (imgW <= 0 || imgH <= 0 || imgW > 100000.0f || imgH > 100000.0f)
        {
            m_previewZoom = 1.0f;
            m_previewPanX = 0.0f;
            m_previewPanY = 0.0f;
            return;
        }

        // Scale to fit with some padding.
        float scaleX = vpW / imgW;
        float scaleY = vpH / imgH;
        m_previewZoom = (std::min)(scaleX, scaleY) * 0.95f;

        // Center the image.
        m_previewPanX = (vpW - imgW * m_previewZoom) * 0.5f - bounds.left * m_previewZoom;
        m_previewPanY = (vpH - imgH * m_previewZoom) * 0.5f - bounds.top * m_previewZoom;
    }

    // -----------------------------------------------------------------------
    // Render loop -- methods (OnRenderTick, RenderFrame) moved to
    // MainWindow.RenderTick.cpp (Phase 4 split). Members of same class.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Output windows
    // -----------------------------------------------------------------------

    void MainWindow::OpenOutputWindow(uint32_t nodeId)
    {
        // Don't open duplicates.
        for (const auto& w : m_outputWindows)
        {
            if (w->NodeId() == nodeId && w->IsOpen())
                return;
        }

        auto* node = m_graph.FindNode(nodeId);
        if (!node) return;

        auto window = std::make_unique<::ShaderLab::Controls::OutputWindow>();
        window->Create(
            m_renderEngine.D3DDevice(),
            m_renderEngine.D2DDeviceContext(),
            m_renderEngine.DXGIFactory(),
            nodeId,
            node->name,
            m_renderEngine.ActiveFormat());

        m_outputWindows.push_back(std::move(window));
    }

    void MainWindow::CloseOutputWindow(uint32_t nodeId)
    {
        std::erase_if(m_outputWindows, [nodeId](const auto& w)
        {
            return w->NodeId() == nodeId;
        });
    }

    void MainWindow::PresentOutputWindows()
    {
        if (m_outputWindows.empty())
            return;

        // Remove closed windows and their corresponding graph nodes.
        std::vector<uint32_t> closedNodeIds;
        std::erase_if(m_outputWindows, [&closedNodeIds](const auto& w) {
            if (!w->IsOpen()) { closedNodeIds.push_back(w->NodeId()); return true; }
            return false;
        });
        for (uint32_t nodeId : closedNodeIds)
        {
            m_graph.RemoveNode(nodeId);
            m_graphEvaluator.InvalidateNode(nodeId);
            // The deleted node owned a cachedOutput pointer that downstream
            // nodes may have inherited via cached effect chains. Be paranoid:
            // null every node's cachedOutput so the next evaluate rebuilds them.
            for (auto& n : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
            {
                n.cachedOutput = nullptr;
                n.dirty = true;
            }
            m_nodeGraphController.RebuildLayout();
            PopulatePreviewNodeSelector();
        }
        if (!closedNodeIds.empty())
        {
            // Force the next render tick so the graph panel repaints without
            // the deleted Output node (otherwise the tick gate would skip
            // rendering since no nodes are dirty and m_outputWindows is now
            // smaller/empty).
            m_forceRender = true;
            m_nodeGraphController.SetNeedsRedraw();
            MarkUnsaved();
        }

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        auto& ft = m_frameTiming;
        std::wstring timingStr = std::format(L"{:.1f}ms (eval {:.1f} + compute {:.1f} + draw {:.1f})",
            ft.totalUs / 1000.0, ft.evaluateUs / 1000.0,
            ft.deferredComputeUs / 1000.0, ft.drawUs / 1000.0 + ft.presentUs / 1000.0);

        for (auto& window : m_outputWindows)
        {
            if (!window->IsReady())
                continue;

            // Sync window title with node name.
            auto* node = m_graph.FindNode(window->NodeId());
            if (node)
                window->SetTitle(node->name);
            window->SetTimingText(timingStr);

            auto* image = ResolveDisplayImage(window->NodeId());
            window->Present(dc, image);
        }
    }

    void MainWindow::OpenLogWindow(uint32_t nodeId)
    {
        // Check if already open.
        for (auto& w : m_logWindows)
        {
            if (w->NodeId() == nodeId && w->IsOpen())
                return;
        }

        auto* node = m_graph.FindNode(nodeId);
        if (!node) return;

        auto window = std::make_unique<::ShaderLab::Controls::LogWindow>();
        window->Create(nodeId, node->name);

        // Populate with existing log entries.
        auto it = m_nodeLogs.find(nodeId);
        if (it != m_nodeLogs.end())
            window->Update(it->second);

        m_logWindows.push_back(std::move(window));
    }

    void MainWindow::UpdateLogWindows()
    {
        // Remove closed windows.
        m_logWindows.erase(
            std::remove_if(m_logWindows.begin(), m_logWindows.end(),
                [](const auto& w) { return !w->IsOpen(); }),
            m_logWindows.end());

        // Update open windows with new log entries.
        for (auto& w : m_logWindows)
        {
            auto it = m_nodeLogs.find(w->NodeId());
            if (it != m_nodeLogs.end())
                w->Update(it->second);
        }
    }

    void MainWindow::UpdateFpsTooltip()
    {
        // Refresh the TextBlock inside the FPS counter's tooltip with a
        // fresh per-phase breakdown. The TextBlock's Text property is
        // observable, so updating it while the tooltip is open re-renders
        // in place -- giving the user a real-time view of where each
        // millisecond is going. Sub-phases sum to <= totalUs (= 1000/fps);
        // the remainder is dispatcher idle / OS overhead between ticks.
        if (!FpsTooltipText()) return;
        const auto& ft = m_frameTiming;
        double fps = m_lastFps;
        double total = ft.totalUs / 1000.0;
        double sumPhases =
            ft.videoTickUs / 1000.0 +
            ft.sourcesPrepUs / 1000.0 + ft.evaluateUs / 1000.0 +
            ft.deferredComputeUs / 1000.0 +
            ft.drawUs / 1000.0 + ft.presentUs / 1000.0 +
            ft.nodeGraphUs / 1000.0 +
            ft.outputWindowsUs / 1000.0 +
            ft.traceUs / 1000.0;
        double idle = (std::max)(0.0, total - sumPhases);

        std::wstring text = std::format(
            L"{:.0f} FPS  ({:.1f} ms total)\n"
            L"\n"
            L"  video tick    {:>6.2f} ms\n"
            L"  sources prep  {:>6.2f} ms\n"
            L"  eval          {:>6.2f} ms\n"
            L"  compute       {:>6.2f} ms  ({} dispatch{})\n"
            L"  draw          {:>6.2f} ms\n"
            L"  present       {:>6.2f} ms\n"
            L"  node graph    {:>6.2f} ms\n"
            L"  output wins   {:>6.2f} ms\n"
            L"  pixel trace   {:>6.2f} ms\n"
            L"  --------------------------\n"
            L"  sum           {:>6.2f} ms\n"
            L"  idle / sched  {:>6.2f} ms",
            fps, total,
            ft.videoTickUs / 1000.0,
            ft.sourcesPrepUs / 1000.0,
            ft.evaluateUs / 1000.0,
            ft.deferredComputeUs / 1000.0, ft.computeDispatches,
                (ft.computeDispatches == 1 ? L"" : L"es"),
            ft.drawUs / 1000.0,
            ft.presentUs / 1000.0,
            ft.nodeGraphUs / 1000.0,
            ft.outputWindowsUs / 1000.0,
            ft.traceUs / 1000.0,
            sumPhases,
            idle);

        if (m_lastVideoFps > 0.1f)
            text += std::format(L"\n\n  video decode  {:>6.0f} fps", m_lastVideoFps);

        FpsTooltipText().Text(winrt::hstring(text));
    }
}
