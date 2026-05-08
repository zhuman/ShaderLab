#pragma once

// Working Space node sync.
//
// The "Working Space" parameter node mirrors the active display profile
// (live or simulated) into 14 typed analysis output fields exposed as
// node properties. Downstream effects can bind any property to one of
// these fields to track the active display profile automatically.
//
// This is a pure walk over the graph + DisplayMonitor query. It is
// intentionally engine-side so the GUI, the headless host, and any
// MCP route that mutates the active profile can all keep Working
// Space nodes in sync via the same code path.

#include "../EngineExport.h"

namespace ShaderLab::Graph
{
    class EffectGraph;
}

namespace ShaderLab::Rendering
{
    class DisplayMonitor;

    // Walk the graph and refresh every "Working Space" parameter node's
    // properties from `monitor.ActiveProfile()`. Marks the node dirty
    // only when at least one field changed value, so callers can pump
    // this every render tick without invalidating the cache when the
    // profile is stable. Returns true if any node was modified.
    SHADERLAB_API bool UpdateWorkingSpaceNodes(
        Graph::EffectGraph& graph,
        DisplayMonitor& monitor);
}
