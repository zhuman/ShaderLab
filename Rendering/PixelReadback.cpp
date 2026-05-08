#include "pch_engine.h"
#include "PixelReadback.h"

#include <cstring>

namespace ShaderLab::Rendering
{
    namespace
    {
        // Resolve the display image for a node: cachedOutput, with the
        // same safety checks MainWindow::ResolveDisplayImage performs
        // (don't return a dirty pointer because the evaluator may have
        // released it; don't return effects with required-but-missing
        // inputs because their cachedOutput is unsafe to use).
        ID2D1Image* ResolveDisplayImage(const Graph::EffectGraph& graph, uint32_t nodeId,
            ReadPixelRegionStatus& status)
        {
            const auto* node = graph.FindNode(nodeId);
            if (!node) { status = ReadPixelRegionStatus::NotFound; return nullptr; }
            if (!node->cachedOutput || node->dirty) {
                status = ReadPixelRegionStatus::NotReady;
                return nullptr;
            }
            if (!node->inputPins.empty())
            {
                auto inputs = graph.GetInputEdges(nodeId);
                if (inputs.empty()) {
                    status = ReadPixelRegionStatus::NotReady;
                    return nullptr;
                }
            }
            return node->cachedOutput;
        }
    }

    ReadPixelRegionResult ReadPixelRegion(
        const Graph::EffectGraph& graph,
        uint32_t nodeId,
        int32_t x, int32_t y, uint32_t w, uint32_t h,
        ID2D1DeviceContext* dc)
    {
        ReadPixelRegionResult result;
        if (!dc) { result.status = ReadPixelRegionStatus::D2DError; return result; }

        ID2D1Image* image = ResolveDisplayImage(graph, nodeId, result.status);
        if (!image) return result;

        // Use 96 DPI so 1 DIP == 1 pixel and GetImageLocalBounds returns
        // pixel coordinates directly.
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(image, &bounds);
        const int32_t imgW = static_cast<int32_t>(bounds.right - bounds.left);
        const int32_t imgH = static_cast<int32_t>(bounds.bottom - bounds.top);
        if (imgW <= 0 || imgH <= 0) {
            dc->SetDpi(oldDpiX, oldDpiY);
            result.status = ReadPixelRegionStatus::InvalidRegion;
            return result;
        }

        // Clip to image bounds. `bounds.left/top` may be non-zero for
        // sources whose origin isn't at (0,0).
        const int32_t x0 = (std::max)(x, 0);
        const int32_t y0 = (std::max)(y, 0);
        const int32_t x1 = (std::min)(static_cast<int32_t>(x + static_cast<int64_t>(w)), imgW);
        const int32_t y1 = (std::min)(static_cast<int32_t>(y + static_cast<int64_t>(h)), imgH);
        if (x1 <= x0 || y1 <= y0) {
            dc->SetDpi(oldDpiX, oldDpiY);
            result.status = ReadPixelRegionStatus::InvalidRegion;
            return result;
        }

        const uint32_t actW = static_cast<uint32_t>(x1 - x0);
        const uint32_t actH = static_cast<uint32_t>(y1 - y0);

        try
        {
            // FP32 RGBA target so we get linear scRGB values directly.
            winrt::com_ptr<ID2D1Bitmap1> targetBitmap;
            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(actW, actH),
                nullptr, 0, bmpProps, targetBitmap.put()));

            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            dc->SetTarget(targetBitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            dc->SetTransform(D2D1::Matrix3x2F::Identity());

            // SOURCE_COPY + NEAREST_NEIGHBOR copies raw values without
            // alpha blend or filtering. DrawImage(image, targetOffset,
            // imageRect, ...) is always 1:1 sampling, so a srcRect of
            // size (actW, actH) at offset (0,0) in the target lands
            // exactly the requested region.
            const float fx0 = static_cast<float>(x0) + bounds.left;
            const float fy0 = static_cast<float>(y0) + bounds.top;
            const float fx1 = static_cast<float>(x1) + bounds.left;
            const float fy1 = static_cast<float>(y1) + bounds.top;
            D2D1_POINT_2F destOffset = D2D1::Point2F(0.0f, 0.0f);
            D2D1_RECT_F srcRect = D2D1::RectF(fx0, fy0, fx1, fy1);
            dc->DrawImage(image, &destOffset, &srcRect,
                D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                D2D1_COMPOSITE_MODE_SOURCE_COPY);
            dc->EndDraw();
            dc->SetTarget(oldTarget.get());

            // Copy GPU bitmap to a CPU-readable bitmap and Map it.
            winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
            D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(actW, actH),
                nullptr, 0, cpuProps, cpuBitmap.put()));
            D2D1_POINT_2U destPt = { 0, 0 };
            D2D1_RECT_U srcRc = { 0, 0, actW, actH };
            winrt::check_hresult(cpuBitmap->CopyFromBitmap(&destPt, targetBitmap.get(), &srcRc));

            D2D1_MAPPED_RECT mapped{};
            winrt::check_hresult(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped));

            result.pixels.resize(static_cast<size_t>(actW) * actH * 4);
            for (uint32_t row = 0; row < actH; ++row)
            {
                const float* srcRow = reinterpret_cast<const float*>(
                    mapped.bits + static_cast<size_t>(row) * mapped.pitch);
                std::memcpy(
                    result.pixels.data() + static_cast<size_t>(row) * actW * 4,
                    srcRow,
                    static_cast<size_t>(actW) * 4 * sizeof(float));
            }
            cpuBitmap->Unmap();

            result.actualWidth = actW;
            result.actualHeight = actH;
            result.status = ReadPixelRegionStatus::Success;
        }
        catch (...)
        {
            result.status = ReadPixelRegionStatus::D2DError;
            result.pixels.clear();
            result.actualWidth = 0;
            result.actualHeight = 0;
        }

        dc->SetDpi(oldDpiX, oldDpiY);
        return result;
    }
}
