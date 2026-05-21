#include "pch_engine.h"
#include "TestCommon.h"
#include "ShaderTestBench.h"
#include "Graph/EffectGraph.h"
#include "Rendering/GraphEvaluator.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/EffectRegistry.h"
#include "Effects/ShaderCompiler.h"
#include "Effects/BytecodeCache.h"
#include "Effects/Performance.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Rendering/RenderThreadDispatcher.h"
#include "Graph/GraphUiSnapshot.h"
#include "Rendering/D3DDefinitions.h"

#include <atomic>
#include <thread>

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

// ============================================================================
// ShaderLab Test Runner — standalone console app sharing the engine code.
// Usage: ShaderLabTests.exe [--adapter warp|default]
// Exit code = number of failures (0 = all passed).
// ============================================================================

// Forward decl from Tests/Math/*.cpp.
namespace ShaderLab::Tests
{
    void TestTransferFunctions(ShaderTestBench& bench);
    void TestColorMatrices(ShaderTestBench& bench);
    void TestMobiusReinhard(ShaderTestBench& bench);
    void TestDeltaE(ShaderTestBench& bench);
    void TestGamut(ShaderTestBench& bench);
}

namespace
{
    // TEST() and g_passed/g_failed live in TestCommon.h so multiple TUs
    // (Tests/Math/*.cpp) can share the same summary counters. Pull them
    // into the local anonymous namespace via using-declarations so the
    // existing test bodies in this file stay unchanged.
    using ShaderLab::Tests::TEST;
    using ShaderLab::Tests::g_passed;
    using ShaderLab::Tests::g_failed;

    // Shared device resources.
    winrt::com_ptr<ID3D11Device5> g_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext4> g_d3dContext;
    winrt::com_ptr<ID2D1Factory7> g_d2dFactory;
    winrt::com_ptr<ID2D1DeviceContext5> g_dc;

    // Phase 7 spike discovered: cachedOutput is only valid while the
    // GraphEvaluator that produced it is alive. The evaluator owns the
    // ID2D1Effect cache; effects own their output ID2D1Image; node
    // pointers are non-owning. Keep this evaluator alive for the
    // lifetime of any test that wants to use cachedOutput downstream
    // (DrawImage, pixel readback, etc).
    ShaderLab::Rendering::GraphEvaluator g_evaluator;

    void Evaluate(ShaderLab::Graph::EffectGraph& graph, ShaderLab::Effects::SourceNodeFactory& sf)
    {
        graph.MarkAllDirty();
        for (auto& node : const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(graph.Nodes()))
        {
            if (node.type == ShaderLab::Graph::NodeType::Source)
            {
                try { sf.PrepareSourceNode(node, g_dc.get(), 0.0, g_d3dDevice.get(), g_d3dContext.get()); }
                catch (...) {}
            }
        }
        g_evaluator.Evaluate(graph, g_dc.get());
        g_evaluator.Evaluate(graph, g_dc.get()); // second pass for new effects
    }

    bool HasOutput(const ShaderLab::Graph::EffectNode& node)
    {
        return node.cachedOutput != nullptr;
    }

    // ========================================================================
    // Test categories
    // ========================================================================

    void TestGraphOperations()
    {
        printf("\n=== Graph Operations ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Graph::EffectGraph g;

        // Add/remove.
        auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto id1 = g.AddNode(std::move(node));
        TEST("AddNode", g.FindNode(id1) != nullptr);
        TEST("NodeCount", g.Nodes().size() == 1);
        g.RemoveNode(id1);
        TEST("RemoveNode", g.FindNode(id1) == nullptr && g.Nodes().empty());

        // Connect/disconnect.
        auto src = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto blur = ShaderLab::Effects::EffectRegistry::Instance().CreateNode(
            *ShaderLab::Effects::EffectRegistry::Instance().FindByName(L"Gaussian Blur"));
        auto srcId = g.AddNode(std::move(src));
        auto blurId = g.AddNode(std::move(blur));
        TEST("Connect", g.Connect(srcId, 0, blurId, 0));
        TEST("EdgeExists", g.GetInputEdges(blurId).size() == 1);
        g.Disconnect(srcId, 0, blurId, 0);
        TEST("Disconnect", g.GetInputEdges(blurId).empty());

        // Topo sort.
        g.Connect(srcId, 0, blurId, 0);
        auto topo = g.TopologicalSort();
        TEST("TopoSort", topo.size() == 2 && topo[0] == srcId);

        // Cycle detection.
        TEST("DetectsCycle", g.WouldCreateCycle(blurId, srcId));
        TEST("AllowsExisting", !g.WouldCreateCycle(srcId, blurId));
    }

    void TestSerialization()
    {
        printf("\n=== Serialization ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Graph::EffectGraph g;
        auto src = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto blur = ShaderLab::Effects::EffectRegistry::Instance().CreateNode(
            *ShaderLab::Effects::EffectRegistry::Instance().FindByName(L"Gaussian Blur"));
        auto srcId = g.AddNode(std::move(src));
        auto blurId = g.AddNode(std::move(blur));
        g.Connect(srcId, 0, blurId, 0);
        g.FindNode(blurId)->properties[L"StandardDeviation"] = 3.0f;

        auto json = g.ToJson();
        TEST("SerializeNotEmpty", !json.empty());

        auto g2 = ShaderLab::Graph::EffectGraph::FromJson(json);
        TEST("DeserializeNodeCount", g2.Nodes().size() == 2);
        TEST("DeserializeEdges", !g2.Edges().empty());

        auto* blurLoaded = g2.FindNode(blurId);
        bool propOk = false;
        if (blurLoaded) {
            auto it = blurLoaded->properties.find(L"StandardDeviation");
            if (it != blurLoaded->properties.end()) {
                auto* fv = std::get_if<float>(&it->second);
                propOk = fv && std::abs(*fv - 3.0f) < 0.01f;
            }
        }
        TEST("PropertyPreserved", propOk);
    }

    void TestSourceEffects()
    {
        printf("\n=== Source Effects ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        const wchar_t* names[] = {
            L"Gamut Source", L"Color Checker", L"Zone Plate",
            L"Gradient Generator", L"HDR Test Pattern"
        };
        for (const auto* name : names)
        {
            std::string testName = "Source_";
            for (const wchar_t* p = name; *p; ++p) testName += static_cast<char>(*p);

            auto* desc = registry.FindByName(name);
            if (!desc) { TEST("FindDescriptor", false); continue; }

            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto id = g.AddNode(std::move(node));
            Evaluate(g, sf);

            auto* n = g.FindNode(id);
            bool ok = n && HasOutput(*n) && n->runtimeError.empty();
            TEST(testName.c_str(), ok);
        }
    }

    void TestAnalysisEffects()
    {
        printf("\n=== Analysis Effects ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        // Wave Monitor + similar viewers migrated to D3D11 compute (Phase
        // 8c) need the BeginDraw + ProcessDeferredCompute + double-eval
        // dance to actually produce output. Apply it uniformly so this
        // test works for both pixel-shader and compute-bridge effects.
        auto runTwoPass = [&](ShaderLab::Graph::EffectGraph& g,
                              ShaderLab::Effects::SourceNodeFactory& sf) {
            g_evaluator.ReleaseCache();
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
        };

        const wchar_t* names[] = {
            L"Luminance Heatmap", L"Gamut Highlight", L"Nit Map"
        };
        for (const auto* name : names)
        {
            auto* desc = registry.FindByName(name);
            if (!desc) { TEST("FindDescriptor", false); continue; }

            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source"));
            auto fxNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto srcId = g.AddNode(std::move(srcNode));
            auto fxId = g.AddNode(std::move(fxNode));
            g.Connect(srcId, 0, fxId, 0);
            runTwoPass(g, sf);

            auto* n = g.FindNode(fxId);
            bool ok = n && HasOutput(*n) && n->runtimeError.empty();
            std::string testName = "Analysis_";
            for (const wchar_t* p = name; *p; ++p) testName += static_cast<char>(*p);
            TEST(testName.c_str(), ok);
        }

        {
            auto* desc = registry.FindByName(L"Split Comparison");
            if (!desc) { TEST("FindDescriptor", false); return; }

            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto src1 = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source"));
            auto src2 = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Color Checker"));
            auto fxNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto src1Id = g.AddNode(std::move(src1));
            auto src2Id = g.AddNode(std::move(src2));
            auto fxId = g.AddNode(std::move(fxNode));
            g.Connect(src1Id, 0, fxId, 0);
            g.Connect(src2Id, 0, fxId, 1);
            runTwoPass(g, sf);

            auto* n = g.FindNode(fxId);
            TEST("Analysis_Split Comparison", n && HasOutput(*n) && n->runtimeError.empty());
        }
    }

    void TestBuiltInD2DEffects()
    {
        printf("\n=== Built-in D2D Effects ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ShaderLab::Effects::EffectRegistry::Instance();

        const wchar_t* names[] = {
            L"Gaussian Blur", L"Brightness", L"Contrast",
            L"Grayscale", L"Invert", L"Saturation",
            L"Hue Rotation", L"Exposure", L"Sharpen",
            L"Edge Detection", L"Crop"
        };
        for (const auto* name : names)
        {
            auto* desc = d2dReg.FindByName(name);
            if (!desc) { TEST("FindD2DEffect", false); continue; }

            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source"));
            auto fxNode = d2dReg.CreateNode(*desc);
            auto srcId = g.AddNode(std::move(srcNode));
            auto fxId = g.AddNode(std::move(fxNode));
            g.Connect(srcId, 0, fxId, 0);
            Evaluate(g, sf);

            auto* n = g.FindNode(fxId);
            bool ok = n && HasOutput(*n);
            std::string testName = "D2D_";
            for (const wchar_t* p = name; *p; ++p) testName += static_cast<char>(*p);
            TEST(testName.c_str(), ok);
        }
    }

    void TestPropertyBindings()
    {
        printf("\n=== Property Bindings ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ShaderLab::Effects::EffectRegistry::Instance();

        ShaderLab::Graph::EffectGraph g;
        ShaderLab::Effects::SourceNodeFactory sf;

        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Gamut Source"));
        auto paramNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Float Parameter"));
        auto blurNode = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));

        auto srcId = g.AddNode(std::move(srcNode));
        auto paramId = g.AddNode(std::move(paramNode));
        auto blurId = g.AddNode(std::move(blurNode));
        g.Connect(srcId, 0, blurId, 0);

        g.FindNode(paramId)->properties[L"Value"] = 5.0f;
        auto err = g.BindProperty(blurId, L"StandardDeviation", paramId, L"Value", 0);
        TEST("BindProperty", err.empty());

        Evaluate(g, sf);
        auto* blur = g.FindNode(blurId);
        auto sdIt = blur->properties.find(L"StandardDeviation");
        bool sdOk = false;
        if (sdIt != blur->properties.end()) {
            auto* fv = std::get_if<float>(&sdIt->second);
            sdOk = fv && *fv >= 4.5f && *fv <= 5.5f;
        }
        TEST("BindingPropagates", sdOk);
    }

    void TestMathNodes()
    {
        printf("\n=== Numeric Expression Node ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        // Effect descriptor exists.
        auto* desc = registry.FindByName(L"Numeric Expression");
        TEST("DescriptorExists", desc != nullptr);
        if (!desc) return;
        TEST("StableEffectId", desc->effectId == L"Math Expression");

        auto baseNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
        TEST("DefaultHasOnlyA",
            baseNode.properties.count(L"A") == 1 &&
            baseNode.properties.count(L"B") == 0 &&
            baseNode.properties.count(L"Expression") == 1);

        // Each entry: expression + (name,value) pairs to set + expected result.
        struct Case {
            const wchar_t* name;
            const wchar_t* expression;
            std::vector<std::pair<const wchar_t*, float>> inputs;
            float expected;
        };
        Case cases[] = {
            { L"Identity",         L"A",                 {{L"A", 7.5f}},                            7.5f },
            { L"AddTwoInputs",     L"A + B",             {{L"A", 3.0f},{L"B", 4.0f}},               7.0f },
            { L"SubtractMultiply", L"(A - B) * C",       {{L"A", 10.0f},{L"B", 4.0f},{L"C", 2.0f}}, 12.0f },
            { L"MaxOfFour",        L"max(A, B, C, D)",   {{L"A", 1.0f},{L"B", 9.0f},{L"C", 3.0f},{L"D", 7.0f}}, 9.0f },
            { L"FiveInputAvg",     L"(A + B + C + D + E) / 5",
                                   {{L"A", 1.0f},{L"B", 2.0f},{L"C", 3.0f},{L"D", 4.0f},{L"E", 5.0f}}, 3.0f },
            { L"SinPi",            L"sin(pi)",           {{L"A", 0.0f}},                            0.0f },
            { L"Conditional",      L"if(A > B, A, B)",   {{L"A", 5.0f},{L"B", 3.0f}},               5.0f },
        };

        for (const auto& c : cases)
        {
            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            // Add as many inputs as required (default has only A).
            for (const auto& [pname, pval] : c.inputs)
            {
                if (node.properties.find(pname) == node.properties.end())
                {
                    ShaderLab::Graph::ParameterDefinition pd;
                    pd.name = pname; pd.typeName = L"float"; pd.defaultValue = 0.0f;
                    pd.minValue = -100000.0f; pd.maxValue = 100000.0f; pd.step = 0.1f;
                    node.customEffect->parameters.push_back(std::move(pd));
                    node.properties[pname] = 0.0f;
                }
                node.properties[pname] = pval;
            }
            node.properties[L"Expression"] = std::wstring(c.expression);

            auto id = g.AddNode(std::move(node));
            Evaluate(g, sf);

            auto* mn = g.FindNode(id);
            bool ok = false;
            float got = 0.0f;
            if (mn) {
                for (const auto& f : mn->analysisOutput.fields) {
                    if (f.name == L"Result") { got = f.components[0]; break; }
                }
                ok = std::abs(got - c.expected) < 0.01f && mn->runtimeError.empty();
            }
            std::string testName = "Eval_";
            for (const wchar_t* p = c.name; *p; ++p) testName += static_cast<char>(*p);
            TEST(testName.c_str(), ok);
        }

        // Parse error path.
        {
            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            node.properties[L"Expression"] = std::wstring(L"A + + +");
            auto id = g.AddNode(std::move(node));
            Evaluate(g, sf);
            auto* mn = g.FindNode(id);
            TEST("ParseErrorReported", mn && !mn->runtimeError.empty());
        }

        // Add/remove inputs at the graph level (mirrors the UI flow).
        {
            ShaderLab::Graph::EffectGraph g;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto id = g.AddNode(std::move(node));
            auto* n = g.FindNode(id);

            auto addInput = [&](const wchar_t* name) {
                ShaderLab::Graph::ParameterDefinition pd;
                pd.name = name; pd.typeName = L"float"; pd.defaultValue = 0.0f;
                pd.minValue = -100000.0f; pd.maxValue = 100000.0f; pd.step = 0.1f;
                n->customEffect->parameters.push_back(std::move(pd));
                n->properties[name] = 0.0f;
            };
            addInput(L"B");
            addInput(L"C");
            TEST("AddInputs",
                n->properties.count(L"B") == 1 && n->properties.count(L"C") == 1);

            // Remove B.
            std::erase_if(n->customEffect->parameters,
                [](const auto& p) { return p.name == L"B"; });
            n->properties.erase(L"B");
            g.UnbindProperty(id, L"B");
            TEST("RemoveInput",
                n->properties.count(L"B") == 0 &&
                n->properties.count(L"A") == 1 && n->properties.count(L"C") == 1);
        }

        // JSON round-trip with custom inputs.
        {
            ShaderLab::Graph::EffectGraph g;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            // Add B and C, set values + expression.
            for (const wchar_t* name : { L"B", L"C" })
            {
                ShaderLab::Graph::ParameterDefinition pd;
                pd.name = name; pd.typeName = L"float"; pd.defaultValue = 0.0f;
                pd.minValue = -100000.0f; pd.maxValue = 100000.0f; pd.step = 0.1f;
                node.customEffect->parameters.push_back(std::move(pd));
                node.properties[name] = 0.0f;
            }
            node.properties[L"A"] = 1.0f;
            node.properties[L"B"] = 2.0f;
            node.properties[L"C"] = 3.0f;
            node.properties[L"Expression"] = std::wstring(L"A + B * C");
            auto id = g.AddNode(std::move(node));

            auto json = g.ToJson();
            auto g2 = ShaderLab::Graph::EffectGraph::FromJson(json);

            ShaderLab::Effects::SourceNodeFactory sf;
            Evaluate(g2, sf);
            auto* mn = g2.FindNode(id);
            float got = 0.0f;
            if (mn) {
                for (const auto& f : mn->analysisOutput.fields)
                    if (f.name == L"Result") { got = f.components[0]; break; }
            }
            TEST("JsonRoundTripInputsAndExpression",
                mn && std::abs(got - 7.0f) < 0.01f); // 1 + 2*3 = 7
        }
    }


    void TestClockNode()
    {
        printf("\n=== Clock Node ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        auto* desc = registry.FindByName(L"Clock");
        TEST("ClockExists", desc != nullptr);
        if (desc) {
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            TEST("IsClock", node.isClock);
            TEST("HasAutoDuration", node.properties.count(L"AutoDuration") > 0);
            TEST("HasTimeOutput", !node.customEffect->analysisFields.empty() &&
                node.customEffect->analysisFields[0].name == L"Time");
        }
    }

    void TestShaderCompilation()
    {
        printf("\n=== Shader Compilation ===\n");

        std::string validPS = R"(
Texture2D Source : register(t0);
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{ return Source.Load(int3(uv0.xy, 0)); }
)";
        auto result = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            validPS, "test.hlsl", "main", "ps_5_0");
        TEST("ValidPixelShader", result.succeeded);

        auto bad = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            "not hlsl!", "bad.hlsl", "main", "ps_5_0");
        TEST("InvalidShaderFails", !bad.succeeded);

        std::string validCS = R"(
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) { output[id.xy] = float4(1,0,0,1); }
)";
        auto csResult = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            validCS, "test.hlsl", "main", "cs_5_0");
        TEST("ValidComputeShader", csResult.succeeded);

        // ---- shaderlab_params.hlsli include + macro mode validation ---------
        // Phase 8: a shader that uses SHADERLAB_PARAM should compile in
        // both cbuffer mode (_SLPARAM_X_GPU=0) and GPU-bound mode
        // (_SLPARAM_X_GPU=1). We don't reflect the resulting bytecode
        // here -- the goal is just to prove the include resolver +
        // macro injection wire all the way through D3DCompile without
        // a syntax error in the macro file itself.
        std::string macroPS = R"(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Exposure, t1)
cbuffer Constants : register(b0) {
    SHADERLAB_PARAM(float, Exposure)
    float Padding;
};
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET {
    SHADERLAB_LOAD_PARAM(float, Exposure)
    return Source.Load(int3(uv0.xy, 0)) * Exposure;
}
)";
        // Default mode: parameter lives in cbuffer.
        auto cbufferMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            macroPS, "test_param_cbuffer.hlsl", "main", "ps_5_0",
            { { "_SLPARAM_Exposure_GPU", "0" } });
        TEST("ShaderLabParamsHlsli_CbufferMode", cbufferMode.succeeded);

        // GPU-bound mode: parameter comes from StructuredBuffer<float4>.
        auto gpuMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            macroPS, "test_param_gpu.hlsl", "main", "ps_5_0",
            { { "_SLPARAM_Exposure_GPU", "1" } });
        TEST("ShaderLabParamsHlsli_GpuMode", gpuMode.succeeded);
    }

    // ------------------------------------------------------------------------
    // BytecodeCache (Phase 8)
    // ------------------------------------------------------------------------

    void TestBytecodeCache()
    {
        printf("\n=== BytecodeCache ===\n");
        using namespace ShaderLab::Effects;
        auto& cache = BytecodeCache::Instance();
        cache.Clear();

        std::string hlsl = R"(
Texture2D Source : register(t0);
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{ return Source.Load(int3(uv0.xy, 0)); }
)";
        std::string canonical = CanonicalizeHlslSource(hlsl);

        BytecodeCompileRequest req;
        req.key.sourceHash         = HashCanonicalSource(canonical);
        req.key.paramSignatureHash = HashParamSignature({});
        req.key.includeLibraryHash = IncludeLibraryHash();
        req.key.macroBitset        = 0;
        req.key.entryPoint         = "main";
        req.key.target             = "ps_5_0";
        req.metadata.effectId      = L"BytecodeCacheTest";
        req.metadata.version       = 1;
        req.hlslSource             = canonical;

        // First call -> miss -> inline compile -> Ready.
        auto r1 = cache.GetOrCompile(req);
        TEST("BytecodeCache_FirstCompileReady", r1.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_FirstCompileBytecodeNonEmpty", !r1.bytecode.empty());
        TEST("BytecodeCache_FirstCompileNotFromCache", !r1.fromCache);

        // Second call with identical key -> cache hit, identical bytecode.
        auto r2 = cache.GetOrCompile(req);
        TEST("BytecodeCache_SecondCompileReady", r2.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_SecondCompileFromCache", r2.fromCache);
        TEST("BytecodeCache_SecondCompileBytecodeMatches",
             r2.bytecode.size() == r1.bytecode.size() &&
             memcmp(r2.bytecode.data(), r1.bytecode.data(), r1.bytecode.size()) == 0);

        // Failed entry caches its diagnostic and doesn't recompile.
        BytecodeCompileRequest bad = req;
        bad.hlslSource             = "broken!";
        bad.key.sourceHash         = HashCanonicalSource(bad.hlslSource);
        auto bad1 = cache.GetOrCompile(bad);
        TEST("BytecodeCache_BadCompileFailed", bad1.status == BytecodeStatus::Failed);
        TEST("BytecodeCache_BadCompileHasDiagnostic", !bad1.errorMessage.empty());

        auto bad2 = cache.GetOrCompile(bad);
        TEST("BytecodeCache_BadCompileFromCacheOnRetry", bad2.fromCache);

        // Macro bitset participates in identity: same source + different
        // macro bits should produce a fresh entry, not reuse the
        // baseline.
        std::string macroPS = R"(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Exposure, t1)
cbuffer Constants : register(b0) {
    SHADERLAB_PARAM(float, Exposure)
    float Padding;
};
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET {
    SHADERLAB_LOAD_PARAM(float, Exposure)
    return Source.Load(int3(uv0.xy, 0)) * Exposure;
}
)";
        std::string macroCanonical = CanonicalizeHlslSource(macroPS);
        BytecodeCompileRequest mBase;
        mBase.key.sourceHash         = HashCanonicalSource(macroCanonical);
        mBase.key.paramSignatureHash = HashParamSignature({ "Exposure" });
        mBase.key.includeLibraryHash = IncludeLibraryHash();
        mBase.key.macroBitset        = 0;
        mBase.key.entryPoint         = "main";
        mBase.key.target             = "ps_5_0";
        mBase.hlslSource             = macroCanonical;
        mBase.gpuBindableParamNames  = { "Exposure" };

        auto cb = cache.GetOrCompile(mBase);
        TEST("BytecodeCache_MacroCbufferOK", cb.status == BytecodeStatus::Ready);

        BytecodeCompileRequest mGpu = mBase;
        mGpu.key.macroBitset = 1;
        auto gp = cache.GetOrCompile(mGpu);
        TEST("BytecodeCache_MacroGpuOK", gp.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_MacroVariantsDistinct",
             cb.bytecode.size() != gp.bytecode.size() ||
             memcmp(cb.bytecode.data(), gp.bytecode.data(), cb.bytecode.size()) != 0);

        // PrecompileCommonShapes enqueues N+1 variants; verify they
        // become Ready after a brief wait.
        cache.Clear();
        BytecodeCacheMetadata meta;
        meta.effectId = L"PrecompileTest";
        meta.version  = 1;
        cache.PrecompileCommonShapes(meta, macroCanonical, "main", "ps_5_0", { "Exposure" });

        // Wait up to 5s for both variants. Drives both the worker
        // queue and (via timeoutMs) the inline-fallback path.
        auto waitFor = [&](uint32_t bitset)
        {
            BytecodeCompileRequest r;
            r.key.sourceHash         = HashCanonicalSource(macroCanonical);
            r.key.paramSignatureHash = HashParamSignature({ "Exposure" });
            r.key.includeLibraryHash = IncludeLibraryHash();
            r.key.macroBitset        = bitset;
            r.key.entryPoint         = "main";
            r.key.target             = "ps_5_0";
            r.hlslSource             = macroCanonical;
            r.gpuBindableParamNames  = { "Exposure" };
            return cache.GetOrCompile(r, 5000);
        };
        auto wb = waitFor(0);
        auto wg = waitFor(1);
        TEST("BytecodeCache_PrecompileBaselineReady", wb.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_PrecompileGpuVariantReady", wg.status == BytecodeStatus::Ready);

        cache.Clear();

        // ---- Disk persistence (Phase 8 p8-cache-disk) -------------------
        // Configure a temp disk root, compile, verify file written,
        // then clear in-memory and verify GetOrCompile re-reads from
        // disk (not from a fresh D3DCompile).
        wchar_t tmpPath[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmpPath);
        std::wstring diskRoot = std::wstring(tmpPath) + L"shaderlab_bytecode_cache_test";
        cache.SetDiskCacheRoot(diskRoot);
        // Wipe any leftover state from a previous test run -- we want
        // a known-empty disk + memory state for the assertions below.
        cache.ClearDisk();
        cache.Clear();

        BytecodeCompileRequest diskReq;
        diskReq.key.sourceHash         = HashCanonicalSource(canonical);
        diskReq.key.paramSignatureHash = HashParamSignature({});
        diskReq.key.includeLibraryHash = IncludeLibraryHash();
        diskReq.key.macroBitset        = 0;
        diskReq.key.entryPoint         = "main";
        diskReq.key.target             = "ps_5_0";
        diskReq.metadata.effectId      = L"DiskPersistTest";
        diskReq.metadata.version       = 1;
        diskReq.hlslSource             = canonical;

        auto d1 = cache.GetOrCompile(diskReq);
        TEST("BytecodeCache_DiskCompileReady", d1.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_DiskCompileNotFromCache", !d1.fromCache);

        // Verify a .cso file landed under the expected hierarchy.
        std::wstring expectedDir = diskRoot + L"\\DiskPersistTest\\1";
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((expectedDir + L"\\*.cso").c_str(), &fd);
        bool csoExists = (h != INVALID_HANDLE_VALUE);
        if (csoExists) ::FindClose(h);
        TEST("BytecodeCache_DiskFileWritten", csoExists);

        // Clear the in-memory cache; subsequent GetOrCompile must
        // hydrate from disk, not recompile.
        cache.Clear();
        size_t inlineBefore = cache.GetStats().inlineCompiles;
        auto d2 = cache.GetOrCompile(diskReq);
        size_t inlineAfter = cache.GetStats().inlineCompiles;
        TEST("BytecodeCache_DiskRehydrateReady", d2.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_DiskRehydrateFromCache", d2.fromCache);
        TEST("BytecodeCache_DiskRehydrateNoNewCompile", inlineAfter == inlineBefore);
        TEST("BytecodeCache_DiskRehydrateBytecodeMatches",
             d2.bytecode.size() == d1.bytecode.size() &&
             memcmp(d2.bytecode.data(), d1.bytecode.data(), d1.bytecode.size()) == 0);

        // Reaper: ClearDisk wipes everything.
        auto reapStats = cache.ClearDisk();
        TEST("BytecodeCache_ClearDiskFreedAtLeastOne", reapStats.filesDeleted >= 1);

        // Cleanup: disable disk cache so subsequent tests don't pollute.
        cache.SetDiskCacheRoot(L"");
        cache.Clear();
    }

    void TestEffectChain()
    {
        printf("\n=== Effect Chain ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ShaderLab::Effects::EffectRegistry::Instance();

        ShaderLab::Graph::EffectGraph g;
        ShaderLab::Effects::SourceNodeFactory sf;

        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto blurNode = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));
        auto invertNode = d2dReg.CreateNode(*d2dReg.FindByName(L"Invert"));

        auto srcId = g.AddNode(std::move(srcNode));
        auto blurId = g.AddNode(std::move(blurNode));
        auto invertId = g.AddNode(std::move(invertNode));
        g.Connect(srcId, 0, blurId, 0);
        g.Connect(blurId, 0, invertId, 0);

        Evaluate(g, sf);
        auto* inv = g.FindNode(invertId);
        TEST("ThreeNodeChain", inv && HasOutput(*inv));
    }

    // Direct exercise of Luminance Statistics (D3D11 compute path) so we can
    // tell whether a Mean=0 readback is a Luminance Statistics-specific bug
    // or something the headless host introduces.
    void TestLuminanceStatistics()
    {
        printf("\n=== Luminance Statistics (D3D11 compute) ===\n");
        // g_evaluator is a global; node IDs (1, 2, ...) collide with
        // prior tests' graphs, so flush its caches before building a
        // fresh graph or CreateOrGetEffect will cache-hit a stale
        // effect from a different test.
        g_evaluator.ReleaseCache();
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* lumDesc = registry.FindByName(L"Luminance Statistics");
        if (!lumDesc) { TEST("LumStatsDescriptorFound", false); return; }

        ShaderLab::Graph::EffectGraph g;
        ShaderLab::Effects::SourceNodeFactory sf;
        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Gamut Source"));
        auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*lumDesc);
        auto srcId = g.AddNode(std::move(srcNode));
        auto statsId = g.AddNode(std::move(statsNode));
        g.Connect(srcId, 0, statsId, 0);
        Evaluate(g, sf);
        // Wrap ProcessDeferredCompute in a BeginDraw/EndDraw because
        // DispatchUserD3D11Compute calls dc->DrawImage internally, and
        // outside an active draw session that DrawImage is a silent
        // no-op (the compute shader reads a black input texture).
        // Mirrors MainWindow::RenderFrame's structure.
        // D2D custom effects need TWO evaluation passes for newly
        // created effects -- first creates/initializes, second
        // produces correct output.
        g_dc->SetTarget(nullptr);
        g_dc->BeginDraw();
        g_evaluator.ProcessDeferredCompute(g, g_dc.get());
        g_dc->EndDraw();
        Evaluate(g, sf);
        g_dc->BeginDraw();
        g_evaluator.ProcessDeferredCompute(g, g_dc.get());
        g_dc->EndDraw();

        auto* n = g.FindNode(statsId);
        TEST("LumStats_NodeExists", n != nullptr);
        if (!n) return;
        TEST("LumStats_AnalysisFieldsPopulated", !n->analysisOutput.fields.empty());

        float mean = 0.0f, maxVal = 0.0f;
        for (const auto& f : n->analysisOutput.fields) {
            if (f.name == L"Mean") mean = f.components[0];
            if (f.name == L"Max")  maxVal = f.components[0];
        }
        // Gamut Source at Luminance=80 covers part of the 64x64 image with
        // gamut-shaped fill. Max should reach ~80 nits; Mean should be a
        // small but positive fraction of that. Both being 0 is the
        // "DrawImage outside draw session" failure mode.
        TEST("LumStats_MaxIsPositive", maxVal > 0.0f);
        TEST("LumStats_MeanIsPositive", mean > 0.0f);
        if (mean == 0.0f || maxVal == 0.0f) {
            printf("    (debug) Mean=%.4f Max=%.4f -- D3D11 dispatch likely not reading source\n",
                mean, maxVal);
        }
    }

    // ----- Phase 8: GPU-binding routing through the bridge -----------------
    //
    // Verifies the feature-flag-on path:
    //   Gamut Source -> Luminance Statistics -> ICtCp Tone Map (D3D11
    //   compute, post-migration) with TargetPeakNits bound to Mean.
    // Asserts:
    //   1. ICtCp produces a non-trivial output (> 0 pixels).
    //   2. Performance::GpuBindingDetections() increments.
    //   3. With the flag off, the same chain still works (CPU readback).
    //   4. With the flag on but no upstream binding, ICtCp still works
    //      (graceful baseline path).
    void TestGpuBindingRouting()
    {
        printf("\n=== Phase 8: GPU-binding routing ===\n");
        g_evaluator.ReleaseCache();

        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* srcDesc   = registry.FindByName(L"Gamut Source");
        auto* statsDesc = registry.FindByName(L"Luminance Statistics");
        auto* ictcpDesc = registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        if (!srcDesc || !statsDesc || !ictcpDesc) {
            TEST("GpuBinding_DescriptorsFound", false);
            return;
        }
        TEST("GpuBinding_ICtCpIsD3D11Compute",
             ictcpDesc->shaderType == ShaderLab::Graph::CustomShaderType::D3D11ComputeShader);

        // ---- Sub-test 1: Feature flag ON, with binding ----
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            auto srcId   = g.AddNode(std::move(srcNode));
            auto statsId = g.AddNode(std::move(statsNode));
            auto ictcpId = g.AddNode(std::move(ictcpNode));

            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, ictcpId, 0);

            // Bind ICtCp's TargetPeakNits to Luminance Statistics' Mean.
            g.BindProperty(ictcpId, L"TargetPeakNits", statsId, L"Mean", 0);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            uint64_t beforeDet = ShaderLab::Performance::GpuBindingDetections();

            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            // Run twice so the first-frame just-created delay clears.
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            uint64_t afterDet = ShaderLab::Performance::GpuBindingDetections();
            TEST("GpuBinding_TelemetryIncremented", afterDet > beforeDet);

            auto* n = g.FindNode(ictcpId);
            TEST("GpuBinding_ICtCpHasOutput_FlagOn", n && n->cachedOutput != nullptr);
            TEST("GpuBinding_ICtCpNoRuntimeError_FlagOn", n && n->runtimeError.empty());

            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }

        // ---- Sub-test 2: Feature flag OFF, with binding ----
        // CPU readback path. Should still work.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            auto srcId   = g.AddNode(std::move(srcNode));
            auto statsId = g.AddNode(std::move(statsNode));
            auto ictcpId = g.AddNode(std::move(ictcpNode));
            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, ictcpId, 0);

            ShaderLab::Graph::PropertyBinding pb;
            ShaderLab::Graph::ComponentSource cs;
            cs.sourceNodeId    = statsId;
            cs.sourceFieldName = L"Mean";
            cs.sourceComponent = 0;
            pb.sources.push_back(cs);
            g.BindProperty(ictcpId, L"TargetPeakNits", statsId, L"Mean", 0);

            g_evaluator.ReleaseCache();
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            auto* n = g.FindNode(ictcpId);
            TEST("GpuBinding_ICtCpHasOutput_FlagOff", n && n->cachedOutput != nullptr);
            TEST("GpuBinding_ICtCpNoRuntimeError_FlagOff", n && n->runtimeError.empty());
        }

        // ---- Sub-test 3: Feature flag ON, NO binding ----
        // Baseline (cbuffer mode) path even with flag on.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            auto srcId   = g.AddNode(std::move(srcNode));
            auto ictcpId = g.AddNode(std::move(ictcpNode));
            g.Connect(srcId, 0, ictcpId, 0);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            g_evaluator.ReleaseCache();
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            auto* n = g.FindNode(ictcpId);
            TEST("GpuBinding_ICtCpHasOutput_FlagOnNoBinding", n && n->cachedOutput != nullptr);
            TEST("GpuBinding_ICtCpNoRuntimeError_FlagOnNoBinding", n && n->runtimeError.empty());

            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }
    }

    // ----- Phase 8c: skip-CPU-readback when GPU-routed -----------------
    //
    // Verifies the Performance::IsSkipUnneededCpuReadbackEnabled() path:
    //   Source -> LumStats -> ICtCp Tone Map (bound via TargetPeakNits = LumStats.Mean).
    // With the skip flag ON and the GPU-binding flag ON, the LumStats
    // dispatch's CPU Map() should be elided -- but ICtCp must still
    // produce correct image output (because it reads LumStats's GPU SRV).
    // With the host hint adding LumStats to interest set, readback runs
    // again. With skip flag OFF, readback always runs (default behavior).
    void TestSkipCpuReadback()
    {
        printf("\n=== Phase 8c: skip-CPU-readback ===\n");
        g_evaluator.ReleaseCache();
        g_evaluator.ClearCpuAnalysisInterest();

        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* srcDesc   = registry.FindByName(L"Gamut Source");
        auto* statsDesc = registry.FindByName(L"Luminance Statistics");
        auto* ictcpDesc = registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        if (!srcDesc || !statsDesc || !ictcpDesc) {
            TEST("SkipReadback_DescriptorsFound", false);
            return;
        }

        auto buildGraph = [&](ShaderLab::Graph::EffectGraph& g,
                              uint32_t& srcId, uint32_t& statsId, uint32_t& ictcpId) {
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            srcId   = g.AddNode(std::move(srcNode));
            statsId = g.AddNode(std::move(statsNode));
            ictcpId = g.AddNode(std::move(ictcpNode));
            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, ictcpId, 0);
            g.BindProperty(ictcpId, L"TargetPeakNits", statsId, L"Mean", 0);
        };

        // ---- Sub-test 1: skip flag ON + GPU flag ON, no host hint -------
        // LumStats.Mean is read by ICtCp via GPU SRV -> readback skipped.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            uint64_t beforeSkip = ShaderLab::Performance::SkippedCpuReadbacks();

            // Two-pass: first creates effects, second produces output.
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            uint64_t afterSkip = ShaderLab::Performance::SkippedCpuReadbacks();
            TEST("SkipReadback_CounterIncrementsWhenSkipping",
                 afterSkip > beforeSkip);

            // ICtCp's image output must still be valid (it reads
            // LumStats.Mean via the upstream SRV at t1).
            auto* ictcp = g.FindNode(ictcpId);
            TEST("SkipReadback_ICtCpHasOutput",
                 ictcp && ictcp->cachedOutput != nullptr);

            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }

        // ---- Sub-test 2: skip flag ON + host hint forces readback ------
        // Adding LumStats to CpuAnalysisInterest re-engages readback so
        // the Properties panel / MCP can read fresh values. Validates
        // the host's escape hatch for any node that needs CPU values.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            g_evaluator.SetCpuAnalysisInterest({ statsId });

            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            auto* stats = g.FindNode(statsId);
            float mean = 0.0f;
            if (stats) {
                for (const auto& f : stats->analysisOutput.fields)
                    if (f.name == L"Mean") { mean = f.components[0]; break; }
            }
            TEST("SkipReadback_HostHintForcesReadback", mean > 0.0f);

            g_evaluator.ClearCpuAnalysisInterest();
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }

        // ---- Sub-test 3: skip flag OFF preserves baseline ---------------
        // Default behavior: every dispatch reads back regardless.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);

            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            auto* stats = g.FindNode(statsId);
            float mean = 0.0f;
            if (stats) {
                for (const auto& f : stats->analysisOutput.fields)
                    if (f.name == L"Mean") { mean = f.components[0]; break; }
            }
            TEST("SkipReadback_FlagOffAlwaysReadsBack", mean > 0.0f);

            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }

        // ---- Sub-test 4: hint throttle ---------------------------------
        // With throttle large, repeated frames with the same hint do NOT
        // re-readback so SkippedCpuReadbacks() advances. With throttle 0,
        // every frame reads back so the skipped counter stays put.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);
            // Drop the LumStats <- ICtCp binding so LumStats is purely
            // a host-hint readback target (no CPU-routed binding consumer
            // would force readback every frame).
            g.UnbindProperty(ictcpId, L"TargetPeakNits");

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            ShaderLab::Performance::SetCpuAnalysisHintThrottleMs(60'000);
            g_evaluator.SetCpuAnalysisInterest({ statsId });

            // First eval pair (fresh hint -> read).
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            uint64_t beforeSkipped = ShaderLab::Performance::SkippedCpuReadbacks();
            for (int i = 0; i < 4; ++i)
            {
                Evaluate(g, sf);
                g_dc->BeginDraw();
                g_evaluator.ProcessDeferredCompute(g, g_dc.get());
                g_dc->EndDraw();
            }
            uint64_t afterSkipped = ShaderLab::Performance::SkippedCpuReadbacks();
            TEST("SkipReadback_HintThrottleSkipsRepeatFrames",
                 afterSkipped > beforeSkipped);

            ShaderLab::Performance::SetCpuAnalysisHintThrottleMs(0);
            // With throttle=0 every frame should re-read LumStats, so the
            // hinted node gets a fresh value. Mutate fields then check
            // it gets repopulated by the next ProcessDeferredCompute.
            auto* statsNode = g.FindNode(statsId);
            if (statsNode)
                statsNode->analysisOutput.fields.clear();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            bool refreshed = statsNode &&
                statsNode->analysisOutput.type == ShaderLab::Graph::AnalysisOutputType::Typed &&
                !statsNode->analysisOutput.fields.empty();
            TEST("SkipReadback_ThrottleZeroReadsHintEveryFrame", refreshed);

            ShaderLab::Performance::SetCpuAnalysisHintThrottleMs(2000);
            g_evaluator.ClearCpuAnalysisInterest();
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }
    }
    //
    // The Phase 7 plan to move the MCP server engine-side and add a
    // ShaderLabHeadless.exe console host depends on being able to render
    // the graph and read pixels back without any DXGI swap chain. The
    // existing TestEffectChain already proves the *evaluate* half works
    // off-screen (the test runner has no swap chain). This test proves
    // the *read pixels back* half works too -- the missing capability
    // for graph_snapshot / render_capture_node / image_stats MCP routes
    // when running in a no-UI host.
    void TestHeadlessReadback()
    {
        printf("\n=== Phase 7 Spike: Headless Readback ===\n");

        // Build the smallest finite-extent graph we can. Gamut Source is
        // a ShaderLab effect with a fixed output size (scRGB FP16) and
        // produces a deterministic CIE xy chromaticity diagram. Any
        // bounded source works; we just need "evaluate -> finite cached
        // output -> can read a pixel".
        auto& reg = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Graph::EffectGraph g;
        ShaderLab::Effects::SourceNodeFactory sf;

        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *reg.FindByName(L"Gamut Source"));
        auto srcId = g.AddNode(std::move(srcNode));

        Evaluate(g, sf);
        auto* node = g.FindNode(srcId);
        if (!node || !node->cachedOutput) {
            TEST("Gamut Source produces cachedOutput", false);
            return;
        }
        TEST("Gamut Source produces cachedOutput", true);

        // Sample the source onto a small but non-trivial target. A 32x32
        // target with no srcRect lets D2D figure out its own tiling.
        // Map and read pixel (16, 16) for the test.
        const UINT kSize = 32;
        winrt::com_ptr<ID2D1Bitmap1> targetBmp;
        D2D1_BITMAP_PROPERTIES1 targetProps{};
        targetProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        targetProps.dpiX = 96.0f;
        targetProps.dpiY = 96.0f;
        HRESULT hr = g_dc->CreateBitmap(D2D1::SizeU(kSize, kSize), nullptr, 0, targetProps, targetBmp.put());
        if (FAILED(hr)) { TEST("CreateBitmap (target)", false); return; }

        winrt::com_ptr<ID2D1Bitmap1> stagingBmp;
        D2D1_BITMAP_PROPERTIES1 stagingProps{};
        stagingProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        stagingProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        stagingProps.dpiX = 96.0f;
        stagingProps.dpiY = 96.0f;
        hr = g_dc->CreateBitmap(D2D1::SizeU(kSize, kSize), nullptr, 0, stagingProps, stagingBmp.put());
        if (FAILED(hr)) { TEST("CreateBitmap (staging)", false); return; }

        float oldDpiX, oldDpiY;
        g_dc->GetDpi(&oldDpiX, &oldDpiY);
        g_dc->SetDpi(96.0f, 96.0f);

        g_dc->SetTarget(targetBmp.get());
        g_dc->BeginDraw();
        g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        // No srcRect, no offset: render whatever's there. D2D figures out
        // its own bounds; for the Gamut Source's fixed-size output this
        // is well-defined.
        g_dc->DrawImage(node->cachedOutput);
        hr = g_dc->EndDraw();
        g_dc->SetTarget(nullptr);
        g_dc->SetDpi(oldDpiX, oldDpiY);
        if (FAILED(hr)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "EndDraw target (hr=0x%08X)", static_cast<uint32_t>(hr));
            TEST(buf, false);
            return;
        }

        D2D1_POINT_2U dstPt = { 0, 0 };
        D2D1_RECT_U srcRectU = { 0, 0, kSize, kSize };
        hr = stagingBmp->CopyFromBitmap(&dstPt, targetBmp.get(), &srcRectU);
        if (FAILED(hr)) { TEST("CopyFromBitmap (staging)", false); return; }

        D2D1_MAPPED_RECT mapped{};
        hr = stagingBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) { TEST("Map staging bitmap", false); return; }

        // Read pixel (16, 16) from the mapped buffer.
        const uint8_t* row = mapped.bits + 16 * mapped.pitch;
        const float* px = reinterpret_cast<const float*>(row) + 16 * 4;
        const float r = px[0], green = px[1], b = px[2], a = px[3];
        stagingBmp->Unmap();

        // The exact value depends on Gamut Source's content at (128,128),
        // which we don't try to predict here. The Phase 7 spike only
        // needs to confirm: (a) the readback path runs without hanging
        // or failing, and (b) the bytes that came back are finite,
        // non-NaN floats. That's the headless-host capability we need
        // for graph_snapshot / render_capture_node / image_stats MCP
        // routes.
        bool finite = std::isfinite(r) && std::isfinite(green)
            && std::isfinite(b) && std::isfinite(a);
        TEST("Headless pixel readback completes without hang", true);
        TEST("Headless pixel readback returns finite floats", finite);

        std::printf("    pixel = (%.4f, %.4f, %.4f, %.4f)\n", r, green, b, a);
    }

    void TestSnapshot()
    {
        printf("\n=== GraphUiSnapshot ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        ShaderLab::Graph::EffectGraph g;
        auto srcDesc = registry.FindByName(L"Gamut Source");
        auto dstDesc = registry.FindByName(L"Gamut Source");
        auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc));
        auto dstId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*dstDesc));
        g.Connect(srcId, 0, dstId, 0);

        // Stash a fake cachedOutput on a node to confirm the snapshot strips
        // it (raw ID2D1Image* must never escape into UI code).
        auto* live = g.FindNode(srcId);
        live->cachedOutput = reinterpret_cast<ID2D1Image*>(static_cast<uintptr_t>(0xDEADBEEFu));
        live->runtimeError = L"oops";
        live->dirty = true;

        auto snap = ShaderLab::Graph::BuildGraphUiSnapshot(
            g, /*previewId*/ dstId, /*graphGen*/ 7, /*frameGen*/ 99);
        TEST("BuildSnapshot returns non-null",        snap != nullptr);
        TEST("Snapshot graphGeneration",              snap->graphGeneration == 7);
        TEST("Snapshot frameGeneration",              snap->frameGeneration == 99);
        TEST("Snapshot previewNodeId",                snap->previewNodeId == dstId);
        TEST("Snapshot node count matches live",      snap->nodes.size() == 2);
        TEST("Snapshot edge count matches live",      snap->edges.size() == 1);
        TEST("Snapshot lookup by id works",           snap->FindNode(srcId) != nullptr);
        TEST("Snapshot lookup unknown id is null",    snap->FindNode(99999u) == nullptr);
        TEST("Snapshot strips cachedOutput",          snap->FindNode(srcId)->cachedOutput == nullptr);
        TEST("Snapshot preserves runtimeError",       snap->FindNode(srcId)->runtimeError == L"oops");
        TEST("Snapshot preserves dirty flag",         snap->FindNode(srcId)->dirty);
        TEST("Snapshot edge mirrors connection",
             snap->edges[0].sourceNodeId == srcId && snap->edges[0].destNodeId == dstId);

        // Mutating the live graph after snapshot must not affect the snap.
        g.RemoveNode(srcId);
        TEST("Live mutation does not affect snapshot",
             snap->nodes.size() == 2 && snap->FindNode(srcId) != nullptr);

        // atomic<shared_ptr> publication round-trip.
        std::atomic<std::shared_ptr<const ShaderLab::Graph::GraphUiSnapshot>> latest{ snap };
        auto loaded = latest.load();
        TEST("atomic<shared_ptr> publication round-trips", loaded == snap);
    }

    void TestRenderThreadDispatcher()
    {
        printf("\n=== RenderThreadDispatcher ===\n");

        // ---- Synchronous mode runs inline, no threading. ------------------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d{ /*synchronous=*/true };
            int counter = 0;
            d.DispatchAsync([&]{ counter++; });
            TEST("synchronous mode runs DispatchAsync inline",
                 counter == 1);

            int sum = d.DispatchSync([]{ return 7 + 35; });
            TEST("synchronous mode DispatchSync returns value", sum == 42);

            // Re-entry from inside a closure runs inline.
            int reentry = 0;
            d.DispatchSync([&]{
                d.DispatchSync([&]{ reentry = 99; });
            });
            TEST("synchronous mode allows re-entrant DispatchSync",
                 reentry == 99);
        }

        // ---- Async-with-consumer-thread happy path. -----------------------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> stop{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                while (!stop.load(std::memory_order_acquire))
                {
                    d.WaitFor(std::chrono::milliseconds(50));
                    d.Drain();
                }
                d.Drain();
            });

            std::atomic<int> hits{ 0 };
            for (int i = 0; i < 100; ++i)
                d.DispatchAsync([&]{ hits.fetch_add(1, std::memory_order_relaxed); });

            // DispatchSync from a non-consumer thread blocks until done.
            int v = d.DispatchSync([&]{
                return hits.load(std::memory_order_relaxed);
            });
            TEST("async drain ran all enqueued closures (>=100)", v >= 100);
            TEST("async DispatchSync returned a value seen on consumer",
                 v == 100);

            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();
        }

        // ---- Re-entrant DispatchSync from inside a closure runs inline. ---
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> stop{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                while (!stop.load(std::memory_order_acquire))
                {
                    d.WaitFor(std::chrono::milliseconds(50));
                    d.Drain();
                }
                d.Drain();
            });

            int outer = 0, inner = 0;
            d.DispatchSync([&]{
                outer = 1;
                // From inside a closure (= consumer thread), DispatchSync
                // must NOT requeue and self-deadlock; it must run inline.
                d.DispatchSync([&]{ inner = 2; });
            });
            TEST("re-entrant DispatchSync did not deadlock",
                 outer == 1 && inner == 2);

            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();
        }

        // ---- Closure exception propagates back through DispatchSync. ------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d{ /*synchronous=*/true };
            bool threw = false;
            try
            {
                d.DispatchSync([]() -> int {
                    throw std::runtime_error("expected test failure");
                });
            }
            catch (const std::exception&) { threw = true; }
            TEST("DispatchSync re-throws closure exception", threw);
        }

        // ---- Shutdown cancels pending closures and unblocks waiters. ------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> consumerRan{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                d.Wait();          // returns when shutdown signaled
                consumerRan.store(true, std::memory_order_release);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            d.Shutdown();
            consumer.join();
            TEST("Shutdown wakes Wait()", consumerRan.load());
            TEST("Shutdown clears queue", d.QueueDepth() == 0);
            TEST("IsShuttingDown reflects state", d.IsShuttingDown());
        }

        // ---- P19: many concurrent producers, single consumer. ------------
        // Stress test: 8 producer threads each post 250 sync closures
        // returning a unique tag. All 2000 closures must run on the consumer
        // thread (single thread id seen) and each producer must observe its
        // own tag back.
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> stop{ false };
            std::atomic<int> totalRan{ 0 };
            std::atomic<std::thread::id> consumerId{};
            std::atomic<bool> mixedThread{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                consumerId.store(std::this_thread::get_id(), std::memory_order_release);
                while (!stop.load(std::memory_order_acquire)) {
                    d.WaitFor(std::chrono::milliseconds(5));
                    d.Drain();
                }
                d.Drain();
            });

            constexpr int producers = 8;
            constexpr int perProducer = 250;
            std::vector<std::thread> producerThreads;
            std::atomic<int> mismatches{ 0 };
            producerThreads.reserve(producers);
            for (int p = 0; p < producers; ++p) {
                producerThreads.emplace_back([&, p]{
                    for (int i = 0; i < perProducer; ++i) {
                        int tag = p * 1000 + i;
                        int got = d.DispatchSync([&, tag]{
                            if (std::this_thread::get_id() != consumerId.load())
                                mixedThread.store(true);
                            totalRan.fetch_add(1, std::memory_order_relaxed);
                            return tag;
                        });
                        if (got != tag) mismatches.fetch_add(1);
                    }
                });
            }
            for (auto& t : producerThreads) t.join();
            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();

            TEST("8 producers x 250 closures all ran",
                totalRan.load() == producers * perProducer);
            TEST("every closure ran on the consumer thread",
                !mixedThread.load());
            TEST("every DispatchSync returned its own tag",
                mismatches.load() == 0);
        }

        // ---- P19: shutdown with many pending closures unblocks all. ------
        // Producers that are blocked in DispatchSync must wake when Shutdown
        // is called. They get an exception (std::runtime_error from the
        // dispatcher), not a hang.
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::thread consumer([&]{
                d.RegisterConsumer();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                // never drain -- closures pile up in the queue
            });

            constexpr int producers = 4;
            std::vector<std::thread> producerThreads;
            std::atomic<int> threwCount{ 0 };
            std::atomic<int> noThrowCount{ 0 };
            for (int p = 0; p < producers; ++p) {
                producerThreads.emplace_back([&]{
                    try {
                        d.DispatchSync([]{ return 1; });
                        noThrowCount.fetch_add(1);
                    } catch (...) {
                        threwCount.fetch_add(1);
                    }
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            d.Shutdown();
            for (auto& t : producerThreads) t.join();
            consumer.join();

            TEST("Shutdown unblocks pending DispatchSync producers",
                threwCount.load() + noThrowCount.load() == producers);
            TEST("at least some producers observed shutdown",
                threwCount.load() > 0);
        }
    }
}

int main(int argc, char* argv[])
{
    winrt::init_apartment();
    MFStartup(MF_VERSION);

    // Parse adapter flag.
    bool useWarp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--adapter" && i + 1 < argc) {
            useWarp = (std::string(argv[i + 1]) == "warp");
            i++;
        }
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("ShaderLab Test Runner\n");
    printf("=====================\n");

    // Create D3D11 device.
    UINT d3dFlags = DefaultD3D11DeviceFlags;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    winrt::com_ptr<ID3D11Device> baseDevice;
    winrt::com_ptr<ID3D11DeviceContext> baseCtx;
    D3D_DRIVER_TYPE driverType = useWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(nullptr, driverType, nullptr, d3dFlags,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseCtx.put());
    if (FAILED(hr)) {
        printf("FATAL: D3D11CreateDevice failed 0x%08X\n", (uint32_t)hr);
        return 1;
    }
    g_d3dDevice = baseDevice.as<ID3D11Device5>();
    g_d3dContext = baseCtx.as<ID3D11DeviceContext4>();

    // Enable multithread protection.
    winrt::com_ptr<ID3D10Multithread> mt;
    g_d3dDevice.as(mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    printf("Device: %s\n", useWarp ? "WARP" : "Hardware");

    // Create D2D.
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory7), reinterpret_cast<void**>(g_d2dFactory.put()));
    winrt::com_ptr<IDXGIDevice> dxgiDev;
    baseDevice->QueryInterface(dxgiDev.put());
    winrt::com_ptr<ID2D1Device6> d2dDevice;
    g_d2dFactory->CreateDevice(dxgiDev.as<IDXGIDevice>().get(),
        reinterpret_cast<ID2D1Device**>(d2dDevice.put()));
    d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        reinterpret_cast<ID2D1DeviceContext**>(g_dc.put()));

    // Register custom effects.
    winrt::com_ptr<ID2D1Factory1> factory1;
    g_d2dFactory->QueryInterface(factory1.put());
    ShaderLab::Effects::RegisterEngineD2DEffects(factory1.get());

    // Run tests.
    TestGraphOperations();
    TestSerialization();
    TestSourceEffects();
    TestAnalysisEffects();
    TestBuiltInD2DEffects();
    TestPropertyBindings();
    TestMathNodes();
    TestClockNode();
    TestShaderCompilation();
    TestBytecodeCache();
    TestEffectChain();
    TestLuminanceStatistics();
    TestGpuBindingRouting();
    TestSkipCpuReadback();
    TestHeadlessReadback();
    TestSnapshot();
    TestRenderThreadDispatcher();

    // ---- Math test bench (Phase 2) -----------------------------------------
    {
        ShaderLab::Tests::ShaderTestBench bench;
        bench.Initialize(g_d3dDevice.get(), g_d3dContext.get());
        ShaderLab::Tests::TestTransferFunctions(bench);
        ShaderLab::Tests::TestColorMatrices(bench);
        ShaderLab::Tests::TestMobiusReinhard(bench);
        ShaderLab::Tests::TestDeltaE(bench);
        ShaderLab::Tests::TestGamut(bench);
        bench.Shutdown();
    }

    // Summary.
    printf("\n========================================\n");
    if (g_failed == 0)
        printf("ALL %d TESTS PASSED\n", g_passed);
    else
        printf("%d PASSED, %d FAILED out of %d\n", g_passed, g_failed, g_passed + g_failed);
    printf("========================================\n");

    MFShutdown();
    winrt::uninit_apartment();
    return g_failed;
}
