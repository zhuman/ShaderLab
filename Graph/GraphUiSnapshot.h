#pragma once

// GraphUiSnapshot
//
// Immutable copy of EffectGraph + per-node runtime data, published by the
// render thread after each frame for UI / MCP consumers. The render thread
// is the single writer to the live `EffectGraph`; every other reader pulls
// the latest snapshot via `std::atomic<std::shared_ptr<const GraphUiSnapshot>>`.
//
// Why a copy instead of just exposing the live graph behind a lock:
//  * Property-bound consumers (Properties panel, FPS tooltip live values, MCP
//    `read-analysis-output`) need a consistent view of one frame's worth of
//    state. A lock would give them a moving target.
//  * Render thread can publish-and-forget: each shared_ptr<const ...> is
//    immutable from the consumer side, freed when the last reader drops it.
//  * Headless / synchronous mode skips publication entirely (no reader exists
//    to consume it), so the cost only applies to the GUI host.
//
// Cost: a 50-node graph with ~5-10 properties per node is ~50KB per snapshot.
// At 60 fps that's ~3 MB/s of allocation traffic. Acceptable; later we can
// pool snapshot allocations or skip rebuilding when graphGeneration hasn't
// bumped, but neither is needed for the initial cut.
//
// Invariants:
//  * `cachedOutput` on every snapshot node is `nullptr` -- raw `ID2D1Image*`
//    pointers are render-thread-only and must never escape into UI code.
//  * Snapshot nodes/edges are value copies of EffectGraph state at the
//    moment Build was called.

#include "../EngineExport.h"
#include "EffectGraph.h"
#include "EffectEdge.h"
#include "EffectNode.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ShaderLab::Graph
{
    struct GraphUiSnapshot
    {
        // Bumps every time a graph mutation is applied on the render thread
        // (AddNode / Connect / SetProperty / etc.). Lets UI tell whether two
        // snapshots represent the same logical graph.
        std::uint64_t graphGeneration{ 0 };

        // Bumps once per render frame. Lets UI tell whether two snapshots
        // came from different evaluation passes (and so might have fresh
        // analysisOutput / runtimeError / dirty values).
        std::uint64_t frameGeneration{ 0 };

        // Value copies of every node and edge from the live graph. Every
        // node's `cachedOutput` is forced to nullptr in the snapshot.
        std::vector<EffectNode> nodes;
        std::vector<EffectEdge> edges;

        // O(1) lookup by node id.
        std::unordered_map<std::uint32_t, std::size_t> nodeIndexById;

        // Currently-selected preview node. 0 means no preview.
        std::uint32_t previewNodeId{ 0 };

        // Returns a pointer into `nodes` (stable for the lifetime of this
        // snapshot) or nullptr if no node with that id exists.
        const EffectNode* FindNode(std::uint32_t id) const
        {
            auto it = nodeIndexById.find(id);
            if (it == nodeIndexById.end()) return nullptr;
            return &nodes[it->second];
        }
    };

    // Build an immutable snapshot from the live graph. Caller must already
    // be on the render thread (the single writer of `graph`).
    SHADERLAB_API std::shared_ptr<const GraphUiSnapshot> BuildGraphUiSnapshot(
        const EffectGraph& graph,
        std::uint32_t previewNodeId,
        std::uint64_t graphGeneration,
        std::uint64_t frameGeneration);
}
