#include "pch_engine.h"
#include "EngineMcpRoutes.h"

#include "../../Graph/EffectGraph.h"
#include "../../Rendering/GraphEvaluator.h"
#include "../../Rendering/DisplayMonitor.h"
#include "../../Rendering/PixelReadback.h"
#include "../../Rendering/CaptureNode.h"
#include "../../Rendering/DisplayProfile.h"
#include "../../Rendering/IccProfileParser.h"
#include "../../Rendering/WorkingSpaceSync.h"
#include "../../Effects/EffectRegistry.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Effects/SourceNodeFactory.h"
#include "../../Effects/ShaderCompiler.h"
#include "../../Effects/CustomComputeShaderEffect.h"
#include "../../Effects/CustomPixelShaderEffect.h"
#include "../../Effects/Performance.h"
#include "../../Version.h"

#include <winrt/Windows.Data.Json.h>
#include <filesystem>

namespace ShaderLab::Mcp
{
    namespace WDJ = winrt::Windows::Data::Json;

    namespace
    {
        // ---- Small response helpers --------------------------------------
        McpHttpServer::Response Json(uint16_t status, const std::string& body)
        {
            McpHttpServer::Response r;
            r.statusCode = status;
            r.body = body;
            r.contentType = "application/json";
            return r;
        }

        McpHttpServer::Response Error(uint16_t status, const std::string& msg)
        {
            return Json(status, "{\"error\":\"" + msg + "\"}");
        }

        std::string WideToUtf8(std::wstring_view ws)
        {
            if (ws.empty()) return {};
            int len = ::WideCharToMultiByte(CP_UTF8, 0,
                ws.data(), static_cast<int>(ws.size()),
                nullptr, 0, nullptr, nullptr);
            std::string out(len, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0,
                ws.data(), static_cast<int>(ws.size()),
                out.data(), len, nullptr, nullptr);
            return out;
        }

        // Escape a UTF-8 string for embedding in a JSON string literal.
        // Mirrors the helper that used to live in MainWindow.McpRoutes.cpp;
        // covers the required JSON escapes plus the most common control-
        // char cases. Same output bytes for ASCII-clean inputs.
        std::string JsonEscape(std::string_view s)
        {
            std::string out;
            out.reserve(s.size() + 8);
            for (char c : s)
            {
                switch (c)
                {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
                }
            }
            return out;
        }

        // Base64 (standard alphabet, '=' padding, no line wrapping).
        // Used for /render/capture-node `inline` PNG payloads.
        std::string Base64Encode(const uint8_t* data, size_t len)
        {
            static constexpr char kAlphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            if (len == 0) return out;
            out.reserve(((len + 2) / 3) * 4);
            size_t i = 0;
            while (i + 2 < len)
            {
                uint32_t v = (uint32_t(data[i]) << 16)
                           | (uint32_t(data[i + 1]) << 8)
                           |  uint32_t(data[i + 2]);
                out.push_back(kAlphabet[(v >> 18) & 0x3F]);
                out.push_back(kAlphabet[(v >> 12) & 0x3F]);
                out.push_back(kAlphabet[(v >> 6)  & 0x3F]);
                out.push_back(kAlphabet[ v        & 0x3F]);
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

        // ---- Node serialization helpers ------------------------------------
        // Used by GET /graph, GET /graph/save, GET /graph/node/{id},
        // GET /custom-effects. Mirrors the byte-identical output the
        // MainWindow helpers produced before migration so existing MCP
        // consumers don't see schema drift.

        std::string GuidToString(const GUID& g)
        {
            wchar_t buf[64]{};
            ::StringFromGUID2(g, buf, 64);
            return WideToUtf8(buf);
        }

        std::string PropertyValueToJson(const ::ShaderLab::Graph::PropertyValue& pv)
        {
            return std::visit([](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                    return std::format("{:.6f}", v);
                else if constexpr (std::is_same_v<T, int32_t>)
                    return std::format("{}", v);
                else if constexpr (std::is_same_v<T, uint32_t>)
                    return std::format("{}", v);
                else if constexpr (std::is_same_v<T, bool>)
                    return v ? "true" : "false";
                else if constexpr (std::is_same_v<T, std::wstring>)
                    return "\"" + JsonEscape(WideToUtf8(v)) + "\"";
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                    return std::format("[{:.6f},{:.6f}]", v.x, v.y);
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                    return std::format("[{:.6f},{:.6f},{:.6f}]", v.x, v.y, v.z);
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                    return std::format("[{:.6f},{:.6f},{:.6f},{:.6f}]", v.x, v.y, v.z, v.w);
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                    return "\"<matrix>\"";
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                    return "\"<curve>\"";
                else
                    return "null";
            }, pv);
        }

        const char* NodeTypeStr(::ShaderLab::Graph::NodeType t)
        {
            using NT = ::ShaderLab::Graph::NodeType;
            switch (t)
            {
            case NT::Source:        return "Source";
            case NT::BuiltInEffect: return "BuiltInEffect";
            case NT::PixelShader:   return "PixelShader";
            case NT::ComputeShader: return "ComputeShader";
            case NT::Output:        return "Output";
            }
            return "Unknown";
        }

        const char* AnalysisFieldTypeStr(::ShaderLab::Graph::AnalysisFieldType t)
        {
            using AFT = ::ShaderLab::Graph::AnalysisFieldType;
            switch (t)
            {
            case AFT::Float:       return "float";
            case AFT::Float2:      return "float2";
            case AFT::Float3:      return "float3";
            case AFT::Float4:      return "float4";
            case AFT::FloatArray:  return "floatarray";
            case AFT::Float2Array: return "float2array";
            case AFT::Float3Array: return "float3array";
            case AFT::Float4Array: return "float4array";
            }
            return "unknown";
        }

        std::string NodeToJson(const ::ShaderLab::Graph::EffectNode& node)
        {
            std::string json = "{";
            json += std::format("\"id\":{},\"name\":\"{}\",\"type\":\"{}\"",
                node.id, JsonEscape(WideToUtf8(node.name)), NodeTypeStr(node.type));
            json += std::format(",\"position\":[{:.1f},{:.1f}]",
                node.position.x, node.position.y);

            // Properties.
            json += ",\"properties\":{";
            bool first = true;
            for (const auto& [key, val] : node.properties)
            {
                if (!first) json += ",";
                json += "\"" + JsonEscape(WideToUtf8(key)) + "\":" + PropertyValueToJson(val);
                first = false;
            }
            json += "}";

            // Pins.
            json += ",\"inputPins\":[";
            for (size_t i = 0; i < node.inputPins.size(); ++i)
            {
                if (i > 0) json += ",";
                json += std::format("{{\"name\":\"{}\",\"index\":{}}}",
                    JsonEscape(WideToUtf8(node.inputPins[i].name)),
                    node.inputPins[i].index);
            }
            json += "],\"outputPins\":[";
            for (size_t i = 0; i < node.outputPins.size(); ++i)
            {
                if (i > 0) json += ",";
                json += std::format("{{\"name\":\"{}\",\"index\":{}}}",
                    JsonEscape(WideToUtf8(node.outputPins[i].name)),
                    node.outputPins[i].index);
            }
            json += "]";

            if (node.effectClsid.has_value())
                json += ",\"effectClsid\":\"" + GuidToString(node.effectClsid.value()) + "\"";
            if (!node.runtimeError.empty())
                json += ",\"runtimeError\":\"" + JsonEscape(WideToUtf8(node.runtimeError)) + "\"";

            // Custom effect definition.
            if (node.customEffect.has_value())
            {
                auto& def = node.customEffect.value();
                using CST = ::ShaderLab::Graph::CustomShaderType;
                const char* shaderTypeStr =
                    def.shaderType == CST::PixelShader ? "PixelShader" :
                    def.shaderType == CST::D3D11ComputeShader ? "D3D11ComputeShader" :
                    "ComputeShader";
                json += ",\"customEffect\":{";
                json += std::format("\"shaderType\":\"{}\",\"compiled\":{},\"bytecodeSize\":{}",
                    shaderTypeStr,
                    def.isCompiled() ? "true" : "false",
                    def.compiledBytecode.size());
                json += ",\"inputNames\":[";
                for (size_t i = 0; i < def.inputNames.size(); ++i)
                {
                    if (i > 0) json += ",";
                    json += "\"" + JsonEscape(WideToUtf8(def.inputNames[i])) + "\"";
                }
                json += "],\"parameters\":[";
                for (size_t i = 0; i < def.parameters.size(); ++i)
                {
                    if (i > 0) json += ",";
                    auto& p = def.parameters[i];
                    json += std::format(
                        "{{\"name\":\"{}\",\"type\":\"{}\",\"min\":{:.4f},\"max\":{:.4f},\"step\":{:.4f}}}",
                        JsonEscape(WideToUtf8(p.name)),
                        JsonEscape(WideToUtf8(p.typeName)),
                        p.minValue, p.maxValue, p.step);
                }
                json += "]";

                json += ",\"hlslSource\":\"" + JsonEscape(WideToUtf8(def.hlslSource)) + "\"";

                if (!def.analysisFields.empty())
                {
                    json += ",\"analysisFields\":[";
                    for (size_t i = 0; i < def.analysisFields.size(); ++i)
                    {
                        if (i > 0) json += ",";
                        const auto& fd = def.analysisFields[i];
                        json += "{\"name\":\"" + JsonEscape(WideToUtf8(fd.name))
                              + "\",\"type\":\"" + AnalysisFieldTypeStr(fd.type) + "\"";
                        if (::ShaderLab::Graph::AnalysisFieldIsArray(fd.type))
                            json += ",\"length\":" + std::to_string(fd.arrayLength);
                        json += "}";
                    }
                    json += "]";
                }
                json += "}";
            }

            // Analysis output results (runtime data).
            using AOT = ::ShaderLab::Graph::AnalysisOutputType;
            if (node.analysisOutput.type == AOT::Typed && !node.analysisOutput.fields.empty())
            {
                json += ",\"analysisResults\":[";
                bool firstField = true;
                for (const auto& fv : node.analysisOutput.fields)
                {
                    if (!firstField) json += ",";
                    firstField = false;
                    json += "{\"name\":\"" + JsonEscape(WideToUtf8(fv.name)) + "\"";
                    if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                    {
                        uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                        json += ",\"value\":[";
                        for (uint32_t c = 0; c < cc; ++c)
                        {
                            if (c > 0) json += ",";
                            json += std::format("{:.6f}", fv.components[c]);
                        }
                        json += "]";
                    }
                    else
                    {
                        json += ",\"value\":[";
                        for (size_t i = 0; i < fv.arrayData.size(); ++i)
                        {
                            if (i > 0) json += ",";
                            json += std::format("{:.6f}", fv.arrayData[i]);
                        }
                        json += "]";
                    }
                    json += "}";
                }
                json += "]";
            }
            else if (node.analysisOutput.type == AOT::Histogram &&
                     !node.analysisOutput.data.empty())
            {
                json += std::format(
                    ",\"analysisResults\":{{\"type\":\"histogram\",\"channel\":{},\"bins\":{}}}",
                    node.analysisOutput.channelIndex,
                    node.analysisOutput.data.size());
            }

            json += "}";
            return json;
        }

        // ---- Phase 7 incremental migration ---------------------------------
        // Each route here used to live in MainWindow.McpRoutes.cpp.
        // Migration pattern:
        //   1. Copy route body into a Register* free function here.
        //   2. Wrap in sink.Dispatch where the route mutates engine state
        //      (anything touching m_graph, properties, etc).
        //   3. Replace MainWindow.McpRoutes.cpp body with a "moved to
        //      EngineMcpRoutes" comment marker.
        //   4. Build + verify.
        //
        // UI-coupled routes stay in MainWindow.McpRoutes.cpp:
        // graph_snapshot, preview/graph view tools, render/preview-node.

        // ---- GET /registry — D2D + ShaderLab effect catalog (static) -------
        void RegisterRegistry(McpHttpServer& server)
        {
            server.AddRoute(L"GET", L"/registry",
                [](const std::wstring& path, const std::string&) -> McpHttpServer::Response {
                    auto& reg = ::ShaderLab::Effects::EffectRegistry::Instance();

                    // /registry/effect/<name> — detailed effect info.
                    if (path.starts_with(L"/registry/effect/"))
                    {
                        auto name = path.substr(17);
                        auto* desc = reg.FindByName(name);
                        if (!desc) return Json(404, R"({"error":"Effect not found"})");

                        WDJ::JsonObject o;
                        o.Insert(L"name", WDJ::JsonValue::CreateStringValue(desc->name));
                        o.Insert(L"category", WDJ::JsonValue::CreateStringValue(desc->category));
                        o.Insert(L"inputCount", WDJ::JsonValue::CreateNumberValue(
                            static_cast<double>(desc->inputPins.size())));
                        WDJ::JsonObject props;
                        for (const auto& [key, meta] : desc->propertyMetadata)
                        {
                            WDJ::JsonObject m;
                            m.Insert(L"min", WDJ::JsonValue::CreateNumberValue(meta.minValue));
                            m.Insert(L"max", WDJ::JsonValue::CreateNumberValue(meta.maxValue));
                            m.Insert(L"step", WDJ::JsonValue::CreateNumberValue(meta.step));
                            if (!meta.enumLabels.empty())
                            {
                                WDJ::JsonArray labels;
                                for (const auto& lab : meta.enumLabels)
                                    labels.Append(WDJ::JsonValue::CreateStringValue(lab));
                                m.Insert(L"enumLabels", labels);
                            }
                            props.Insert(key, m);
                        }
                        o.Insert(L"properties", props);
                        return Json(200, WideToUtf8(o.Stringify()));
                    }

                    // /registry — list all effects.
                    WDJ::JsonArray arr;
                    for (const auto& d : reg.All())
                    {
                        WDJ::JsonObject o;
                        o.Insert(L"name", WDJ::JsonValue::CreateStringValue(d.name));
                        o.Insert(L"category", WDJ::JsonValue::CreateStringValue(d.category));
                        o.Insert(L"inputCount", WDJ::JsonValue::CreateNumberValue(
                            static_cast<double>(d.inputPins.size())));
                        arr.Append(o);
                    }
                    return Json(200, WideToUtf8(arr.Stringify()));
                });
        }

        // ---- POST /graph/connect — wire output pin -> input pin -----------
        // Note: the GUI app also runs m_nodeGraphController.AutoLayout()
        // and adds NodeLog entries. Those are UI side effects; the engine
        // route just does the graph mutation. The GUI's render tick will
        // pick up the dirty state and refresh the canvas next frame.
        void RegisterConnect(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/connect",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t srcId = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcId"));
                            uint32_t srcPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcPin"));
                            uint32_t dstId = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstId"));
                            uint32_t dstPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstPin"));
                            bool ok = ctx.graph->Connect(srcId, srcPin, dstId, dstPin);
                            ctx.graph->MarkAllDirty();
                            if (ok) sink.OnGraphStructureChanged();
                            return Json(200,
                                std::string("{\"connected\":") + (ok ? "true" : "false") + "}");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/disconnect — remove a single edge ----------------
        void RegisterDisconnect(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/disconnect",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t srcId = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcId"));
                            uint32_t srcPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcPin"));
                            uint32_t dstId = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstId"));
                            uint32_t dstPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstPin"));
                            bool ok = ctx.graph->Disconnect(srcId, srcPin, dstId, dstPin);
                            ctx.graph->MarkAllDirty();
                            if (ok) sink.OnGraphStructureChanged();
                            return Json(200,
                                std::string("{\"disconnected\":") + (ok ? "true" : "false") + "}");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/bind-property — bind property to analysis output -
        // Note: the GUI's m_nodeGraphController.RebuildLayout() drops out;
        // the render tick will pick up the dirty state and rebuild
        // automatically. Headless host has no canvas anyway.
        void RegisterBindProperty(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/bind-property",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            auto propName = std::wstring(jobj.GetNamedString(L"propertyName"));
                            uint32_t srcNodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"sourceNodeId"));
                            auto srcFieldName = std::wstring(jobj.GetNamedString(L"sourceFieldName"));
                            uint32_t srcComponent = jobj.HasKey(L"sourceComponent")
                                ? static_cast<uint32_t>(jobj.GetNamedNumber(L"sourceComponent")) : 0;

                            auto err = ctx.graph->BindProperty(nodeId, propName, srcNodeId, srcFieldName, srcComponent);
                            if (!err.empty())
                                return Json(400, "{\"error\":\"" + WideToUtf8(err) + "\"}");
                            sink.OnGraphStructureChanged();
                            return Json(200, R"({"ok":true})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/unbind-property -----------------------------------
        void RegisterUnbindProperty(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/unbind-property",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            auto propName = std::wstring(jobj.GetNamedString(L"propertyName"));
                            if (!ctx.graph->UnbindProperty(nodeId, propName))
                                return Json(404, R"({"error":"No binding for that property"})");
                            sink.OnGraphStructureChanged();
                            return Json(200, R"({"ok":true})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/add-node ------------------------------------------
        // Big route with multiple node-type branches:
        //   * "Custom Compute Shader" / "Custom Pixel Shader" / "Custom D3D11
        //     Compute Shader" -- create a fresh user-authored shader node.
        //   * Any ShaderLab effect name -- look up in ShaderLabEffects registry.
        //   * "Video Source" / "Image Source" -- create + PrepareSourceNode.
        //   * Any built-in D2D effect name -- look up in EffectRegistry.
        //
        // After AddNode the OnNodeAdded event fires so the host (if any)
        // can run AutoLayout + PopulatePreviewNodeSelector + log entry.
        // Same UI path the toolbar AddNode flyout takes.
        void RegisterAddNode(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/add-node",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            if (!jobj.HasKey(L"effectName"))
                                return Json(400, R"({"error":"Provide effectName"})");
                            auto name = jobj.GetNamedString(L"effectName");

                            auto addAndReply = [&](Graph::EffectNode&& node) -> McpHttpServer::Response {
                                auto id = ctx.graph->AddNode(std::move(node));
                                ctx.graph->MarkAllDirty();
                                sink.OnNodeAdded(id);
                                return Json(200, "{\"nodeId\":" + std::to_string(id) + "}");
                            };

                            // Custom shader node creation.
                            if (name == L"Custom Compute Shader" || name == L"Custom Pixel Shader" ||
                                name == L"Custom D3D11 Compute Shader")
                            {
                                Graph::EffectNode node;
                                bool isCompute = (name == L"Custom Compute Shader");
                                bool isD3D11 = (name == L"Custom D3D11 Compute Shader");
                                node.type = (isCompute || isD3D11)
                                    ? Graph::NodeType::ComputeShader
                                    : Graph::NodeType::PixelShader;
                                node.name = std::wstring(name);

                                if (!isD3D11)
                                {
                                    node.effectClsid = isCompute
                                        ? ::ShaderLab::Effects::CustomComputeShaderEffect::CLSID_CustomComputeShader
                                        : ::ShaderLab::Effects::CustomPixelShaderEffect::CLSID_CustomPixelShader;
                                    node.outputPins.push_back({ L"Output", 0 });
                                }

                                Graph::CustomEffectDefinition def;
                                def.shaderType = isD3D11
                                    ? Graph::CustomShaderType::D3D11ComputeShader
                                    : isCompute
                                        ? Graph::CustomShaderType::ComputeShader
                                        : Graph::CustomShaderType::PixelShader;
                                CoCreateGuid(&def.shaderGuid);
                                def.inputNames.push_back(L"Source");
                                node.inputPins.push_back({ L"I0", 0 });
                                if (isCompute) { def.threadGroupX = 8; def.threadGroupY = 8; def.threadGroupZ = 1; }
                                if (isD3D11)
                                {
                                    def.analysisOutputType = Graph::AnalysisOutputType::Typed;
                                    def.analysisFields.push_back(
                                        { L"Result", Graph::AnalysisFieldType::Float4 });
                                }
                                node.customEffect = std::move(def);
                                return addAndReply(std::move(node));
                            }

                            // ShaderLab effects registry lookup.
                            if (auto* slDesc = ::ShaderLab::Effects::ShaderLabEffects::Instance().FindByName(name))
                            {
                                auto node = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*slDesc);
                                return addAndReply(std::move(node));
                            }

                            // Built-in D2D effects registry lookup.
                            if (auto* desc = ::ShaderLab::Effects::EffectRegistry::Instance().FindByName(name))
                            {
                                auto node = ::ShaderLab::Effects::EffectRegistry::CreateNode(*desc);
                                return addAndReply(std::move(node));
                            }

                            // Special source types: Video / Image. Optional
                            // filePath; if present, PrepareSourceNode kicks in.
                            std::wstring wname(name.begin(), name.end());
                            std::wstring filePath;
                            if (jobj.HasKey(L"filePath"))
                                filePath = std::wstring(jobj.GetNamedString(L"filePath"));

                            auto displayName = [&](const wchar_t* fallback) {
                                return filePath.empty() ? std::wstring(fallback)
                                    : filePath.substr(filePath.find_last_of(L"\\/") + 1);
                            };

                            if (wname == L"Video Source" || wname == L"Video")
                            {
                                auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateVideoSourceNode(
                                    filePath, displayName(L"Video Source"));
                                auto id = ctx.graph->AddNode(std::move(node));
                                if (!filePath.empty() && ctx.sourceFactory && ctx.dc)
                                {
                                    if (auto* graphNode = ctx.graph->FindNode(id))
                                        ctx.sourceFactory->PrepareSourceNode(*graphNode,
                                            static_cast<ID2D1DeviceContext5*>(ctx.dc), 0.0,
                                            ctx.d3dDevice, ctx.d3dContext);
                                }
                                ctx.graph->MarkAllDirty();
                                sink.OnNodeAdded(id);
                                return Json(200, "{\"nodeId\":" + std::to_string(id) + "}");
                            }
                            if (wname == L"Image Source" || wname == L"Image")
                            {
                                auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(
                                    filePath, displayName(L"Image Source"));
                                auto id = ctx.graph->AddNode(std::move(node));
                                if (!filePath.empty() && ctx.sourceFactory && ctx.dc)
                                {
                                    if (auto* graphNode = ctx.graph->FindNode(id))
                                        ctx.sourceFactory->PrepareSourceNode(*graphNode,
                                            static_cast<ID2D1DeviceContext5*>(ctx.dc), 0.0,
                                            ctx.d3dDevice, ctx.d3dContext);
                                }
                                ctx.graph->MarkAllDirty();
                                sink.OnNodeAdded(id);
                                return Json(200, "{\"nodeId\":" + std::to_string(id) + "}");
                            }

                            return Json(400, R"({"error":"Unknown effect name"})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid JSON"})"); }
                    });
                });
        }

        // ---- GET /effect/hlsl/<nodeId> -- read HLSL of a custom effect -----
        // Read-only; no Dispatch needed. Library effects (ShaderLab built-in)
        // are reported with isLibraryEffect=true so agents know they're
        // read-only-shipped and shouldn't try to recompile them.
        void RegisterEffectHlsl(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/effect/hlsl/",
                [&sink](const std::wstring& path, const std::string&) -> McpHttpServer::Response
                {
                    return sink.Dispatch([path](EngineContext& ctx) -> McpHttpServer::Response {
                        if (path.size() <= 13)
                            return Json(400, R"({"error":"Missing nodeId in URL"})");
                        uint32_t nodeId = 0;
                        try { nodeId = static_cast<uint32_t>(std::stoul(path.substr(13))); }
                        catch (...) { return Json(400, R"({"error":"Invalid nodeId"})"); }

                        auto* node = ctx.graph->FindNode(nodeId);
                        if (!node)
                            return Json(404, "{\"error\":\"Node " + std::to_string(nodeId) + " not found\"}");

                        std::string runtimeErr = JsonEscape(WideToUtf8(node->runtimeError));
                        std::string nameEsc = JsonEscape(WideToUtf8(node->name));

                        if (!node->customEffect.has_value())
                        {
                            std::string j = "{\"nodeId\":" + std::to_string(nodeId)
                                + ",\"hasCustomEffect\":false,\"name\":\"" + nameEsc
                                + "\",\"runtimeError\":\"" + runtimeErr + "\"}";
                            return Json(200, j);
                        }

                        const auto& def = node->customEffect.value();
                        const char* shaderTypeStr =
                            (def.shaderType == Graph::CustomShaderType::PixelShader)
                                ? "PixelShader" : "ComputeShader";

                        std::string inputsJson = "[";
                        for (size_t i = 0; i < def.inputNames.size(); ++i)
                        {
                            if (i) inputsJson += ",";
                            inputsJson += "\"" + JsonEscape(WideToUtf8(def.inputNames[i])) + "\"";
                        }
                        inputsJson += "]";

                        std::string paramsJson = "[";
                        for (size_t i = 0; i < def.parameters.size(); ++i)
                        {
                            if (i) paramsJson += ",";
                            paramsJson += "{\"name\":\""
                                + JsonEscape(WideToUtf8(def.parameters[i].name)) + "\"}";
                        }
                        paramsJson += "]";

                        std::string libBlock;
                        if (!def.shaderLabEffectId.empty())
                        {
                            libBlock = std::string(",\"isLibraryEffect\":true,\"shaderLabEffectId\":\"")
                                + JsonEscape(WideToUtf8(def.shaderLabEffectId))
                                + "\",\"shaderLabEffectVersion\":"
                                + std::to_string(def.shaderLabEffectVersion);
                        }
                        else
                        {
                            libBlock = ",\"isLibraryEffect\":false";
                        }

                        std::string j = "{\"nodeId\":" + std::to_string(nodeId)
                            + ",\"hasCustomEffect\":true,\"name\":\"" + nameEsc + "\""
                            + ",\"shaderType\":\"" + shaderTypeStr + "\""
                            + ",\"hlslSource\":\"" + JsonEscape(WideToUtf8(def.hlslSource)) + "\""
                            + ",\"inputNames\":" + inputsJson
                            + ",\"parameters\":" + paramsJson
                            + ",\"bytecodeSize\":" + std::to_string(def.compiledBytecode.size())
                            + ",\"isCompiled\":" + (def.isCompiled() ? "true" : "false")
                            + ",\"runtimeError\":\"" + runtimeErr + "\""
                            + libBlock + "}";
                        return Json(200, j);
                    });
                });
        }

        // ---- POST /graph/clear ---------------------------------------------
        // Engine drops graph state and the evaluator cache. The OnGraphCleared
        // event runs the host's UI cleanup (output windows, preview selector
        // reset). Same path /graph/clear via UI button takes.
        void RegisterClear(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/clear",
                [&sink](const std::wstring&, const std::string&) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&sink](EngineContext& ctx) -> McpHttpServer::Response {
                        ctx.evaluator->ReleaseCache();
                        ctx.graph->Clear();
                        ctx.graph->MarkAllDirty();
                        sink.OnGraphCleared();
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // ---- POST /graph/load ----------------------------------------------
        // Body is the full graph JSON. Engine deserializes + assigns; the
        // OnGraphLoaded event runs the host's per-load setup (heartbeats,
        // re-opens output windows for nodes that had them, preview selector
        // refresh). Same path the file-open dialog takes.
        void RegisterLoad(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/load",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    // Parse on the listener thread; assignment requires the
                    // dispatch thread (it owns m_graph).
                    Graph::EffectGraph loaded;
                    try
                    {
                        loaded = Graph::EffectGraph::FromJson(winrt::to_hstring(body));
                    }
                    catch (const std::exception& ex)
                    {
                        return Json(400, std::string(R"({"error":")") + ex.what() + R"("})");
                    }
                    return sink.Dispatch([&loaded, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        ctx.evaluator->ReleaseCache();
                        *ctx.graph = std::move(loaded);
                        ctx.graph->MarkAllDirty();
                        sink.OnGraphLoaded();
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // ---- POST /graph/remove-node ---------------------------------------
        void RegisterRemoveNode(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/remove-node",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            ctx.graph->RemoveNode(nodeId);
                            ctx.graph->MarkAllDirty();
                            sink.OnNodeRemoved(nodeId);
                            return Json(200, R"({"ok":true})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/set-property — mutates m_graph -------------------
        void RegisterSetProperty(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/set-property",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            auto key = std::wstring(jobj.GetNamedString(L"key"));
                            auto val = jobj.GetNamedValue(L"value");

                            auto* node = ctx.graph->FindNode(nodeId);
                            if (!node) return Json(404, R"({"error":"Node not found"})");

                            switch (val.ValueType())
                            {
                            case WDJ::JsonValueType::Number:
                            {
                                bool isUint = false;
                                if (node->customEffect.has_value())
                                {
                                    for (const auto& p : node->customEffect->parameters)
                                    {
                                        if (p.name == key && p.typeName == L"uint")
                                        { isUint = true; break; }
                                    }
                                }
                                auto existIt = node->properties.find(key);
                                if (existIt != node->properties.end() &&
                                    std::holds_alternative<uint32_t>(existIt->second))
                                    isUint = true;

                                if (isUint)
                                    node->properties[key] = static_cast<uint32_t>(val.GetNumber());
                                else
                                    node->properties[key] = static_cast<float>(val.GetNumber());
                                break;
                            }
                            case WDJ::JsonValueType::Boolean:
                                node->properties[key] = val.GetBoolean();
                                break;
                            case WDJ::JsonValueType::String:
                                node->properties[key] = std::wstring(val.GetString());
                                break;
                            case WDJ::JsonValueType::Array:
                            {
                                auto arr = val.GetArray();
                                if (arr.Size() == 2)
                                    node->properties[key] = winrt::Windows::Foundation::Numerics::float2{
                                        static_cast<float>(arr.GetAt(0).GetNumber()),
                                        static_cast<float>(arr.GetAt(1).GetNumber()) };
                                else if (arr.Size() == 3)
                                    node->properties[key] = winrt::Windows::Foundation::Numerics::float3{
                                        static_cast<float>(arr.GetAt(0).GetNumber()),
                                        static_cast<float>(arr.GetAt(1).GetNumber()),
                                        static_cast<float>(arr.GetAt(2).GetNumber()) };
                                else if (arr.Size() == 4)
                                    node->properties[key] = winrt::Windows::Foundation::Numerics::float4{
                                        static_cast<float>(arr.GetAt(0).GetNumber()),
                                        static_cast<float>(arr.GetAt(1).GetNumber()),
                                        static_cast<float>(arr.GetAt(2).GetNumber()),
                                        static_cast<float>(arr.GetAt(3).GetNumber()) };
                                break;
                            }
                            default:
                                return Json(400, R"({"error":"Unsupported value type"})");
                            }
                            node->dirty = true;
                            ctx.graph->MarkAllDirty();

                            // Special-cased properties that mirror to dedicated node fields.
                            // Match both casings: graph storage uses `IsPlaying`
                            // (PascalCase, matching the Clock + Video effect
                            // descriptors and graph_get_node output), but legacy
                            // callers used lowercase. Without this mirror, the
                            // runtime `node.isPlaying` bool stays false and
                            // Clock/Video never tick (RenderTick gates on
                            // `node.isPlaying`, not on the property map).
                            if (key == L"isPlaying" || key == L"IsPlaying")
                            {
                                if (auto* bv = std::get_if<bool>(&node->properties[key]))
                                    node->isPlaying = *bv;
                            }
                            if (key == L"shaderPath")
                            {
                                if (auto* sv = std::get_if<std::wstring>(&node->properties[key]))
                                    node->shaderPath = *sv;
                            }

                            sink.OnNodeChanged(nodeId);
                            return Json(200, R"({"ok":true})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }


        void RegisterPixelRegion(McpHttpServer& server, IEngineCommandSink& sink)
        {
            // POST /render/pixel-region -- Read FP32 RGBA pixel grid.
            // Body: { nodeId, x, y, w, h }   (capped at 32x32 = 1024 pixels)
            server.AddRoute(L"POST", L"/render/pixel-region",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        WDJ::JsonObject jo{ nullptr };
                        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                            return Json(400, R"({"error":"Invalid JSON body"})");
                        for (auto k : { L"nodeId", L"x", L"y", L"w", L"h" })
                            if (!jo.HasKey(k))
                                return Json(400,
                                    "{\"error\":\"Missing required field: " + WideToUtf8(k) + "\"}");

                        uint32_t nodeId = static_cast<uint32_t>(jo.GetNamedNumber(L"nodeId"));
                        int32_t  x = static_cast<int32_t>(jo.GetNamedNumber(L"x"));
                        int32_t  y = static_cast<int32_t>(jo.GetNamedNumber(L"y"));
                        uint32_t w = static_cast<uint32_t>(jo.GetNamedNumber(L"w"));
                        uint32_t h = static_cast<uint32_t>(jo.GetNamedNumber(L"h"));

                        // Cap region area at 32x32 (1024 pixels). Per-axis cap of
                        // 64 lets the agent ask for a thin strip (e.g. 64x4) but
                        // never more than 1024 total samples.
                        if (w == 0 || h == 0)
                            return Json(400, R"({"error":"w and h must be > 0"})");
                        if (w > 64 || h > 64 || (w * h) > 1024)
                            return Json(400,
                                "{\"error\":\"Region too large (cap: each axis <= 64, total area <= 1024)\"}");

                        // Force a fresh frame so dirty nodes evaluate before
                        // readback. Headless host's renderFrame is a no-op
                        // (caller is expected to have evaluated the graph).
                        if (ctx.renderFrame) ctx.renderFrame();

                        auto rr = Rendering::ReadPixelRegion(*ctx.graph, nodeId, x, y, w, h, ctx.dc);
                        switch (rr.status)
                        {
                        case Rendering::ReadPixelRegionStatus::NotFound:
                            return Json(404,
                                "{\"error\":\"Node " + std::to_string(nodeId) + " not found\"}");
                        case Rendering::ReadPixelRegionStatus::NotReady:
                            return Json(409,
                                "{\"error\":\"Node " + std::to_string(nodeId)
                                + " is not yet evaluated\",\"notReady\":true}");
                        case Rendering::ReadPixelRegionStatus::InvalidRegion:
                            return Json(404, R"({"error":"Region is empty after clipping to image bounds"})");
                        case Rendering::ReadPixelRegionStatus::D2DError:
                            return Json(500, R"({"error":"D2D readback error"})");
                        case Rendering::ReadPixelRegionStatus::Success: break;
                        }

                        // Build the JSON response. Float formatting matches the
                        // pre-migration MainWindow body so MCP clients see the
                        // same representation.
                        std::string json = "{\"nodeId\":" + std::to_string(nodeId)
                            + ",\"requestedX\":" + std::to_string(x)
                            + ",\"requestedY\":" + std::to_string(y)
                            + ",\"requestedW\":" + std::to_string(w)
                            + ",\"requestedH\":" + std::to_string(h)
                            + ",\"actualW\":" + std::to_string(rr.actualWidth)
                            + ",\"actualH\":" + std::to_string(rr.actualHeight)
                            + ",\"channelOrder\":[\"r\",\"g\",\"b\",\"a\"],\"pixels\":[";
                        char buf[32];
                        for (size_t i = 0; i < rr.pixels.size(); ++i)
                        {
                            if (i) json += ",";
                            int n = std::snprintf(buf, sizeof(buf), "%.6f", rr.pixels[i]);
                            json.append(buf, n);
                        }
                        json += "]}";
                        return Json(200, json);
                    });
                });
        }
        // ---- GET /graph — full graph state, /graph/save, /graph/node/{id} -
        // The host that wants /graph to surface a "previewNodeId" provides
        // ctx.getPreviewNodeId. Headless leaves it null and we emit 0,
        // which matches "no preview pane" semantics.
        void RegisterGetGraph(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/graph",
                [&sink](const std::wstring& path, const std::string&) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&path](EngineContext& ctx) -> McpHttpServer::Response {
                        // /graph/save -> raw graph JSON via EffectGraph::ToJson.
                        if (path == L"/graph/save")
                        {
                            auto json = ctx.graph->ToJson();
                            return Json(200, WideToUtf8(std::wstring(json)));
                        }
                        // /graph/node/{id} -> single node detail.
                        if (path.starts_with(L"/graph/node/"))
                        {
                            auto idStr = path.substr(12);
                            uint32_t nodeId = 0;
                            try { nodeId = static_cast<uint32_t>(std::stoul(idStr)); }
                            catch (...) { return Json(400, R"({"error":"Invalid node ID"})"); }
                            auto* node = ctx.graph->FindNode(nodeId);
                            if (!node) return Json(404, R"({"error":"Node not found"})");
                            return Json(200, NodeToJson(*node));
                        }

                        // /graph -> full graph state.
                        std::string json = "{\"nodes\":[";
                        bool first = true;
                        for (const auto& node : ctx.graph->Nodes())
                        {
                            if (!first) json += ",";
                            json += NodeToJson(node);
                            first = false;
                        }
                        json += "],\"edges\":[";
                        first = true;
                        for (const auto& edge : ctx.graph->Edges())
                        {
                            if (!first) json += ",";
                            json += std::format(
                                "{{\"srcId\":{},\"srcPin\":{},\"dstId\":{},\"dstPin\":{}}}",
                                edge.sourceNodeId, edge.sourcePin,
                                edge.destNodeId, edge.destPin);
                            first = false;
                        }
                        uint32_t previewId = ctx.getPreviewNodeId ? ctx.getPreviewNodeId() : 0;
                        json += std::format("],\"previewNodeId\":{}}}", previewId);
                        return Json(200, json);
                    });
                });
        }

        // ---- GET /custom-effects — all nodes with a customEffect def -----
        void RegisterCustomEffects(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/custom-effects",
                [&sink](const std::wstring&, const std::string&) -> McpHttpServer::Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> McpHttpServer::Response {
                        std::string json = "[";
                        bool first = true;
                        for (const auto& node : ctx.graph->Nodes())
                        {
                            if (!node.customEffect.has_value()) continue;
                            if (!first) json += ",";
                            json += NodeToJson(node);
                            first = false;
                        }
                        json += "]";
                        return Json(200, json);
                    });
                });
        }

        // ---- GET /analysis/{id} — analysis output fields ------------------
        void RegisterAnalysisOutput(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/analysis/",
                [&sink](const std::wstring& path, const std::string&) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&path](EngineContext& ctx) -> McpHttpServer::Response {
                        auto rest = path.substr(10); // after "/analysis/"
                        uint32_t nodeId = 0;
                        try { nodeId = static_cast<uint32_t>(std::stoul(rest)); }
                        catch (...) { return Json(400, R"({"error":"Invalid node ID"})"); }
                        auto* node = ctx.graph->FindNode(nodeId);
                        if (!node) return Json(404, R"({"error":"Node not found"})");

                        // Phase 8c: ensure freshness. If the host has the
                        // skip-readback flag enabled, the node's
                        // analysisOutput.fields may be stale by 1+ frames.
                        // Temporarily disable the flag, force a render,
                        // then restore. Cost: one frame of full readback
                        // per MCP analysis read (acceptable given MCP
                        // calls are out-of-band and infrequent).
                        const bool prevSkip =
                            ::ShaderLab::Performance::IsSkipUnneededCpuReadbackEnabled();
                        if (prevSkip && ctx.renderFrame)
                        {
                            ::ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
                            ctx.renderFrame();
                            ::ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(prevSkip);
                            // Re-resolve: the graph could have changed.
                            node = ctx.graph->FindNode(nodeId);
                            if (!node) return Json(404, R"({"error":"Node not found"})");
                        }

                        using AOT = ::ShaderLab::Graph::AnalysisOutputType;
                        if (node->analysisOutput.type != AOT::Typed ||
                            node->analysisOutput.fields.empty())
                            return Json(200, R"({"fields":[]})");

                        std::string json = R"({"fields":[)";
                        bool first = true;
                        for (const auto& fv : node->analysisOutput.fields)
                        {
                            if (!first) json += ",";
                            first = false;
                            json += "{\"name\":\"" + JsonEscape(WideToUtf8(fv.name)) + "\"";
                            json += ",\"type\":\"" + std::string(AnalysisFieldTypeStr(fv.type)) + "\"";
                            if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                            {
                                uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                                json += ",\"value\":[";
                                for (uint32_t c = 0; c < cc; ++c)
                                {
                                    if (c > 0) json += ",";
                                    json += std::format("{:.6f}", fv.components[c]);
                                }
                                json += "]";
                            }
                            else
                            {
                                uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                                uint32_t count = stride > 0
                                    ? static_cast<uint32_t>(fv.arrayData.size()) / stride
                                    : 0;
                                json += ",\"count\":" + std::to_string(count);
                                json += ",\"value\":[";
                                for (size_t i = 0; i < fv.arrayData.size(); ++i)
                                {
                                    if (i > 0) json += ",";
                                    json += std::format("{:.6f}", fv.arrayData[i]);
                                }
                                json += "]";
                            }
                            json += "}";
                        }
                        json += "]}";
                        return Json(200, json);
                    });
                });
        }
        // ---- POST /render/image-bounds — return raw GetImageLocalBounds -----
        // Body: { nodeId }. Returns { width, height } at 96 DPI in pixels.
        // Useful for diagnosing rect-bloat issues that the capture-node
        // route hides via its maxDim clamp.
        void RegisterImageBounds(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/render/image-bounds",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body](EngineContext& ctx) -> McpHttpServer::Response {
                        WDJ::JsonObject jo{ nullptr };
                        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                            return Json(400, R"({"error":"Invalid JSON body"})");
                        if (!jo.HasKey(L"nodeId"))
                            return Json(400, R"({"error":"'nodeId' is required"})");
                        uint32_t nodeId = static_cast<uint32_t>(jo.GetNamedNumber(L"nodeId"));
                        if (ctx.renderFrame) ctx.renderFrame();
                        auto* node = ctx.graph->FindNode(nodeId);
                        if (!node || !node->cachedOutput)
                            return Json(404, R"({"error":"Node not ready"})");
                        float oldDpiX = 0, oldDpiY = 0;
                        ctx.dc->GetDpi(&oldDpiX, &oldDpiY);
                        ctx.dc->SetDpi(96.0f, 96.0f);
                        D2D1_RECT_F bounds{};
                        ctx.dc->GetImageLocalBounds(node->cachedOutput, &bounds);
                        ctx.dc->SetDpi(oldDpiX, oldDpiY);
                        return Json(200, std::format(
                            R"({{"left":{},"top":{},"right":{},"bottom":{},"width":{},"height":{}}})",
                            bounds.left, bounds.top, bounds.right, bounds.bottom,
                            bounds.right - bounds.left, bounds.bottom - bounds.top));
                    });
                });
        }

        // ---- POST /render/capture-node — render any node to PNG ------------
        // Body: { nodeId, inline?:bool }. Saves PNG to %TEMP%; returns
        // path + size. If inline=true, also returns a base64 PNG payload.
        // Uses Rendering::CaptureNodeAsPng so this route is identical
        // between GUI and headless hosts.
        void RegisterCaptureNode(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/render/capture-node",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body](EngineContext& ctx) -> McpHttpServer::Response {
                        WDJ::JsonObject jo{ nullptr };
                        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                            return Json(400, R"({"error":"Invalid JSON body"})");
                        if (!jo.HasKey(L"nodeId"))
                            return Json(400, R"({"error":"'nodeId' is required"})");
                        uint32_t nodeId = static_cast<uint32_t>(jo.GetNamedNumber(L"nodeId"));
                        bool wantInline = jo.HasKey(L"inline")
                            && jo.GetNamedValue(L"inline").ValueType() == WDJ::JsonValueType::Boolean
                            && jo.GetNamedBoolean(L"inline");

                        // Force a fresh frame so dirty nodes evaluate before
                        // capture. Headless host's renderFrame is a no-op.
                        if (ctx.renderFrame) ctx.renderFrame();

                        auto cap = ::ShaderLab::Rendering::CaptureNodeAsPng(
                            *ctx.graph, nodeId, ctx.dc);
                        using S = ::ShaderLab::Rendering::CaptureNodeStatus;
                        switch (cap.status)
                        {
                        case S::NotFound:
                            return Json(404, std::format(
                                R"({{"error":"Node {} not found"}})", nodeId));
                        case S::NotReady:
                            return Json(409, std::format(
                                R"({{"error":"Node {} is not yet evaluated","notReady":true}})", nodeId));
                        case S::EmptyImage:
                            return Json(500, R"({"error":"Capture failed: empty image"})");
                        case S::D2DError:
                            return Json(500, R"({"error":"Capture failed"})");
                        case S::Success:
                            break;
                        }

                        // Persist to %TEMP% with PID + nodeId + monotonic seq
                        // so concurrent captures don't collide.
                        static std::atomic<uint32_t> s_seq{ 0 };
                        uint32_t seq = s_seq.fetch_add(1, std::memory_order_relaxed);
                        wchar_t tempPath[MAX_PATH]{};
                        ::GetTempPathW(MAX_PATH, tempPath);
                        std::wstring filePath = std::format(
                            L"{}shaderlab_node_{}_{}_{}.png",
                            tempPath, ::GetCurrentProcessId(), nodeId, seq);
                        HANDLE hFile = ::CreateFileW(filePath.c_str(),
                            GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (hFile == INVALID_HANDLE_VALUE)
                            return Json(500, R"({"error":"Failed to create temp file"})");
                        DWORD written = 0;
                        ::WriteFile(hFile, cap.png.data(),
                            static_cast<DWORD>(cap.png.size()), &written, nullptr);
                        ::CloseHandle(hFile);

                        std::string escapedPath = JsonEscape(WideToUtf8(filePath));
                        if (wantInline)
                        {
                            auto b64 = Base64Encode(cap.png.data(), cap.png.size());
                            return Json(200, std::format(
                                R"({{"path":"{}","size":{},"nodeId":{},"width":{},"height":{},"mimeType":"image/png","base64":"{}"}})",
                                escapedPath, cap.png.size(), nodeId,
                                cap.width, cap.height, b64));
                        }
                        return Json(200, std::format(
                            R"({{"path":"{}","size":{},"nodeId":{},"width":{},"height":{},"mimeType":"image/png"}})",
                            escapedPath, cap.png.size(), nodeId,
                            cap.width, cap.height));
                    });
                });
        }
        // ---- POST /effect/compile — recompile a custom-effect node's HLSL -
        // Body: { nodeId, hlsl, analysisFields?:[...] }
        // Compiles HLSL via D3DCompile (ps_5_0 or cs_5_0 based on the
        // node's existing shaderType), then applies the new bytecode +
        // generates a new shaderGuid + updates analysis fields. Fires
        // OnCustomEffectRecompiled so the GUI rebuilds the canvas
        // layout (parameter pins may have changed) and Add Node flyout.
        // Mirrors EffectDesignerWindow's "Update in Graph" path.
        void RegisterCompileEffect(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/effect/compile",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            auto hlsl = std::wstring(jobj.GetNamedString(L"hlsl"));

                            // Normalize line endings before D3DCompile.
                            std::string hlslUtf8 = WideToUtf8(hlsl);
                            for (auto& ch : hlslUtf8) { if (ch == '\r') ch = '\n'; }

                            auto* node = ctx.graph->FindNode(nodeId);
                            if (!node || !node->customEffect.has_value())
                                return Json(404,
                                    R"({"error":"Custom effect node not found"})");

                            using CST = ::ShaderLab::Graph::CustomShaderType;
                            std::string target =
                                (node->customEffect->shaderType == CST::PixelShader)
                                ? "ps_5_0" : "cs_5_0";
                            auto result = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
                                hlslUtf8, "McpCompile", "main", target);

                            if (!result.succeeded)
                            {
                                std::string err = JsonEscape(WideToUtf8(result.ErrorMessage()));
                                return Json(200, std::format(
                                    "{{\"compiled\":false,\"error\":\"{}\"}}", err));
                            }

                            auto* blob = result.bytecode.get();
                            std::vector<uint8_t> bytecode(blob->GetBufferSize());
                            std::memcpy(bytecode.data(), blob->GetBufferPointer(),
                                blob->GetBufferSize());

                            // Optional analysisFields update.
                            using AFT = ::ShaderLab::Graph::AnalysisFieldType;
                            std::vector<::ShaderLab::Graph::AnalysisFieldDescriptor> newFields;
                            bool hasAnalysisFields = jobj.HasKey(L"analysisFields");
                            if (hasAnalysisFields)
                            {
                                auto fieldsArr = jobj.GetNamedArray(L"analysisFields");
                                for (uint32_t fi = 0; fi < fieldsArr.Size(); ++fi)
                                {
                                    auto fobj = fieldsArr.GetObjectAt(fi);
                                    ::ShaderLab::Graph::AnalysisFieldDescriptor fd;
                                    fd.name = std::wstring(fobj.GetNamedString(L"name"));
                                    auto typeTag = std::wstring(fobj.GetNamedString(L"type"));
                                    if (typeTag == L"float")        fd.type = AFT::Float;
                                    else if (typeTag == L"float2")  fd.type = AFT::Float2;
                                    else if (typeTag == L"float3")  fd.type = AFT::Float3;
                                    else if (typeTag == L"float4")  fd.type = AFT::Float4;
                                    else if (typeTag == L"floatarray")  fd.type = AFT::FloatArray;
                                    else if (typeTag == L"float2array") fd.type = AFT::Float2Array;
                                    else if (typeTag == L"float3array") fd.type = AFT::Float3Array;
                                    else if (typeTag == L"float4array") fd.type = AFT::Float4Array;
                                    if (fobj.HasKey(L"length"))
                                        fd.arrayLength = static_cast<uint32_t>(
                                            fobj.GetNamedNumber(L"length"));
                                    newFields.push_back(std::move(fd));
                                }
                            }

                            // Apply: bytecode + fresh shaderGuid + optional fields.
                            auto& def = node->customEffect.value();
                            def.hlslSource = hlsl;
                            def.compiledBytecode = std::move(bytecode);
                            ::CoCreateGuid(&def.shaderGuid);
                            if (hasAnalysisFields)
                            {
                                def.analysisFields = std::move(newFields);
                                def.analysisOutputType = def.analysisFields.empty()
                                    ? ::ShaderLab::Graph::AnalysisOutputType::None
                                    : ::ShaderLab::Graph::AnalysisOutputType::Typed;
                            }

                            node->dirty = true;
                            ctx.graph->MarkAllDirty();
                            ctx.evaluator->UpdateNodeShader(nodeId, *node);

                            // Auto-rename if another custom-effect node has the
                            // same display name but different HLSL. Same
                            // policy MainWindow::EnforceCustomEffectNameUniqueness
                            // applies to the EffectDesigner "Update in Graph"
                            // path: append " (N)" suffix until unique.
                            const auto& modHlsl = def.hlslSource;
                            const auto& modName = node->name;
                            bool conflict = false;
                            for (const auto& other : ctx.graph->Nodes())
                            {
                                if (other.id == nodeId) continue;
                                if (other.name != modName) continue;
                                if (!other.customEffect.has_value()) continue;
                                if (other.customEffect->hlslSource != modHlsl)
                                {
                                    conflict = true;
                                    break;
                                }
                            }
                            if (conflict)
                            {
                                std::wstring baseName = modName;
                                auto parenPos = baseName.rfind(L" (");
                                if (parenPos != std::wstring::npos && baseName.back() == L')')
                                    baseName = baseName.substr(0, parenPos);
                                for (int suffix = 2; suffix < 100; ++suffix)
                                {
                                    std::wstring candidate = baseName + L" ("
                                        + std::to_wstring(suffix) + L")";
                                    bool taken = false;
                                    for (const auto& other : ctx.graph->Nodes())
                                    {
                                        if (other.id == nodeId) continue;
                                        if (other.name == candidate &&
                                            other.customEffect.has_value() &&
                                            other.customEffect->hlslSource != modHlsl)
                                        {
                                            taken = true;
                                            break;
                                        }
                                    }
                                    if (!taken)
                                    {
                                        node->name = candidate;
                                        break;
                                    }
                                }
                            }

                            sink.OnCustomEffectRecompiled(nodeId);

                            return Json(200, std::format(
                                "{{\"compiled\":true,\"bytecodeSize\":{}}}",
                                def.compiledBytecode.size()));
                        }
                        catch (...)
                        {
                            return Json(400, R"({"error":"Invalid request"})");
                        }
                    });
                });
        }
        // ---- Display profile routes ---------------------------------------
        // Shared serializer matching the byte format the MainWindow
        // routes used to emit (so MCP consumers don't see schema drift).
        std::string SerializeProfile(const ::ShaderLab::Rendering::DisplayProfile& p)
        {
            return std::format(
                "{{\"name\":\"{}\",\"hdrEnabled\":{},\"bitsPerColor\":{}"
                ",\"sdrWhiteNits\":{:.2f},\"peakNits\":{:.2f}"
                ",\"minNits\":{:.4f},\"maxFullFrameNits\":{:.2f}"
                ",\"gamut\":\"{}\",\"isSimulated\":{}"
                ",\"primaryRed\":[{:.4f},{:.4f}]"
                ",\"primaryGreen\":[{:.4f},{:.4f}]"
                ",\"primaryBlue\":[{:.4f},{:.4f}]"
                ",\"whitePoint\":[{:.4f},{:.4f}]}}",
                JsonEscape(WideToUtf8(p.profileName)),
                p.caps.hdrEnabled ? "true" : "false",
                p.caps.bitsPerColor,
                p.caps.sdrWhiteLevelNits, p.caps.maxLuminanceNits,
                p.caps.minLuminanceNits, p.caps.maxFullFrameLuminanceNits,
                JsonEscape(WideToUtf8(::ShaderLab::Rendering::GamutIdToString(p.gamut))),
                p.isSimulated ? "true" : "false",
                p.primaryRed.x, p.primaryRed.y,
                p.primaryGreen.x, p.primaryGreen.y,
                p.primaryBlue.x, p.primaryBlue.y,
                p.whitePoint.x, p.whitePoint.y);
        }

        // GET /display/profiles  — All built-in presets + active + live.
        void RegisterGetDisplayProfiles(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/display/profiles",
                [&sink](const std::wstring&, const std::string&) -> McpHttpServer::Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> McpHttpServer::Response {
                        auto presets = ::ShaderLab::Rendering::AllPresets();
                        std::string json = "{\"presets\":[";
                        for (size_t i = 0; i < presets.size(); ++i)
                        {
                            if (i) json += ",";
                            json += "{\"index\":" + std::to_string(i) + ",\"profile\":";
                            json += SerializeProfile(presets[i]);
                            json += "}";
                        }
                        json += "],\"active\":" + SerializeProfile(ctx.displayMonitor->ActiveProfile());
                        json += ",\"live\":"  + SerializeProfile(ctx.displayMonitor->LiveProfile());
                        json += ",\"isSimulated\":";
                        json += (ctx.displayMonitor->IsSimulated() ? "true" : "false");
                        if (ctx.getLoadedIccProfile)
                        {
                            auto icc = ctx.getLoadedIccProfile();
                            if (icc.has_value())
                                json += ",\"loadedIcc\":" + SerializeProfile(icc.value());
                        }
                        json += "}";
                        return Json(200, json);
                    });
                });
        }

        // POST /display/profile — apply a simulated profile.
        // Body: exactly one of {preset, presetIndex, iccPath, custom}.
        void RegisterSetDisplayProfile(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/display/profile",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> McpHttpServer::Response {
                        using namespace ::ShaderLab::Rendering;

                        WDJ::JsonObject jo{ nullptr };
                        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                            return Json(400, R"({"error":"Invalid JSON body"})");

                        int count = 0;
                        if (jo.HasKey(L"preset"))      ++count;
                        if (jo.HasKey(L"presetIndex")) ++count;
                        if (jo.HasKey(L"iccPath"))     ++count;
                        if (jo.HasKey(L"custom"))      ++count;
                        if (count != 1)
                            return Json(400, R"({"error":"Specify exactly one of: preset, presetIndex, iccPath, custom"})");

                        std::optional<DisplayProfile> chosen;

                        if (jo.HasKey(L"preset"))
                        {
                            auto name = std::wstring(jo.GetNamedString(L"preset"));
                            DisplayProfile p{};
                            if      (name == L"PresetSrgbSdr"     || name == L"sRGB SDR (80 nits)")              p = PresetSrgbSdr();
                            else if (name == L"PresetSrgb270"     || name == L"sRGB SDR (270 nits, typical laptop)") p = PresetSrgb270();
                            else if (name == L"PresetAdobeRGB"    || name == L"Adobe RGB (1998)")                p = PresetAdobeRGB();
                            else if (name == L"PresetP3_600"      || name == L"DCI-P3 HDR (600 nits, MacBook Pro-class)") p = PresetP3_600();
                            else if (name == L"PresetP3_1000"     || name == L"DCI-P3 HDR (1000 nits, reference monitor)") p = PresetP3_1000();
                            else if (name == L"PresetBT2020_1000" || name == L"BT.2020 HDR (1000 nits, HDR TV)")  p = PresetBT2020_1000();
                            else if (name == L"PresetBT2020_4000" || name == L"BT.2020 HDR (4000 nits, mastering)") p = PresetBT2020_4000();
                            else
                                return Json(400, std::format(
                                    R"({{"error":"Unknown preset: {}"}})",
                                    JsonEscape(WideToUtf8(name))));
                            chosen = p;
                        }
                        else if (jo.HasKey(L"presetIndex"))
                        {
                            auto idx = static_cast<size_t>(jo.GetNamedNumber(L"presetIndex"));
                            auto presets = AllPresets();
                            if (idx >= presets.size())
                                return Json(400, std::format(
                                    "{{\"error\":\"presetIndex out of range (0-{})\"}}",
                                    presets.size() - 1));
                            chosen = presets[idx];
                        }
                        else if (jo.HasKey(L"iccPath"))
                        {
                            auto path = std::wstring(jo.GetNamedString(L"iccPath"));
                            if (!std::filesystem::exists(path))
                                return Json(400, std::format(
                                    R"({{"error":"ICC file not found: {}"}})",
                                    JsonEscape(WideToUtf8(path))));
                            auto parsed = IccProfileParser::LoadFromFile(path);
                            if (!parsed.has_value() || !parsed->valid)
                                return Json(400, std::format(
                                    R"({{"error":"Failed to parse ICC profile: {}"}})",
                                    JsonEscape(WideToUtf8(path))));
                            chosen = DisplayProfileFromIcc(parsed.value());
                            if (ctx.setLoadedIccProfile)
                                ctx.setLoadedIccProfile(chosen.value());
                        }
                        else  // custom
                        {
                            auto co = jo.GetNamedObject(L"custom");
                            DisplayProfile p{};
                            p.isSimulated = true;

                            if (co.HasKey(L"name"))
                                p.profileName = std::wstring(co.GetNamedString(L"name"));
                            else
                                p.profileName = L"Custom MCP profile";

                            p.caps.hdrEnabled = co.HasKey(L"hdrEnabled") && co.GetNamedBoolean(L"hdrEnabled");
                            p.caps.colorSpace = p.caps.hdrEnabled
                                ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
                            p.caps.bitsPerColor = p.caps.hdrEnabled ? 10 : 8;
                            p.caps.sdrWhiteLevelNits = co.HasKey(L"sdrWhiteNits")
                                ? static_cast<float>(co.GetNamedNumber(L"sdrWhiteNits"))
                                : (p.caps.hdrEnabled ? 203.0f : 80.0f);
                            if (!co.HasKey(L"peakNits"))
                                return Json(400, R"({"error":"custom profile requires 'peakNits'"})");
                            p.caps.maxLuminanceNits = static_cast<float>(co.GetNamedNumber(L"peakNits"));
                            p.caps.minLuminanceNits = co.HasKey(L"minNits")
                                ? static_cast<float>(co.GetNamedNumber(L"minNits")) : 0.5f;
                            p.caps.maxFullFrameLuminanceNits = co.HasKey(L"maxFullFrameNits")
                                ? static_cast<float>(co.GetNamedNumber(L"maxFullFrameNits"))
                                : p.caps.maxLuminanceNits;

                            auto readChroma = [&](const wchar_t* key, ChromaticityXY& dst) -> bool {
                                if (!co.HasKey(key)) return true;
                                auto arr = co.GetNamedArray(key);
                                if (arr.Size() != 2) return false;
                                dst.x = static_cast<float>(arr.GetNumberAt(0));
                                dst.y = static_cast<float>(arr.GetNumberAt(1));
                                return true;
                            };
                            if (!readChroma(L"primaryRed",   p.primaryRed)   ||
                                !readChroma(L"primaryGreen", p.primaryGreen) ||
                                !readChroma(L"primaryBlue",  p.primaryBlue)  ||
                                !readChroma(L"whitePoint",   p.whitePoint))
                                return Json(400, R"({"error":"primaries / whitePoint must be 2-element arrays"})");

                            p.gamut = GamutId::Custom;
                            if (co.HasKey(L"gamut"))
                            {
                                auto gn = std::wstring(co.GetNamedString(L"gamut"));
                                if      (gn == L"sRGB")    p.gamut = GamutId::sRGB;
                                else if (gn == L"DCI-P3"  || gn == L"P3" || gn == L"DCI_P3")    p.gamut = GamutId::DCI_P3;
                                else if (gn == L"BT.2020" || gn == L"BT2020" || gn == L"Rec2020") p.gamut = GamutId::BT2020;
                                else                       p.gamut = GamutId::Custom;
                            }
                            chosen = p;
                        }

                        ctx.displayMonitor->SetSimulatedProfile(chosen.value());
                        ::ShaderLab::Rendering::UpdateWorkingSpaceNodes(*ctx.graph, *ctx.displayMonitor);
                        sink.OnDisplayProfileChanged();

                        auto active = ctx.displayMonitor->ActiveProfile();
                        return Json(200, std::format(
                            R"({{"ok":true,"applied":"{}","hdrEnabled":{},"peakNits":{:.2f}}})",
                            JsonEscape(WideToUtf8(active.profileName)),
                            active.caps.hdrEnabled ? "true" : "false",
                            active.caps.maxLuminanceNits));
                    });
                });
        }

        // POST /display/profile/clear — revert to the live OS profile.
        void RegisterClearDisplayProfile(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/display/profile/clear",
                [&sink](const std::wstring&, const std::string&) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&sink](EngineContext& ctx) -> McpHttpServer::Response {
                        ctx.displayMonitor->ClearSimulatedProfile();
                        ::ShaderLab::Rendering::UpdateWorkingSpaceNodes(*ctx.graph, *ctx.displayMonitor);
                        sink.OnDisplayProfileChanged();
                        return Json(200, R"({"ok":true,"isSimulated":false})");
                    });
                });
        }
    }

    void RegisterEngineRoutes(McpHttpServer& server, IEngineCommandSink& sink)
    {
        RegisterRegistry(server);
        RegisterEffectHlsl(server, sink);
        RegisterAddNode(server, sink);
        RegisterRemoveNode(server, sink);
        RegisterConnect(server, sink);
        RegisterDisconnect(server, sink);
        RegisterBindProperty(server, sink);
        RegisterUnbindProperty(server, sink);
        RegisterClear(server, sink);
        RegisterLoad(server, sink);
        RegisterSetProperty(server, sink);
        RegisterPixelRegion(server, sink);
        RegisterGetGraph(server, sink);
        RegisterCustomEffects(server, sink);
        RegisterAnalysisOutput(server, sink);
        RegisterImageBounds(server, sink);
        RegisterCaptureNode(server, sink);
        RegisterCompileEffect(server, sink);
        RegisterGetDisplayProfiles(server, sink);
        RegisterSetDisplayProfile(server, sink);
        RegisterClearDisplayProfile(server, sink);
    }
}
