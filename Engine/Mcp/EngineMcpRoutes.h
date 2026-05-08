#pragma once

// Engine MCP routes — engine-side route handlers for the MCP server.
//
// Phase 7 architecture: the McpHttpServer runs in the engine DLL.
// The GUI app and headless host both instantiate it and register a
// mix of routes:
//
//   * Engine-pure routes (graph mutation, render capture, pixel
//     readback, image stats, etc) live in this TU and are registered
//     by both apps via `RegisterEngineRoutes`.
//
//   * UI-coupled routes (graph_snapshot of the canvas, preview pan/zoom,
//     graph view tools) stay in `MainWindow.McpRoutes.cpp` because
//     they touch WinUI controls that don't exist headless.
//
// Routes execute through `IEngineCommandSink::Dispatch`, which marshals
// the closure to the right thread for the host:
//   * GUI app: dispatches to the UI thread via DispatcherQueue (so
//     concurrent UI tick and MCP requests don't race the graph).
//   * Headless host: synchronous direct call (single-threaded MCP
//     access; the listener thread serializes requests).

#include "pch_engine.h"
#include "../../EngineExport.h"
#include "../../Rendering/DisplayProfile.h"
#include "McpHttpServer.h"

#include <functional>
#include <optional>

namespace ShaderLab::Graph
{
    class EffectGraph;
}
namespace ShaderLab::Rendering
{
    class GraphEvaluator;
    class DisplayMonitor;
}
namespace ShaderLab::Effects
{
    class SourceNodeFactory;
}

namespace ShaderLab::Mcp
{
    // Bag of references the engine routes need. Built fresh by the host
    // each time Dispatch fires so the references are valid for the
    // closure's lifetime. Members are non-owning.
    struct EngineContext
    {
        Graph::EffectGraph*           graph{ nullptr };
        Rendering::GraphEvaluator*    evaluator{ nullptr };
        Rendering::DisplayMonitor*    displayMonitor{ nullptr };
        Effects::SourceNodeFactory*   sourceFactory{ nullptr };
        ID2D1DeviceContext*           dc{ nullptr };
        ID3D11Device*                 d3dDevice{ nullptr };
        ID3D11DeviceContext*          d3dContext{ nullptr };

        // Force a fresh evaluation of the graph if the host has any
        // dirty propagation / tick logic. Headless: no-op (caller did
        // this already). GUI: calls MainWindow::RenderFrame so dirty
        // nodes are repopulated before readback / capture.
        // Returning void; cannot fail.
        std::function<void()>         renderFrame;

        // Host-specific "preview node" id for `GET /graph` responses.
        // GUI returns m_previewNodeId; headless returns 0 (no preview pane).
        // Optional — if unset, routes that surface previewNodeId default
        // to 0.
        std::function<uint32_t()>     getPreviewNodeId;

        // Optional host-state shim for the most-recently-loaded ICC
        // profile (so it can appear under "loadedIcc" in the
        // /display/profiles response and be re-applied through the
        // profile dropdown without re-parsing the file). The GUI
        // installs both; headless leaves both null.
        std::function<std::optional<Rendering::DisplayProfile>()> getLoadedIccProfile;
        std::function<void(const Rendering::DisplayProfile&)>     setLoadedIccProfile;
    };

    // Functional / closure-based command sink (Q4 architecture choice).
    // The route handler hands a closure to Dispatch; the host runs it
    // on the right thread and returns the McpHttpServer::Response back
    // to the listener thread that the route returns on.
    //
    // Engine state mutations also fire **events** (the OnXxx virtuals
    // below). Hosts override them to keep their own UI / output windows
    // / preview selector in sync — mirroring the path the user's own
    // UI interactions take. Events fire **inside** Dispatch (i.e., on
    // the dispatch thread) immediately after the closure mutates engine
    // state, so the GUI sees changes synchronously and the engine route
    // returns to the listener with the UI already up-to-date.
    class SHADERLAB_API IEngineCommandSink
    {
    public:
        virtual ~IEngineCommandSink() = default;

        // Run `closure` on whichever thread is appropriate for this
        // host. Closure receives a freshly-built EngineContext.
        // Synchronous: the calling thread blocks until the closure
        // completes. Closure exceptions propagate.
        virtual McpHttpServer::Response Dispatch(
            std::function<McpHttpServer::Response(EngineContext&)> closure) = 0;

        // ---- Engine state-change events (UI hooks) -----------------------
        //
        // Routes call these AFTER successful engine mutation, from
        // inside the Dispatch closure. Default impls are no-ops; the
        // headless host doesn't override them. The GUI host wires them
        // to the same methods that handle native user UI interactions
        // (e.g. OnNodeAdded -> PopulatePreviewNodeSelector + AutoLayout,
        // matching what happens when the user adds a node via the
        // toolbar).
        virtual void OnNodeAdded(uint32_t /*nodeId*/) {}
        virtual void OnNodeRemoved(uint32_t /*nodeId*/) {}
        virtual void OnNodeChanged(uint32_t /*nodeId*/) {}
        virtual void OnGraphCleared() {}
        virtual void OnGraphLoaded() {}
        virtual void OnGraphStructureChanged() {}  // edges added/removed

        // Custom effect HLSL recompiled: shader bytecode + parameters
        // changed. GUI rebuilds the canvas layout (parameter pins may
        // have changed), enforces unique effect names, and refreshes
        // the Add Node flyout. Default = no-op (headless).
        virtual void OnCustomEffectRecompiled(uint32_t /*nodeId*/) {}

        // Active display profile changed: simulated / live / ICC. GUI
        // marks all nodes dirty, refreshes the status bar and profile
        // selector to match. Headless = no-op. The route already runs
        // Rendering::UpdateWorkingSpaceNodes inside its closure so
        // Working Space nodes are sync'd before this fires.
        virtual void OnDisplayProfileChanged() {}
    };

    // Register all engine-pure routes on the given server. Idempotent:
    // call once per process. The sink must outlive the server (handlers
    // capture it by reference).
    SHADERLAB_API void RegisterEngineRoutes(
        McpHttpServer& server,
        IEngineCommandSink& sink);
}
