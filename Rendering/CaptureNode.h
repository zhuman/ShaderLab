#pragma once

// Capture a graph node's output as a PNG byte blob.
//
// Engine-side helper used by:
//   * Engine::Mcp /render/capture-node route
//   * ShaderLabHeadless render path (via reuse)
//   * GUI MainWindow::CaptureNodeAsPng / CapturePreviewAsPng wrappers
//
// All three contexts need the same logic: resolve a node's cachedOutput,
// validate it is ready (not dirty, has inputs if required), render to a
// CPU-readable BGRA8 bitmap, and WIC-encode to PNG.

#include "../EngineExport.h"

#include <cstdint>
#include <vector>

struct ID2D1DeviceContext;

namespace ShaderLab::Graph
{
    class EffectGraph;
}

namespace ShaderLab::Rendering
{
    enum class CaptureNodeStatus
    {
        Success,
        NotFound,       // No node with that ID
        NotReady,       // Node exists but is dirty or missing required inputs
        EmptyImage,     // cachedOutput resolves but bounds are 0x0
        D2DError        // WIC / D2D / map failed
    };

    struct CaptureNodeResult
    {
        CaptureNodeStatus    status{ CaptureNodeStatus::D2DError };
        std::vector<uint8_t> png;
        uint32_t             width{ 0 };
        uint32_t             height{ 0 };
    };

    // Render `nodeId`'s cached output to a PNG byte blob via D2D + WIC.
    // The caller must ensure the graph has been evaluated this frame
    // (i.e. dirty nodes resolved). The function does NOT trigger
    // evaluation -- that's the caller's responsibility.
    //
    // `maxDim` clamps the longer edge of the output to bound encoder cost
    // for very large images. Defaults to 2048.
    SHADERLAB_API CaptureNodeResult CaptureNodeAsPng(
        Graph::EffectGraph& graph,
        uint32_t nodeId,
        ID2D1DeviceContext* dc,
        uint32_t maxDim = 2048);
}
