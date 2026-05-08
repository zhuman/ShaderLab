#include "pch_engine.h"
#include "Performance.h"

namespace ShaderLab::Performance
{
    namespace {
        // Phase 8 v1.6: GPU-binding fast path defaults ON.
        std::atomic<bool>     g_enabled{ true };
        std::atomic<uint64_t> g_detections{ 0 };
        // Phase 8c: skip-readback opt-in defaults OFF.
        std::atomic<bool>     g_skipReadback{ false };
        std::atomic<uint64_t> g_skipped{ 0 };
    }

    bool IsGpuBindingsEnabled()
    {
        return g_enabled.load(std::memory_order_relaxed);
    }

    void SetGpuBindingsEnabled(bool enabled)
    {
        g_enabled.store(enabled, std::memory_order_relaxed);
    }

    uint64_t GpuBindingDetections()
    {
        return g_detections.load(std::memory_order_relaxed);
    }

    void IncrementGpuBindingDetection()
    {
        g_detections.fetch_add(1, std::memory_order_relaxed);
    }

    bool IsSkipUnneededCpuReadbackEnabled()
    {
        return g_skipReadback.load(std::memory_order_relaxed);
    }

    void SetSkipUnneededCpuReadbackEnabled(bool enabled)
    {
        g_skipReadback.store(enabled, std::memory_order_relaxed);
    }

    uint64_t SkippedCpuReadbacks()
    {
        return g_skipped.load(std::memory_order_relaxed);
    }

    void IncrementSkippedCpuReadbacks()
    {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
    }

    namespace { std::atomic<uint32_t> g_hintThrottleMs{ 2000 }; }

    uint32_t CpuAnalysisHintThrottleMs()
    {
        return g_hintThrottleMs.load(std::memory_order_relaxed);
    }

    void SetCpuAnalysisHintThrottleMs(uint32_t ms)
    {
        g_hintThrottleMs.store(ms, std::memory_order_relaxed);
    }
}
