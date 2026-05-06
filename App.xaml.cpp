#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Rendering/RenderEngine.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/D3DDefinitions.h"
#include "Graph/EffectGraph.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/EffectRegistry.h"
#include "Effects/ShaderCompiler.h"
#include <ShlObj.h>
#pragma comment(lib, "shell32.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::ShaderLab::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // Parse flags from command line.
        std::wstring cmdLine = GetCommandLineW();
        bool autoMcp = (cmdLine.find(L"--mcp") != std::wstring::npos);
        auto devicePref = ::ShaderLab::Rendering::DevicePreference::Default;
        if (cmdLine.find(L"--warp") != std::wstring::npos)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
        else if (cmdLine.find(L"--gpu") != std::wstring::npos)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Hardware;

        // Also check environment variables.
        wchar_t envBuf[16]{};
        if (GetEnvironmentVariableW(L"SHADERLAB_MCP", envBuf, 16) > 0)
            autoMcp = (envBuf[0] == L'1' || envBuf[0] == L't' || envBuf[0] == L'T');
        if (GetEnvironmentVariableW(L"SHADERLAB_WARP", envBuf, 16) > 0)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;

        // Also check config file: %LOCALAPPDATA%\ShaderLab\config.json
        // Format: {"mcp": true, "renderer": "warp"|"gpu"|"default"}
        {
            // Try multiple paths: real LOCALAPPDATA via SHGetKnownFolderPath,
            // and the env var (which may be virtualized in packaged apps).
            std::vector<std::wstring> configPaths;

            // Real LOCALAPPDATA via shell API.
            PWSTR realAppData = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &realAppData)))
            {
                configPaths.push_back(std::wstring(realAppData) + L"\\ShaderLab\\config.json");
                CoTaskMemFree(realAppData);
            }

            // Env var fallback.
            wchar_t appData[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH) > 0)
            {
                auto envPath = std::wstring(appData) + L"\\ShaderLab\\config.json";
                if (configPaths.empty() || configPaths[0] != envPath)
                    configPaths.push_back(envPath);
            }

            for (const auto& configPath : configPaths)
            {
                HANDLE hFile = CreateFileW(configPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, 0, nullptr);
                if (hFile != INVALID_HANDLE_VALUE)
                {
                    char buf[1024]{};
                    DWORD bytesRead = 0;
                    ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr);
                    CloseHandle(hFile);
                    std::string json(buf, bytesRead);
                    if (json.find("\"mcp\"") != std::string::npos &&
                        json.find("true") != std::string::npos)
                        autoMcp = true;
                    if (json.find("\"warp\"") != std::string::npos)
                        devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
                    else if (json.find("\"gpu\"") != std::string::npos)
                        devicePref = ::ShaderLab::Rendering::DevicePreference::Hardware;
                    if (json.find("\"test\": true") != std::string::npos)
                    {
                        int failures = 99;
                        try { failures = RunTestMode(static_cast<int>(devicePref)); }
                        catch (const winrt::hresult_error& ex) {
                            OutputDebugStringW(std::format(L"[TEST] Exception: 0x{:08X}\n",
                                static_cast<uint32_t>(ex.code())).c_str());
                        }
                        catch (const std::exception& ex) {
                            OutputDebugStringA(ex.what());
                        }
                        catch (...) {
                            OutputDebugStringW(L"[TEST] Unknown exception\n");
                        }
                        ExitProcess(static_cast<UINT>(failures));
                        return;
                    }
                    break;
                }
            }
        }

        // ---- CLI Mode: headless graph evaluation ----
        if (cmdLine.find(L"--cli") != std::wstring::npos)
        {
            RunCliMode(cmdLine, static_cast<int>(devicePref));
            Application::Current().Exit();
            return;
        }

        // ---- Reap mode: scan %TEMP% for orphaned ShaderLab media dirs ----
        // and delete any whose .heartbeat (or directory mtime, for legacy
        // dirs without a heartbeat) is older than the staleness threshold.
        // Useful for CI / scripted regression: no UI, no prompt, exits
        // with the count of dirs reaped.
        if (cmdLine.find(L"--reap-now") != std::wstring::npos)
        {
            // Standard staleness threshold: 2.5 x heartbeat interval. Keep
            // this default in sync with MainWindow::HeartbeatStaleSec.
            uint64_t staleSec = 150;

            // Optional --reap-stale-sec=<N> override for tests that don't
            // want to wait through the default window.
            {
                auto p = cmdLine.find(L"--reap-stale-sec=");
                if (p != std::wstring::npos)
                {
                    p += wcslen(L"--reap-stale-sec=");
                    auto end = cmdLine.find(L' ', p);
                    auto str = cmdLine.substr(p, end - p);
                    try
                    {
                        staleSec = std::stoull(str);
                    }
                    catch (...) {}
                }
            }

            wchar_t tempBuf[MAX_PATH + 1]{};
            DWORD len = ::GetTempPathW(MAX_PATH, tempBuf);
            uint32_t reaped = 0;
            if (len > 0)
            {
                std::wstring tempRoot(tempBuf, len);

                FILETIME nowFt{};
                ::GetSystemTimeAsFileTime(&nowFt);
                const uint64_t now = (static_cast<uint64_t>(nowFt.dwHighDateTime) << 32)
                                   | nowFt.dwLowDateTime;
                const uint64_t staleTicks = staleSec * 10'000'000ULL;

                // Allocate / use stderr for status reporting.
                if (!AttachConsole(ATTACH_PARENT_PROCESS))
                    AllocConsole();
                FILE* fp = nullptr;
                freopen_s(&fp, "CONOUT$", "w", stderr);

                std::error_code ec;
                for (const auto& entry : std::filesystem::directory_iterator(tempRoot, ec))
                {
                    if (ec) break;
                    if (!entry.is_directory(ec)) continue;
                    const auto name = entry.path().filename().wstring();
                    if (!name.starts_with(L"ShaderLab-")) continue;

                    FILETIME ft{};
                    std::wstring beat = entry.path().wstring() + L"\\.heartbeat";
                    HANDLE h = ::CreateFileW(beat.c_str(),
                        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN, nullptr);
                    if (h != INVALID_HANDLE_VALUE)
                    {
                        ::GetFileTime(h, nullptr, nullptr, &ft);
                        ::CloseHandle(h);
                    }
                    else
                    {
                        WIN32_FILE_ATTRIBUTE_DATA fad{};
                        if (::GetFileAttributesExW(entry.path().c_str(),
                            GetFileExInfoStandard, &fad))
                            ft = fad.ftLastWriteTime;
                    }
                    const uint64_t touched =
                        (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                    if (touched != 0 && (now <= touched || (now - touched) <= staleTicks))
                    {
                        fwprintf(stderr, L"[reap] keep   %s (fresh)\n", name.c_str());
                        continue;
                    }

                    std::error_code rmEc;
                    std::filesystem::remove_all(entry.path(), rmEc);
                    if (rmEc)
                    {
                        fwprintf(stderr, L"[reap] FAIL   %s (%hs)\n",
                            name.c_str(), rmEc.message().c_str());
                    }
                    else
                    {
                        fwprintf(stderr, L"[reap] delete %s\n", name.c_str());
                        ++reaped;
                    }
                }
                fwprintf(stderr, L"[reap] done; %u folder%s removed\n",
                    reaped, reaped == 1 ? L"" : L"s");
            }
            ExitProcess(reaped);
            return;
        }

        auto mw = make<MainWindow>();
        auto* impl = winrt::get_self<MainWindow>(mw);
        if (autoMcp)
            impl->SetAutoStartMcp(true);
        impl->SetDevicePreference(devicePref);

        // File activation: when the user double-clicks a .effectgraph
        // file in Explorer, the OS launches us with the file path on
        // the command line. We just scan for it here -- proper
        // Microsoft.Windows.AppLifecycle activation routing isn't
        // worth the WinAppSDK reunion glue for a single argument.
        {
            int argc = 0;
            wchar_t** argv = ::CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv)
            {
                for (int i = 1; i < argc; ++i)
                {
                    std::wstring a = argv[i];
                    // Skip our own flags.
                    if (a.starts_with(L"--")) continue;
                    auto lower = a;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                    if (lower.ends_with(L".effectgraph"))
                    {
                        impl->SetPendingOpenPath(a);
                        break;
                    }
                }
                ::LocalFree(argv);
            }
        }

        window = mw;
        window.Activate();
    }

    void App::RunCliMode(const std::wstring& cmdLine, int devicePrefInt)
    {
        auto devicePref = static_cast<::ShaderLab::Rendering::DevicePreference>(devicePrefInt);
        // Attach or allocate a console for output.
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            AllocConsole();
        FILE* fOut = nullptr;
        freopen_s(&fOut, "CONOUT$", "w", stdout);

        // Parse --graph and --output paths.
        auto extractArg = [&](const std::wstring& flag) -> std::wstring {
            auto pos = cmdLine.find(flag);
            if (pos == std::wstring::npos) return std::wstring();
            pos += flag.size();
            while (pos < cmdLine.size() && cmdLine[pos] == L' ') pos++;
            if (pos >= cmdLine.size()) return std::wstring();
            if (cmdLine[pos] == L'"')
            {
                auto end = cmdLine.find(L'"', pos + 1);
                return (end != std::wstring::npos) ? cmdLine.substr(pos + 1, end - pos - 1) : std::wstring();
            }
            auto end = cmdLine.find(L' ', pos);
            return (end != std::wstring::npos) ? cmdLine.substr(pos, end - pos) : cmdLine.substr(pos);
        };

        auto graphPath = extractArg(L"--graph");
        auto outputDir = extractArg(L"--output");
        auto adapterName = extractArg(L"--adapter");

        if (graphPath.empty())
        {
            wprintf(L"Usage: ShaderLab.exe --cli --graph <path.json> --output <dir> [--adapter warp|default]\n");
            return;
        }
        if (outputDir.empty()) outputDir = L".";

        // Handle --adapter flag.
        if (!adapterName.empty())
        {
            if (adapterName == L"warp" || adapterName == L"WARP")
                devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
            else if (adapterName != L"default")
            {
                // Try to find adapter by name substring.
                auto adapters = ::ShaderLab::Rendering::RenderEngine::EnumerateAdapters();
                for (const auto& a : adapters)
                {
                    if (a.name.find(adapterName) != std::wstring::npos)
                    {
                        devicePref = ::ShaderLab::Rendering::DevicePreference::Adapter;
                        // Store LUID for later — need to set on engine.
                        break;
                    }
                }
            }
        }

        wprintf(L"[CLI] Loading graph: %s\n", graphPath.c_str());

        // Load graph JSON.
        std::wstring graphJson;
        {
            HANDLE hf = CreateFileW(graphPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, 0, nullptr);
            if (hf == INVALID_HANDLE_VALUE)
            {
                wprintf(L"[CLI] Error: cannot open graph file\n");
                return;
            }
            DWORD size = GetFileSize(hf, nullptr);
            std::string utf8(size, '\0');
            DWORD read = 0;
            ReadFile(hf, utf8.data(), size, &read, nullptr);
            CloseHandle(hf);
            graphJson = winrt::to_hstring(utf8).c_str();
        }

        // Create headless D3D/D2D device stack.
        ::ShaderLab::Rendering::RenderEngine engine;
        // Initialize without a swap chain panel — just device resources.
        // We need a minimal init path. Use the internal CreateDeviceResources.
        // For now, use a simpler approach: create device directly.
        UINT d3dFlags = DefaultD3D11DeviceFlags;
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        winrt::com_ptr<ID3D11Device> baseDevice;
        winrt::com_ptr<ID3D11DeviceContext> baseCtx;
        D3D_DRIVER_TYPE driverType = (devicePref == ::ShaderLab::Rendering::DevicePreference::Warp)
            ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        HRESULT hr = D3D11CreateDevice(nullptr, driverType, nullptr, d3dFlags,
            featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseCtx.put());
        if (FAILED(hr))
        {
            wprintf(L"[CLI] Error: D3D11CreateDevice failed 0x%08X\n", (uint32_t)hr);
            return;
        }

        // Create D2D factory and device.
        winrt::com_ptr<ID2D1Factory7> d2dFactory;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory7), reinterpret_cast<void**>(d2dFactory.put()));

        winrt::com_ptr<IDXGIDevice> dxgiDev;
        baseDevice->QueryInterface(dxgiDev.put());
        winrt::com_ptr<ID2D1Device6> d2dDevice;
        d2dFactory->CreateDevice(dxgiDev.as<IDXGIDevice>().get(),
            reinterpret_cast<ID2D1Device**>(d2dDevice.put()));
        winrt::com_ptr<ID2D1DeviceContext5> dc;
        d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            reinterpret_cast<ID2D1DeviceContext**>(dc.put()));

        // Register custom D2D effects.
        winrt::com_ptr<ID2D1Factory1> factory1;
        d2dFactory->QueryInterface(factory1.put());
        ::ShaderLab::Effects::RegisterEngineD2DEffects(factory1.get());

        wprintf(L"[CLI] Device created (%s)\n",
            driverType == D3D_DRIVER_TYPE_WARP ? L"WARP" : L"Hardware");

        // Load and evaluate graph.
        auto graph = ::ShaderLab::Graph::EffectGraph::FromJson(winrt::hstring(graphJson));
        graph.MarkAllDirty();

        // Prepare source nodes.
        ::ShaderLab::Effects::SourceNodeFactory sourceFactory;
        for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(graph.Nodes()))
        {
            if (node.type == ::ShaderLab::Graph::NodeType::Source)
            {
                try {
                    sourceFactory.PrepareSourceNode(node, dc.get(), 0.0,
                        baseDevice.as<ID3D11Device5>().get(),
                        baseCtx.as<ID3D11DeviceContext4>().get());
                } catch (...) {
                    wprintf(L"[CLI] Warning: failed to prepare source node %d\n", node.id);
                }
            }
        }

        // Evaluate.
        ::ShaderLab::Rendering::GraphEvaluator evaluator;
        evaluator.Evaluate(graph, dc.get());
        if (graph.HasDirtyNodes())
            evaluator.Evaluate(graph, dc.get());

        // Save output for each node that has cachedOutput.
        // Create output directory if needed.
        CreateDirectoryW(outputDir.c_str(), nullptr);

        uint32_t saved = 0;
        for (const auto& node : graph.Nodes())
        {
            if (!node.cachedOutput) continue;
            if (node.type == ::ShaderLab::Graph::NodeType::Output ||
                node.outputPins.empty()) // leaf nodes
            {
                // Render to bitmap.
                D2D1_RECT_F bounds{};
                dc->GetImageLocalBounds(node.cachedOutput, &bounds);
                uint32_t w = static_cast<uint32_t>(bounds.right - bounds.left);
                uint32_t h = static_cast<uint32_t>(bounds.bottom - bounds.top);
                if (w == 0 || h == 0 || w > 8192 || h > 8192) continue;

                winrt::com_ptr<ID2D1Bitmap1> renderBmp;
                D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_TARGET,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bmpProps, renderBmp.put());
                if (!renderBmp) continue;

                winrt::com_ptr<ID2D1Image> oldTarget;
                dc->GetTarget(oldTarget.put());
                dc->SetTarget(renderBmp.get());
                dc->BeginDraw();
                dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));
                dc->DrawImage(node.cachedOutput);
                dc->EndDraw();
                dc->SetTarget(oldTarget.get());

                // CPU readback.
                winrt::com_ptr<ID2D1Bitmap1> cpuBmp;
                D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, cpuProps, cpuBmp.put());
                if (!cpuBmp) continue;
                cpuBmp->CopyFromBitmap(nullptr, renderBmp.get(), nullptr);

                D2D1_MAPPED_RECT mapped{};
                cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);

                // Write PNG via WIC.
                winrt::com_ptr<IWICImagingFactory> wic;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
                if (!wic) { cpuBmp->Unmap(); continue; }

                std::wstring outPath = std::wstring(outputDir) + L"\\" +
                    std::format(L"node_{}.png", node.id);
                winrt::com_ptr<IWICStream> stream;
                wic->CreateStream(stream.put());
                stream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE);

                winrt::com_ptr<IWICBitmapEncoder> encoder;
                wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
                encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);

                winrt::com_ptr<IWICBitmapFrameEncode> frame;
                encoder->CreateNewFrame(frame.put(), nullptr);
                frame->Initialize(nullptr);
                frame->SetSize(w, h);
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                frame->SetPixelFormat(&fmt);
                frame->WritePixels(h, mapped.pitch, mapped.pitch * h,
                    const_cast<BYTE*>(mapped.bits));
                frame->Commit();
                encoder->Commit();

                cpuBmp->Unmap();
                wprintf(L"[CLI] Saved: %s (%dx%d)\n", outPath.c_str(), w, h);
                saved++;
            }
        }

        wprintf(L"[CLI] Done. %d images saved.\n", saved);
    }

    int App::RunTestMode(int devicePrefInt)
    {
        auto devicePref = static_cast<::ShaderLab::Rendering::DevicePreference>(devicePrefInt);

        // Write results to a file the test runner can read.
        wchar_t tempPath[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring resultPath = std::wstring(tempPath) + L"shaderlab_test_results.txt";
        FILE* rf = nullptr;
        _wfopen_s(&rf, resultPath.c_str(), L"w");

        int passed = 0, failed = 0;
        auto LOG = [&](const char* fmt, ...) {
            va_list args; va_start(args, fmt);
            if (rf) { vfprintf(rf, fmt, args); fprintf(rf, "\n"); fflush(rf); }
            char buf[512]; vsnprintf(buf, sizeof(buf), fmt, args);
            OutputDebugStringA(buf); OutputDebugStringA("\n");
            va_end(args);
        };
        auto TEST = [&](const char* name, bool result) {
            if (result) { LOG("  [PASS] %s", name); passed++; }
            else        { LOG("  [FAIL] %s", name); failed++; }
        };

        LOG("[TEST] Creating device (%s)...",
            devicePref == ::ShaderLab::Rendering::DevicePreference::Warp ? "WARP" : "Hardware");

        UINT d3dFlags = DefaultD3D11DeviceFlags;
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        winrt::com_ptr<ID3D11Device> baseDevice;
        winrt::com_ptr<ID3D11DeviceContext> baseCtx;
        D3D_DRIVER_TYPE driverType = (devicePref == ::ShaderLab::Rendering::DevicePreference::Warp)
            ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        HRESULT hr = D3D11CreateDevice(nullptr, driverType, nullptr, d3dFlags,
            featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseCtx.put());
        if (FAILED(hr)) { LOG("[TEST] FATAL: D3D11CreateDevice failed 0x%08X", (uint32_t)hr); if (rf) fclose(rf); return 1; }

        winrt::com_ptr<ID3D10Multithread> mt;
        baseDevice.as(mt);
        if (mt) mt->SetMultithreadProtected(TRUE);

        auto d3dDev5 = baseDevice.as<ID3D11Device5>().get();
        auto d3dCtx4 = baseCtx.as<ID3D11DeviceContext4>().get();

        winrt::com_ptr<ID2D1Factory7> d2dFactory;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory7), reinterpret_cast<void**>(d2dFactory.put()));
        winrt::com_ptr<IDXGIDevice> dxgiDev;
        baseDevice->QueryInterface(dxgiDev.put());
        winrt::com_ptr<ID2D1Device6> d2dDevice;
        d2dFactory->CreateDevice(dxgiDev.as<IDXGIDevice>().get(),
            reinterpret_cast<ID2D1Device**>(d2dDevice.put()));
        winrt::com_ptr<ID2D1DeviceContext5> dc;
        d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            reinterpret_cast<ID2D1DeviceContext**>(dc.put()));

        winrt::com_ptr<ID2D1Factory1> factory1;
        d2dFactory->QueryInterface(factory1.put());
        ::ShaderLab::Effects::RegisterEngineD2DEffects(factory1.get());

        LOG("[TEST] Device created");

        auto& registry = ::ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ::ShaderLab::Effects::EffectRegistry::Instance();

        // Helper: evaluate graph. Returns the evaluator (must stay alive while checking output).
        auto evaluate = [&](::ShaderLab::Graph::EffectGraph& g, ::ShaderLab::Effects::SourceNodeFactory& sf,
            ::ShaderLab::Rendering::GraphEvaluator& evaluator) {
            g.MarkAllDirty();
            for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(g.Nodes()))
                if (node.type == ::ShaderLab::Graph::NodeType::Source)
                    try { sf.PrepareSourceNode(node, dc.get(), 0.0, d3dDev5, d3dCtx4); } catch (...) {}
            evaluator.Evaluate(g, dc.get());
            evaluator.Evaluate(g, dc.get());
        };
        auto hasOutput = [&](const ::ShaderLab::Graph::EffectNode& node) -> bool {
            if (!node.cachedOutput) return false;
            try {
                D2D1_RECT_F b{}; HRESULT hr2 = dc->GetImageLocalBounds(node.cachedOutput, &b);
                return SUCCEEDED(hr2) && (b.right - b.left) > 0 && (b.bottom - b.top) > 0;
            } catch (...) { return false; }
        };

        // ---- Graph Operations ----
        LOG("\n=== Graph Operations ===");
        {
            ::ShaderLab::Graph::EffectGraph g;
            auto n = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
            auto id = g.AddNode(std::move(n));
            TEST("AddNode", g.FindNode(id) != nullptr);
            g.RemoveNode(id);
            TEST("RemoveNode", g.Nodes().empty());

            auto src = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
            auto blur = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));
            auto sId = g.AddNode(std::move(src));
            auto bId = g.AddNode(std::move(blur));
            TEST("Connect", g.Connect(sId, 0, bId, 0));
            g.Disconnect(sId, 0, bId, 0);
            TEST("Disconnect", g.GetInputEdges(bId).empty());
            g.Connect(sId, 0, bId, 0);
            TEST("TopoSort", g.TopologicalSort().size() == 2);
            // WouldCreateCycle: blur→src WOULD create a cycle (src→blur exists).
            TEST("DetectsCycle", g.WouldCreateCycle(bId, sId));
            // src→blur would NOT create a new cycle (it already exists).
            TEST("AllowsExisting", !g.WouldCreateCycle(sId, bId));
        }

        // ---- Serialization ----
        LOG("\n=== Serialization ===");
        {
            ::ShaderLab::Graph::EffectGraph g;
            auto src = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
            auto blur = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));
            auto sId = g.AddNode(std::move(src));
            auto bId = g.AddNode(std::move(blur));
            g.Connect(sId, 0, bId, 0);
            g.FindNode(bId)->properties[L"StandardDeviation"] = 3.0f;
            auto json = g.ToJson();
            TEST("Serialize", !json.empty());
            auto g2 = ::ShaderLab::Graph::EffectGraph::FromJson(json);
            TEST("DeserializeNodes", g2.Nodes().size() == 2);
            TEST("DeserializeEdges", !g2.Edges().empty());
            auto* bl = g2.FindNode(bId);
            bool propOk = false;
            if (bl) { auto it = bl->properties.find(L"StandardDeviation"); if (it != bl->properties.end()) { auto* f = std::get_if<float>(&it->second); propOk = f && std::abs(*f - 3.0f) < 0.01f; } }
            TEST("DeserializeProperty", propOk);
        }

        // ---- Source Effects ----
        LOG("\n=== Source Effects ===");
        { const wchar_t* names[] = { L"Gamut Source", L"Color Checker", L"Zone Plate", L"Gradient Generator", L"HDR Test Pattern" };
          for (auto* name : names) {
            std::string tn = "Source_"; for (auto* p = name; *p; ++p) tn += (char)*p;
            try {
            LOG("  testing %s...", tn.c_str());
            ::ShaderLab::Graph::EffectGraph g; ::ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(name));
            auto id = g.AddNode(std::move(node));
            LOG("    evaluating...");
            ::ShaderLab::Rendering::GraphEvaluator ev; evaluate(g, sf, ev);
            LOG("    checking output...");
            auto* n = g.FindNode(id);
            TEST(tn.c_str(), n && hasOutput(*n) && n->runtimeError.empty());
            } catch (...) { TEST(tn.c_str(), false); }
        }}

        // ---- Analysis Effects ----
        LOG("\n=== Analysis Effects ===");
        { const wchar_t* names[] = { L"Luminance Heatmap", L"Gamut Highlight", L"Nit Map", L"Waveform Monitor" };
          for (auto* name : names) {
            std::string tn = "Analysis_"; for (auto* p = name; *p; ++p) tn += (char)*p;
            try {
            ::ShaderLab::Graph::EffectGraph g; ::ShaderLab::Effects::SourceNodeFactory sf;
            auto srcN = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
            auto fxN = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(name));
            auto sId = g.AddNode(std::move(srcN)); auto fId = g.AddNode(std::move(fxN));
            g.Connect(sId, 0, fId, 0); ::ShaderLab::Rendering::GraphEvaluator ev; evaluate(g, sf, ev);
            auto* n = g.FindNode(fId);
            TEST(tn.c_str(), n && hasOutput(*n) && n->runtimeError.empty());
            } catch (...) { TEST(tn.c_str(), false); }
        }}
        // Split Comparison needs 2 inputs.
        { try {
            ::ShaderLab::Graph::EffectGraph g; ::ShaderLab::Effects::SourceNodeFactory sf;
            auto src1 = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
            auto src2 = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Color Checker"));
            auto split = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Split Comparison"));
            auto s1 = g.AddNode(std::move(src1)); auto s2 = g.AddNode(std::move(src2));
            auto spId = g.AddNode(std::move(split));
            g.Connect(s1, 0, spId, 0); g.Connect(s2, 0, spId, 1);
            ::ShaderLab::Rendering::GraphEvaluator ev; evaluate(g, sf, ev);
            auto* n = g.FindNode(spId);
            TEST("Analysis_Split Comparison", n && hasOutput(*n) && n->runtimeError.empty());
        } catch (...) { TEST("Analysis_Split Comparison", false); } }

        // ---- Built-in D2D Effects ----
        LOG("\n=== D2D Effects ===");
        { const wchar_t* names[] = { L"Gaussian Blur", L"Brightness", L"Contrast", L"Grayscale", L"Invert", L"Saturation", L"Hue Rotation", L"Exposure", L"Sharpen", L"Edge Detection", L"Crop" };
          for (auto* name : names) {
            std::string tn = "D2D_"; for (auto* p = name; *p; ++p) tn += (char)*p;
            try {
            ::ShaderLab::Graph::EffectGraph g; ::ShaderLab::Effects::SourceNodeFactory sf;
            auto srcN = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
            auto fxN = d2dReg.CreateNode(*d2dReg.FindByName(name));
            auto sId = g.AddNode(std::move(srcN)); auto fId = g.AddNode(std::move(fxN));
            g.Connect(sId, 0, fId, 0); ::ShaderLab::Rendering::GraphEvaluator ev; evaluate(g, sf, ev);
            auto* n = g.FindNode(fId);
            TEST(tn.c_str(), n && hasOutput(*n));
            } catch (...) { TEST(tn.c_str(), false); }
        }}

        // ---- Math Nodes ----
        LOG("\n=== Math Nodes ===");
        { struct MT { const wchar_t* n; float a, b, e; };
          MT tests[] = { {L"Add",3,7,10}, {L"Subtract",10,3,7}, {L"Multiply",4,5,20}, {L"Divide",20,4,5}, {L"Max",3,7,7}, {L"Min",3,7,3} };
          for (auto& t : tests) {
            ::ShaderLab::Graph::EffectGraph g; ::ShaderLab::Effects::SourceNodeFactory sf;
            auto pA = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Float Parameter"));
            auto pB = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Float Parameter"));
            auto m = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(t.n));
            auto aId = g.AddNode(std::move(pA)); auto bId = g.AddNode(std::move(pB));
            auto mId = g.AddNode(std::move(m));
            g.FindNode(aId)->properties[L"Value"] = t.a; g.FindNode(bId)->properties[L"Value"] = t.b;
            g.BindProperty(mId, L"A", aId, L"Value", 0); g.BindProperty(mId, L"B", bId, L"Value", 0);
            ::ShaderLab::Rendering::GraphEvaluator ev; evaluate(g, sf, ev);
            auto* mn = g.FindNode(mId); bool ok = false;
            if (mn) for (auto& f : mn->analysisOutput.fields) if (f.name == L"Result") { ok = std::abs(f.components[0] - t.e) < 0.01f; break; }
            std::string tn = "Math_"; for (auto* p = t.n; *p; ++p) tn += (char)*p;
            TEST(tn.c_str(), ok);
        }}

        // ---- Property Bindings ----
        LOG("\n=== Bindings ===");
        { ::ShaderLab::Graph::EffectGraph g; ::ShaderLab::Effects::SourceNodeFactory sf;
          auto src = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
          auto param = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Float Parameter"));
          auto blur = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));
          auto sId = g.AddNode(std::move(src)); auto pId = g.AddNode(std::move(param)); auto bId = g.AddNode(std::move(blur));
          g.Connect(sId, 0, bId, 0); g.FindNode(pId)->properties[L"Value"] = 5.0f;
          TEST("BindProperty", g.BindProperty(bId, L"StandardDeviation", pId, L"Value", 0).empty());
          ::ShaderLab::Rendering::GraphEvaluator ev; evaluate(g, sf, ev);
          auto* b = g.FindNode(bId); auto it = b->properties.find(L"StandardDeviation");
          bool ok = it != b->properties.end(); if (ok) { auto* f = std::get_if<float>(&it->second); ok = f && *f >= 4.5f; }
          TEST("BindingPropagates", ok);
        }

        // ---- Clock ----
        LOG("\n=== Clock ===");
        { auto* desc = registry.FindByName(L"Clock");
          TEST("ClockExists", desc != nullptr);
          if (desc) { auto n = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            TEST("IsClock", n.isClock);
            TEST("HasAutoDuration", n.properties.count(L"AutoDuration") > 0);
            TEST("HasTimeOutput", !n.customEffect->analysisFields.empty() && n.customEffect->analysisFields[0].name == L"Time");
        }}

        // ---- Shader Compilation ----
        LOG("\n=== Shader Compilation ===");
        { auto r = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
            "Texture2D S:register(t0);float4 main(float4 p:SV_POSITION,float4 u:TEXCOORD0):SV_TARGET{return S.Load(int3(u.xy,0));}",
            "t.hlsl", "main", "ps_5_0");
          TEST("ValidPS", r.succeeded);
          auto b = ::ShaderLab::Effects::ShaderCompiler::CompileFromString("bad!", "b.hlsl", "main", "ps_5_0");
          TEST("InvalidFails", !b.succeeded);
          auto c = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
            "RWTexture2D<float4> o:register(u0);[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){o[id.xy]=float4(1,0,0,1);}",
            "t.hlsl", "main", "cs_5_0");
          TEST("ValidCS", c.succeeded);
        }

        // ---- Effect Chain ----
        LOG("\n=== Effect Chain ===");
        { ::ShaderLab::Graph::EffectGraph g; ::ShaderLab::Effects::SourceNodeFactory sf;
          auto src = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
          auto blur = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));
          auto inv = d2dReg.CreateNode(*d2dReg.FindByName(L"Invert"));
          auto sId = g.AddNode(std::move(src)); auto bId = g.AddNode(std::move(blur)); auto iId = g.AddNode(std::move(inv));
          g.Connect(sId, 0, bId, 0); g.Connect(bId, 0, iId, 0);
          ::ShaderLab::Rendering::GraphEvaluator ev; evaluate(g, sf, ev);
          TEST("ThreeNodeChain", g.FindNode(iId) && hasOutput(*g.FindNode(iId)));
        }

        // ---- Summary ----
        LOG("\n========================================");
        if (failed == 0) LOG("ALL %d TESTS PASSED", passed);
        else LOG("%d PASSED, %d FAILED out of %d", passed, failed, passed + failed);
        LOG("========================================");

        if (rf) fclose(rf);
        return failed;
    }
}
