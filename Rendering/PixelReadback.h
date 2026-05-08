#pragma once

// Pixel readback helpers for the engine. Used by:
//   - The MCP `read_pixel_region` route (today via MainWindow shim;
//     post-`p7-mcp-move` directly engine-side).
//   - ShaderLabHeadless `--pixels x,y,w,h` mode for scripted full-
//     accuracy sampling.
//   - The PixelInspectorController and PixelTraceController follow a
//     similar pattern but read 1x1 regions for the live UI overlay;
//     this header is for the small-region readback case.
//
// The output is FP32 RGBA in scRGB linear-light, row-major, no
// gamma encoding. Caller decides what to do with it (encode PNG,
// compute statistics, write to a binary blob, etc).

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/EffectGraph.h"

#include <cstdint>
#include <vector>

namespace ShaderLab::Rendering
{
    enum class ReadPixelRegionStatus
    {
        Success = 0,
        NotFound,        // node id doesn't exist in graph
        NotReady,        // node exists but no cachedOutput (dirty or missing inputs)
        InvalidRegion,   // requested region clipped to nothing inside image bounds
        D2DError,        // D2D bitmap creation, draw, or map failed
    };

    struct ReadPixelRegionResult
    {
        ReadPixelRegionStatus status{ ReadPixelRegionStatus::NotFound };

        // Row-major FP32 RGBA pixels, length = actualWidth * actualHeight * 4.
        // Empty unless status == Success.
        std::vector<float> pixels;

        // The actual region read after clipping the request to image bounds.
        // May be smaller than (w, h) if the request extends past the edge.
        // (0, 0) when status != Success.
        uint32_t actualWidth{ 0 };
        uint32_t actualHeight{ 0 };
    };

    // Read an FP32 RGBA region from a graph node's cachedOutput.
    //
    // The caller is responsible for evaluating the graph first (Evaluate
    // + flush). This function does NOT call MarkAllDirty / Evaluate —
    // it reads whatever cachedOutput is currently populated. Hosts that
    // need to "force a fresh frame" should evaluate before calling.
    //
    // (x, y) and (w, h) are in image-local pixel coordinates; the
    // request is clipped to the image's bounds (via GetImageLocalBounds
    // on the DC) before the readback.
    SHADERLAB_API ReadPixelRegionResult ReadPixelRegion(
        const Graph::EffectGraph& graph,
        uint32_t nodeId,
        int32_t x, int32_t y, uint32_t w, uint32_t h,
        ID2D1DeviceContext* dc);
}
