#include "pch_engine.h"
#include "CaptureNode.h"

#include "../Graph/EffectGraph.h"

#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

namespace ShaderLab::Rendering
{
    namespace
    {
        ID2D1Image* ResolveDisplayImage(const Graph::EffectGraph& graph,
            uint32_t nodeId, CaptureNodeStatus& status)
        {
            const auto* node = graph.FindNode(nodeId);
            if (!node) { status = CaptureNodeStatus::NotFound; return nullptr; }
            if (!node->cachedOutput || node->dirty) {
                status = CaptureNodeStatus::NotReady;
                return nullptr;
            }
            if (!node->inputPins.empty())
            {
                auto inputs = graph.GetInputEdges(nodeId);
                if (inputs.empty()) {
                    status = CaptureNodeStatus::NotReady;
                    return nullptr;
                }
            }
            return node->cachedOutput;
        }
    }

    CaptureNodeResult CaptureNodeAsPng(
        Graph::EffectGraph& graph,
        uint32_t nodeId,
        ID2D1DeviceContext* dc,
        uint32_t maxDim)
    {
        CaptureNodeResult result;
        if (!dc) { result.status = CaptureNodeStatus::D2DError; return result; }

        ID2D1Image* image = ResolveDisplayImage(graph, nodeId, result.status);
        if (!image) return result;

        // Use 96 DPI so GetImageLocalBounds returns pixel coordinates.
        // Restore the dc's prior DPI and target afterwards so this helper
        // doesn't disturb the host's rendering state.
        float oldDpiX = 0.f, oldDpiY = 0.f;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(image, &bounds);
        uint32_t w = static_cast<uint32_t>(bounds.right - bounds.left);
        uint32_t h = static_cast<uint32_t>(bounds.bottom - bounds.top);

        dc->SetDpi(oldDpiX, oldDpiY);

        if (w == 0 || h == 0) {
            result.status = CaptureNodeStatus::EmptyImage;
            return result;
        }
        w = (std::min)(w, maxDim);
        h = (std::min)(h, maxDim);

        try
        {
            winrt::com_ptr<ID2D1Bitmap1> renderBitmap;
            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
                bmpProps, renderBitmap.put()));

            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            dc->SetTarget(renderBitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->DrawImage(image);
            dc->EndDraw();
            dc->SetTarget(oldTarget.get());

            winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
            D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
                cpuProps, cpuBitmap.put()));
            D2D1_POINT_2U destPt = { 0, 0 };
            D2D1_RECT_U srcRc = { 0, 0, w, h };
            winrt::check_hresult(cpuBitmap->CopyFromBitmap(&destPt, renderBitmap.get(), &srcRc));

            D2D1_MAPPED_RECT mapped{};
            winrt::check_hresult(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped));

            // PNG-encode mapped BGRA bits via WIC on an in-memory IStream.
            winrt::com_ptr<IWICImagingFactory> wicFactory;
            winrt::check_hresult(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wicFactory.put())));

            winrt::com_ptr<IStream> memStream;
            ::CreateStreamOnHGlobal(nullptr, TRUE, memStream.put());

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(wicFactory->CreateEncoder(GUID_ContainerFormatPng,
                nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(memStream.get(), WICBitmapEncoderNoCache));

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
            winrt::check_hresult(frame->Initialize(nullptr));
            winrt::check_hresult(frame->SetSize(w, h));
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            winrt::check_hresult(frame->SetPixelFormat(&fmt));
            winrt::check_hresult(frame->WritePixels(h, mapped.pitch, mapped.pitch * h, mapped.bits));
            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());

            cpuBitmap->Unmap();

            STATSTG stat{};
            memStream->Stat(&stat, STATFLAG_NONAME);
            ULONG pngSize = static_cast<ULONG>(stat.cbSize.QuadPart);
            result.png.resize(pngSize);
            LARGE_INTEGER zero{};
            memStream->Seek(zero, STREAM_SEEK_SET, nullptr);
            memStream->Read(result.png.data(), pngSize, nullptr);

            result.status = CaptureNodeStatus::Success;
            result.width = w;
            result.height = h;
            return result;
        }
        catch (...)
        {
            result.status = CaptureNodeStatus::D2DError;
            return result;
        }
    }
}
