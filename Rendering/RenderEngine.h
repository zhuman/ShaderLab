#pragma once

#include "pch.h"
#include "PipelineFormat.h"
#include "DisplayMonitor.h"

namespace ShaderLab::Rendering
{
    // Device creation preference.
    enum class DevicePreference
    {
        Default,    // Hardware, fallback to WARP on failure
        Hardware,   // Force hardware GPU (fail if unavailable)
        Warp,       // Force WARP software renderer
        Adapter,    // Use specific adapter (set via SetPreferredAdapter)
    };

    struct AdapterInfo {
        std::wstring name;
        uint32_t vendorId{};
        uint32_t deviceId{};
        size_t dedicatedVideoMemoryMB{};
        LUID luid{};
        bool isWarp{};
    };

    // Core rendering engine: owns D3D11 device, D2D1 device context, and the
    // DXGI swap chain bound to a WinUI 3 SwapChainPanel.
    //
    // Lifecycle:
    //   1. Construct
    //   2. Initialize(hwnd, swapChainPanel) — creates device stack + swap chain
    //   3. Resize(w, h) — called on SizeChanged
    //   4. BeginDraw / EndDraw / Present — per-frame rendering
    //   5. SetPipelineFormat — switch format, recreates swap chain
    //
    // The engine does NOT own the graph evaluator — that lives at a higher level
    // and calls BeginDraw/EndDraw to render into the D2D device context.
    class RenderEngine
    {
    public:
        RenderEngine() = default;
        ~RenderEngine() = default;

        RenderEngine(const RenderEngine&) = delete;
        RenderEngine& operator=(const RenderEngine&) = delete;

        // Create D3D11 device, D2D factory/device/context, swap chain on the panel.
        void Initialize(HWND hwnd,
                        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel,
                        const PipelineFormat& format = FormatScRgbFP16,
                        DevicePreference devicePref = DevicePreference::Default);

        // Release all GPU resources.
        void Shutdown();

        // Respond to SwapChainPanel size changes.
        void Resize(uint32_t widthPixels, uint32_t heightPixels);

        // Switch the entire pipeline to a new format (recreates swap chain + RT).
        void SetPipelineFormat(const PipelineFormat& format);

        // --- Per-frame rendering ---

        // Begins a D2D drawing session on the back-buffer render target.
        // Returns the D2D device context to draw into.
        ID2D1DeviceContext5* BeginDraw();

        // Ends the D2D drawing session.
        void EndDraw();

        // Presents the back buffer to the swap chain.
        void Present(bool vsync = true);

        // --- Accessors ---

        bool IsInitialized() const { return m_d3dDevice != nullptr; }

        ID3D11Device5*          D3DDevice()       const { return m_d3dDevice.get(); }
        ID3D11DeviceContext4*   D3DContext()       const { return m_d3dContext.get(); }
        ID2D1Factory7*          D2DFactory()       const { return m_d2dFactory.get(); }
        ID2D1Device6*           D2DDevice()        const { return m_d2dDevice.get(); }
        // Default D2D context. Lives on whichever thread last touched it
        // historically (UI thread). After P7, this is the UI-thread context
        // for capture / pixel-inspector / source-prep paths that still run
        // on UI. The render worker uses RenderD2DContext() instead.
        ID2D1DeviceContext5*    D2DDeviceContext()  const { return m_d2dDeviceContext.get(); }
        // Dedicated D2D context for the render-worker thread. Created from
        // the same multi-threaded D2D device as D2DDeviceContext, so
        // resources interop, but state (target/transform/dpi) is
        // independent. This gives the worker its own BeginDraw/EndDraw
        // session so concurrent UI-thread BeginDraw on the default context
        // can't put the device into error state mid-draw.
        ID2D1DeviceContext5*    RenderD2DContext()  const { return m_renderD2dContext.get(); }
        IDXGISwapChain3*        SwapChain()        const { return m_swapChain.get(); }
        IDXGIFactory7*          DXGIFactory()      const { return m_dxgiFactory.get(); }

        const PipelineFormat&   ActiveFormat()     const { return m_format; }
        uint32_t                BackBufferWidth()  const { return m_width; }
        uint32_t                BackBufferHeight() const { return m_height; }

        // GPU / adapter info.
        const std::wstring&     AdapterName()      const { return m_adapterName; }
        bool                    IsWarp()           const { return m_isWarp; }
        void SetPreferredAdapterLuid(LUID luid) { m_preferredAdapterLuid = luid; }

        // Enumerate available DXGI adapters.
        static std::vector<AdapterInfo> EnumerateAdapters();

        // Reinitialize on a different adapter. Tears down all GPU resources
        // and recreates the device stack on the specified adapter.
        // Caller must release all device-dependent resources before calling.
        void Reinitialize(DevicePreference devicePref, LUID adapterLuid = {});

        // ---- Offscreen render target (Phase 7 render-thread split) -------
        //
        // Render thread renders into a pair of double-buffered D2D bitmaps
        // (each backed by its own D3D11 texture); UI thread blits the
        // most-recently-published buffer into the SwapChainPanel-bound
        // swap chain. This decouples graph evaluation from the XAML
        // SwapChainPanel apartment-affinity rules.
        //
        // EnsureOffscreenTargets(w, h) (re)creates two textures + render-
        // side D2D bitmap wrappers when the desired size differs from the
        // current size. Caller (UI thread, on resize) is responsible for
        // also recreating any UI-side D2D bitmap wrappers of the same
        // textures via the OffscreenTextureForUi(idx) accessor.
        bool EnsureOffscreenTargets(uint32_t width, uint32_t height);
        ID3D11Texture2D* OffscreenTexture(uint32_t idx) const
        {
            return idx < 2 ? m_offscreenTexture[idx].get() : nullptr;
        }
        ID2D1Bitmap1* OffscreenRenderBitmap(uint32_t idx) const
        {
            return idx < 2 ? m_offscreenRenderBitmap[idx].get() : nullptr;
        }
        uint32_t OffscreenWidth()  const { return m_offscreenWidth; }
        uint32_t OffscreenHeight() const { return m_offscreenHeight; }
        void ReleaseOffscreenTargets();

    private:
        void CreateDeviceResources(DevicePreference devicePref);
        void CreateSwapChain(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel);
        void ConfigureSwapChainColorSpace();
        void CreateRenderTarget();
        void ReleaseRenderTarget();

        // Device stack
        winrt::com_ptr<IDXGIFactory7>           m_dxgiFactory;
        winrt::com_ptr<ID3D11Device5>           m_d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext4>     m_d3dContext;
        winrt::com_ptr<ID2D1Factory7>           m_d2dFactory;
        winrt::com_ptr<ID2D1Device6>            m_d2dDevice;
        winrt::com_ptr<ID2D1DeviceContext5>      m_d2dDeviceContext;
        winrt::com_ptr<ID2D1DeviceContext5>      m_renderD2dContext; // P7: dedicated render-thread context

        // Swap chain
        winrt::com_ptr<IDXGISwapChain3>         m_swapChain;
        winrt::com_ptr<ID2D1Bitmap1>            m_renderTarget;

        // Offscreen target pair (Phase 7). Render thread renders into one,
        // UI thread blits the other into m_swapChain. Index swap is managed
        // by MainWindow (atomic publish protocol).
        winrt::com_ptr<ID3D11Texture2D>         m_offscreenTexture[2];
        winrt::com_ptr<ID2D1Bitmap1>            m_offscreenRenderBitmap[2];
        uint32_t                                m_offscreenWidth{ 0 };
        uint32_t                                m_offscreenHeight{ 0 };

        // State
        PipelineFormat  m_format{ FormatScRgbFP16 };
        uint32_t        m_width{ 0 };
        uint32_t        m_height{ 0 };
        HWND            m_hwnd{ nullptr };
        std::wstring    m_adapterName;
        bool            m_isWarp{ false };
        LUID            m_preferredAdapterLuid{};

        // Keep panel reference for swap chain recreation on format change.
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
    };
}
