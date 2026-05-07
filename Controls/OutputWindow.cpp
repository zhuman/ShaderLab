#include "pch.h"
#include "OutputWindow.h"

#include <microsoft.ui.xaml.media.dxinterop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Input.h>

namespace ShaderLab::Controls
{
    OutputWindow::~OutputWindow()
    {
        Close();
    }

    void OutputWindow::Create(
        ID3D11Device5* d3dDevice,
        ID2D1DeviceContext5* dc,
        IDXGIFactory7* dxgiFactory,
        uint32_t nodeId,
        const std::wstring& nodeName,
        const Rendering::PipelineFormat& format)
    {
        m_d3dDevice = d3dDevice;
        m_dc = dc;
        m_dxgiFactory = dxgiFactory;
        m_nodeId = nodeId;
        m_format = format;
        m_fpsTime = std::chrono::steady_clock::now();

        try
        {
            namespace MUX = winrt::Microsoft::UI::Xaml;
            namespace MUXC = MUX::Controls;

            m_window = MUX::Window();
            m_window.Title(winrt::hstring(nodeName));

            // Build layout: SwapChainPanel + status bar.
            auto rootGrid = MUXC::Grid();
            auto row0 = MUX::GridLength{ 1.0, MUX::GridUnitType::Star };
            auto row1 = MUX::GridLength{ 1.0, MUX::GridUnitType::Auto };
            auto rowDef0 = MUXC::RowDefinition();
            rowDef0.Height(row0);
            auto rowDef1 = MUXC::RowDefinition();
            rowDef1.Height(row1);
            rootGrid.RowDefinitions().Append(rowDef0);
            rootGrid.RowDefinitions().Append(rowDef1);

            m_panel = MUXC::SwapChainPanel();
            MUXC::Grid::SetRow(m_panel, 0);
            rootGrid.Children().Append(m_panel);

            // Status bar with FPS.
            auto statusBar = MUXC::Grid();
            statusBar.Padding(MUX::ThicknessHelper::FromLengths(8, 2, 8, 2));
            statusBar.Background(MUX::Media::SolidColorBrush(
                winrt::Windows::UI::Color{ 255, 32, 32, 32 }));
            MUXC::Grid::SetRow(statusBar, 1);

            m_fpsText = MUXC::TextBlock();
            m_fpsText.Text(L"0 FPS");
            m_fpsText.Foreground(MUX::Media::SolidColorBrush(
                winrt::Windows::UI::Color{ 255, 180, 180, 180 }));
            m_fpsText.FontSize(11);
            m_fpsText.VerticalAlignment(MUX::VerticalAlignment::Center);
            statusBar.Children().Append(m_fpsText);

            // Save button in status bar (right-aligned)
            auto saveBtn = MUXC::Button();
            saveBtn.Content(winrt::box_value(L"Save"));
            saveBtn.FontSize(11);
            saveBtn.Padding(MUX::ThicknessHelper::FromLengths(8, 2, 8, 2));
            saveBtn.HorizontalAlignment(MUX::HorizontalAlignment::Right);
            saveBtn.Click([this](auto&&, auto&&) { SaveImageAsync(); });
            statusBar.Children().Append(saveBtn);

            rootGrid.Children().Append(statusBar);

            m_window.Content(rootGrid);

            // Handle window close.
            m_closedToken = m_window.Closed([this](auto&&, auto&&)
            {
                m_isOpen = false;
            });

            // Pointer events for pan/zoom.
            m_panel.PointerWheelChanged([this](auto&&, MUX::Input::PointerRoutedEventArgs const& args)
            {
                auto point = args.GetCurrentPoint(m_panel);
                int delta = point.Properties().MouseWheelDelta();
                float cursorX = static_cast<float>(point.Position().X);
                float cursorY = static_cast<float>(point.Position().Y);

                float factor = (delta > 0) ? 1.1f : (1.0f / 1.1f);
                float newZoom = std::clamp(m_zoom * factor, 0.01f, 100.0f);

                m_panX = cursorX - (cursorX - m_panX) * (newZoom / m_zoom);
                m_panY = cursorY - (cursorY - m_panY) * (newZoom / m_zoom);
                m_zoom = newZoom;
                m_autoFit = false;
                args.Handled(true);
            });

            m_panel.PointerPressed([this](auto&&, MUX::Input::PointerRoutedEventArgs const& args)
            {
                auto point = args.GetCurrentPoint(m_panel);
                if (point.Properties().IsMiddleButtonPressed() ||
                    point.Properties().IsRightButtonPressed())
                {
                    m_isPanning = true;
                    m_autoFit = false;
                    m_panStartX = static_cast<float>(point.Position().X);
                    m_panStartY = static_cast<float>(point.Position().Y);
                    m_panOriginX = m_panX;
                    m_panOriginY = m_panY;
                    m_panel.as<MUX::UIElement>().CapturePointer(args.Pointer());
                    args.Handled(true);
                }
            });

            m_panel.PointerMoved([this](auto&&, MUX::Input::PointerRoutedEventArgs const& args)
            {
                if (m_isPanning)
                {
                    auto point = args.GetCurrentPoint(m_panel);
                    float dx = static_cast<float>(point.Position().X) - m_panStartX;
                    float dy = static_cast<float>(point.Position().Y) - m_panStartY;
                    m_panX = m_panOriginX + dx;
                    m_panY = m_panOriginY + dy;
                    args.Handled(true);
                }
            });

            m_panel.PointerReleased([this](auto&&, MUX::Input::PointerRoutedEventArgs const& args)
            {
                if (m_isPanning)
                {
                    m_isPanning = false;
                    m_panel.as<MUX::UIElement>().ReleasePointerCapture(args.Pointer());
                    args.Handled(true);
                }
            });

            // Double-click to fit.
            m_panel.DoubleTapped([this](auto&&, auto&&)
            {
                m_needsFit = true;
                m_autoFit = true;
            });

            // Wait for panel Loaded before creating DXGI resources.
            m_panel.Loaded([this](auto&&, auto&&)
            {
                try
                {
                    ResizeForPanelSize(m_panel.ActualWidth(), m_panel.ActualHeight());

                    m_sizeChangedToken = m_panel.SizeChanged(
                        { this, &OutputWindow::OnPanelSizeChanged });
                    m_compositionScaleChangedToken = m_panel.CompositionScaleChanged([this](auto&&, auto&&)
                    {
                        UpdatePanelScale();
                        ResizeForPanelSize(m_panel.ActualWidth(), m_panel.ActualHeight());
                    });
                }
                catch (...)
                {
                    OutputDebugStringW(L"[OutputWindow] Failed to create swap chain on Loaded\n");
                }
            });

            m_window.AppWindow().Resize(winrt::Windows::Graphics::SizeInt32{ 800, 600 });
            m_window.Activate();
            m_isOpen = true;
        }
        catch (const winrt::hresult_error& ex)
        {
            OutputDebugStringW(std::format(L"[OutputWindow] Create failed: {}\n",
                std::wstring_view(ex.message())).c_str());
            m_isOpen = false;
        }
    }

    void OutputWindow::FitToView(ID2D1DeviceContext5* dc, ID2D1Image* image)
    {
        if (!image || m_width == 0 || m_height == 0)
            return;

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(image, &bounds);
        float imgW = bounds.right - bounds.left;
        float imgH = bounds.bottom - bounds.top;
        if (imgW <= 0 || imgH <= 0)
            return;

        float dpi = 96.0f * m_panel.CompositionScaleX();
        float vpW = static_cast<float>(m_width) / (dpi / 96.0f);
        float vpH = static_cast<float>(m_height) / (dpi / 96.0f);
        m_zoom = (std::min)(vpW / imgW, vpH / imgH);
        m_panX = (vpW - imgW * m_zoom) * 0.5f;
        m_panY = (vpH - imgH * m_zoom) * 0.5f;
    }

    void OutputWindow::Present(ID2D1DeviceContext5* dc, ID2D1Image* image)
    {
        if (!m_isOpen || !dc || !m_swapChain || !m_renderTarget)
            return;

        try
        {
            // Handle pending resize.
            if (m_needsResize)
            {
                m_needsResize = false;
                ReleaseRenderTarget();

                if (m_width > 0 && m_height > 0)
                {
                    HRESULT hr = m_swapChain->ResizeBuffers(0, m_width, m_height,
                        DXGI_FORMAT_UNKNOWN, 0);
                    if (SUCCEEDED(hr))
                        CreateRenderTarget();
                }

                if (!m_renderTarget)
                    return;

                m_needsFit = true;
            }

            // Auto-fit: scale to fill window until user manually pans/zooms.
            if ((m_needsFit || m_autoFit) && image)
            {
                m_needsFit = false;
                FitToView(dc, image);
            }

            // Store for save.
            m_lastImage = image;

            // Save the main window's render target and state.
            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            float oldDpiX, oldDpiY;
            dc->GetDpi(&oldDpiX, &oldDpiY);
            auto oldTransform = D2D1::Matrix3x2F::Identity();
            dc->GetTransform(&oldTransform);

            // Set our render target.
            dc->SetTarget(m_renderTarget.get());
            float dpi = 96.0f * m_panel.CompositionScaleX();
            dc->SetDpi(dpi, dpi);

            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));

            if (image)
            {
                // Apply pan/zoom transform.
                dc->SetTransform(
                    D2D1::Matrix3x2F::Scale(m_zoom, m_zoom) *
                    D2D1::Matrix3x2F::Translation(m_panX, m_panY));
                dc->DrawImage(
                    image,
                    m_zoom < 1.0f
                        ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                        : D2D1_INTERPOLATION_MODE_LINEAR);
            }
            else
            {
                // Draw "No Input" indicator for broken/disconnected graph chain.
                winrt::com_ptr<IDWriteFactory> dwFactory;
                DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory), dwFactory.as<IUnknown>().put());
                if (dwFactory)
                {
                    winrt::com_ptr<IDWriteTextFormat> fmt;
                    dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
                        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", fmt.put());
                    if (fmt)
                    {
                        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        winrt::com_ptr<ID2D1SolidColorBrush> brush;
                        dc->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.8f), brush.put());
                        if (brush)
                        {
                            D2D1_SIZE_F sz = m_renderTarget->GetSize();
                            dc->DrawText(L"No Input", 8, fmt.get(),
                                D2D1::RectF(0, 0, sz.width, sz.height), brush.get());
                        }
                    }
                }
            }

            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            HRESULT hr = dc->EndDraw();
            if (FAILED(hr))
            {
                OutputDebugStringW(std::format(L"[OutputWindow] EndDraw error hr=0x{:08X}\n",
                    static_cast<uint32_t>(hr)).c_str());
            }

            DXGI_PRESENT_PARAMETERS params{};
            m_swapChain->Present1(1, 0, &params);

            // Restore the main window's render target and state.
            dc->SetTarget(oldTarget.get());
            dc->SetDpi(oldDpiX, oldDpiY);
            dc->SetTransform(&oldTransform);

            // Update FPS counter.
            m_frameCount++;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<double>(now - m_fpsTime).count();
            if (elapsed >= 1.0)
            {
                uint32_t fps = static_cast<uint32_t>(m_frameCount / elapsed);
                if (m_timingText.empty())
                    m_fpsText.Text(std::to_wstring(fps) + L" FPS");
                else
                    m_fpsText.Text(std::format(L"{} FPS | {}", fps, m_timingText));
                m_frameCount = 0;
                m_fpsTime = now;
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            OutputDebugStringW(std::format(L"[OutputWindow] Present failed: {}\n",
                std::wstring_view(ex.message())).c_str());
        }
        catch (...) {}
    }

    void OutputWindow::Close()
    {
        m_isOpen = false;
        ReleaseRenderTarget();
        m_swapChain = nullptr;

        if (m_window)
        {
            try
            {
                if (m_panel && m_sizeChangedToken.value != 0)
                    m_panel.SizeChanged(m_sizeChangedToken);
                if (m_panel && m_compositionScaleChangedToken.value != 0)
                    m_panel.CompositionScaleChanged(m_compositionScaleChangedToken);
                m_window.Close();
            }
            catch (...) {}
            m_window = nullptr;
        }

        m_panel = nullptr;
        m_fpsText = nullptr;
    }

    void OutputWindow::SetTitle(const std::wstring& title)
    {
        m_nodeName = title;
        if (m_window)
            m_window.Title(winrt::hstring(title));
    }

    void OutputWindow::SetTimingText(const std::wstring& text)
    {
        m_timingText = text;
    }

    void OutputWindow::CreateSwapChain()
    {
        if (!m_d3dDevice || !m_dxgiFactory || m_width == 0 || m_height == 0)
            return;

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
        HRESULT hr = m_dxgiFactory->CreateSwapChainForComposition(
            m_d3dDevice,
            &desc,
            nullptr,
            swapChain1.put());

        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[OutputWindow] CreateSwapChain failed hr=0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
            return;
        }

        m_swapChain = swapChain1.as<IDXGISwapChain3>();
        m_swapChain->SetColorSpace1(m_format.colorSpace);

        auto panelNative = m_panel.as<ISwapChainPanelNative>();
        panelNative->SetSwapChain(m_swapChain.get());
        UpdatePanelScale();
    }

    void OutputWindow::UpdatePanelScale()
    {
        if (!m_swapChain || !m_panel)
            return;

        winrt::com_ptr<IDXGISwapChain2> swapChain2;
        if (FAILED(m_swapChain->QueryInterface(IID_PPV_ARGS(swapChain2.put()))))
            return;

        float scaleX = (std::max)(1.0f, static_cast<float>(m_panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(m_panel.CompositionScaleY()));

        DXGI_MATRIX_3X2_F matrix{};
        matrix._11 = 1.0f / scaleX;
        matrix._22 = 1.0f / scaleY;
        swapChain2->SetMatrixTransform(&matrix);
    }

    void OutputWindow::CreateRenderTarget()
    {
        if (!m_swapChain || !m_dc)
            return;

        winrt::com_ptr<IDXGISurface2> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
        if (FAILED(hr)) return;

        D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(m_format.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));

        hr = m_dc->CreateBitmapFromDxgiSurface(
            backBuffer.get(),
            &bitmapProps,
            m_renderTarget.put());

        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[OutputWindow] CreateRenderTarget failed hr=0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
        }
    }

    void OutputWindow::ReleaseRenderTarget()
    {
        m_renderTarget = nullptr;
    }

    void OutputWindow::OnPanelSizeChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
    {
        ResizeForPanelSize(args.NewSize().Width, args.NewSize().Height);
    }

    void OutputWindow::ResizeForPanelSize(double widthDips, double heightDips)
    {
        auto scaleX = static_cast<double>((std::max)(1.0f, static_cast<float>(m_panel.CompositionScaleX())));
        auto scaleY = static_cast<double>((std::max)(1.0f, static_cast<float>(m_panel.CompositionScaleY())));
        uint32_t w = static_cast<uint32_t>((std::max)(1.0, std::ceil(widthDips * scaleX)));
        uint32_t h = static_cast<uint32_t>((std::max)(1.0, std::ceil(heightDips * scaleY)));

        if (w == m_width && h == m_height)
            return;

        m_width = w;
        m_height = h;

        if (!m_swapChain)
        {
            CreateSwapChain();
            CreateRenderTarget();
        }
        else
        {
            m_needsResize = true;
        }
    }

    winrt::fire_and_forget OutputWindow::SaveImageAsync()
    {
        if (!m_dc || !m_lastImage || !m_window)
            co_return;

        // Get HWND for the file picker.
        HWND hwnd{ nullptr };
        auto windowNative = m_window.try_as<::IWindowNative>();
        if (windowNative)
            windowNative->get_WindowHandle(&hwnd);
        if (!hwnd) co_return;

        winrt::Windows::Storage::Pickers::FileSavePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);

        std::wstring suggestedName = m_nodeName.empty() ? L"output" : m_nodeName;
        for (auto& ch : suggestedName)
            if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|')
                ch = L'_';
        picker.SuggestedFileName(winrt::hstring(suggestedName));
        picker.FileTypeChoices().Insert(L"JPEG XR (HDR)", winrt::single_threaded_vector<winrt::hstring>({ L".jxr" }));
        picker.FileTypeChoices().Insert(L"PNG Image (SDR)", winrt::single_threaded_vector<winrt::hstring>({ L".png" }));

        auto file = co_await picker.PickSaveFileAsync();
        if (!file) co_return;

        auto* dc = m_dc;
        auto* image = m_lastImage;
        if (!dc || !image) co_return;

        try
        {
            float oldDpiX, oldDpiY;
            dc->GetDpi(&oldDpiX, &oldDpiY);
            dc->SetDpi(96.0f, 96.0f);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());

            D2D1_RECT_F bounds{};
            dc->GetImageLocalBounds(image, &bounds);
            uint32_t w = static_cast<uint32_t>(bounds.right - bounds.left);
            uint32_t h = static_cast<uint32_t>(bounds.bottom - bounds.top);
            dc->SetDpi(oldDpiX, oldDpiY);
            if (w == 0 || h == 0) co_return;

            auto fileExt = std::wstring(file.FileType().c_str());
            bool isJxr = (fileExt == L".jxr" || fileExt == L".wdp");

            DXGI_FORMAT renderFormat = isJxr
                ? DXGI_FORMAT_R16G16B16A16_FLOAT
                : DXGI_FORMAT_B8G8R8A8_UNORM;

            winrt::com_ptr<ID2D1Bitmap1> renderBitmap;
            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(renderFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bmpProps, renderBitmap.put());

            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            dc->SetTarget(renderBitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0, 0, 1.0f));
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->DrawImage(image);
            dc->EndDraw();
            dc->SetTarget(oldTarget.get());

            winrt::com_ptr<IWICImagingFactory> wicFactory;
            winrt::check_hresult(CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wicFactory.put())));

            auto filePath = std::wstring(file.Path().c_str());
            GUID containerFormat = isJxr ? GUID_ContainerFormatWmp : GUID_ContainerFormatPng;

            winrt::com_ptr<IWICStream> stream;
            winrt::check_hresult(wicFactory->CreateStream(stream.put()));
            winrt::check_hresult(stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE));

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(wicFactory->CreateEncoder(containerFormat, nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::com_ptr<IPropertyBag2> encoderOptions;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), encoderOptions.put()));

            if (isJxr && encoderOptions)
            {
                PROPBAG2 option{};
                option.pstrName = const_cast<LPOLESTR>(L"Lossless");
                VARIANT val{};
                val.vt = VT_BOOL;
                val.boolVal = VARIANT_TRUE;
                encoderOptions->Write(1, &option, &val);
            }

            winrt::check_hresult(frame->Initialize(encoderOptions.get()));
            winrt::check_hresult(frame->SetSize(w, h));

            WICPixelFormatGUID pixelFormat = isJxr
                ? GUID_WICPixelFormat64bppRGBAHalf
                : GUID_WICPixelFormat32bppBGRA;
            winrt::check_hresult(frame->SetPixelFormat(&pixelFormat));

            winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
            D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(renderFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, cpuProps, cpuBitmap.put());
            D2D1_POINT_2U destPoint = { 0, 0 };
            D2D1_RECT_U srcRect = { 0, 0, w, h };
            cpuBitmap->CopyFromBitmap(&destPoint, renderBitmap.get(), &srcRect);

            D2D1_MAPPED_RECT mapped{};
            winrt::check_hresult(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped));
            winrt::check_hresult(frame->WritePixels(h, mapped.pitch, mapped.pitch * h, mapped.bits));
            cpuBitmap->Unmap();

            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());

            if (m_fpsText)
                m_fpsText.Text(L"Saved: " + file.Name());
        }
        catch (...) {}
    }
}
