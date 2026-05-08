#include "pch_engine.h"
#include "EffectGraph.h"
#include "../Effects/ShaderCompiler.h"
#include "../Version.h"

#include <winrt/Windows.Data.Json.h>

namespace WDJ = winrt::Windows::Data::Json;

namespace ShaderLab::Graph
{
    // -----------------------------------------------------------------------
    // Node management
    // -----------------------------------------------------------------------
     
    uint32_t EffectGraph::AddNode(EffectNode node)
    {
        node.id = m_nextId++;
        node.dirty = true;
        m_nodes.push_back(std::move(node));
        return m_nodes.back().id;
    }

    void EffectGraph::RemoveNode(uint32_t nodeId)
    {
        // Note: previously this method refused to delete the last Output
        // node ("always keep at least one"). That protection was removed
        // because closing an Output node's external window now removes
        // the node (PresentOutputWindows in MainWindow.xaml.cpp), and
        // refusing the removal left the graph with a dangling Output
        // node and no window. The render path tolerates an output-less
        // graph just fine -- nothing is "needed" so evaluation no-ops
        // until the user adds a new Output node.

        // Remove all edges referencing this node.
        std::erase_if(m_edges, [nodeId](const EffectEdge& e)
        {
            return e.sourceNodeId == nodeId || e.destNodeId == nodeId;
        });

        // Remove property bindings referencing this node as a source.
        for (auto& node : m_nodes)
        {
            std::erase_if(node.propertyBindings, [nodeId](const auto& pair)
            {
                const auto& b = pair.second;
                if (b.wholeArray)
                    return b.wholeArraySourceNodeId == nodeId;
                for (const auto& src : b.sources)
                    if (src.has_value() && src->sourceNodeId == nodeId)
                        return true;
                return false;
            });
        }

        // Remove the node itself.
        std::erase_if(m_nodes, [nodeId](const EffectNode& n)
        {
            return n.id == nodeId;
        });
    }

    EffectNode* EffectGraph::FindNode(uint32_t nodeId)
    {
        auto it = std::ranges::find_if(m_nodes, [nodeId](const EffectNode& n) { return n.id == nodeId; });
        return it != m_nodes.end() ? &(*it) : nullptr;
    }

    const EffectNode* EffectGraph::FindNode(uint32_t nodeId) const
    {
        auto it = std::ranges::find_if(m_nodes, [nodeId](const EffectNode& n) { return n.id == nodeId; });
        return it != m_nodes.end() ? &(*it) : nullptr;
    }

    // -----------------------------------------------------------------------
    // Edge management
    // -----------------------------------------------------------------------

    bool EffectGraph::Connect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin)
    {
        if (srcId == dstId)
            return false;

        if (!FindNode(srcId) || !FindNode(dstId))
            return false;

        if (WouldCreateCycle(srcId, dstId))
            return false;

        // Ensure a dest input pin has at most one incoming edge.
        DisconnectInput(dstId, dstPin);

        m_edges.push_back({ srcId, srcPin, dstId, dstPin });
        FindNode(dstId)->dirty = true;
        return true;
    }

    bool EffectGraph::Disconnect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin)
    {
        EffectEdge target{ srcId, srcPin, dstId, dstPin };
        auto it = std::ranges::find(m_edges, target);
        if (it != m_edges.end())
        {
            m_edges.erase(it);
            if (auto* dst = FindNode(dstId))
                dst->dirty = true;
            return true;
        }
        return false;
    }

    void EffectGraph::DisconnectInput(uint32_t dstId, uint32_t dstPin)
    {
        std::erase_if(m_edges, [dstId, dstPin](const EffectEdge& e)
        {
            return e.destNodeId == dstId && e.destPin == dstPin;
        });
    }

    std::vector<const EffectEdge*> EffectGraph::GetInputEdges(uint32_t nodeId) const
    {
        std::vector<const EffectEdge*> result;
        for (const auto& e : m_edges)
        {
            if (e.destNodeId == nodeId)
                result.push_back(&e);
        }
        return result;
    }

    std::vector<const EffectEdge*> EffectGraph::GetOutputEdges(uint32_t nodeId) const
    {
        std::vector<const EffectEdge*> result;
        for (const auto& e : m_edges)
        {
            if (e.sourceNodeId == nodeId)
                result.push_back(&e);
        }
        return result;
    }

    std::vector<uint32_t> EffectGraph::GetOutputNodeIds() const
    {
        std::vector<uint32_t> ids;
        for (const auto& node : m_nodes)
        {
            if (node.type == NodeType::Output)
                ids.push_back(node.id);
        }
        return ids;
    }

    // -----------------------------------------------------------------------
    // Topological sort (Kahn's algorithm)
    // -----------------------------------------------------------------------

    std::vector<uint32_t> EffectGraph::TopologicalSort() const
    {
        // Build adjacency list and in-degree map.
        // Includes both image edges AND property binding dependencies.
        std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
        std::unordered_map<uint32_t, uint32_t> inDegree;

        for (const auto& node : m_nodes)
        {
            adj[node.id];            // ensure entry exists
            inDegree[node.id] = 0;
        }

        // Image edges: source → dest.
        for (const auto& edge : m_edges)
        {
            adj[edge.sourceNodeId].push_back(edge.destNodeId);
            inDegree[edge.destNodeId]++;
        }

        // Property binding edges: collect all unique source nodes.
        for (const auto& node : m_nodes)
        {
            std::unordered_set<uint32_t> bindingSources;
            for (const auto& [propName, binding] : node.propertyBindings)
            {
                if (binding.wholeArray)
                {
                    bindingSources.insert(binding.wholeArraySourceNodeId);
                }
                else
                {
                    for (const auto& src : binding.sources)
                    {
                        if (src.has_value())
                            bindingSources.insert(src->sourceNodeId);
                    }
                }
            }
            for (uint32_t srcId : bindingSources)
            {
                if (adj.contains(srcId))
                {
                    adj[srcId].push_back(node.id);
                    inDegree[node.id]++;
                }
            }
        }

        // Seed queue with zero-in-degree nodes.
        std::queue<uint32_t> q;
        for (const auto& [id, deg] : inDegree)
        {
            if (deg == 0)
                q.push(id);
        }

        std::vector<uint32_t> sorted;
        sorted.reserve(m_nodes.size());

        while (!q.empty())
        {
            uint32_t current = q.front();
            q.pop();
            sorted.push_back(current);

            for (uint32_t neighbor : adj[current])
            {
                if (--inDegree[neighbor] == 0)
                    q.push(neighbor);
            }
        }

        if (sorted.size() != m_nodes.size())
            throw std::logic_error("EffectGraph contains a cycle");

        return sorted;
    }

    // -----------------------------------------------------------------------
    // Cycle detection (DFS reachability check)
    // -----------------------------------------------------------------------

    bool EffectGraph::WouldCreateCycle(uint32_t srcId, uint32_t dstId) const
    {
        // If dstId can already reach srcId via image edges,
        // adding src->dst creates a cycle.
        // NOTE: data bindings are NOT followed — they are resolved lazily
        // and metadata flows (Duration→StopTime) are not render cycles.
        std::unordered_set<uint32_t> visited;
        std::queue<uint32_t> work;
        work.push(dstId);

        while (!work.empty())
        {
            uint32_t current = work.front();
            work.pop();

            if (current == srcId)
                return true;

            if (!visited.insert(current).second)
                continue;

            // Follow image edges only.
            for (const auto& edge : m_edges)
            {
                if (edge.sourceNodeId == current)
                    work.push(edge.destNodeId);
            }
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Property bindings
    // -----------------------------------------------------------------------

    bool EffectGraph::IsBindablePropertyType(const PropertyValue& value)
    {
        return std::visit([](auto&& v) -> bool
        {
            using T = std::decay_t<decltype(v)>;
            return std::is_same_v<T, float> ||
                   std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2> ||
                   std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3> ||
                   std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4> ||
                   std::is_same_v<T, std::vector<float>>;
        }, value);
    }

    std::wstring EffectGraph::BindProperty(
        uint32_t destNodeId,
        const std::wstring& propertyName,
        uint32_t sourceNodeId,
        const std::wstring& sourceFieldName,
        uint32_t sourceComponent)
    {
        auto* destNode = FindNode(destNodeId);
        if (!destNode) return L"Destination node not found";

        auto* srcNode = FindNode(sourceNodeId);
        if (!srcNode) return L"Source node not found";

        // Verify source has typed analysis output.
        bool hasAnalysis = false;
        if (srcNode->customEffect.has_value() &&
            srcNode->customEffect->analysisOutputType == AnalysisOutputType::Typed)
            hasAnalysis = true;
        else if (srcNode->analysisOutput.type == AnalysisOutputType::Typed &&
                 !srcNode->analysisOutput.fields.empty())
            hasAnalysis = true;
        if (!hasAnalysis)
            return L"Source node has no typed analysis output";

        // Verify field exists on source and get its type.
        const AnalysisFieldDescriptor* srcField = nullptr;
        AnalysisFieldDescriptor liveField{};  // for live-data fallback
        if (srcNode->customEffect.has_value())
        {
            for (const auto& fd : srcNode->customEffect->analysisFields)
            {
                if (fd.name == sourceFieldName) { srcField = &fd; break; }
            }
        }
        // Fallback: check live analysisOutput fields (e.g., video Duration/Position).
        if (!srcField)
        {
            for (const auto& fv : srcNode->analysisOutput.fields)
            {
                if (fv.name == sourceFieldName)
                {
                    liveField.name = fv.name;
                    liveField.type = fv.type;
                    srcField = &liveField;
                    break;
                }
            }
        }
        if (!srcField) return L"Source field not found";

        // Verify destination property exists and is bindable.
        auto propIt = destNode->properties.find(propertyName);
        if (propIt == destNode->properties.end())
            return L"Property not found on destination node";
        if (!IsBindablePropertyType(propIt->second))
            return L"Property type is not bindable (must be float, float2, float3, float4, or float array)";

        // Type compatibility check.
        bool srcIsArray = AnalysisFieldIsArray(srcField->type);
        bool destIsArray = std::holds_alternative<std::vector<float>>(propIt->second);

        if (srcIsArray && !destIsArray)
            return L"Cannot bind array output to scalar property";
        if (!srcIsArray && destIsArray)
            return L"Cannot bind scalar output to array property";

        // Scalar→scalar: wider source is OK (user picks component via sourceComponent).
        // Narrower source→wider dest: replicate (float→float4 fills x,x,x,0).

        // Cycle check — only considers image edges, not data bindings.
        // Data bindings (analysis outputs → properties) are resolved lazily
        // and don't create render dependency cycles.
        if (WouldCreateCycle(sourceNodeId, destNodeId))
            return L"Binding would create a cycle";

        // Create binding with smart defaults.
        PropertyBinding binding;

        if (destIsArray && srcIsArray)
        {
            // Whole-array passthrough.
            binding.wholeArray = true;
            binding.wholeArraySourceNodeId = sourceNodeId;
            binding.wholeArraySourceFieldName = sourceFieldName;
        }
        else
        {
            // Per-component: determine dest component count.
            uint32_t destCC = std::visit([](auto&& v) -> uint32_t
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>) return 1;
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>) return 2;
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>) return 3;
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>) return 4;
                else return 1;
            }, propIt->second);

            uint32_t srcCC = AnalysisFieldComponentCount(srcField->type);
            binding.sources.resize(destCC);

            if (destCC == 1)
            {
                // float dest: pick the requested component from source.
                binding.sources[0] = ComponentSource{ sourceNodeId, sourceFieldName, 0, sourceComponent };
            }
            else
            {
                // Vector dest: map 1:1 from source components.
                for (uint32_t c = 0; c < destCC; ++c)
                {
                    if (c < srcCC)
                        binding.sources[c] = ComponentSource{ sourceNodeId, sourceFieldName, 0, c };
                    // else: leave as nullopt (keeps authored default)
                }
            }
        }

        destNode->propertyBindings[propertyName] = std::move(binding);
        destNode->dirty = true;
        MarkAllDirty();
        return {};  // success
    }

    bool EffectGraph::UnbindProperty(uint32_t nodeId, const std::wstring& propertyName)
    {
        auto* node = FindNode(nodeId);
        if (!node) return false;
        if (node->propertyBindings.erase(propertyName) == 0)
            return false;
        node->dirty = true;
        MarkAllDirty();
        return true;
    }

    // -----------------------------------------------------------------------
    // Dirty / cache helpers
    // -----------------------------------------------------------------------

    void EffectGraph::MarkAllDirty()
    {
        for (auto& node : m_nodes)
            node.dirty = true;
    }

    bool EffectGraph::HasDirtyNodes() const
    {
        for (const auto& node : m_nodes)
            if (node.dirty) return true;
        return false;
    }

    void EffectGraph::ClearCachedOutputs()
    {
        for (auto& node : m_nodes)
            node.cachedOutput = nullptr;
    }

    void EffectGraph::Clear()
    {
        m_nodes.clear();
        m_edges.clear();
        m_nextId = 1;
    }

    // -----------------------------------------------------------------------
    // JSON serialization helpers (anonymous namespace)
    // -----------------------------------------------------------------------
    namespace
    {
        WDJ::JsonObject PropertyValueToJson(const std::wstring& key, const PropertyValue& value)
        {
            WDJ::JsonObject obj;
            obj.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(key));
            obj.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(PropertyValueTypeTag(value)));

            std::visit([&obj](auto&& v)
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateNumberValue(v));
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateNumberValue(static_cast<double>(v)));
                }
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateNumberValue(static_cast<double>(v)));
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateBooleanValue(v));
                }
                else if constexpr (std::is_same_v<T, std::wstring>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateStringValue(v));
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                {
                    WDJ::JsonArray arr;
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.x));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.y));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                {
                    WDJ::JsonArray arr;
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.x));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.y));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.z));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                {
                    WDJ::JsonArray arr;
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.x));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.y));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.z));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.w));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                {
                    // Serialize as flat 20-element array (row-major: 5 rows × 4 cols).
                    WDJ::JsonArray arr;
                    const float* p = &v._11;
                    for (int i = 0; i < 20; ++i)
                        arr.Append(WDJ::JsonValue::CreateNumberValue(p[i]));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                {
                    WDJ::JsonArray arr;
                    for (float f : v)
                        arr.Append(WDJ::JsonValue::CreateNumberValue(f));
                    obj.SetNamedValue(L"value", arr);
                }
            }, value);

            return obj;
        }

        PropertyValue PropertyValueFromJson(const WDJ::JsonObject& obj)
        {
            auto type = std::wstring(obj.GetNamedString(L"type"));

            if (type == L"float")
                return static_cast<float>(obj.GetNamedNumber(L"value"));
            if (type == L"int")
                return static_cast<int32_t>(obj.GetNamedNumber(L"value"));
            if (type == L"uint")
                return static_cast<uint32_t>(obj.GetNamedNumber(L"value"));
            if (type == L"bool")
                return obj.GetNamedBoolean(L"value");
            if (type == L"string")
                return std::wstring(obj.GetNamedString(L"value"));

            auto arr = obj.GetNamedArray(L"value");
            if (type == L"float2")
            {
                return winrt::Windows::Foundation::Numerics::float2{
                    static_cast<float>(arr.GetNumberAt(0)),
                    static_cast<float>(arr.GetNumberAt(1))
                };
            }
            if (type == L"float3")
            {
                return winrt::Windows::Foundation::Numerics::float3{
                    static_cast<float>(arr.GetNumberAt(0)),
                    static_cast<float>(arr.GetNumberAt(1)),
                    static_cast<float>(arr.GetNumberAt(2))
                };
            }
            if (type == L"float4")
            {
                return winrt::Windows::Foundation::Numerics::float4{
                    static_cast<float>(arr.GetNumberAt(0)),
                    static_cast<float>(arr.GetNumberAt(1)),
                    static_cast<float>(arr.GetNumberAt(2)),
                    static_cast<float>(arr.GetNumberAt(3))
                };
            }
            if (type == L"matrix5x4")
            {
                D2D1_MATRIX_5X4_F m{};
                float* p = &m._11;
                uint32_t count = (std::min)(arr.Size(), 20u);
                for (uint32_t i = 0; i < count; ++i)
                    p[i] = static_cast<float>(arr.GetNumberAt(i));
                return m;
            }
            if (type == L"floatarray")
            {
                std::vector<float> v;
                v.reserve(arr.Size());
                for (uint32_t i = 0; i < arr.Size(); ++i)
                    v.push_back(static_cast<float>(arr.GetNumberAt(i)));
                return v;
            }

            throw std::invalid_argument("Unknown property type in JSON");
        }

        WDJ::JsonObject NodeToJson(const EffectNode& node)
        {
            WDJ::JsonObject obj;
            obj.SetNamedValue(L"id", WDJ::JsonValue::CreateNumberValue(node.id));
            obj.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(node.name));
            obj.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(NodeTypeToString(node.type)));

            // Position
            WDJ::JsonArray pos;
            pos.Append(WDJ::JsonValue::CreateNumberValue(node.position.x));
            pos.Append(WDJ::JsonValue::CreateNumberValue(node.position.y));
            obj.SetNamedValue(L"position", pos);

            // Properties
            WDJ::JsonArray props;
            for (const auto& [key, value] : node.properties)
            {
                props.Append(PropertyValueToJson(key, value));
            }
            obj.SetNamedValue(L"properties", props);

            // Property bindings.
            if (!node.propertyBindings.empty())
            {
                WDJ::JsonObject bindingsObj;
                for (const auto& [propName, binding] : node.propertyBindings)
                {
                    WDJ::JsonObject bobj;
                    if (binding.wholeArray)
                    {
                        bobj.SetNamedValue(L"wholeArray", WDJ::JsonValue::CreateBooleanValue(true));
                        bobj.SetNamedValue(L"sourceNodeId", WDJ::JsonValue::CreateNumberValue(binding.wholeArraySourceNodeId));
                        bobj.SetNamedValue(L"sourceFieldName", WDJ::JsonValue::CreateStringValue(binding.wholeArraySourceFieldName));
                    }
                    else
                    {
                        WDJ::JsonArray srcs;
                        for (const auto& src : binding.sources)
                        {
                            if (src.has_value())
                            {
                                WDJ::JsonObject sobj;
                                sobj.SetNamedValue(L"nodeId", WDJ::JsonValue::CreateNumberValue(src->sourceNodeId));
                                sobj.SetNamedValue(L"field", WDJ::JsonValue::CreateStringValue(src->sourceFieldName));
                                sobj.SetNamedValue(L"index", WDJ::JsonValue::CreateNumberValue(src->sourceIndex));
                                sobj.SetNamedValue(L"comp", WDJ::JsonValue::CreateNumberValue(src->sourceComponent));
                                srcs.Append(sobj);
                            }
                            else
                            {
                                srcs.Append(WDJ::JsonValue::CreateNullValue());
                            }
                        }
                        bobj.SetNamedValue(L"sources", srcs);
                    }
                    bindingsObj.SetNamedValue(propName, bobj);
                }
                obj.SetNamedValue(L"propertyBindings", bindingsObj);
            }

            // Effect CLSID (stored as string for portability)
            if (node.effectClsid.has_value())
            {
                wchar_t guidStr[64]{};
                StringFromGUID2(node.effectClsid.value(), guidStr, 64);
                obj.SetNamedValue(L"effectClsid", WDJ::JsonValue::CreateStringValue(guidStr));
            }

            // Shader path
            if (node.shaderPath.has_value())
            {
                obj.SetNamedValue(L"shaderPath", WDJ::JsonValue::CreateStringValue(node.shaderPath.value()));
            }

            // Pins
            WDJ::JsonArray inPins;
            for (const auto& pin : node.inputPins)
            {
                WDJ::JsonObject p;
                p.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(pin.name));
                p.SetNamedValue(L"index", WDJ::JsonValue::CreateNumberValue(pin.index));
                inPins.Append(p);
            }
            obj.SetNamedValue(L"inputPins", inPins);

            WDJ::JsonArray outPins;
            for (const auto& pin : node.outputPins)
            {
                WDJ::JsonObject p;
                p.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(pin.name));
                p.SetNamedValue(L"index", WDJ::JsonValue::CreateNumberValue(pin.index));
                outPins.Append(p);
            }
            obj.SetNamedValue(L"outputPins", outPins);

            // Custom effect definition.
            if (node.customEffect.has_value())
            {
                WDJ::JsonObject ced;
                auto& def = node.customEffect.value();
                ced.SetNamedValue(L"shaderType", WDJ::JsonValue::CreateNumberValue(
                    static_cast<double>(def.shaderType)));
                ced.SetNamedValue(L"hlslSource", WDJ::JsonValue::CreateStringValue(def.hlslSource));

                WDJ::JsonArray inputs;
                for (const auto& name : def.inputNames)
                    inputs.Append(WDJ::JsonValue::CreateStringValue(name));
                ced.SetNamedValue(L"inputNames", inputs);

                WDJ::JsonArray params;
                for (const auto& p : def.parameters)
                {
                    WDJ::JsonObject po;
                    po.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(p.name));
                    po.SetNamedValue(L"typeName", WDJ::JsonValue::CreateStringValue(p.typeName));
                    po.SetNamedValue(L"minValue", WDJ::JsonValue::CreateNumberValue(p.minValue));
                    po.SetNamedValue(L"maxValue", WDJ::JsonValue::CreateNumberValue(p.maxValue));
                    po.SetNamedValue(L"step", WDJ::JsonValue::CreateNumberValue(p.step));
                    po.SetNamedValue(L"default", PropertyValueToJson(p.name, p.defaultValue));
                    if (!p.enumLabels.empty())
                    {
                        WDJ::JsonArray labels;
                        for (const auto& label : p.enumLabels)
                            labels.Append(WDJ::JsonValue::CreateStringValue(label));
                        po.SetNamedValue(L"enumLabels", labels);
                    }
                    if (!p.visibleWhen.empty())
                        po.SetNamedValue(L"visibleWhen", WDJ::JsonValue::CreateStringValue(p.visibleWhen));
                    if (p.gpuBindable)
                        po.SetNamedValue(L"gpuBindable", WDJ::JsonValue::CreateBooleanValue(true));
                    params.Append(po);
                }
                ced.SetNamedValue(L"parameters", params);

                ced.SetNamedValue(L"threadGroupX", WDJ::JsonValue::CreateNumberValue(def.threadGroupX));
                ced.SetNamedValue(L"threadGroupY", WDJ::JsonValue::CreateNumberValue(def.threadGroupY));
                ced.SetNamedValue(L"threadGroupZ", WDJ::JsonValue::CreateNumberValue(def.threadGroupZ));
                ced.SetNamedValue(L"analysisOutputType", WDJ::JsonValue::CreateNumberValue(
                    static_cast<double>(def.analysisOutputType)));
                ced.SetNamedValue(L"analysisOutputSize", WDJ::JsonValue::CreateNumberValue(def.analysisOutputSize));

                // Serialize typed analysis fields.
                if (!def.analysisFields.empty())
                {
                    WDJ::JsonArray fields;
                    for (const auto& fd : def.analysisFields)
                    {
                        WDJ::JsonObject fobj;
                        fobj.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(fd.name));
                        // Serialize type as string tag.
                        std::wstring typeTag;
                        switch (fd.type)
                        {
                        case AnalysisFieldType::Float:       typeTag = L"float"; break;
                        case AnalysisFieldType::Float2:      typeTag = L"float2"; break;
                        case AnalysisFieldType::Float3:      typeTag = L"float3"; break;
                        case AnalysisFieldType::Float4:      typeTag = L"float4"; break;
                        case AnalysisFieldType::FloatArray:   typeTag = L"floatarray"; break;
                        case AnalysisFieldType::Float2Array:  typeTag = L"float2array"; break;
                        case AnalysisFieldType::Float3Array:  typeTag = L"float3array"; break;
                        case AnalysisFieldType::Float4Array:  typeTag = L"float4array"; break;
                        }
                        fobj.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(typeTag));
                        if (AnalysisFieldIsArray(fd.type))
                            fobj.SetNamedValue(L"length", WDJ::JsonValue::CreateNumberValue(fd.arrayLength));
                        if (fd.gpuPublish)
                            fobj.SetNamedValue(L"gpuPublish", WDJ::JsonValue::CreateBooleanValue(true));
                        fields.Append(fobj);
                    }
                    ced.SetNamedValue(L"analysisFields", fields);
                }

                // ShaderLab effect identity (for version upgrade detection).
                if (!def.shaderLabEffectId.empty())
                {
                    ced.SetNamedValue(L"shaderLabEffectId", WDJ::JsonValue::CreateStringValue(def.shaderLabEffectId));
                    ced.SetNamedValue(L"shaderLabEffectVersion", WDJ::JsonValue::CreateNumberValue(def.shaderLabEffectVersion));
                }

                obj.SetNamedValue(L"customEffect", ced);
            }

            return obj;
        }

        EffectNode NodeFromJson(const WDJ::JsonObject& obj)
        {
            EffectNode node;
            node.id = static_cast<uint32_t>(obj.GetNamedNumber(L"id"));
            node.name = std::wstring(obj.GetNamedString(L"name"));
            node.type = NodeTypeFromString(std::wstring(obj.GetNamedString(L"type")));

            auto pos = obj.GetNamedArray(L"position");
            node.position = {
                static_cast<float>(pos.GetNumberAt(0)),
                static_cast<float>(pos.GetNumberAt(1))
            };

            auto props = obj.GetNamedArray(L"properties");
            std::wstring legacyAnalysisFieldsJson;
            std::wstring legacyPropertyBindingsJson;
            for (uint32_t i = 0; i < props.Size(); ++i)
            {
                auto propObj = props.GetObjectAt(i);
                auto key = std::wstring(propObj.GetNamedString(L"name"));
                // Extract legacy internal metadata stored as string properties.
                if (key == L"analysisFields")
                {
                    if (propObj.HasKey(L"value"))
                        legacyAnalysisFieldsJson = std::wstring(propObj.GetNamedString(L"value"));
                    continue;
                }
                if (key == L"propertyBindings")
                {
                    if (propObj.HasKey(L"value"))
                        legacyPropertyBindingsJson = std::wstring(propObj.GetNamedString(L"value"));
                    continue;
                }
                node.properties[key] = PropertyValueFromJson(propObj);
            }

            // Property bindings.
            if (obj.HasKey(L"propertyBindings"))
            {
                auto bindingsObj = obj.GetNamedObject(L"propertyBindings");
                for (const auto& pair : bindingsObj)
                {
                    auto propName = std::wstring(pair.Key());
                    auto bobj = pair.Value().GetObject();
                    PropertyBinding binding;

                    if (bobj.HasKey(L"wholeArray") && bobj.GetNamedBoolean(L"wholeArray"))
                    {
                        // New whole-array format.
                        binding.wholeArray = true;
                        binding.wholeArraySourceNodeId = static_cast<uint32_t>(bobj.GetNamedNumber(L"sourceNodeId"));
                        binding.wholeArraySourceFieldName = std::wstring(bobj.GetNamedString(L"sourceFieldName"));
                    }
                    else if (bobj.HasKey(L"sources"))
                    {
                        // New per-component format.
                        auto srcs = bobj.GetNamedArray(L"sources");
                        for (uint32_t si = 0; si < srcs.Size(); ++si)
                        {
                            if (srcs.GetAt(si).ValueType() == WDJ::JsonValueType::Null)
                            {
                                binding.sources.push_back(std::nullopt);
                            }
                            else
                            {
                                auto sobj = srcs.GetObjectAt(si);
                                ComponentSource cs;
                                cs.sourceNodeId = static_cast<uint32_t>(sobj.GetNamedNumber(L"nodeId"));
                                cs.sourceFieldName = std::wstring(sobj.GetNamedString(L"field"));
                                cs.sourceIndex = static_cast<uint32_t>(sobj.GetNamedNumber(L"index"));
                                cs.sourceComponent = static_cast<uint32_t>(sobj.GetNamedNumber(L"comp"));
                                binding.sources.push_back(cs);
                            }
                        }
                    }
                    else if (bobj.HasKey(L"sourceNodeId"))
                    {
                        // Legacy single-source format → migrate.
                        uint32_t srcNodeId = static_cast<uint32_t>(bobj.GetNamedNumber(L"sourceNodeId"));
                        auto srcField = std::wstring(bobj.GetNamedString(L"sourceFieldName"));
                        uint32_t srcComp = static_cast<uint32_t>(bobj.GetNamedNumber(L"sourceComponent"));

                        // Determine dest component count from property.
                        auto propIt = node.properties.find(propName);
                        uint32_t destCC = 1;
                        if (propIt != node.properties.end())
                        {
                            destCC = std::visit([](auto&& v) -> uint32_t
                            {
                                using T = std::decay_t<decltype(v)>;
                                if constexpr (std::is_same_v<T, float>) return 1;
                                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>) return 2;
                                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>) return 3;
                                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>) return 4;
                                else if constexpr (std::is_same_v<T, std::vector<float>>) return 0; // array
                                else return 1;
                            }, propIt->second);
                        }

                        if (destCC == 0)
                        {
                            binding.wholeArray = true;
                            binding.wholeArraySourceNodeId = srcNodeId;
                            binding.wholeArraySourceFieldName = srcField;
                        }
                        else if (destCC == 1)
                        {
                            binding.sources.push_back(ComponentSource{ srcNodeId, srcField, 0, srcComp });
                        }
                        else
                        {
                            binding.sources.resize(destCC);
                            for (uint32_t c = 0; c < destCC; ++c)
                                binding.sources[c] = ComponentSource{ srcNodeId, srcField, 0, c };
                        }
                    }

                    node.propertyBindings[propName] = std::move(binding);
                }
            }

            if (obj.HasKey(L"effectClsid"))
            {
                GUID guid{};
                CLSIDFromString(std::wstring(obj.GetNamedString(L"effectClsid")).c_str(), &guid);
                node.effectClsid = guid;
            }

            if (obj.HasKey(L"shaderPath"))
            {
                node.shaderPath = std::wstring(obj.GetNamedString(L"shaderPath"));
            }

            // Pins
            {
                auto arr = obj.GetNamedArray(L"inputPins");
                for (uint32_t i = 0; i < arr.Size(); ++i)
                {
                    auto p = arr.GetObjectAt(i);
                    node.inputPins.push_back({
                        std::wstring(p.GetNamedString(L"name")),
                        static_cast<uint32_t>(p.GetNamedNumber(L"index"))
                    });
                }
            }
            {
                auto arr = obj.GetNamedArray(L"outputPins");
                for (uint32_t i = 0; i < arr.Size(); ++i)
                {
                    auto p = arr.GetObjectAt(i);
                    node.outputPins.push_back({
                        std::wstring(p.GetNamedString(L"name")),
                        static_cast<uint32_t>(p.GetNamedNumber(L"index"))
                    });
                }
            }

            // Custom effect definition.
            if (obj.HasKey(L"customEffect"))
            {
                auto ced = obj.GetNamedObject(L"customEffect");
                CustomEffectDefinition def;
                def.shaderType = static_cast<CustomShaderType>(
                    static_cast<int>(ced.GetNamedNumber(L"shaderType")));
                def.hlslSource = std::wstring(ced.GetNamedString(L"hlslSource"));

                auto inputs = ced.GetNamedArray(L"inputNames");
                for (uint32_t i = 0; i < inputs.Size(); ++i)
                    def.inputNames.push_back(std::wstring(inputs.GetStringAt(i)));

                auto params = ced.GetNamedArray(L"parameters");
                for (uint32_t i = 0; i < params.Size(); ++i)
                {
                    auto po = params.GetObjectAt(i);
                    ParameterDefinition pd;
                    pd.name = std::wstring(po.GetNamedString(L"name"));
                    pd.typeName = std::wstring(po.GetNamedString(L"typeName"));
                    pd.minValue = static_cast<float>(po.GetNamedNumber(L"minValue"));
                    pd.maxValue = static_cast<float>(po.GetNamedNumber(L"maxValue"));
                    pd.step = static_cast<float>(po.GetNamedNumber(L"step"));
                    if (po.HasKey(L"default"))
                        pd.defaultValue = PropertyValueFromJson(po.GetNamedObject(L"default"));
                    if (po.HasKey(L"enumLabels"))
                    {
                        auto labels = po.GetNamedArray(L"enumLabels");
                        for (uint32_t j = 0; j < labels.Size(); ++j)
                            pd.enumLabels.push_back(std::wstring(labels.GetStringAt(j)));
                    }
                    if (po.HasKey(L"visibleWhen"))
                        pd.visibleWhen = std::wstring(po.GetNamedString(L"visibleWhen"));
                    if (po.HasKey(L"gpuBindable"))
                        pd.gpuBindable = po.GetNamedBoolean(L"gpuBindable");
                    def.parameters.push_back(std::move(pd));
                }

                def.threadGroupX = static_cast<uint32_t>(ced.GetNamedNumber(L"threadGroupX"));
                def.threadGroupY = static_cast<uint32_t>(ced.GetNamedNumber(L"threadGroupY"));
                def.threadGroupZ = static_cast<uint32_t>(ced.GetNamedNumber(L"threadGroupZ"));
                def.analysisOutputType = static_cast<AnalysisOutputType>(
                    static_cast<int>(ced.GetNamedNumber(L"analysisOutputType")));
                def.analysisOutputSize = static_cast<uint32_t>(ced.GetNamedNumber(L"analysisOutputSize"));

                // Deserialize typed analysis fields (new format).
                if (ced.HasKey(L"analysisFields"))
                {
                    auto fields = ced.GetNamedArray(L"analysisFields");
                    for (uint32_t fi = 0; fi < fields.Size(); ++fi)
                    {
                        auto fobj = fields.GetObjectAt(fi);
                        AnalysisFieldDescriptor fd;
                        fd.name = std::wstring(fobj.GetNamedString(L"name"));
                        auto typeTag = std::wstring(fobj.GetNamedString(L"type"));
                        if (typeTag == L"float")        fd.type = AnalysisFieldType::Float;
                        else if (typeTag == L"float2")   fd.type = AnalysisFieldType::Float2;
                        else if (typeTag == L"float3")   fd.type = AnalysisFieldType::Float3;
                        else if (typeTag == L"float4")   fd.type = AnalysisFieldType::Float4;
                        else if (typeTag == L"floatarray")  fd.type = AnalysisFieldType::FloatArray;
                        else if (typeTag == L"float2array") fd.type = AnalysisFieldType::Float2Array;
                        else if (typeTag == L"float3array") fd.type = AnalysisFieldType::Float3Array;
                        else if (typeTag == L"float4array") fd.type = AnalysisFieldType::Float4Array;
                        if (fobj.HasKey(L"length"))
                            fd.arrayLength = static_cast<uint32_t>(fobj.GetNamedNumber(L"length"));
                        if (fobj.HasKey(L"gpuPublish"))
                            fd.gpuPublish = fobj.GetNamedBoolean(L"gpuPublish");
                        def.analysisFields.push_back(std::move(fd));
                    }
                }
                // Legacy: convert old analysisFieldNames (all float4) to typed fields.
                else if (ced.HasKey(L"analysisFieldNames"))
                {
                    auto fields = ced.GetNamedArray(L"analysisFieldNames");
                    for (uint32_t fi = 0; fi < fields.Size(); ++fi)
                    {
                        AnalysisFieldDescriptor fd;
                        fd.name = std::wstring(fields.GetStringAt(fi));
                        fd.type = AnalysisFieldType::Float4;
                        def.analysisFields.push_back(std::move(fd));
                    }
                }

                // Infer Typed output type when fields exist but type wasn't set.
                if (!def.analysisFields.empty() && def.analysisOutputType == AnalysisOutputType::None)
                    def.analysisOutputType = AnalysisOutputType::Typed;

                // ShaderLab effect identity (for version upgrade detection).
                if (ced.HasKey(L"shaderLabEffectId"))
                    def.shaderLabEffectId = std::wstring(ced.GetNamedString(L"shaderLabEffectId"));
                if (ced.HasKey(L"shaderLabEffectVersion"))
                    def.shaderLabEffectVersion = static_cast<uint32_t>(ced.GetNamedNumber(L"shaderLabEffectVersion"));

                // Recompile from source on load.
                CoCreateGuid(&def.shaderGuid);
                std::string target = (def.shaderType == CustomShaderType::PixelShader)
                    ? "ps_5_0" : "cs_5_0"; // Both D2D and D3D11 compute use cs_5_0
                // WinUI TextBox stores \r as line separator; D3DCompile needs \n.
                std::string hlslUtf8(def.hlslSource.begin(), def.hlslSource.end());
                for (auto& ch : hlslUtf8)
                {
                    if (ch == '\r') ch = '\n';
                }
                auto compileResult = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
                    hlslUtf8, "GraphLoad", "main", target);
                if (compileResult.succeeded && compileResult.bytecode)
                {
                    auto* blob = compileResult.bytecode.get();
                    def.compiledBytecode.resize(blob->GetBufferSize());
                    memcpy(def.compiledBytecode.data(), blob->GetBufferPointer(), blob->GetBufferSize());
                }

                node.customEffect = std::move(def);
            }

            // Migrate legacy analysisFields from string property to customEffect.
            if (!legacyAnalysisFieldsJson.empty() && node.customEffect.has_value() &&
                node.customEffect->analysisFields.empty())
            {
                try
                {
                    auto arr = WDJ::JsonArray::Parse(legacyAnalysisFieldsJson);
                    for (uint32_t fi = 0; fi < arr.Size(); ++fi)
                    {
                        auto fobj = arr.GetObjectAt(fi);
                        AnalysisFieldDescriptor fd;
                        fd.name = std::wstring(fobj.GetNamedString(L"name"));
                        auto typeTag = std::wstring(fobj.GetNamedString(L"type"));
                        if (typeTag == L"float")        fd.type = AnalysisFieldType::Float;
                        else if (typeTag == L"float2")   fd.type = AnalysisFieldType::Float2;
                        else if (typeTag == L"float3")   fd.type = AnalysisFieldType::Float3;
                        else if (typeTag == L"float4")   fd.type = AnalysisFieldType::Float4;
                        else if (typeTag == L"floatarray")  fd.type = AnalysisFieldType::FloatArray;
                        else if (typeTag == L"float2array") fd.type = AnalysisFieldType::Float2Array;
                        else if (typeTag == L"float3array") fd.type = AnalysisFieldType::Float3Array;
                        else if (typeTag == L"float4array") fd.type = AnalysisFieldType::Float4Array;
                        if (fobj.HasKey(L"length"))
                            fd.arrayLength = static_cast<uint32_t>(fobj.GetNamedNumber(L"length"));
                        node.customEffect->analysisFields.push_back(std::move(fd));
                    }
                    if (!node.customEffect->analysisFields.empty())
                        node.customEffect->analysisOutputType = AnalysisOutputType::Typed;
                }
                catch (...) {} // Ignore malformed legacy data.
            }

            // Migrate legacy propertyBindings from string property.
            if (!legacyPropertyBindingsJson.empty() && node.propertyBindings.empty())
            {
                try
                {
                    auto arr = WDJ::JsonArray::Parse(legacyPropertyBindingsJson);
                    for (uint32_t bi = 0; bi < arr.Size(); ++bi)
                    {
                        auto bobj = arr.GetObjectAt(bi);
                        PropertyBinding binding;
                        uint32_t srcNodeId = static_cast<uint32_t>(bobj.GetNamedNumber(L"sourceNodeId"));
                        auto srcField = std::wstring(bobj.GetNamedString(L"sourceField"));
                        uint32_t srcComp = static_cast<uint32_t>(bobj.GetNamedNumber(L"component"));
                        binding.sources.push_back(ComponentSource{ srcNodeId, srcField, 0, srcComp });
                        auto targetProp = std::wstring(bobj.GetNamedString(L"targetProperty"));
                        node.propertyBindings[targetProp] = std::move(binding);
                    }
                }
                catch (...) {}
            }

            node.dirty = true;
            return node;
        }

        WDJ::JsonObject EdgeToJson(const EffectEdge& edge)
        {
            WDJ::JsonObject obj;
            obj.SetNamedValue(L"sourceNodeId", WDJ::JsonValue::CreateNumberValue(edge.sourceNodeId));
            obj.SetNamedValue(L"sourcePin", WDJ::JsonValue::CreateNumberValue(edge.sourcePin));
            obj.SetNamedValue(L"destNodeId", WDJ::JsonValue::CreateNumberValue(edge.destNodeId));
            obj.SetNamedValue(L"destPin", WDJ::JsonValue::CreateNumberValue(edge.destPin));
            return obj;
        }

        EffectEdge EdgeFromJson(const WDJ::JsonObject& obj)
        {
            return EffectEdge{
                .sourceNodeId = static_cast<uint32_t>(obj.GetNamedNumber(L"sourceNodeId")),
                .sourcePin    = static_cast<uint32_t>(obj.GetNamedNumber(L"sourcePin")),
                .destNodeId   = static_cast<uint32_t>(obj.GetNamedNumber(L"destNodeId")),
                .destPin      = static_cast<uint32_t>(obj.GetNamedNumber(L"destPin")),
            };
        }
    }

    // -----------------------------------------------------------------------
    // JSON round-trip
    // -----------------------------------------------------------------------

    winrt::hstring EffectGraph::ToJson() const
    {
        WDJ::JsonObject root;

        // Version metadata
        root.SetNamedValue(L"formatVersion", WDJ::JsonValue::CreateNumberValue(::ShaderLab::GraphFormatVersion));
        root.SetNamedValue(L"appVersion", WDJ::JsonValue::CreateStringValue(::ShaderLab::VersionString));

        // Nodes
        WDJ::JsonArray nodesArr;
        for (const auto& node : m_nodes)
            nodesArr.Append(NodeToJson(node));
        root.SetNamedValue(L"nodes", nodesArr);

        // Edges
        WDJ::JsonArray edgesArr;
        for (const auto& edge : m_edges)
            edgesArr.Append(EdgeToJson(edge));
        root.SetNamedValue(L"edges", edgesArr);

        // Next ID (so deserialized graphs can continue adding nodes)
        root.SetNamedValue(L"nextId", WDJ::JsonValue::CreateNumberValue(m_nextId));

        return root.Stringify();
    }

    EffectGraph EffectGraph::FromJson(winrt::hstring const& json)
    {
        auto root = WDJ::JsonObject::Parse(json);

        // Check graph format version compatibility.
        if (root.HasKey(L"formatVersion"))
        {
            uint32_t fileVersion = static_cast<uint32_t>(root.GetNamedNumber(L"formatVersion"));
            if (fileVersion > ::ShaderLab::GraphFormatVersion)
            {
                std::wstring appVer;
                if (root.HasKey(L"appVersion"))
                    appVer = std::wstring(root.GetNamedString(L"appVersion"));
                throw std::runtime_error(
                    "This graph was saved with a newer version of ShaderLab"
                    + (appVer.empty() ? std::string{} : " (v" + std::string(appVer.begin(), appVer.end()) + ")")
                    + ". Please update ShaderLab to open it.");
            }
        }

        EffectGraph graph;

        auto nodesArr = root.GetNamedArray(L"nodes");
        for (uint32_t i = 0; i < nodesArr.Size(); ++i)
        {
            graph.m_nodes.push_back(NodeFromJson(nodesArr.GetObjectAt(i)));
        }

        auto edgesArr = root.GetNamedArray(L"edges");
        for (uint32_t i = 0; i < edgesArr.Size(); ++i)
        {
            graph.m_edges.push_back(EdgeFromJson(edgesArr.GetObjectAt(i)));
        }

        graph.m_nextId = static_cast<uint32_t>(root.GetNamedNumber(L"nextId"));

        return graph;
    }
}
