#pragma once

#include "pch.h"
#include "../Rendering/PipelineFormat.h"

namespace ShaderLab::Controls
{
    // A lightweight output window that presents a single graph node's output
    // in its own OS window with pan/zoom controls and FPS ticker.
    class OutputWindow
    {
    public:
        OutputWindow() = default;
        ~OutputWindow();

        OutputWindow(const OutputWindow&) = delete;
        OutputWindow& operator=(const OutputWindow&) = delete;

        void Create(
            ID3D11Device5* d3dDevice,
            ID2D1DeviceContext5* dc,
            IDXGIFactory7* dxgiFactory,
            uint32_t nodeId,
            const std::wstring& nodeName,
            const Rendering::PipelineFormat& format);

        void Present(ID2D1DeviceContext5* dc, ID2D1Image* image);
        void Close();

        bool IsOpen() const { return m_isOpen; }
        bool IsReady() const { return m_swapChain != nullptr; }
        uint32_t NodeId() const { return m_nodeId; }
        void SetTitle(const std::wstring& title);
        void SetTimingText(const std::wstring& text);

    private:
        void CreateSwapChain();
        void CreateRenderTarget();
        void ReleaseRenderTarget();
        void ResizeForPanelSize(double widthDips, double heightDips);
        void UpdatePanelScale();
        void OnPanelSizeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
        void FitToView(ID2D1DeviceContext5* dc, ID2D1Image* image);
        winrt::fire_and_forget SaveImageAsync();

        // Shared (non-owning).
        ID3D11Device5* m_d3dDevice{ nullptr };
        IDXGIFactory7* m_dxgiFactory{ nullptr };
        ID2D1DeviceContext5* m_dc{ nullptr };

        // Owned per-window resources.
        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_fpsText{ nullptr };
        winrt::com_ptr<IDXGISwapChain3> m_swapChain;
        winrt::com_ptr<ID2D1Bitmap1> m_renderTarget;

        // Last rendered image (non-owning, for save).
        ID2D1Image* m_lastImage{ nullptr };
        std::wstring m_nodeName;

        Rendering::PipelineFormat m_format{ Rendering::FormatScRgbFP16 };
        uint32_t m_nodeId{ 0 };
        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };
        bool m_isOpen{ false };
        bool m_needsResize{ false };
        bool m_needsFit{ true };
        bool m_autoFit{ true };  // Auto-fit until user zooms/pans

        // Pan / zoom state.
        float m_zoom{ 1.0f };
        float m_panX{ 0.0f };
        float m_panY{ 0.0f };
        bool m_isPanning{ false };
        float m_panStartX{ 0.0f };
        float m_panStartY{ 0.0f };
        float m_panOriginX{ 0.0f };
        float m_panOriginY{ 0.0f };

        // FPS counter.
        uint32_t m_frameCount{ 0 };
        std::chrono::steady_clock::time_point m_fpsTime;
        std::wstring m_timingText;

        // Event tokens for cleanup.
        winrt::event_token m_sizeChangedToken{};
        winrt::event_token m_compositionScaleChangedToken{};
        winrt::event_token m_closedToken{};
    };
}
