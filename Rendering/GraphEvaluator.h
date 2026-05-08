#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/EffectGraph.h"
#include "../Effects/CustomPixelShaderEffect.h"
#include "../Effects/CustomComputeShaderEffect.h"
#include "../Effects/CustomComputeBridgeEffect.h"
#include "../Effects/ShaderCompiler.h"
#include "D3D11ComputeRunner.h"

namespace ShaderLab::Rendering
{
    // Evaluates an EffectGraph by walking nodes in topological order,
    // creating / caching D2D effects, wiring inputs from upstream outputs,
    // and returning the final ID2D1Image* for presentation.
    //
    // The evaluator keeps a per-node effect cache so that effects are only
    // recreated when the node type or CLSID changes (not every frame).
    // Properties are re-applied each frame on dirty nodes.
    //
    // Usage:
    //   auto* finalImage = evaluator.Evaluate(graph, deviceContext);
    //   if (finalImage) { dc->DrawImage(finalImage); }
    class SHADERLAB_API GraphEvaluator
    {
    public:
        GraphEvaluator() = default;
        GraphEvaluator(const GraphEvaluator&) = delete;
        GraphEvaluator& operator=(const GraphEvaluator&) = delete;

        // Walk the graph in topological order and produce the final output image.
        // Returns nullptr if the graph is empty or has no Output node.
        ID2D1Image* Evaluate(Graph::EffectGraph& graph, ID2D1DeviceContext5* dc);

        // Run deferred D3D11 compute dispatches for statistics nodes.
        // Call AFTER Evaluate (and double-eval) when all D2D effects are initialized.
        // Returns true if any compute dispatches ran (analysis output changed).
        bool ProcessDeferredCompute(Graph::EffectGraph& graph, ID2D1DeviceContext5* dc);
        size_t DeferredComputeCount() const { return m_deferredCompute.size(); }

        // Phase 8c: when set true, EvaluateNode for D3D11 compute effects
        // skips appending to `m_deferredCompute`. The host sets this before
        // the post-PDC re-evaluate pass (which exists only to re-apply
        // properties on D2D effects downstream of just-dispatched compute
        // bridges) so leftover compute entries don't leak into the next
        // frame's PDC and cause a duplicate dispatch with stale source.
        // Reset to false before pass 1 of each frame.
        void SetDeferredComputeFrozen(bool frozen) { m_deferredComputeFrozen = frozen; }

        // Resolve property bindings for Source nodes only (lightweight, no D2D).
        // Call before TickAndUploadVideos so video nodes see updated Time values.
        void ResolveSourceBindings(Graph::EffectGraph& graph);

        // Release all cached D2D effects (e.g., on device lost or graph clear).
        // The graph-aware overload also clears every node's non-owning cachedOutput
        // pointer so the render path can't dereference a freed effect.
        void ReleaseCache();
        void ReleaseCache(Graph::EffectGraph& graph);

        // Invalidate the cached effect for a specific node (e.g., CLSID changed).
        // The graph-aware overload also clears node.cachedOutput.
        void InvalidateNode(uint32_t nodeId);
        void InvalidateNode(Graph::EffectGraph& graph, uint32_t nodeId);

        // Update an existing cached effect's shader bytecode in-place (for recompile).
        // If the effect isn't cached yet, does nothing (next Evaluate will create it).
        void UpdateNodeShader(uint32_t nodeId, const Graph::EffectNode& node);

        // Phase 8c: host hint set for which nodes need their analysis
        // values on the CPU. UI integration: MainWindow updates this
        // each frame with the currently-selected node id (so its
        // Properties-panel display stays live) plus any node whose
        // values an MCP route is about to read. Default empty: when
        // empty AND the skip-readback flag is on, only nodes detected
        // as feeding a non-GPU-served binding will get readback.
        // When the skip-readback flag is off (the default), this set
        // is ignored and every compute dispatch reads back to CPU.
        //
        // Throttling: nodes that are newly added to interest get an
        // immediate readback; subsequent frames are throttled to
        // Performance::CpuAnalysisHintThrottleMs (default 2000 ms,
        // 0.5 Hz). Re-selecting a node after it left interest treats
        // it as fresh.
        void SetCpuAnalysisInterest(std::unordered_set<uint32_t> ids)
        {
            // Drop the throttle timestamp for any node that left the
            // interest set so a subsequent re-selection reads back
            // immediately rather than waiting for the throttle window.
            for (auto it = m_lastHintReadbackTime.begin();
                 it != m_lastHintReadbackTime.end();)
            {
                if (!ids.count(it->first)) it = m_lastHintReadbackTime.erase(it);
                else ++it;
            }
            m_cpuAnalysisInterest = std::move(ids);
        }
        void AddCpuAnalysisInterest(uint32_t id)
        {
            m_cpuAnalysisInterest.insert(id);
        }
        void ClearCpuAnalysisInterest()
        {
            m_cpuAnalysisInterest.clear();
            m_lastHintReadbackTime.clear();
        }

    private:
        // Create or retrieve the cached D2D effect for a built-in effect node.
        ID2D1Effect* GetOrCreateEffect(
            ID2D1DeviceContext5* dc,
            const Graph::EffectNode& node);

        // Apply the node's property map to its D2D effect.
        // Uses effective properties (authored + binding overrides).
        void ApplyProperties(
            ID2D1Effect* effect,
            const Graph::EffectNode& node,
            const std::map<std::wstring, Graph::PropertyValue>& effectiveProps);

        // Resolve property bindings: build effective properties map from
        // authored defaults + upstream analysis output values.
        // Returns true if any binding produced a new value (node should be dirtied).
        bool ResolveBindings(
            Graph::EffectNode& node,
            const Graph::EffectGraph& graph,
            std::map<std::wstring, Graph::PropertyValue>& effectiveProps);

        // Wire input edges: for each input pin on destNode, find the upstream
        // node's cachedOutput and call effect->SetInput(pin, image).
        void WireInputs(
            ID2D1Effect* effect,
            const Graph::EffectNode& destNode,
            const Graph::EffectGraph& graph);

        // Per-node effect cache: nodeId → D2D effect.
        // Effects are reused across frames; only properties are updated.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Effect>> m_effectCache;

        // Per-node owning reference to each effect's output image.
        // ID2D1Effect::GetOutput() returns an AddRef'd pointer; if we only stash
        // the raw pointer in EffectNode::cachedOutput, the local com_ptr releases
        // its ref at scope-exit. The image then survives only by whatever ref
        // the effect holds internally -- and D2D will release/recreate the
        // output proxy on operations like SetInput-toggle, leaving cachedOutput
        // dangling until the next GetOutput. We hold an owning ref here so the
        // image lives as long as the effect does. The D2D debug layer
        // (d2d1debug3.dll) flags use-after-release otherwise.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Image>> m_outputCache;

        // Per-node custom effect impl cache for host-side API access.
        struct CustomEffectEntry
        {
            Effects::CustomPixelShaderEffect* pixelImpl{ nullptr };
            Effects::CustomComputeShaderEffect* computeImpl{ nullptr };
        };
        std::unordered_map<uint32_t, CustomEffectEntry> m_customImplCache;

        // Phase 8: per-node CustomComputeBridgeEffect impl cache.
        // Populated when CreateOrGetEffect creates a bridge for a
        // D3D11ComputeShader node. ProcessDeferredCompute looks up the
        // bridge here and calls ICustomComputeBridge::Dispatch.
        // Bridge lifetime is owned by m_effectCache (the ID2D1Effect
        // outer); this raw pointer is valid as long as that entry is.
        std::unordered_map<uint32_t, Effects::CustomComputeBridgeEffect*> m_bridgeImplCache;

        // Apply bytecode and cbuffer to a custom effect node.
        void ApplyCustomEffect(
            ID2D1Effect* effect,
            Graph::EffectNode& node,
            const std::map<std::wstring, Graph::PropertyValue>& effectiveProps);

        // Force D2D to compute the histogram and read output data.
        void ReadHistogramOutput(
            ID2D1DeviceContext5* dc,
            ID2D1Effect* effect,
            Graph::EffectNode& node);

        // Read back key-value analysis data from custom compute effect output pixels.
        void ReadCustomAnalysisOutput(
            ID2D1DeviceContext5* dc,
            Graph::EffectNode& node);

        // Temp target for forcing effect computation.
        winrt::com_ptr<ID2D1Bitmap1> m_analysisTarget;
        uint32_t m_analysisTargetW{ 0 };
        uint32_t m_analysisTargetH{ 0 };
        DXGI_FORMAT m_analysisTargetFormat{ DXGI_FORMAT_UNKNOWN };

        // Tracks nodes whose D2D effects were created this frame.
        // Analysis readback is deferred by one frame for these nodes.
        std::unordered_set<uint32_t> m_justCreated;

        // Dummy 1x1 bitmap used as input for zero-input source effects.
        // D2D custom pixel shaders require at least 1 input for sizing,
        // but source effects generate their own content.
        winrt::com_ptr<ID2D1Bitmap1> m_dummySourceBitmap;
        void EnsureDummySourceBitmap(ID2D1DeviceContext5* dc);

        // Phase 8 unified bridge dispatch. Replaces the pre-Phase-8
        // DispatchUserD3D11Compute (analysis-only) and DispatchImageCompute
        // (image-producing) paths -- both now route through
        // CustomComputeBridgeEffect::Dispatch.
        void DispatchViaBridge(
            ID2D1DeviceContext5* dc,
            const Graph::EffectGraph& graph,
            Graph::EffectNode& node,
            ID2D1Image* inputImage,
            ID2D1Bitmap1* preRenderedInput,
            Effects::CustomComputeBridgeEffect* bridge,
            bool readbackToCpu);

        // Phase 8c skip-readback predicate. Returns true iff the binding
        // (consumer node `consumer`, parameter `paramName`, source pin
        // described by the binding's first ComponentSource) will be
        // served entirely via the GPU SRV path during DispatchViaBridge.
        // The predicate is conservative: anything that would cause the
        // bindingPlan to be cleared inside DispatchViaBridge (variant
        // bytecode missing, multi-component sources, gpuBindable=false,
        // bridge missing, SRV unavailable) returns false here so the
        // pre-pass falls back to "needs CPU readback" for the source.
        bool CanServeBindingViaGpu(
            const Graph::EffectNode&         consumer,
            const std::wstring&              paramName,
            const Graph::PropertyBinding&    binding,
            const Graph::EffectGraph&        graph) const;

        // Phase 8c host hint: nodes whose analysis values must be
        // populated on the CPU this frame (currently-selected node, MCP
        // targets, etc). Default empty.
        std::unordered_set<uint32_t> m_cpuAnalysisInterest;

        // Phase 8c hint throttle: per-node timestamp of the last
        // host-hint-driven readback. Used to rate-limit the selected-
        // node / canvas-label readback to ~0.5 Hz (configurable via
        // Performance::CpuAnalysisHintThrottleMs). Newly hinted nodes
        // (added to interest after not being there) get an immediate
        // readback because their entry is missing from this map.
        // Hinted nodes that drop out of interest have their timestamp
        // forgotten so re-selecting them later behaves as a fresh hint.
        // CPU-routed bindings bypass this map entirely -- they always
        // need a fresh value because the consumer reads it directly
        // from `analysisOutput.fields` every frame.
        std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>
            m_lastHintReadbackTime;

        // Phase 8 perf: per-input FP32 pre-render cache used by
        // ProcessDeferredCompute to amortize the source DrawImage
        // across multiple deferred-compute consumers in the same
        // frame. Bitmap is reused frame-to-frame when dimensions
        // match; reallocated otherwise.
        struct SharedPreRenderEntry
        {
            winrt::com_ptr<ID2D1Bitmap1> bitmap;
            UINT32                       width{ 0 };
            UINT32                       height{ 0 };
        };
        std::unordered_map<ID2D1Image*, SharedPreRenderEntry> m_sharedPreRenderCache;

        // Deferred D3D11 compute dispatches (node ID + upstream image).
        struct DeferredCompute {
            uint32_t nodeId;
            ID2D1Image* inputImage;  // non-owning, valid until next Evaluate
            winrt::com_ptr<ID2D1Bitmap1> preRenderedInput;  // owning, pre-rendered bitmap (optional)
        };
        std::vector<DeferredCompute> m_deferredCompute;
        bool m_deferredComputeFrozen{ false };

        // Pre-render a D2D image to an FP32 bitmap at 96 DPI.
        winrt::com_ptr<ID2D1Bitmap1> PreRenderInputBitmap(
            ID2D1DeviceContext5* dc, ID2D1Image* inputImage);
    };
}
