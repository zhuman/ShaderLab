#include "pch.h"
#include "RenderEngine.h"
#include "D3DDefinitions.h"

#include <microsoft.ui.xaml.media.dxinterop.h>

namespace ShaderLab::Rendering
{
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void RenderEngine::Initialize(
        HWND hwnd,
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel,
        const PipelineFormat& format,
        DevicePreference devicePref)
    {
        m_hwnd = hwnd;
        m_panel = panel;
        m_format = format;

        CreateDeviceResources(devicePref);
        CreateSwapChain(panel);
        ConfigureSwapChainColorSpace();
        CreateRenderTarget();
    }

    void RenderEngine::Shutdown()
    {
        ReleaseRenderTarget();

        // Clear the swap chain reference from the XAML panel BEFORE
        // releasing it, otherwise the compositor crashes accessing
        // a dangling swap chain.
        if (m_panel)
        {
            try
            {
                auto panelNative = m_panel.as<ISwapChainPanelNative>();
                if (panelNative) panelNative->SetSwapChain(nullptr);
            }
            catch (...) {}
        }

        m_swapChain = nullptr;
        m_d2dDeviceContext = nullptr;
        m_d2dDevice = nullptr;
        m_d2dFactory = nullptr;
        m_d3dContext = nullptr;
        m_d3dDevice = nullptr;
        m_dxgiFactory = nullptr;
        m_panel = nullptr;
    }

    // -----------------------------------------------------------------------
    // Device creation
    // -----------------------------------------------------------------------

    void RenderEngine::CreateDeviceResources(DevicePreference devicePref)
    {
        // --- DXGI Factory ---
        UINT factoryFlags = 0;
        winrt::check_hresult(
            CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(m_dxgiFactory.put())));

        // --- D3D11 Device ---
        UINT d3dFlags = DefaultD3D11DeviceFlags; // required for D2D interop
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        winrt::com_ptr<ID3D11Device> baseDevice;
        winrt::com_ptr<ID3D11DeviceContext> baseContext;
        HRESULT hr = E_FAIL;

        if (devicePref == DevicePreference::Adapter)
        {
            // Create on a specific adapter identified by LUID.
            winrt::com_ptr<IDXGIAdapter1> adapter;
            for (UINT i = 0; m_dxgiFactory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (desc.AdapterLuid.LowPart == m_preferredAdapterLuid.LowPart &&
                    desc.AdapterLuid.HighPart == m_preferredAdapterLuid.HighPart)
                {
                    hr = D3D11CreateDevice(
                        adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, d3dFlags,
                        featureLevels, ARRAYSIZE(featureLevels),
                        D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseContext.put());
                    break;
                }
                adapter = nullptr;
            }
        }

        if (FAILED(hr))
        {
            D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_HARDWARE;
            if (devicePref == DevicePreference::Warp)
                driverType = D3D_DRIVER_TYPE_WARP;

            hr = D3D11CreateDevice(
                nullptr, driverType, nullptr, d3dFlags,
                featureLevels, ARRAYSIZE(featureLevels),
                D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseContext.put());

            // Default: fallback to WARP if hardware fails.
            if (FAILED(hr) && (devicePref == DevicePreference::Default || devicePref == DevicePreference::Adapter))
            {
                hr = D3D11CreateDevice(
                    nullptr, D3D_DRIVER_TYPE_WARP, nullptr, d3dFlags,
                    featureLevels, ARRAYSIZE(featureLevels),
                    D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseContext.put());
            }
        }
        winrt::check_hresult(hr);

        m_d3dDevice = baseDevice.as<ID3D11Device5>();
        m_d3dContext = baseContext.as<ID3D11DeviceContext4>();

        // Enable multithread protection for DXVA2 video decode on background threads.
        {
            winrt::com_ptr<ID3D10Multithread> mt;
            m_d3dDevice.as(mt);
            if (mt) mt->SetMultithreadProtected(TRUE);
        }

        // Query adapter info (GPU name, WARP detection).
        {
            winrt::com_ptr<IDXGIDevice> dxgiDev;
            m_d3dDevice.as(dxgiDev);
            if (dxgiDev)
            {
                winrt::com_ptr<IDXGIAdapter> adapter;
                dxgiDev->GetAdapter(adapter.put());
                if (adapter)
                {
                    DXGI_ADAPTER_DESC desc{};
                    adapter->GetDesc(&desc);
                    m_adapterName = desc.Description;
                    // WARP adapter has VendorId 0x1414 (Microsoft) and DeviceId 0x8C
                    m_isWarp = (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C);
                }
            }
        }

        // --- D2D Factory ---
        D2D1_FACTORY_OPTIONS d2dOptions{};
#ifdef _DEBUG
        d2dOptions.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
        winrt::check_hresult(
            D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory7),
                &d2dOptions,
                m_d2dFactory.put_void()));

        // --- D2D Device (from D3D11 device via DXGI) ---
        winrt::com_ptr<IDXGIDevice4> dxgiDevice;
        dxgiDevice = m_d3dDevice.as<IDXGIDevice4>();

        winrt::com_ptr<ID2D1Device> baseD2DDevice;
        winrt::check_hresult(
            m_d2dFactory->CreateDevice(dxgiDevice.get(), baseD2DDevice.put()));
        m_d2dDevice = baseD2DDevice.as<ID2D1Device6>();

        // --- D2D Device Context ---
        winrt::com_ptr<ID2D1DeviceContext> baseDC;
        winrt::check_hresult(
            m_d2dDevice->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                baseDC.put()));
        m_d2dDeviceContext = baseDC.as<ID2D1DeviceContext5>();
    }

    // -----------------------------------------------------------------------
    // Swap chain
    // -----------------------------------------------------------------------

    void RenderEngine::CreateSwapChain(
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel)
    {
        // Query the panel's current pixel size.
        auto scale = static_cast<double>(panel.CompositionScaleX());
        m_width = static_cast<uint32_t>((std::max)(1.0, panel.ActualWidth() * scale));
        m_height = static_cast<uint32_t>((std::max)(1.0, panel.ActualHeight() * scale));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.Format = m_format.dxgiFormat;
        desc.Stereo = FALSE;
        desc.SampleDesc = { 1, 0 };
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Flags = 0;

        winrt::com_ptr<IDXGISwapChain1> swapChain1;
        winrt::check_hresult(
            m_dxgiFactory->CreateSwapChainForComposition(
                m_d3dDevice.as<IDXGIDevice>().get(),
                &desc,
                nullptr,
                swapChain1.put()));

        m_swapChain = swapChain1.as<IDXGISwapChain3>();

        // Bind to SwapChainPanel via ISwapChainPanelNative.
        auto panelNative = panel.as<ISwapChainPanelNative>();
        winrt::check_hresult(panelNative->SetSwapChain(m_swapChain.get()));
    }

    void RenderEngine::ConfigureSwapChainColorSpace()
    {
        if (!m_swapChain)
            return;

        // SetColorSpace1 tells DXGI how to interpret the pixel data.
        winrt::check_hresult(
            m_swapChain->SetColorSpace1(m_format.colorSpace));
    }

    // -----------------------------------------------------------------------
    // Render target (D2D bitmap backed by swap chain back buffer)
    // -----------------------------------------------------------------------

    void RenderEngine::CreateRenderTarget()
    {
        if (!m_swapChain || !m_d2dDeviceContext)
            return;

        winrt::com_ptr<IDXGISurface2> backBuffer;
        winrt::check_hresult(
            m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())));

        D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(m_format.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));

        winrt::com_ptr<ID2D1Bitmap1> targetBitmap;
        winrt::check_hresult(
            m_d2dDeviceContext->CreateBitmapFromDxgiSurface(
                backBuffer.get(),
                &bitmapProps,
                targetBitmap.put()));

        m_renderTarget = std::move(targetBitmap);
        m_d2dDeviceContext->SetTarget(m_renderTarget.get());

        // Set DPI to match the panel's composition scale.
        float dpi = 96.0f;
        if (m_panel)
        {
            dpi = 96.0f * m_panel.CompositionScaleX();
        }
        m_d2dDeviceContext->SetDpi(dpi, dpi);
    }

    void RenderEngine::ReleaseRenderTarget()
    {
        if (m_d2dDeviceContext)
            m_d2dDeviceContext->SetTarget(nullptr);
        m_renderTarget = nullptr;
    }

    // -----------------------------------------------------------------------
    // Resize
    // -----------------------------------------------------------------------

    void RenderEngine::Resize(uint32_t widthPixels, uint32_t heightPixels)
    {
        if (!m_swapChain)
            return;

        if (widthPixels == 0 || heightPixels == 0)
            return;

        if (widthPixels == m_width && heightPixels == m_height)
            return;

        m_width = widthPixels;
        m_height = heightPixels;

        // Must release the render target before resizing buffers.
        ReleaseRenderTarget();

        // Flush any pending D3D work.
        m_d3dContext->Flush();

        winrt::check_hresult(
            m_swapChain->ResizeBuffers(
                0,          // keep current count
                m_width,
                m_height,
                DXGI_FORMAT_UNKNOWN,  // keep current format
                0));

        CreateRenderTarget();
    }

    // -----------------------------------------------------------------------
    // Pipeline format change
    // -----------------------------------------------------------------------

    void RenderEngine::SetPipelineFormat(const PipelineFormat& format)
    {
        if (m_format == format)
            return;

        m_format = format;

        if (!m_swapChain)
            return;

        // Must release RT before resizing/reformatting.
        ReleaseRenderTarget();
        m_d3dContext->Flush();

        // ResizeBuffers with new format.
        winrt::check_hresult(
            m_swapChain->ResizeBuffers(
                0,
                m_width,
                m_height,
                m_format.dxgiFormat,
                0));

        ConfigureSwapChainColorSpace();
        CreateRenderTarget();
    }

    // -----------------------------------------------------------------------
    // Per-frame rendering
    // -----------------------------------------------------------------------

    ID2D1DeviceContext5* RenderEngine::BeginDraw()
    {
        m_d2dDeviceContext->BeginDraw();
        return m_d2dDeviceContext.get();
    }

    void RenderEngine::EndDraw()
    {
        HRESULT hr = m_d2dDeviceContext->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
        {
            // Device lost — caller should recreate resources.
            ReleaseRenderTarget();
        }
        else if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[Render] EndDraw error hr=0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
        }
    }

    void RenderEngine::Present(bool vsync)
    {
        DXGI_PRESENT_PARAMETERS params{};
        winrt::check_hresult(
            m_swapChain->Present1(vsync ? 1 : 0, 0, &params));
    }

    // -----------------------------------------------------------------------
    // Adapter enumeration
    // -----------------------------------------------------------------------

    std::vector<AdapterInfo> RenderEngine::EnumerateAdapters()
    {
        std::vector<AdapterInfo> result;

        winrt::com_ptr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put()))))
            return result;

        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            // Skip software adapters other than WARP.
            bool isWarp = (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !isWarp)
            {
                adapter = nullptr;
                continue;
            }

            AdapterInfo info;
            info.name = desc.Description;
            info.vendorId = desc.VendorId;
            info.deviceId = desc.DeviceId;
            info.dedicatedVideoMemoryMB = static_cast<size_t>(desc.DedicatedVideoMemory / (1024 * 1024));
            info.luid = desc.AdapterLuid;
            info.isWarp = isWarp;
            result.push_back(std::move(info));

            adapter = nullptr;
        }

        // Always add WARP as an option if not already present.
        bool hasWarp = false;
        for (const auto& a : result)
            if (a.isWarp) { hasWarp = true; break; }
        if (!hasWarp)
        {
            AdapterInfo warp;
            warp.name = L"Microsoft Basic Render Driver (WARP)";
            warp.isWarp = true;
            result.push_back(std::move(warp));
        }

        return result;
    }

    // -----------------------------------------------------------------------
    // Reinitialize on a different adapter
    // -----------------------------------------------------------------------

    void RenderEngine::Reinitialize(DevicePreference devicePref, LUID adapterLuid)
    {
        m_preferredAdapterLuid = adapterLuid;

        // Save state that Shutdown clears.
        auto savedPanel = m_panel;
        auto savedHwnd = m_hwnd;
        auto savedFormat = m_format;
        auto savedWidth = m_width;
        auto savedHeight = m_height;

        Shutdown();

        m_panel = savedPanel;
        m_hwnd = savedHwnd;
        m_format = savedFormat;

        // Recreate on the new adapter.
        CreateDeviceResources(devicePref);
        CreateSwapChain(m_panel);
        ConfigureSwapChainColorSpace();
        CreateRenderTarget();

        if (savedWidth > 0 && savedHeight > 0)
            Resize(savedWidth, savedHeight);
    }
}
