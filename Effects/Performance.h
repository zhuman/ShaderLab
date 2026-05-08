#pragma once

#include "../EngineExport.h"
#include <atomic>

namespace ShaderLab::Performance
{
    // Phase 8 GPU-binding feature flag
    // ==================================
    //
    // When enabled, the evaluator's binding-resolution pass detects
    // upstream effects that publish IEngineComputeOutput AND consumer
    // parameters flagged gpuBindable, and routes them GPU-side via the
    // CustomComputeBridgeEffect's SetGpuBinding entry instead of via
    // Map() readback. v1.6 ships with this default ON.
    //
    // CLI flag --enable-gpu-bindings / --disable-gpu-bindings flips it
    // for headless. Tests can flip it via SetGpuBindingsEnabled to
    // exercise both paths.
    //
    // Routing behavior:
    //   - D3D11 compute consumers (via CustomComputeBridgeEffect): bind
    //     the upstream IEngineComputeOutput SRV at the consumer's
    //     t-slot via the bridge's SetGpuBinding entry. Variant bytecode
    //     (compiled with _SLPARAM_<name>_GPU=1 macros) comes from the
    //     BytecodeCache (eager precompile fills the +N variants on
    //     first encounter so the swap is a cache hit).
    //   - D2D pixel-shader consumers (no bridge entry): the m_bridgeImplCache
    //     miss naturally skips the GPU plan; CPU readback path runs as
    //     graceful fallback. A compute upstream feeding a pixel shader
    //     downstream still works -- just at today's cbuffer-pack speed.

    SHADERLAB_API bool IsGpuBindingsEnabled();
    SHADERLAB_API void SetGpuBindingsEnabled(bool enabled);

    // Phase 8c: skip-CPU-readback opt-in flag.
    // ============================================
    //
    // When enabled, the evaluator skips the CPU `Map(D3D11_MAP_READ)` of
    // each compute node's analysis structured buffer when no consumer
    // needs the values on the CPU this frame. "Needs CPU" =
    //   (1) any property binding that consumes this node's analysis is
    //       NOT served via GPU SRV (covers pixel-shader consumers, multi-
    //       component bindings, gpuBindable=false params, GpuBindings
    //       feature flag off);
    //   (2) the host explicitly hinted the node via
    //       GraphEvaluator::SetCpuAnalysisInterest (UI selected node,
    //       MCP read_analysis_output target, etc.).
    //
    // When skipping is in effect for a node, `EffectNode::analysisOutput.fields`
    // retains the previous-frame values (or empty if never read). Callers
    // that need fresh values must either include the node in the host
    // hint set or disable the feature for that frame.
    //
    // Default OFF for safe rollout. Tests run with default off.
    SHADERLAB_API bool IsSkipUnneededCpuReadbackEnabled();
    SHADERLAB_API void SetSkipUnneededCpuReadbackEnabled(bool enabled);

    // Telemetry: number of dispatches whose CPU readback was skipped
    // since process start. Useful for confirming the optimization
    // actually fires under workload.
    SHADERLAB_API uint64_t SkippedCpuReadbacks();
    SHADERLAB_API void IncrementSkippedCpuReadbacks();

    // Phase 8c throttle: when skip-readback is enabled, host-hinted CPU
    // analysis reads (the selected-node Properties panel display, the
    // node-graph canvas value labels) are throttled to this interval.
    // CPU-routed *property bindings* are unaffected -- they still get
    // a fresh value every frame because their consumer needs it.
    //
    // Default: 2000 ms (0.5 Hz). Static graphs effectively read once
    // per selection because the values dont change between throttled
    // readbacks; video graphs update twice per second which matches
    // human-readable refresh rate. Set to 0 to disable throttling
    // (every frame for hinted nodes).
    SHADERLAB_API uint32_t CpuAnalysisHintThrottleMs();
    SHADERLAB_API void SetCpuAnalysisHintThrottleMs(uint32_t ms);

    // Telemetry: count of bindings the evaluator detected as GPU-routable
    // since process start (whether or not actually routed).
    SHADERLAB_API uint64_t GpuBindingDetections();

    // Internal: bumped by the evaluator when a binding is detected as
    // GPU-routable. Exported for engine-side use only.
    SHADERLAB_API void IncrementGpuBindingDetection();
}
