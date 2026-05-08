#include "pch.h"
#include "NodeGraphController.h"

namespace ShaderLab::Controls
{
    // -----------------------------------------------------------------------
    // Graph binding and layout
    // -----------------------------------------------------------------------

    void NodeGraphController::SetGraph(Graph::EffectGraph* graph)
    {
        m_graph = graph;
        if (m_graph)
            RebuildLayout();
    }

    void NodeGraphController::RebuildLayout()
    {
        m_visuals.clear();
        if (!m_graph) return;

        for (const auto& node : m_graph->Nodes())
        {
            m_visuals[node.id] = ComputeNodeVisual(node);
        }
        m_needsRedraw = true;
    }

    void NodeGraphController::AutoLayout()
    {
        if (!m_graph || m_graph->IsEmpty()) return;

        // 1. Compute topological depth for each node (longest path from sources).
        //    Consider both image edges AND property bindings as dependencies.
        std::unordered_map<uint32_t, int> depth;
        std::vector<uint32_t> topoOrder;
        try { topoOrder = m_graph->TopologicalSort(); }
        catch (...) { return; } // Cycle — can't layout.

        // Helper: collect all upstream node IDs for a given node (image edges + bindings).
        auto getParentIds = [&](uint32_t nodeId) -> std::vector<uint32_t>
        {
            std::unordered_set<uint32_t> parents;
            // Image edges.
            for (const auto* edge : m_graph->GetInputEdges(nodeId))
                parents.insert(edge->sourceNodeId);
            // Property bindings.
            const auto* node = m_graph->FindNode(nodeId);
            if (node)
            {
                for (const auto& [propName, binding] : node->propertyBindings)
                {
                    if (binding.wholeArray)
                    {
                        if (binding.wholeArraySourceNodeId != 0)
                            parents.insert(binding.wholeArraySourceNodeId);
                    }
                    else
                    {
                        for (const auto& src : binding.sources)
                            if (src && src->sourceNodeId != 0)
                                parents.insert(src->sourceNodeId);
                    }
                }
            }
            return { parents.begin(), parents.end() };
        };

        for (uint32_t id : topoOrder)
        {
            int maxParentDepth = -1;
            for (uint32_t parentId : getParentIds(id))
            {
                auto it = depth.find(parentId);
                if (it != depth.end())
                    maxParentDepth = (std::max)(maxParentDepth, it->second);
            }
            depth[id] = maxParentDepth + 1;
        }

        // 2. Group nodes by depth column.
        int maxDepth = 0;
        for (auto& [id, d] : depth)
            maxDepth = (std::max)(maxDepth, d);

        std::vector<std::vector<uint32_t>> columns(maxDepth + 1);
        for (uint32_t id : topoOrder)
            columns[depth[id]].push_back(id);

        // 3. Order nodes within each column to minimize edge crossings.
        //    Heuristic: sort by the average Y position of connected upstream nodes.
        for (int col = 1; col <= maxDepth; ++col)
        {
            auto& colNodes = columns[col];
            std::sort(colNodes.begin(), colNodes.end(), [&](uint32_t a, uint32_t b)
            {
                auto avgParentY = [&](uint32_t nodeId) -> float
                {
                    float sum = 0; int count = 0;
                    for (uint32_t parentId : getParentIds(nodeId))
                    {
                        auto* parent = m_graph->FindNode(parentId);
                        if (parent) { sum += parent->position.y; ++count; }
                    }
                    return count > 0 ? sum / count : 0.0f;
                };
                return avgParentY(a) < avgParentY(b);
            });
        }

        // 4. Assign positions: columns left-to-right, using actual node heights.
        constexpr float colSpacing = 250.0f;
        constexpr float rowGap = 30.0f;  // gap between nodes
        constexpr float startX = 60.0f;
        constexpr float startY = 60.0f;

        for (int col = 0; col <= maxDepth; ++col)
        {
            float x = startX + col * colSpacing;
            float y = startY;
            for (uint32_t id : columns[col])
            {
                auto* node = m_graph->FindNode(id);
                if (node)
                {
                    node->position = { x, y };
                    // Compute actual height for spacing.
                    auto v = ComputeNodeVisual(*node);
                    float nodeHeight = v.bounds.bottom - v.bounds.top;
                    y += nodeHeight + rowGap;
                }
            }
        }

        // 5. Center columns vertically relative to the tallest column.
        // First compute actual column heights.
        float maxColHeight = 0;
        for (int col = 0; col <= maxDepth; ++col)
        {
            float colH = 0;
            for (uint32_t id : columns[col])
            {
                auto* node = m_graph->FindNode(id);
                if (node)
                {
                    auto v = ComputeNodeVisual(*node);
                    colH += (v.bounds.bottom - v.bounds.top) + rowGap;
                }
            }
            maxColHeight = (std::max)(maxColHeight, colH);
        }

        for (int col = 0; col <= maxDepth; ++col)
        {
            float colH = 0;
            for (uint32_t id : columns[col])
            {
                auto* node = m_graph->FindNode(id);
                if (node)
                {
                    auto v = ComputeNodeVisual(*node);
                    colH += (v.bounds.bottom - v.bounds.top) + rowGap;
                }
            }
            float offset = (maxColHeight - colH) * 0.5f;
            for (uint32_t id : columns[col])
            {
                auto* node = m_graph->FindNode(id);
                if (node)
                    node->position.y += offset;
            }
        }

        RebuildLayout();
    }

    void NodeGraphController::SetPanOffset(float x, float y)
    {
        m_panOffset = { x, y };
        m_needsRedraw = true;
    }

    void NodeGraphController::SetZoom(float zoom)
    {
        m_zoom = (std::max)(0.1f, (std::min)(zoom, 5.0f));
        m_needsRedraw = true;
    }

    D2D1_RECT_F NodeGraphController::ContentBounds() const
    {
        if (m_visuals.empty())
            return D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);

        bool first = true;
        D2D1_RECT_F acc{};
        for (const auto& [id, v] : m_visuals)
        {
            const auto& b = v.bounds;
            if (b.right <= b.left || b.bottom <= b.top) continue;
            if (first)
            {
                acc = b;
                first = false;
            }
            else
            {
                acc.left   = (std::min)(acc.left,   b.left);
                acc.top    = (std::min)(acc.top,    b.top);
                acc.right  = (std::max)(acc.right,  b.right);
                acc.bottom = (std::max)(acc.bottom, b.bottom);
            }
        }
        if (first)
            return D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
        return acc;
    }

    // -----------------------------------------------------------------------
    // Hit testing
    // -----------------------------------------------------------------------

    uint32_t NodeGraphController::HitTestNode(D2D1_POINT_2F canvasPoint) const
    {
        // unordered_map has no ordering — just check all nodes.
        uint32_t hitId = 0;
        for (const auto& [id, v] : m_visuals)
        {
            if (canvasPoint.x >= v.bounds.left && canvasPoint.x <= v.bounds.right &&
                canvasPoint.y >= v.bounds.top && canvasPoint.y <= v.bounds.bottom)
            {
                hitId = id;
            }
        }
        return hitId;
    }

    bool NodeGraphController::HitTestPin(
        D2D1_POINT_2F canvasPoint,
        uint32_t& nodeId, uint32_t& pinIndex, bool& isOutput, bool& isDataPin) const
    {
        constexpr float hitRadius = PinRadius * 2.5f;

        for (const auto& [id, visual] : m_visuals)
        {
            // Check image output pins.
            for (uint32_t i = 0; i < visual.outputPinPositions.size(); ++i)
            {
                auto& p = visual.outputPinPositions[i];
                float dx = canvasPoint.x - p.x;
                float dy = canvasPoint.y - p.y;
                if (dx * dx + dy * dy <= hitRadius * hitRadius)
                {
                    nodeId = id; pinIndex = i; isOutput = true; isDataPin = false;
                    return true;
                }
            }
            // Check image input pins.
            for (uint32_t i = 0; i < visual.inputPinPositions.size(); ++i)
            {
                auto& p = visual.inputPinPositions[i];
                float dx = canvasPoint.x - p.x;
                float dy = canvasPoint.y - p.y;
                if (dx * dx + dy * dy <= hitRadius * hitRadius)
                {
                    nodeId = id; pinIndex = i; isOutput = false; isDataPin = false;
                    return true;
                }
            }
            // Check data output pins.
            for (uint32_t i = 0; i < visual.dataOutputPinPositions.size(); ++i)
            {
                auto& p = visual.dataOutputPinPositions[i];
                float dx = canvasPoint.x - p.x;
                float dy = canvasPoint.y - p.y;
                if (dx * dx + dy * dy <= hitRadius * hitRadius)
                {
                    nodeId = id; pinIndex = i; isOutput = true; isDataPin = true;
                    return true;
                }
            }
            // Check data input pins.
            for (uint32_t i = 0; i < visual.dataInputPinPositions.size(); ++i)
            {
                auto& p = visual.dataInputPinPositions[i];
                float dx = canvasPoint.x - p.x;
                float dy = canvasPoint.y - p.y;
                if (dx * dx + dy * dy <= hitRadius * hitRadius)
                {
                    nodeId = id; pinIndex = i; isOutput = false; isDataPin = true;
                    return true;
                }
            }
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Node dragging
    // -----------------------------------------------------------------------

    void NodeGraphController::BeginDragNodes(D2D1_POINT_2F startPoint)
    {
        m_selection.isDragging = true;
        m_selection.dragStart = startPoint;
        m_selection.dragOffset = { 0.0f, 0.0f };
    }

    void NodeGraphController::UpdateDragNodes(D2D1_POINT_2F currentPoint)
    {
        if (!m_selection.isDragging || !m_graph)
            return;

        // currentPoint is in canvas space (already pan/zoom adjusted).
        float dx = currentPoint.x - m_selection.dragStart.x;
        float dy = currentPoint.y - m_selection.dragStart.y;

        float ddx = dx - m_selection.dragOffset.x;
        float ddy = dy - m_selection.dragOffset.y;

        for (uint32_t nodeId : m_selection.selectedNodeIds)
        {
            auto* node = m_graph->FindNode(nodeId);
            if (node)
            {
                node->position.x += ddx;
                node->position.y += ddy;
            }
            // Update visual.
            auto vit = m_visuals.find(nodeId);
            if (vit != m_visuals.end())
            {
                vit->second.bounds.left += ddx;
                vit->second.bounds.right += ddx;
                vit->second.bounds.top += ddy;
                vit->second.bounds.bottom += ddy;
                for (auto& p : vit->second.inputPinPositions)  { p.x += ddx; p.y += ddy; }
                for (auto& p : vit->second.outputPinPositions) { p.x += ddx; p.y += ddy; }
                for (auto& p : vit->second.dataInputPinPositions)  { p.x += ddx; p.y += ddy; }
                for (auto& p : vit->second.dataOutputPinPositions) { p.x += ddx; p.y += ddy; }
                // Update slider rect for parameter nodes.
                if (vit->second.isParameterNode)
                {
                    vit->second.sliderRect.left += ddx;
                    vit->second.sliderRect.right += ddx;
                    vit->second.sliderRect.top += ddy;
                    vit->second.sliderRect.bottom += ddy;
                }
                if (vit->second.isClockNode)
                {
                    vit->second.playButtonRect.left += ddx;
                    vit->second.playButtonRect.right += ddx;
                    vit->second.playButtonRect.top += ddy;
                    vit->second.playButtonRect.bottom += ddy;
                }
            }
        }

        m_selection.dragOffset = { dx, dy };
        m_needsRedraw = true;
    }

    void NodeGraphController::EndDragNodes()
    {
        m_selection.isDragging = false;
        m_needsRedraw = true;
    }

    // -----------------------------------------------------------------------
    // Connection dragging
    // -----------------------------------------------------------------------

    void NodeGraphController::BeginConnection(uint32_t nodeId, uint32_t pinIndex, bool fromOutput, bool isDataPin)
    {
        m_connectionDrag.active = true;
        m_connectionDrag.sourceNodeId = nodeId;
        m_connectionDrag.sourcePin = pinIndex;
        m_connectionDrag.fromOutput = fromOutput;
        m_connectionDrag.isDataPin = isDataPin;

        // Set start position to the pin center.
        auto it = m_visuals.find(nodeId);
        if (it != m_visuals.end())
        {
            if (isDataPin)
            {
                if (fromOutput && pinIndex < it->second.dataOutputPinPositions.size())
                    m_connectionDrag.currentPos = it->second.dataOutputPinPositions[pinIndex];
                else if (!fromOutput && pinIndex < it->second.dataInputPinPositions.size())
                    m_connectionDrag.currentPos = it->second.dataInputPinPositions[pinIndex];
            }
            else
            {
                if (fromOutput && pinIndex < it->second.outputPinPositions.size())
                    m_connectionDrag.currentPos = it->second.outputPinPositions[pinIndex];
                else if (!fromOutput && pinIndex < it->second.inputPinPositions.size())
                    m_connectionDrag.currentPos = it->second.inputPinPositions[pinIndex];
            }
        }
    }

    void NodeGraphController::UpdateConnection(D2D1_POINT_2F currentPoint)
    {
        if (m_connectionDrag.active)
        {
            m_connectionDrag.currentPos = currentPoint;
            m_needsRedraw = true;
        }
    }

    bool NodeGraphController::EndConnection(D2D1_POINT_2F dropPoint)
    {
        if (!m_connectionDrag.active || !m_graph)
        {
            CancelConnection();
            return false;
        }

        // dropPoint is in canvas space (caller already converted).
        D2D1_POINT_2F canvasPoint = dropPoint;

        uint32_t targetNodeId = 0;
        uint32_t targetPin = 0;
        bool targetIsOutput = false;
        bool targetIsData = false;

        if (!HitTestPin(canvasPoint, targetNodeId, targetPin, targetIsOutput, targetIsData))
        {
            CancelConnection();
            return false;
        }

        // Must connect output→input, same pin type (image↔image, data↔data).
        if (targetIsData != m_connectionDrag.isDataPin)
        {
            CancelConnection();
            return false;
        }

        bool result = false;
        bool wasDataBinding = m_connectionDrag.isDataPin;
        uint32_t connSrcId = 0, connSrcPin = 0, connDstId = 0, connDstPin = 0;

        if (m_connectionDrag.isDataPin)
        {
            // Data pin connection → creates a PropertyBinding.
            uint32_t srcNodeId, dstNodeId;
            uint32_t srcPinIdx, dstPinIdx;
            if (m_connectionDrag.fromOutput && !targetIsOutput)
            {
                srcNodeId = m_connectionDrag.sourceNodeId;
                srcPinIdx = m_connectionDrag.sourcePin;
                dstNodeId = targetNodeId;
                dstPinIdx = targetPin;
            }
            else if (!m_connectionDrag.fromOutput && targetIsOutput)
            {
                srcNodeId = targetNodeId;
                srcPinIdx = targetPin;
                dstNodeId = m_connectionDrag.sourceNodeId;
                dstPinIdx = m_connectionDrag.sourcePin;
            }
            else
            {
                CancelConnection();
                return false;
            }

            // Resolve pin indices to field/property names.
            auto srcIt = m_visuals.find(srcNodeId);
            auto dstIt = m_visuals.find(dstNodeId);
            if (srcIt != m_visuals.end() && dstIt != m_visuals.end() &&
                srcPinIdx < srcIt->second.dataOutputPinNames.size() &&
                dstPinIdx < dstIt->second.dataInputPinNames.size())
            {
                auto& fieldName = srcIt->second.dataOutputPinNames[srcPinIdx];
                auto& propName = dstIt->second.dataInputPinNames[dstPinIdx];
                auto err = m_graph->BindProperty(dstNodeId, propName, srcNodeId, fieldName, 0);
                result = err.empty();
            }
            connSrcId = srcNodeId; connSrcPin = srcPinIdx;
            connDstId = dstNodeId; connDstPin = dstPinIdx;
        }
        else
        {
            // Image pin connection (existing behavior).
            if (m_connectionDrag.fromOutput && !targetIsOutput)
            {
                connSrcId = m_connectionDrag.sourceNodeId; connSrcPin = m_connectionDrag.sourcePin;
                connDstId = targetNodeId; connDstPin = targetPin;
                result = m_graph->Connect(connSrcId, connSrcPin, connDstId, connDstPin);
            }
            else if (!m_connectionDrag.fromOutput && targetIsOutput)
            {
                connSrcId = targetNodeId; connSrcPin = targetPin;
                connDstId = m_connectionDrag.sourceNodeId; connDstPin = m_connectionDrag.sourcePin;
                result = m_graph->Connect(connSrcId, connSrcPin, connDstId, connDstPin);
            }
        }

        CancelConnection();
        if (result)
        {
            RebuildLayout();
            if (m_connectionCallback)
                m_connectionCallback(connSrcId, connSrcPin, connDstId, connDstPin, wasDataBinding);
        }
        return result;
    }

    void NodeGraphController::CancelConnection()
    {
        m_connectionDrag.active = false;
    }

    // -----------------------------------------------------------------------
    // Selection
    // -----------------------------------------------------------------------

    void NodeGraphController::SelectNode(uint32_t nodeId, bool addToSelection)
    {
        if (!addToSelection)
            m_selection.selectedNodeIds.clear();
        m_selection.selectedNodeIds.insert(nodeId);
        m_needsRedraw = true;
    }

    void NodeGraphController::DeselectNode(uint32_t nodeId)
    {
        m_selection.selectedNodeIds.erase(nodeId);
        m_needsRedraw = true;
    }

    void NodeGraphController::DeselectAll()
    {
        m_selection.selectedNodeIds.clear();
        m_needsRedraw = true;
    }

    void NodeGraphController::SelectAll()
    {
        if (!m_graph) return;
        for (const auto& node : m_graph->Nodes())
        {
            m_selection.selectedNodeIds.insert(node.id);
        }
        m_needsRedraw = true;
    }

    void NodeGraphController::DeleteSelected()
    {
        if (!m_graph) return;

        for (uint32_t nodeId : m_selection.selectedNodeIds)
        {
            m_graph->RemoveNode(nodeId);
            m_visuals.erase(nodeId);
        }
        m_selection.selectedNodeIds.clear();
        m_needsRedraw = true;
    }

    // -----------------------------------------------------------------------
    // Node operations
    // -----------------------------------------------------------------------

    uint32_t NodeGraphController::AddNode(Graph::EffectNode node, D2D1_POINT_2F canvasPos)
    {
        if (!m_graph) return 0;

        // Auto-position if zero.
        if (canvasPos.x == 0.0f && canvasPos.y == 0.0f)
        {
            // Place new nodes at the center of the user's current viewport in
            // canvas space, so they don't disappear off-screen when the user
            // has panned/zoomed away from the origin. Stagger successive adds
            // so a chain of additions doesn't stack on a single point.
            float cx = (-m_panOffset.x + m_viewportW * 0.5f) / (m_zoom > 0.0f ? m_zoom : 1.0f);
            float cy = (-m_panOffset.y + m_viewportH * 0.5f) / (m_zoom > 0.0f ? m_zoom : 1.0f);
            float offset = static_cast<float>(m_autoStaggerCount % 8) * 24.0f;
            canvasPos = { cx - NodeWidth * 0.5f + offset, cy - 30.0f + offset };
            ++m_autoStaggerCount;

            // Try to avoid landing exactly on top of an existing node.
            if (m_graph)
            {
                for (int attempt = 0; attempt < 8; ++attempt)
                {
                    bool overlap = false;
                    for (const auto& n : m_graph->Nodes())
                    {
                        if (std::abs(n.position.x - canvasPos.x) < 8.0f &&
                            std::abs(n.position.y - canvasPos.y) < 8.0f)
                        {
                            overlap = true;
                            break;
                        }
                    }
                    if (!overlap) break;
                    canvasPos.x += 24.0f;
                    canvasPos.y += 24.0f;
                }
            }
        }

        node.position = { canvasPos.x, canvasPos.y };
        uint32_t id = m_graph->AddNode(std::move(node));

        auto* added = m_graph->FindNode(id);
        if (added)
            m_visuals[id] = ComputeNodeVisual(*added);

        return id;
    }

    uint32_t NodeGraphController::EnsureOutputNode()
    {
        if (!m_graph) return 0;

        // Check if an output node already exists.
        for (const auto& node : m_graph->Nodes())
        {
            if (node.type == Graph::NodeType::Output)
                return node.id;
        }

        Graph::EffectNode outputNode;
        outputNode.name = L"Output";
        outputNode.type = Graph::NodeType::Output;
        outputNode.inputPins = { { L"Input", 0 } };
        return AddNode(std::move(outputNode), { 600.0f, 200.0f });
    }

    // -----------------------------------------------------------------------
    // Visual computation
    // -----------------------------------------------------------------------

    NodeVisual NodeGraphController::ComputeNodeVisual(const Graph::EffectNode& node) const
    {
        NodeVisual v;
        v.nodeId = node.id;

        uint32_t maxImagePins = (std::max)(
            static_cast<uint32_t>(node.inputPins.size()),
            static_cast<uint32_t>(node.outputPins.size()));

        // Count data pins: bindable properties (float/float2/3/4) as inputs,
        // analysis fields as outputs. Build name (for binding lookup) and label (for display).
        for (const auto& [key, val] : node.properties)
        {
            // For nodes with a customEffect, only show input pins for properties
            // that correspond to a declared parameter. Properties that exist
            // solely as host-driven bootstrap values (e.g. Working Space, fed
            // by hiddenDefaults + UpdateWorkingSpaceNodes) must NOT appear as
            // input pins — they are sink-only analysis backing storage.
            if (node.customEffect.has_value())
            {
                bool isDeclaredParam = false;
                for (const auto& p : node.customEffect->parameters)
                {
                    if (p.name == key) { isDeclaredParam = true; break; }
                }
                if (!isDeclaredParam) continue;
            }
            // Skip conditionally hidden parameters.
            if (node.customEffect.has_value())
            {
                bool condHidden = false;
                for (const auto& p : node.customEffect->parameters)
                {
                    if (p.name == key && !p.visibleWhen.empty())
                    {
                        condHidden = !Graph::EvaluateVisibleWhen(p.visibleWhen, node.properties);
                        break;
                    }
                }
                if (condHidden) continue;
            }
            if (Graph::EffectGraph::IsBindablePropertyType(val))
            {
                v.dataInputPinNames.push_back(key);
                v.dataInputPinLabels.push_back(key);
            }
        }

        if (node.customEffect.has_value() &&
            node.customEffect->analysisOutputType == Graph::AnalysisOutputType::Typed)
        {
            for (const auto& fd : node.customEffect->analysisFields)
            {
                v.dataOutputPinNames.push_back(fd.name);
                v.dataOutputPinLabels.push_back(fd.name);
            }
        }
        else if (node.analysisOutput.type == Graph::AnalysisOutputType::Typed &&
                 !node.analysisOutput.fields.empty())
        {
            // Source nodes (e.g., video) may have analysis output set directly
            // without customEffect. Discover output pins from live data.
            for (const auto& fv : node.analysisOutput.fields)
            {
                v.dataOutputPinNames.push_back(fv.name);
                v.dataOutputPinLabels.push_back(fv.name);
            }
        }

        uint32_t dataInputCount = static_cast<uint32_t>(v.dataInputPinNames.size());
        uint32_t dataOutputCount = static_cast<uint32_t>(v.dataOutputPinNames.size());

        // Compute body height: image section + gap + data section.
        float imageBodyHeight = (std::max)(maxImagePins * v.pinSpacing + 10.0f, 40.0f);
        float dataBodyHeight = 0.0f;
        if (dataInputCount > 0 || dataOutputCount > 0)
        {
            uint32_t maxDataPins = (std::max)(dataInputCount, dataOutputCount);
            dataBodyHeight = 8.0f + maxDataPins * v.pinSpacing;  // 8px gap
        }

        // Detect parameter nodes (no HLSL, data-only, not Source nodes).
        v.isParameterNode = node.type != Graph::NodeType::Source &&
            node.customEffect.has_value() &&
            node.customEffect->hlslSource.empty() &&
            node.customEffect->analysisOutputType == Graph::AnalysisOutputType::Typed;
        v.isClockNode = node.isClock;

        // Parameter nodes: shrink image body (no image I/O) and add slider space.
        float sliderHeight = 0.0f;
        if (v.isParameterNode)
        {
            imageBodyHeight = 4.0f;  // minimal gap
            sliderHeight = v.isClockNode ? 48.0f : 28.0f;  // extra space for play button
        }

        // Data-only effects (have HLSL but no image output pin): shrink image body
        // only if they also have no image input pins.
        bool isDataOnlyEffect = node.customEffect.has_value() &&
            !node.customEffect->hlslSource.empty() &&
            node.outputPins.empty() &&
            node.customEffect->analysisOutputType == Graph::AnalysisOutputType::Typed;
        if (isDataOnlyEffect && node.inputPins.empty())
            imageBodyHeight = 4.0f;

        float totalHeight = v.headerHeight + imageBodyHeight + sliderHeight + dataBodyHeight;

        v.bounds = {
            node.position.x,
            node.position.y,
            node.position.x + NodeWidth,
            node.position.y + totalHeight
        };

        // Compute image pin positions.
        float pinStartY = node.position.y + v.headerHeight + 15.0f;
        for (uint32_t i = 0; i < node.inputPins.size(); ++i)
        {
            v.inputPinPositions.push_back({
                node.position.x,
                pinStartY + i * v.pinSpacing
            });
        }
        for (uint32_t i = 0; i < node.outputPins.size(); ++i)
        {
            v.outputPinPositions.push_back({
                node.position.x + NodeWidth,
                pinStartY + i * v.pinSpacing
            });
        }

        // Compute data pin positions (below image pins + slider).
        float dataStartY = node.position.y + v.headerHeight + imageBodyHeight + sliderHeight + 4.0f;

        // Slider rect for parameter nodes.
        if (v.isParameterNode)
        {
            float sliderY = node.position.y + v.headerHeight + imageBodyHeight;
            if (v.isClockNode)
            {
                // Play button on top row, slider below.
                v.playButtonRect = {
                    node.position.x + 12.0f,
                    sliderY + 2.0f,
                    node.position.x + 32.0f,
                    sliderY + 18.0f
                };
                v.sliderRect = {
                    node.position.x + 36.0f,
                    sliderY + 2.0f,
                    node.position.x + NodeWidth - 12.0f,
                    sliderY + 18.0f
                };
            }
            else
            {
                v.sliderRect = {
                    node.position.x + 12.0f,
                    sliderY + 4.0f,
                    node.position.x + NodeWidth - 12.0f,
                    sliderY + sliderHeight - 4.0f
                };
            }
        }
        for (uint32_t i = 0; i < dataInputCount; ++i)
        {
            v.dataInputPinPositions.push_back({
                node.position.x,
                dataStartY + i * v.pinSpacing
            });
        }
        for (uint32_t i = 0; i < dataOutputCount; ++i)
        {
            v.dataOutputPinPositions.push_back({
                node.position.x + NodeWidth,
                dataStartY + i * v.pinSpacing
            });
        }

        return v;
    }

    // -----------------------------------------------------------------------
    // Coordinate transforms
    // -----------------------------------------------------------------------

    D2D1_POINT_2F NodeGraphController::ScreenToCanvas(D2D1_POINT_2F screenPoint) const
    {
        return {
            (screenPoint.x - m_panOffset.x) / m_zoom,
            (screenPoint.y - m_panOffset.y) / m_zoom
        };
    }

    D2D1_POINT_2F NodeGraphController::CanvasToScreen(D2D1_POINT_2F canvasPoint) const
    {
        return {
            canvasPoint.x * m_zoom + m_panOffset.x,
            canvasPoint.y * m_zoom + m_panOffset.y
        };
    }

    // -----------------------------------------------------------------------
    // D2D Resource management
    // -----------------------------------------------------------------------

    void NodeGraphController::ReleaseDeviceResources()
    {
        m_brushNode = nullptr;
        m_brushHeader = nullptr;
        m_brushEdge = nullptr;
        m_brushPin = nullptr;
        m_brushSelection = nullptr;
        m_brushText = nullptr;
        m_brushDataPin = nullptr;
        m_brushDataEdge = nullptr;
        m_textFormat = nullptr;
        m_pinLabelFormat = nullptr;
        m_resourcesCreated = false;
        m_needsRedraw = true;
    }

    void NodeGraphController::EnsureResources(ID2D1DeviceContext* dc)
    {
        if (m_resourcesCreated) return;

        dc->CreateSolidColorBrush(D2D1::ColorF(0x252528), m_brushNode.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0x3A7BD5), m_brushHeader.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0x6B6B70), m_brushEdge.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0xB0B0B8), m_brushPin.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0x3A7BD5, 0.3f), m_brushSelection.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0xE8E8EC), m_brushText.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0xF5A623), m_brushDataPin.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0xF5A623, 0.75f), m_brushDataEdge.put());

        winrt::com_ptr<IDWriteFactory> dwriteFactory;
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory.put()));

        if (dwriteFactory)
        {
            dwriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                12.0f, L"en-US",
                m_textFormat.put());

            if (m_textFormat)
            {
                m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            dwriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                9.0f, L"en-US",
                m_pinLabelFormat.put());

            if (m_pinLabelFormat)
            {
                m_pinLabelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }

        m_resourcesCreated = true;
    }

    // -----------------------------------------------------------------------
    // Rendering
    // -----------------------------------------------------------------------

    void NodeGraphController::Render(ID2D1DeviceContext* dc, D2D1_SIZE_F /*viewportSize*/)
    {
        if (!m_graph || !dc) return;

        EnsureResources(dc);

        // Apply pan/zoom transform.
        D2D1_MATRIX_3X2_F transform =
            D2D1::Matrix3x2F::Scale(m_zoom, m_zoom) *
            D2D1::Matrix3x2F::Translation(m_panOffset.x, m_panOffset.y);
        dc->SetTransform(transform);

        RenderEdges(dc);
        RenderNodes(dc);
        RenderConnectionDrag(dc);

        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        m_needsRedraw = false;
    }

    void NodeGraphController::RenderEdges(ID2D1DeviceContext* dc)
    {
        if (!m_brushEdge) return;

        for (const auto& edge : m_graph->Edges())
        {
            auto srcIt = m_visuals.find(edge.sourceNodeId);
            auto dstIt = m_visuals.find(edge.destNodeId);
            if (srcIt == m_visuals.end() || dstIt == m_visuals.end())
                continue;

            D2D1_POINT_2F start{};
            D2D1_POINT_2F end{};

            if (edge.sourcePin < srcIt->second.outputPinPositions.size())
                start = srcIt->second.outputPinPositions[edge.sourcePin];
            if (edge.destPin < dstIt->second.inputPinPositions.size())
                end = dstIt->second.inputPinPositions[edge.destPin];

            // Draw a bezier curve between the pins.
            float dx = (end.x - start.x) * 0.4f;
            D2D1_POINT_2F cp1 = { start.x + dx, start.y };
            D2D1_POINT_2F cp2 = { end.x - dx, end.y };

            winrt::com_ptr<ID2D1PathGeometry> path;
            winrt::com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            factory->CreatePathGeometry(path.put());

            if (path)
            {
                winrt::com_ptr<ID2D1GeometrySink> sink;
                path->Open(sink.put());
                if (sink)
                {
                    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddBezier({ cp1, cp2, end });
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                }
                dc->DrawGeometry(path.get(), m_brushEdge.get(), 2.0f);
            }
        }

        // Render data edges (property bindings) as orange curves.
        if (m_brushDataEdge)
        {
            for (const auto& node : m_graph->Nodes())
            {
                auto dstIt = m_visuals.find(node.id);
                if (dstIt == m_visuals.end()) continue;

                for (const auto& [propName, binding] : node.propertyBindings)
                {
                    // Collect unique (sourceNodeId, sourceFieldName) pairs from the binding.
                    struct SrcRef { uint32_t nodeId; std::wstring field; };
                    std::vector<SrcRef> srcRefs;
                    if (binding.wholeArray)
                    {
                        srcRefs.push_back({ binding.wholeArraySourceNodeId, binding.wholeArraySourceFieldName });
                    }
                    else
                    {
                        for (const auto& src : binding.sources)
                        {
                            if (!src.has_value()) continue;
                            bool dup = false;
                            for (const auto& r : srcRefs)
                                if (r.nodeId == src->sourceNodeId && r.field == src->sourceFieldName)
                                { dup = true; break; }
                            if (!dup)
                                srcRefs.push_back({ src->sourceNodeId, src->sourceFieldName });
                        }
                    }

                    // Find dest data input pin position.
                    D2D1_POINT_2F end{};
                    bool foundDst = false;
                    for (uint32_t i = 0; i < dstIt->second.dataInputPinNames.size(); ++i)
                    {
                        if (dstIt->second.dataInputPinNames[i] == propName &&
                            i < dstIt->second.dataInputPinPositions.size())
                        { end = dstIt->second.dataInputPinPositions[i]; foundDst = true; break; }
                    }
                    if (!foundDst) continue;

                    // Draw one edge per unique source.
                    for (const auto& ref : srcRefs)
                    {
                        auto srcIt = m_visuals.find(ref.nodeId);
                        if (srcIt == m_visuals.end()) continue;

                        D2D1_POINT_2F start{};
                        bool foundSrc = false;
                        for (uint32_t i = 0; i < srcIt->second.dataOutputPinNames.size(); ++i)
                        {
                            if (srcIt->second.dataOutputPinNames[i] == ref.field &&
                                i < srcIt->second.dataOutputPinPositions.size())
                            { start = srcIt->second.dataOutputPinPositions[i]; foundSrc = true; break; }
                        }
                        if (!foundSrc) continue;

                        float bdx = (end.x - start.x) * 0.4f;
                        winrt::com_ptr<ID2D1PathGeometry> bpath;
                        winrt::com_ptr<ID2D1Factory> bfactory;
                        dc->GetFactory(bfactory.put());
                        bfactory->CreatePathGeometry(bpath.put());
                        if (bpath)
                        {
                            winrt::com_ptr<ID2D1GeometrySink> bsink;
                            bpath->Open(bsink.put());
                            if (bsink)
                            {
                                bsink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
                                bsink->AddBezier({ { start.x + bdx, start.y }, { end.x - bdx, end.y }, end });
                                bsink->EndFigure(D2D1_FIGURE_END_OPEN);
                                bsink->Close();
                            }
                            dc->DrawGeometry(bpath.get(), m_brushDataEdge.get(), 1.5f);
                        }
                    }
                }
            }
        }
    }

    void NodeGraphController::RenderNodes(ID2D1DeviceContext* dc)
    {
        for (const auto& [nodeId, visual] : m_visuals)
        {
            const auto* node = m_graph->FindNode(nodeId);
            if (!node) continue;

            bool selected = m_selection.selectedNodeIds.contains(nodeId);

            // Node body.
            D2D1_ROUNDED_RECT rrect = { visual.bounds, NodeCornerRadius, NodeCornerRadius };
            if (m_brushNode)
            {
                bool isDataOnly = !visual.isParameterNode && node->outputPins.empty() &&
                    node->customEffect.has_value() && !node->customEffect->hlslSource.empty();
                D2D1_COLOR_F bodyColor = visual.isParameterNode ? D2D1::ColorF(0x1A2E3A)
                    : isDataOnly ? D2D1::ColorF(0x2A1E3A)
                    : NodeColor(node->type);
                m_brushNode->SetColor(bodyColor);
                dc->FillRoundedRectangle(rrect, m_brushNode.get());
            }

            // Selection highlight.
            if (selected && m_brushSelection)
            {
                dc->DrawRoundedRectangle(rrect, m_brushSelection.get(), 2.0f);
            }

            // Header bar.
            D2D1_RECT_F headerRect = {
                visual.bounds.left, visual.bounds.top,
                visual.bounds.right, visual.bounds.top + visual.headerHeight
            };
            D2D1_ROUNDED_RECT headerRRect = { headerRect, NodeCornerRadius, NodeCornerRadius };
            if (m_brushHeader)
            {
                bool isDataOnly = !visual.isParameterNode && node->outputPins.empty() &&
                    node->customEffect.has_value() && !node->customEffect->hlslSource.empty();
                D2D1_COLOR_F headerColor = visual.isParameterNode ? D2D1::ColorF(0x00897B)
                    : isDataOnly ? D2D1::ColorF(0x7B1FA2)
                    : NodeHeaderColor(node->type);
                m_brushHeader->SetColor(headerColor);
                dc->FillRoundedRectangle(headerRRect, m_brushHeader.get());
            }

            // Node title.
            if (m_brushText && m_textFormat)
            {
                dc->DrawText(
                    node->name.c_str(),
                    static_cast<UINT32>(node->name.size()),
                    m_textFormat.get(),
                    headerRect,
                    m_brushText.get());
            }

            // Math Expression: show the formula text under the header.
            if (m_pinLabelFormat && m_brushText && node->customEffect.has_value() &&
                node->customEffect->shaderLabEffectId == L"Math Expression")
            {
                auto eIt = node->properties.find(L"Expression");
                if (eIt != node->properties.end())
                {
                    if (auto* s = std::get_if<std::wstring>(&eIt->second))
                    {
                        std::wstring display = L"= " + *s;
                        D2D1_RECT_F exprRect = {
                            visual.bounds.left + 6.0f,
                            headerRect.bottom + 1.0f,
                            visual.bounds.right - 6.0f,
                            headerRect.bottom + 18.0f
                        };
                        m_pinLabelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        dc->DrawText(display.c_str(), static_cast<UINT32>(display.size()),
                            m_pinLabelFormat.get(), exprRect, m_brushText.get());
                    }
                }
            }

            // Input pins (image).
            if (m_brushPin)
            {
                for (uint32_t i = 0; i < visual.inputPinPositions.size(); ++i)
                {
                    auto& p = visual.inputPinPositions[i];
                    dc->FillEllipse({ p, PinRadius, PinRadius }, m_brushPin.get());
                    // Label from customEffect input names.
                    if (m_pinLabelFormat && node->customEffect.has_value() &&
                        i < node->customEffect->inputNames.size())
                    {
                        auto& label = node->customEffect->inputNames[i];
                        D2D1_RECT_F labelRect = {
                            p.x + PinRadius + 3.0f, p.y - 7.0f,
                            visual.bounds.right - 4.0f, p.y + 7.0f
                        };
                        m_pinLabelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        dc->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                            m_pinLabelFormat.get(), labelRect, m_brushPin.get());
                    }
                }
                // Output pins (image).
                for (uint32_t i = 0; i < visual.outputPinPositions.size(); ++i)
                {
                    auto& p = visual.outputPinPositions[i];
                    dc->FillEllipse({ p, PinRadius, PinRadius }, m_brushPin.get());
                }
            }

            // Data pins (diamonds, orange).
            if (m_brushDataPin)
            {
                constexpr float dr = PinRadius * 0.85f;
                auto drawDiamond = [&](D2D1_POINT_2F center)
                {
                    // Rotated square (diamond).
                    D2D1_POINT_2F pts[4] = {
                        { center.x,      center.y - dr },  // top
                        { center.x + dr, center.y      },  // right
                        { center.x,      center.y + dr },  // bottom
                        { center.x - dr, center.y      },  // left
                    };
                    winrt::com_ptr<ID2D1PathGeometry> diamond;
                    winrt::com_ptr<ID2D1Factory> factory;
                    dc->GetFactory(factory.put());
                    factory->CreatePathGeometry(diamond.put());
                    if (diamond)
                    {
                        winrt::com_ptr<ID2D1GeometrySink> sink;
                        diamond->Open(sink.put());
                        if (sink)
                        {
                            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
                            sink->AddLine(pts[1]);
                            sink->AddLine(pts[2]);
                            sink->AddLine(pts[3]);
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sink->Close();
                        }
                        dc->FillGeometry(diamond.get(), m_brushDataPin.get());
                    }
                };

                // Data input pins (left side) + labels with current values.
                for (uint32_t i = 0; i < visual.dataInputPinPositions.size(); ++i)
                {
                    drawDiamond(visual.dataInputPinPositions[i]);
                    if (m_pinLabelFormat && i < visual.dataInputPinLabels.size() && i < visual.dataInputPinNames.size())
                    {
                        std::wstring label = visual.dataInputPinLabels[i];
                        auto propIt = node->properties.find(visual.dataInputPinNames[i]);
                        if (propIt != node->properties.end())
                        {
                            float val = 0;
                            if (auto* f = std::get_if<float>(&propIt->second)) val = *f;

                            // Look up enum label if available.
                            std::wstring valStr;
                            if (node->customEffect.has_value())
                            {
                                for (const auto& p : node->customEffect->parameters)
                                {
                                    if (p.name == visual.dataInputPinNames[i] && !p.enumLabels.empty())
                                    {
                                        uint32_t idx = static_cast<uint32_t>(val + 0.5f);
                                        if (idx < p.enumLabels.size())
                                            valStr = p.enumLabels[idx];
                                        break;
                                    }
                                }
                            }
                            if (valStr.empty())
                                valStr = std::format(L"{:.4g}", val);
                            label += L" = " + valStr;
                        }
                        D2D1_RECT_F labelRect = {
                            visual.dataInputPinPositions[i].x + PinRadius + 3.0f,
                            visual.dataInputPinPositions[i].y - 7.0f,
                            visual.bounds.right - 4.0f,
                            visual.dataInputPinPositions[i].y + 7.0f
                        };
                        m_pinLabelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        dc->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                            m_pinLabelFormat.get(), labelRect, m_brushDataPin.get());
                    }
                }

                // Data output pins (right side) + labels.
                // For data-only nodes with analysis output, show "Name = value" combined.
                bool hasAnalysisValues = node->analysisOutput.type == Graph::AnalysisOutputType::Typed &&
                    !node->analysisOutput.fields.empty() &&
                    (visual.isParameterNode || node->outputPins.empty());

                for (uint32_t i = 0; i < visual.dataOutputPinPositions.size(); ++i)
                {
                    drawDiamond(visual.dataOutputPinPositions[i]);
                    if (m_pinLabelFormat && i < visual.dataOutputPinLabels.size())
                    {
                        std::wstring label = visual.dataOutputPinLabels[i];

                        // Append analysis value if available.
                        if (hasAnalysisValues && i < visual.dataOutputPinNames.size())
                        {
                            for (const auto& fv : node->analysisOutput.fields)
                            {
                                if (fv.name == visual.dataOutputPinNames[i])
                                {
                                    label += std::format(L" = {:.4g}", fv.components[0]);
                                    break;
                                }
                            }
                        }

                        D2D1_RECT_F labelRect = {
                            visual.bounds.left + 4.0f,
                            visual.dataOutputPinPositions[i].y - 7.0f,
                            visual.dataOutputPinPositions[i].x - PinRadius - 3.0f,
                            visual.dataOutputPinPositions[i].y + 7.0f
                        };
                        m_pinLabelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                        dc->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                            m_pinLabelFormat.get(), labelRect, m_brushDataPin.get());
                    }
                }
            }

            // Inline slider for parameter nodes.
            if (visual.isParameterNode && node->customEffect.has_value())
            {
                if (visual.isClockNode)
                {
                    // Clock node: play/pause button + time seek slider.
                    float startTime = 0.0f, stopTime = 10.0f;
                    auto stIt = node->properties.find(L"StartTime");
                    if (stIt != node->properties.end())
                        if (auto* f = std::get_if<float>(&stIt->second)) startTime = *f;
                    auto etIt = node->properties.find(L"StopTime");
                    if (etIt != node->properties.end())
                        if (auto* f = std::get_if<float>(&etIt->second)) stopTime = *f;
                    float duration = stopTime - startTime;
                    if (duration <= 0.0f) duration = 1.0f;

                    float currentTime = startTime + static_cast<float>(node->clockTime);
                    float t = static_cast<float>(node->clockTime / duration);
                    t = (std::max)(0.0f, (std::min)(1.0f, t));

                    // Play/Pause button (triangle or double-bar).
                    {
                        winrt::com_ptr<ID2D1SolidColorBrush> btnBrush;
                        dc->CreateSolidColorBrush(
                            node->isPlaying ? D2D1::ColorF(0x4CAF50) : D2D1::ColorF(0xB0B0B8),
                            btnBrush.put());
                        if (btnBrush)
                        {
                            float cx = (visual.playButtonRect.left + visual.playButtonRect.right) * 0.5f;
                            float cy = (visual.playButtonRect.top + visual.playButtonRect.bottom) * 0.5f;
                            if (node->isPlaying)
                            {
                                // Pause icon (two bars).
                                dc->FillRectangle({ cx - 4, cy - 5, cx - 1, cy + 5 }, btnBrush.get());
                                dc->FillRectangle({ cx + 1, cy - 5, cx + 4, cy + 5 }, btnBrush.get());
                            }
                            else
                            {
                                // Play icon (triangle).
                                D2D1_POINT_2F pts[3] = {
                                    { cx - 4, cy - 6 }, { cx - 4, cy + 6 }, { cx + 5, cy }
                                };
                                winrt::com_ptr<ID2D1PathGeometry> tri;
                                winrt::com_ptr<ID2D1Factory> factory;
                                dc->GetFactory(factory.put());
                                if (factory)
                                {
                                    factory->CreatePathGeometry(tri.put());
                                    if (tri)
                                    {
                                        winrt::com_ptr<ID2D1GeometrySink> sink;
                                        tri->Open(sink.put());
                                        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
                                        sink->AddLine(pts[1]);
                                        sink->AddLine(pts[2]);
                                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                        sink->Close();
                                        dc->FillGeometry(tri.get(), btnBrush.get());
                                    }
                                }
                            }
                        }
                    }

                    // Seek slider track.
                    float trackY = (visual.sliderRect.top + visual.sliderRect.bottom) * 0.5f;
                    float trackH = 4.0f;
                    D2D1_ROUNDED_RECT track = {
                        { visual.sliderRect.left, trackY - trackH * 0.5f,
                          visual.sliderRect.right, trackY + trackH * 0.5f },
                        2.0f, 2.0f
                    };
                    winrt::com_ptr<ID2D1SolidColorBrush> trackBrush;
                    dc->CreateSolidColorBrush(D2D1::ColorF(0x444444), trackBrush.put());
                    if (trackBrush) dc->FillRoundedRectangle(track, trackBrush.get());

                    float fillRight = visual.sliderRect.left + t * (visual.sliderRect.right - visual.sliderRect.left);
                    D2D1_ROUNDED_RECT fill = {
                        { visual.sliderRect.left, trackY - trackH * 0.5f,
                          fillRight, trackY + trackH * 0.5f },
                        2.0f, 2.0f
                    };
                    winrt::com_ptr<ID2D1SolidColorBrush> fillBrush;
                    dc->CreateSolidColorBrush(D2D1::ColorF(0x4CAF50), fillBrush.put());
                    if (fillBrush) dc->FillRoundedRectangle(fill, fillBrush.get());

                    float handleX = fillRight;
                    winrt::com_ptr<ID2D1SolidColorBrush> handleBrush;
                    dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), handleBrush.put());
                    if (handleBrush) dc->FillEllipse({ { handleX, trackY }, 4.0f, 4.0f }, handleBrush.get());

                    // Time display below slider.
                    if (m_pinLabelFormat)
                    {
                        std::wstring timeText = std::format(L"{:.1f}s / {:.1f}s", currentTime, stopTime);
                        D2D1_RECT_F valRect = {
                            visual.sliderRect.left, visual.sliderRect.bottom,
                            visual.sliderRect.right, visual.sliderRect.bottom + 14.0f
                        };
                        m_pinLabelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        if (m_brushText)
                            dc->DrawText(timeText.c_str(), static_cast<UINT32>(timeText.size()),
                                m_pinLabelFormat.get(), valRect, m_brushText.get());
                    }
                }
                else
                {
                // Regular parameter slider.
                auto propIt = node->properties.find(L"Value");
                if (propIt != node->properties.end())
                {
                    float val = 0.0f;
                    if (auto* f = std::get_if<float>(&propIt->second)) val = *f;

                    float pMin = 0.0f, pMax = 1.0f;
                    // Use dynamic Min/Max properties if present (parameter nodes).
                    auto minIt = node->properties.find(L"Min");
                    auto maxIt = node->properties.find(L"Max");
                    if (minIt != node->properties.end()) {
                        if (auto* f = std::get_if<float>(&minIt->second)) pMin = *f;
                    } else {
                        for (const auto& p : node->customEffect->parameters)
                            if (p.name == L"Value") { pMin = p.minValue; break; }
                    }
                    if (maxIt != node->properties.end()) {
                        if (auto* f = std::get_if<float>(&maxIt->second)) pMax = *f;
                    } else {
                        for (const auto& p : node->customEffect->parameters)
                            if (p.name == L"Value") { pMax = p.maxValue; break; }
                    }

                    float t = (pMax > pMin) ? (val - pMin) / (pMax - pMin) : 0.0f;
                    t = (std::max)(0.0f, (std::min)(1.0f, t));

                    float trackY = (visual.sliderRect.top + visual.sliderRect.bottom) * 0.5f;
                    float trackH = 4.0f;
                    D2D1_ROUNDED_RECT track = {
                        { visual.sliderRect.left, trackY - trackH * 0.5f,
                          visual.sliderRect.right, trackY + trackH * 0.5f },
                        2.0f, 2.0f
                    };
                    winrt::com_ptr<ID2D1SolidColorBrush> trackBrush;
                    dc->CreateSolidColorBrush(D2D1::ColorF(0x444444), trackBrush.put());
                    if (trackBrush) dc->FillRoundedRectangle(track, trackBrush.get());

                    float fillRight = visual.sliderRect.left + t * (visual.sliderRect.right - visual.sliderRect.left);
                    D2D1_ROUNDED_RECT fill = {
                        { visual.sliderRect.left, trackY - trackH * 0.5f,
                          fillRight, trackY + trackH * 0.5f },
                        2.0f, 2.0f
                    };
                    winrt::com_ptr<ID2D1SolidColorBrush> fillBrush;
                    dc->CreateSolidColorBrush(D2D1::ColorF(0x00BCD4), fillBrush.put());
                    if (fillBrush) dc->FillRoundedRectangle(fill, fillBrush.get());

                    float handleX = fillRight;
                    winrt::com_ptr<ID2D1SolidColorBrush> handleBrush;
                    dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), handleBrush.put());
                    if (handleBrush) dc->FillEllipse({ { handleX, trackY }, 5.0f, 5.0f }, handleBrush.get());

                    if (m_pinLabelFormat)
                    {
                        std::wstring valText;
                        for (const auto& p : node->customEffect->parameters)
                        {
                            if (p.name == L"Value" && !p.enumLabels.empty())
                            {
                                uint32_t idx = static_cast<uint32_t>(val + 0.5f);
                                if (idx < p.enumLabels.size())
                                    valText = p.enumLabels[idx];
                                break;
                            }
                        }
                        if (valText.empty())
                            valText = std::format(L"{:.2f}", val);

                        D2D1_RECT_F valRect = {
                            visual.sliderRect.left, visual.sliderRect.bottom - 2.0f,
                            visual.sliderRect.right, visual.sliderRect.bottom + 12.0f
                        };
                        m_pinLabelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        if (m_brushText)
                            dc->DrawText(valText.c_str(), static_cast<UINT32>(valText.size()),
                                m_pinLabelFormat.get(), valRect, m_brushText.get());
                    }
                }
                } // end else (regular parameter)
            }
        }
    }

    void NodeGraphController::RenderConnectionDrag(ID2D1DeviceContext* dc)
    {
        if (!m_connectionDrag.active || !m_brushEdge) return;

        auto it = m_visuals.find(m_connectionDrag.sourceNodeId);
        if (it == m_visuals.end()) return;

        D2D1_POINT_2F start{};
        if (m_connectionDrag.isDataPin)
        {
            if (m_connectionDrag.fromOutput &&
                m_connectionDrag.sourcePin < it->second.dataOutputPinPositions.size())
                start = it->second.dataOutputPinPositions[m_connectionDrag.sourcePin];
            else if (!m_connectionDrag.fromOutput &&
                     m_connectionDrag.sourcePin < it->second.dataInputPinPositions.size())
                start = it->second.dataInputPinPositions[m_connectionDrag.sourcePin];
        }
        else
        {
            if (m_connectionDrag.fromOutput &&
                m_connectionDrag.sourcePin < it->second.outputPinPositions.size())
                start = it->second.outputPinPositions[m_connectionDrag.sourcePin];
            else if (!m_connectionDrag.fromOutput &&
                     m_connectionDrag.sourcePin < it->second.inputPinPositions.size())
                start = it->second.inputPinPositions[m_connectionDrag.sourcePin];
        }

        D2D1_POINT_2F end = m_connectionDrag.currentPos;

        // Draw line from pin to cursor — orange for data, yellow for image.
        auto* brush = m_connectionDrag.isDataPin && m_brushDataEdge
            ? m_brushDataEdge.get() : m_brushEdge.get();
        if (!m_connectionDrag.isDataPin)
            m_brushEdge->SetColor(D2D1::ColorF(0xFFFF00, 0.7f));
        dc->DrawLine(start, end, brush, 2.0f);
        if (!m_connectionDrag.isDataPin)
            m_brushEdge->SetColor(D2D1::ColorF(0x999999));
    }

    // -----------------------------------------------------------------------
    // Color helpers
    // -----------------------------------------------------------------------

    D2D1_COLOR_F NodeGraphController::NodeColor(Graph::NodeType type)
    {
        switch (type)
        {
        case Graph::NodeType::Source:        return D2D1::ColorF(0x1E3A1E);
        case Graph::NodeType::BuiltInEffect: return D2D1::ColorF(0x1E2440);
        case Graph::NodeType::PixelShader:   return D2D1::ColorF(0x3A1E1E);
        case Graph::NodeType::ComputeShader: return D2D1::ColorF(0x3A2E1E);
        case Graph::NodeType::Output:        return D2D1::ColorF(0x2A2A2E);
        default:                             return D2D1::ColorF(0x252528);
        }
    }

    D2D1_COLOR_F NodeGraphController::NodeHeaderColor(Graph::NodeType type)
    {
        switch (type)
        {
        case Graph::NodeType::Source:        return D2D1::ColorF(0x43A047);
        case Graph::NodeType::BuiltInEffect: return D2D1::ColorF(0x3A7BD5);
        case Graph::NodeType::PixelShader:   return D2D1::ColorF(0xE53935);
        case Graph::NodeType::ComputeShader: return D2D1::ColorF(0xF5A623);
        case Graph::NodeType::Output:        return D2D1::ColorF(0x78909C);
        default:                             return D2D1::ColorF(0x3A7BD5);
        }
    }

    uint32_t NodeGraphController::HitTestSlider(D2D1_POINT_2F canvasPoint) const
    {
        for (const auto& [id, v] : m_visuals)
        {
            if (!v.isParameterNode) continue;
            if (canvasPoint.x >= v.sliderRect.left && canvasPoint.x <= v.sliderRect.right &&
                canvasPoint.y >= v.sliderRect.top - 6.0f && canvasPoint.y <= v.sliderRect.bottom + 6.0f)
                return id;
        }
        return 0;
    }

    uint32_t NodeGraphController::HitTestPlayButton(D2D1_POINT_2F canvasPoint) const
    {
        for (const auto& [id, v] : m_visuals)
        {
            if (!v.isClockNode) continue;
            if (canvasPoint.x >= v.playButtonRect.left - 4.0f &&
                canvasPoint.x <= v.playButtonRect.right + 4.0f &&
                canvasPoint.y >= v.playButtonRect.top - 4.0f &&
                canvasPoint.y <= v.playButtonRect.bottom + 4.0f)
                return id;
        }
        return 0;
    }

    namespace
    {
        // Squared distance from a point to a cubic bezier, sampled.
        float DistanceToBezierSqr(D2D1_POINT_2F p,
            D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, D2D1_POINT_2F d)
        {
            constexpr int kSamples = 24;
            float best = (std::numeric_limits<float>::max)();
            for (int i = 0; i <= kSamples; ++i)
            {
                float t = static_cast<float>(i) / kSamples;
                float u = 1.0f - t;
                float bx = u*u*u*a.x + 3*u*u*t*b.x + 3*u*t*t*c.x + t*t*t*d.x;
                float by = u*u*u*a.y + 3*u*u*t*b.y + 3*u*t*t*c.y + t*t*t*d.y;
                float dx = p.x - bx, dy = p.y - by;
                float d2 = dx*dx + dy*dy;
                if (d2 < best) best = d2;
            }
            return best;
        }
    }

    NodeGraphController::EdgeHit
    NodeGraphController::HitTestEdge(D2D1_POINT_2F canvasPoint, float tolerance) const
    {
        EdgeHit best{};
        if (!m_graph) return best;

        const float tol2 = tolerance * tolerance;
        float bestDist = tol2;

        // Image edges.
        for (const auto& edge : m_graph->Edges())
        {
            auto srcIt = m_visuals.find(edge.sourceNodeId);
            auto dstIt = m_visuals.find(edge.destNodeId);
            if (srcIt == m_visuals.end() || dstIt == m_visuals.end()) continue;
            if (edge.sourcePin >= srcIt->second.outputPinPositions.size()) continue;
            if (edge.destPin >= dstIt->second.inputPinPositions.size()) continue;

            D2D1_POINT_2F start = srcIt->second.outputPinPositions[edge.sourcePin];
            D2D1_POINT_2F end   = dstIt->second.inputPinPositions[edge.destPin];
            float dx = (end.x - start.x) * 0.4f;
            D2D1_POINT_2F cp1 = { start.x + dx, start.y };
            D2D1_POINT_2F cp2 = { end.x - dx, end.y };

            float d2 = DistanceToBezierSqr(canvasPoint, start, cp1, cp2, end);
            if (d2 < bestDist)
            {
                bestDist = d2;
                best = EdgeHit{ true, false,
                    edge.sourceNodeId, edge.sourcePin,
                    edge.destNodeId, edge.destPin,
                    {}, {} };
            }
        }

        // Data binding edges.
        for (const auto& node : m_graph->Nodes())
        {
            auto dstIt = m_visuals.find(node.id);
            if (dstIt == m_visuals.end()) continue;

            for (const auto& [propName, binding] : node.propertyBindings)
            {
                D2D1_POINT_2F end{};
                bool foundDst = false;
                for (uint32_t i = 0; i < dstIt->second.dataInputPinNames.size(); ++i)
                {
                    if (dstIt->second.dataInputPinNames[i] == propName &&
                        i < dstIt->second.dataInputPinPositions.size())
                    { end = dstIt->second.dataInputPinPositions[i]; foundDst = true; break; }
                }
                if (!foundDst) continue;

                struct SrcRef { uint32_t nodeId; std::wstring field; };
                std::vector<SrcRef> srcRefs;
                if (binding.wholeArray)
                    srcRefs.push_back({ binding.wholeArraySourceNodeId, binding.wholeArraySourceFieldName });
                else
                {
                    for (const auto& src : binding.sources)
                    {
                        if (!src.has_value()) continue;
                        bool dup = false;
                        for (const auto& r : srcRefs)
                            if (r.nodeId == src->sourceNodeId && r.field == src->sourceFieldName)
                            { dup = true; break; }
                        if (!dup) srcRefs.push_back({ src->sourceNodeId, src->sourceFieldName });
                    }
                }

                for (const auto& ref : srcRefs)
                {
                    auto srcIt = m_visuals.find(ref.nodeId);
                    if (srcIt == m_visuals.end()) continue;

                    D2D1_POINT_2F start{};
                    bool foundSrc = false;
                    for (uint32_t i = 0; i < srcIt->second.dataOutputPinNames.size(); ++i)
                    {
                        if (srcIt->second.dataOutputPinNames[i] == ref.field &&
                            i < srcIt->second.dataOutputPinPositions.size())
                        { start = srcIt->second.dataOutputPinPositions[i]; foundSrc = true; break; }
                    }
                    if (!foundSrc) continue;

                    float bdx = (end.x - start.x) * 0.4f;
                    D2D1_POINT_2F cp1 = { start.x + bdx, start.y };
                    D2D1_POINT_2F cp2 = { end.x - bdx, end.y };
                    float d2 = DistanceToBezierSqr(canvasPoint, start, cp1, cp2, end);
                    if (d2 < bestDist)
                    {
                        bestDist = d2;
                        best = EdgeHit{ true, true, ref.nodeId, 0, node.id, 0,
                                        propName, ref.field };
                    }
                }
            }
        }

        return best;
    }

    bool NodeGraphController::RemoveEdge(const EdgeHit& hit)
    {
        if (!hit.found || !m_graph) return false;

        if (!hit.isDataBinding)
        {
            bool ok = m_graph->Disconnect(
                hit.sourceNodeId, hit.sourcePin,
                hit.destNodeId, hit.destPin);
            if (ok)
            {
                m_needsRedraw = true;
            }
            return ok;
        }

        // Data binding: clear sources from this binding that reference the
        // hit's source node + field. If nothing remains, unbind the property.
        auto* destNode = m_graph->FindNode(hit.destNodeId);
        if (!destNode) return false;
        auto bIt = destNode->propertyBindings.find(hit.destPropertyName);
        if (bIt == destNode->propertyBindings.end()) return false;

        auto& binding = bIt->second;
        bool removeAll = false;
        if (binding.wholeArray)
        {
            removeAll = (binding.wholeArraySourceNodeId == hit.sourceNodeId &&
                         binding.wholeArraySourceFieldName == hit.sourceFieldName);
        }
        else
        {
            bool anyLeft = false;
            for (auto& src : binding.sources)
            {
                if (src.has_value() &&
                    src->sourceNodeId == hit.sourceNodeId &&
                    src->sourceFieldName == hit.sourceFieldName)
                {
                    src.reset();
                }
                else if (src.has_value())
                {
                    anyLeft = true;
                }
            }
            removeAll = !anyLeft;
        }

        if (removeAll)
            m_graph->UnbindProperty(hit.destNodeId, hit.destPropertyName);
        else
        {
            destNode->dirty = true;
            m_graph->MarkAllDirty();
        }
        m_needsRedraw = true;
        return true;
    }

    bool NodeGraphController::UpdateSliderDrag(uint32_t nodeId, D2D1_POINT_2F canvasPoint)
    {
        auto vIt = m_visuals.find(nodeId);
        if (vIt == m_visuals.end() || !vIt->second.isParameterNode) return false;

        auto* node = m_graph->FindNode(nodeId);
        if (!node || !node->customEffect.has_value()) return false;

        // Clock nodes: seek by setting clockTime.
        if (vIt->second.isClockNode)
        {
            float startTime = 0.0f, stopTime = 10.0f;
            auto stIt = node->properties.find(L"StartTime");
            if (stIt != node->properties.end())
                if (auto* f = std::get_if<float>(&stIt->second)) startTime = *f;
            auto etIt = node->properties.find(L"StopTime");
            if (etIt != node->properties.end())
                if (auto* f = std::get_if<float>(&etIt->second)) stopTime = *f;
            float duration = stopTime - startTime;
            if (duration <= 0.0f) duration = 1.0f;

            float t = (canvasPoint.x - vIt->second.sliderRect.left)
                    / (vIt->second.sliderRect.right - vIt->second.sliderRect.left);
            t = (std::max)(0.0f, (std::min)(1.0f, t));
            node->clockTime = static_cast<double>(t * duration);
            node->dirty = true;
            m_needsRedraw = true;
            return true;
        }

        float pMin = 0.0f, pMax = 1.0f, step = 0.01f;
        bool hasEnumLabels = false;
        // Use dynamic Min/Max properties if present.
        auto minIt = node->properties.find(L"Min");
        auto maxIt = node->properties.find(L"Max");
        if (minIt != node->properties.end()) {
            if (auto* f = std::get_if<float>(&minIt->second)) pMin = *f;
        }
        if (maxIt != node->properties.end()) {
            if (auto* f = std::get_if<float>(&maxIt->second)) pMax = *f;
        }
        for (const auto& p : node->customEffect->parameters)
        {
            if (p.name == L"Value")
            {
                if (minIt == node->properties.end()) pMin = p.minValue;
                if (maxIt == node->properties.end()) pMax = p.maxValue;
                step = p.step;
                hasEnumLabels = !p.enumLabels.empty();
                break;
            }
        }

        float t = (canvasPoint.x - vIt->second.sliderRect.left)
                / (vIt->second.sliderRect.right - vIt->second.sliderRect.left);
        t = (std::max)(0.0f, (std::min)(1.0f, t));
        float newVal = pMin + t * (pMax - pMin);

        // Snap to step.
        if (step > 0.001f)
            newVal = std::round(newVal / step) * step;
        newVal = (std::max)(pMin, (std::min)(pMax, newVal));

        auto propIt = node->properties.find(L"Value");
        if (propIt == node->properties.end()) return false;

        float oldVal = 0.0f;
        if (auto* f = std::get_if<float>(&propIt->second)) oldVal = *f;

        if (std::abs(newVal - oldVal) < 0.0001f) return false;

        propIt->second = newVal;
        node->dirty = true;
        m_needsRedraw = true;
        return true;
    }

    bool NodeGraphController::IsParameterNode(uint32_t nodeId) const
    {
        auto it = m_visuals.find(nodeId);
        return it != m_visuals.end() && it->second.isParameterNode;
    }
}
