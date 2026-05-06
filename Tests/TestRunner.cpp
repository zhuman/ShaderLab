#include "pch_engine.h"
#include "Graph/EffectGraph.h"
#include "Rendering/GraphEvaluator.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/EffectRegistry.h"
#include "Effects/ShaderCompiler.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Rendering/D3DDefinitions.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

// ============================================================================
// ShaderLab Test Runner — standalone console app sharing the engine code.
// Usage: ShaderLabTests.exe [--adapter warp|default]
// Exit code = number of failures (0 = all passed).
// ============================================================================

namespace
{
    int g_passed = 0;
    int g_failed = 0;

    void TEST(const char* name, bool result)
    {
        if (result) {
            printf("  [PASS] %s\n", name);
            g_passed++;
        }
        else {
            printf("  [FAIL] %s\n", name);
            g_failed++;
        }
        fflush(stdout);
    }

    // Shared device resources.
    winrt::com_ptr<ID3D11Device5> g_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext4> g_d3dContext;
    winrt::com_ptr<ID2D1Factory7> g_d2dFactory;
    winrt::com_ptr<ID2D1DeviceContext5> g_dc;

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
        ShaderLab::Rendering::GraphEvaluator evaluator;
        evaluator.Evaluate(graph, g_dc.get());
        evaluator.Evaluate(graph, g_dc.get()); // second pass for new effects
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

        const wchar_t* names[] = {
            L"Luminance Heatmap", L"Gamut Highlight", L"Nit Map",
            L"Waveform Monitor"
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
            Evaluate(g, sf);

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
            Evaluate(g, sf);

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
    TestEffectChain();

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
