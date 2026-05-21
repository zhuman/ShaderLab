#pragma once

#include "pch.h"
#include "../Graph/EffectGraph.h"
#include "../Effects/EffectRegistry.h"
#include "../Rendering/RenderThreadDispatcher.h"

namespace ShaderLab::Controls
{
    // Visual representation of a node for hit-testing and rendering.
    struct NodeVisual
    {
        uint32_t nodeId{ 0 };
        D2D1_RECT_F bounds{};           // Bounding rectangle on the canvas
        float headerHeight{ 28.0f };    // Height of the title bar
        float pinSpacing{ 20.0f };      // Vertical spacing between pins

        // Image pin positions (center of the pin circle).
        std::vector<D2D1_POINT_2F> inputPinPositions;
        std::vector<D2D1_POINT_2F> outputPinPositions;

        // Data pin positions and names (for property bindings).
        std::vector<D2D1_POINT_2F> dataInputPinPositions;
        std::vector<D2D1_POINT_2F> dataOutputPinPositions;
        std::vector<std::wstring>  dataInputPinNames;    // "PropName (type)"
        std::vector<std::wstring>  dataOutputPinNames;   // field names (for binding lookup)
        std::vector<std::wstring>  dataInputPinLabels;   // "PropName (type)" for display
        std::vector<std::wstring>  dataOutputPinLabels;  // "FieldName (type)" for display

        // Parameter node inline control.
        bool isParameterNode{ false };
        bool isClockNode{ false };
        D2D1_RECT_F sliderRect{};       // Slider track rect (for hit-test)
        D2D1_RECT_F playButtonRect{};   // Play/pause button rect (clock nodes)
    };

    // Describes a pending connection drag operation.
    struct ConnectionDrag
    {
        bool     active{ false };
        uint32_t sourceNodeId{ 0 };
        uint32_t sourcePin{ 0 };
        bool     fromOutput{ true };    // true = dragging from an output pin
        bool     isDataPin{ false };    // true = data pin (property binding)
        D2D1_POINT_2F currentPos{};     // Current mouse/pointer position
    };

    // Selection state.
    struct SelectionState
    {
        std::unordered_set<uint32_t> selectedNodeIds;
        bool isDragging{ false };
        D2D1_POINT_2F dragStart{};
        D2D1_POINT_2F dragOffset{};     // Accumulated offset during drag
    };

    // Node graph editor controller — manages the visual layout, interaction,
    // and rendering of an EffectGraph as a canvas-based node editor.
    //
    // Responsibilities:
    //   - Layout nodes on a 2D canvas with positions from EffectNode::position
    //   - Hit-test nodes, pins, and edges for pointer interaction
    //   - Manage node selection, multi-select, and drag-move
    //   - Handle connection creation/deletion via pin drag
    //   - Provide D2D rendering commands for node/edge visualization
    //   - Auto-layout for newly added nodes
    //
    // The controller does NOT own the EffectGraph or the D2D device context.
    // The host (MainWindow) provides these and calls Render() each frame.
    class NodeGraphController
    {
    public:
        NodeGraphController() = default;

        // Bind to an effect graph. Must be called before any interaction.
        void SetGraph(Graph::EffectGraph* graph);

        // Bind the render-thread dispatcher. All graph-mutating calls
        // (Connect/Disconnect/AddNode/RemoveNode/BindProperty etc.) route
        // through this so they execute on whichever thread owns rendering.
        // Until the worker thread spawns (Phase 7), the dispatcher runs in
        // synchronous mode and closures execute inline -- equivalent to
        // calling m_graph->X() directly. Setting nullptr falls back to the
        // direct-call path (e.g. tests that don't have a dispatcher).
        void SetDispatcher(::ShaderLab::Rendering::RenderThreadDispatcher* d) { m_dispatcher = d; }

        // ---- Layout ----

        // Rebuild visual layout from the current graph state.
        void RebuildLayout();

        // Auto-arrange nodes left-to-right by topological depth,
        // minimizing edge crossings. Called when nodes/edges change.
        void AutoLayout();

        // Set canvas pan offset (scrolling).
        void SetPanOffset(float x, float y);
        D2D1_POINT_2F PanOffset() const { return m_panOffset; }

        // Set canvas zoom level (1.0 = 100%).
        void SetZoom(float zoom);
        float Zoom() const { return m_zoom; }

        // Inform the controller of the current visible viewport size in pixels.
        // Used to place auto-positioned new nodes in the user's current view.
        void SetViewportSize(float w, float h) { m_viewportW = w; m_viewportH = h; }

        // Returns the axis-aligned bounding box of all node visuals in canvas
        // (pre-pan/zoom) space. Returns {0,0,0,0} when no nodes are laid out.
        // Caller is responsible for calling RebuildLayout() first if the graph
        // may have changed since the last layout.
        D2D1_RECT_F ContentBounds() const;

        // ---- Interaction ----

        // Hit-test a canvas point. Returns the node ID under the point, or 0.
        uint32_t HitTestNode(D2D1_POINT_2F canvasPoint) const;

        // Hit-test for a pin. Returns true and fills nodeId/pinIndex/isOutput/isDataPin.
        bool HitTestPin(D2D1_POINT_2F canvasPoint,
                        uint32_t& nodeId, uint32_t& pinIndex, bool& isOutput,
                        bool& isDataPin) const;

        // Hit-test for inline slider on parameter nodes. Returns node ID or 0.
        uint32_t HitTestSlider(D2D1_POINT_2F canvasPoint) const;

        // Hit-test for play/pause button on clock nodes. Returns node ID or 0.
        uint32_t HitTestPlayButton(D2D1_POINT_2F canvasPoint) const;

        // Result of HitTestEdge.
        struct EdgeHit
        {
            bool found{ false };
            bool isDataBinding{ false };
            // Image edge fields (when !isDataBinding).
            uint32_t sourceNodeId{ 0 };
            uint32_t sourcePin{ 0 };
            uint32_t destNodeId{ 0 };
            uint32_t destPin{ 0 };
            // Data binding fields (when isDataBinding).
            std::wstring destPropertyName;
            std::wstring sourceFieldName;  // for unique-source disambiguation
        };

        // Hit-test for an edge (image or data binding) under the given canvas
        // point. Returns the closest hit within tolerance, or {found=false}.
        EdgeHit HitTestEdge(D2D1_POINT_2F canvasPoint, float tolerance = 6.0f) const;

        // Remove the given edge from the graph. Returns true on success.
        bool RemoveEdge(const EdgeHit& hit);

        // Update slider value from canvas X position during drag.
        // Returns true if value changed.
        bool UpdateSliderDrag(uint32_t nodeId, D2D1_POINT_2F canvasPoint);

        // Check if a node is a parameter node (no HLSL, data output only).
        bool IsParameterNode(uint32_t nodeId) const;

        // Begin dragging selected nodes.
        void BeginDragNodes(D2D1_POINT_2F startPoint);
        void UpdateDragNodes(D2D1_POINT_2F currentPoint);
        void EndDragNodes();

        // Begin dragging a connection from a pin.
        void BeginConnection(uint32_t nodeId, uint32_t pinIndex, bool fromOutput, bool isDataPin = false);
        void UpdateConnection(D2D1_POINT_2F currentPoint);
        bool EndConnection(D2D1_POINT_2F dropPoint);
        void CancelConnection();

        // ---- Selection ----

        void SelectNode(uint32_t nodeId, bool addToSelection = false);
        void DeselectNode(uint32_t nodeId);
        void DeselectAll();
        void SelectAll();
        const std::unordered_set<uint32_t>& SelectedNodes() const { return m_selection.selectedNodeIds; }

        // Delete all selected nodes and their edges.
        void DeleteSelected();

        // ---- Node operations ----

        // Add a node at a canvas position (auto-positioned if pos is zero).
        uint32_t AddNode(Graph::EffectNode node, D2D1_POINT_2F canvasPos);

        // Add an output node if one doesn't exist.
        uint32_t EnsureOutputNode();

        // ---- Rendering ----

        // Render the node graph to a D2D device context.
        // The caller should set up the transform for pan/zoom.
        void Render(ID2D1DeviceContext* dc, D2D1_SIZE_F viewportSize);

        // ---- Constants ----

        static constexpr float NodeWidth = 180.0f;
        static constexpr float PinRadius = 6.0f;
        static constexpr float NodeCornerRadius = 6.0f;

        // Optional callback for logging connection events.
        // Args: srcNodeId, srcPin, dstNodeId, dstPin, isDataBinding
        using ConnectionCallback = std::function<void(uint32_t, uint32_t, uint32_t, uint32_t, bool)>;
        void SetConnectionCallback(ConnectionCallback cb) { m_connectionCallback = std::move(cb); }

    private:
        // Convert screen point to canvas point (accounting for pan/zoom).
        D2D1_POINT_2F ScreenToCanvas(D2D1_POINT_2F screenPoint) const;
        D2D1_POINT_2F CanvasToScreen(D2D1_POINT_2F canvasPoint) const;

        // Compute the visual bounds for a node.
        NodeVisual ComputeNodeVisual(const Graph::EffectNode& node) const;

        // Rendering helpers.
        void RenderEdges(ID2D1DeviceContext* dc);
        void RenderNodes(ID2D1DeviceContext* dc);
        void RenderConnectionDrag(ID2D1DeviceContext* dc);

        // Color helpers for node types.
        static D2D1_COLOR_F NodeColor(Graph::NodeType type);
        static D2D1_COLOR_F NodeHeaderColor(Graph::NodeType type);

        Graph::EffectGraph* m_graph{ nullptr };
        ::ShaderLab::Rendering::RenderThreadDispatcher* m_dispatcher{ nullptr };
        ConnectionCallback m_connectionCallback;

        // Cached visual layout.
        std::unordered_map<uint32_t, NodeVisual> m_visuals;

        // Interaction state.
        SelectionState m_selection;
        ConnectionDrag m_connectionDrag;

        // Canvas transform.
        D2D1_POINT_2F m_panOffset{ 0.0f, 0.0f };
        float m_zoom{ 1.0f };

        // Auto-layout position for new nodes.
        float m_nextAutoX{ 50.0f };
        float m_nextAutoY{ 50.0f };
        int   m_autoStaggerCount{ 0 };

        // Visible viewport size in pixels (set by host on init/resize).
        float m_viewportW{ 800.0f };
        float m_viewportH{ 600.0f };

        // Cached D2D resources (created on first Render call).
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushNode;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushHeader;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushEdge;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushPin;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushSelection;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushText;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushDataPin;     // Orange for data pins
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushDataEdge;    // Orange for data edges
        winrt::com_ptr<IDWriteTextFormat>     m_textFormat;
        winrt::com_ptr<IDWriteTextFormat>     m_pinLabelFormat;   // Small text for pin labels
        bool m_resourcesCreated{ false };
        bool m_needsRedraw{ true }; // Set when graph topology/selection/layout changes

        void EnsureResources(ID2D1DeviceContext* dc);

    public:
        void SetNeedsRedraw() { m_needsRedraw = true; }

        // Release cached D2D resources (call after GPU device change).
        void ReleaseDeviceResources();
        bool NeedsRedraw() const { return m_needsRedraw; }
    };
}
