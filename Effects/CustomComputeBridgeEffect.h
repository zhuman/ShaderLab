#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/PropertyValue.h"
#include "IEngineComputeOutput.h"
#include "../Rendering/D3D11ComputeRunner.h"

namespace ShaderLab::Effects
{
    // CustomComputeBridgeEffect
    // =========================
    //
    // A regular D2D effect (`ID2D1EffectImpl` + `ID2D1DrawTransform`)
    // that wraps user-authored D3D11 compute shaders. From the
    // graph's perspective it behaves like any other effect: lives in
    // `EffectGraph::EffectNode::cachedEffect`, takes one image input,
    // and produces an image output. Internally it dispatches a D3D11
    // compute shader against the input each time the host calls
    // `Dispatch`, populating either:
    //
    //   * an analysis structured buffer (for data-only nodes such as
    //     Luminance Statistics / Channel Statistics / Chromaticity
    //     Statistics / gamut-coverage analysis), readable by
    //     downstream consumers via `IEngineComputeOutput::GetAnalysisSrv`
    //     for the Phase 8 GPU-binding fast path, or
    //
    //   * a D3D11-compute-produced output texture wrapped as an
    //     `ID2D1Bitmap1` (for image-producing nodes such as
    //     CIE Histogram), exposed via `GetImageOutput` for the
    //     evaluator to install as `node->cachedOutput`.
    //
    // The effect's own D2D output (its pass-through pixel shader) is
    // unused -- downstream nodes wire to `node->cachedOutput`
    // directly. The pass-through exists only so the effect satisfies
    // D2D's effect-shape contract (one input -> one output).
    //
    // Replaces the pre-Phase-8 special-case branch in
    // `GraphEvaluator::EvaluateNode` for `customEffect.shaderType ==
    // D3D11ComputeShader` and the parallel `m_d3d11RunnerCache` /
    // `m_imageComputeCache` maps. After this, `D3D11ComputeShader`
    // effects route through the same `CreateOrGetEffect` path every
    // other custom effect does -- the only thing the host has to do
    // is QI the bridge for `ICustomComputeBridge` and call `Dispatch`
    // from the deferred-compute pass.

    // {149A1DAA-1CF1-4298-862C-5A7F3C1B4CD1}
    inline constexpr GUID IID_ICustomComputeBridge = {
        0x149a1daa, 0x1cf1, 0x4298,
        { 0x86, 0x2c, 0x5a, 0x7f, 0x3c, 0x1b, 0x4c, 0xd1 }
    };

    // Engine-internal interface the host uses to drive bridge
    // dispatches. Independent of `IEngineComputeOutput`, which is the
    // *consumer*-side interface; this one is the *driver* side.
    struct __declspec(uuid("149A1DAA-1CF1-4298-862C-5A7F3C1B4CD1"))
    ICustomComputeBridge : IUnknown
    {
        // Install the user's compiled compute shader bytecode. Called
        // once per (effect, definition-version). The bridge compiles
        // the D3D11 shader on the first dispatch.
        virtual HRESULT STDMETHODCALLTYPE SetCompiledBytecode(
            const BYTE* bytecode, UINT32 bytecodeSize) = 0;

        // Dispatch the user compute shader against `inputImage`.
        //
        // - `inputImage` is the upstream D2D image (typically
        //   `srcNode->cachedOutput`). The bridge pre-renders it into
        //   an FP32 bitmap, hands the texture to the D3D11 compute
        //   runner, and flushes D2D before dispatch.
        // - `cbufferData` is the user-property cbuffer bytes; the
        //   runner prepends `Width` + `Height` automatically.
        // - `analysisFloat4Count` is the number of float4 slots the
        //   shader writes to the analysis structured buffer
        //   (`RWStructuredBuffer<float4> Result : register(u0)`).
        //   Use 0 if the shader has no analysis output.
        // - `imageOutputW` / `imageOutputH` request a D3D11-compute-
        //   produced output texture of that size. Pass 0 / 0 for
        //   analysis-only effects.
        // - `outAnalysisFloats` (optional, may be null) collects a
        //   CPU-side copy of the analysis output for hosts that need
        //   to populate `EffectNode::analysisOutput.fields`.
        //
        // After a successful return the analysis SRV is available via
        // `IEngineComputeOutput::GetAnalysisSrv`, and the image
        // output (when requested) via `GetImageOutput`.
        virtual HRESULT STDMETHODCALLTYPE Dispatch(
            ID2D1DeviceContext5* dc,
            ID2D1Image* inputImage,
            const BYTE* cbufferData,
            UINT32 cbufferSize,
            UINT32 analysisFloat4Count,
            UINT32 imageOutputW,
            UINT32 imageOutputH,
            std::vector<float>* outAnalysisFloats) = 0;

        // The bitmap returned by the most recent `Dispatch` call when
        // it requested an image output. Lifetime is tied to the
        // bridge effect's lifetime; downstream nodes can treat it as
        // a non-owning `ID2D1Bitmap1*`. Returns nullptr for analysis-
        // only effects.
        virtual ID2D1Bitmap1* STDMETHODCALLTYPE GetImageOutput() = 0;

        // Phase 8 GPU-binding (consumer side). The evaluator calls
        // this for each binding it routes upstream-effect-SRV ->
        // consumer-t-slot before each dispatch. Bindings are cleared
        // automatically at the end of every Dispatch so a stale
        // binding doesn't leak into the next frame's dispatch.
        // Slot is the HLSL `t<slot>` register (1, 2, ...). srv is
        // the upstream effect's IEngineComputeOutput::GetAnalysisSrv()
        // result (non-owning -- caller guarantees lifetime through the
        // dispatch call).
        virtual HRESULT STDMETHODCALLTYPE SetGpuBinding(
            UINT32 slot, ID3D11ShaderResourceView* srv) = 0;

        // Set the dispatch dimensions for the next call. Defaults to
        // (1,1,1) for analysis-only effects; image-producing per-pixel
        // effects (e.g. ICtCp Tone Map after migration) call this
        // before Dispatch with (W/tx, H/ty, 1) where tx,ty are the
        // shader's [numthreads] dimensions.
        virtual HRESULT STDMETHODCALLTYPE SetDispatchDims(
            UINT32 x, UINT32 y, UINT32 z) = 0;
    };

    // The actual D2D effect class. Implements:
    //   * `ID2D1EffectImpl` + `ID2D1DrawTransform` -- the pass-through
    //     pixel shader that satisfies D2D's "one input -> one output"
    //     contract. Output is unused (downstream nodes wire to
    //     `node->cachedOutput`).
    //   * `ICustomComputeBridge` -- driver-side interface for the
    //     evaluator's deferred-compute pass.
    //   * `IEngineComputeOutput` -- consumer-side interface for the
    //     Phase 8 GPU-binding fast path. Delegates to the runner.
    class CustomComputeBridgeEffect
        : public ID2D1EffectImpl
        , public ID2D1DrawTransform
        , public ICustomComputeBridge
        , public IEngineComputeOutput
    {
    public:
        // {6D69E5C2-1AC0-481E-9F94-3DB8CCAD5710}
        static constexpr GUID CLSID_CustomComputeBridge =
        { 0x6d69e5c2, 0x1ac0, 0x481e, { 0x9f, 0x94, 0x3d, 0xb8, 0xcc, 0xad, 0x57, 0x10 } };

        // Register this effect with a D2D factory. Call once at startup.
        // Single shared CLSID across all D3D11 compute custom-effect
        // instances -- the per-instance shader bytecode is set via
        // `SetCompiledBytecode` after `CreateEffect`.
        static HRESULT RegisterEffect(ID2D1Factory1* factory);

        // Captured by `CreateFactory` so the evaluator can call
        // ICustomComputeBridge methods on the just-created instance.
        // Mirrors the s_lastCreated convention used by
        // CustomPixelShaderEffect / CustomComputeShaderEffect.
        // Thread-local so concurrent CreateEffect calls on different
        // device contexts don't collide.
        static thread_local CustomComputeBridgeEffect* s_lastCreated;

        // D2D creation callback (invoked by CreateEffect).
        static HRESULT __stdcall CreateFactory(IUnknown** effect);

        // ---- IUnknown ----
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        IFACEMETHODIMP_(ULONG) AddRef() override;
        IFACEMETHODIMP_(ULONG) Release() override;

        // ---- ID2D1EffectImpl ----
        IFACEMETHODIMP Initialize(
            ID2D1EffectContext* effectContext,
            ID2D1TransformGraph* transformGraph) override;
        IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE changeType) override;
        IFACEMETHODIMP SetGraph(ID2D1TransformGraph* transformGraph) override;

        // ---- ID2D1DrawTransform ----
        IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* drawInfo) override;

        // ---- ID2D1Transform ----
        IFACEMETHODIMP MapInputRectsToOutputRect(
            const D2D1_RECT_L* inputRects,
            const D2D1_RECT_L* inputOpaqueSubRects,
            UINT32 inputRectCount,
            D2D1_RECT_L* outputRect,
            D2D1_RECT_L* outputOpaqueSubRect) override;
        IFACEMETHODIMP MapOutputRectToInputRects(
            const D2D1_RECT_L* outputRect,
            D2D1_RECT_L* inputRects,
            UINT32 inputRectCount) const override;
        IFACEMETHODIMP MapInvalidRect(
            UINT32 inputIndex,
            D2D1_RECT_L invalidInputRect,
            D2D1_RECT_L* invalidOutputRect) const override;

        // ---- ID2D1TransformNode ----
        IFACEMETHODIMP_(UINT32) GetInputCount() const override { return 1; }

        // ---- ICustomComputeBridge ----
        HRESULT STDMETHODCALLTYPE SetCompiledBytecode(
            const BYTE* bytecode, UINT32 bytecodeSize) override;
        HRESULT STDMETHODCALLTYPE Dispatch(
            ID2D1DeviceContext5* dc,
            ID2D1Image* inputImage,
            const BYTE* cbufferData,
            UINT32 cbufferSize,
            UINT32 analysisFloat4Count,
            UINT32 imageOutputW,
            UINT32 imageOutputH,
            std::vector<float>* outAnalysisFloats) override;
        ID2D1Bitmap1* STDMETHODCALLTYPE GetImageOutput() override
        { return m_imageOutput.get(); }

        HRESULT STDMETHODCALLTYPE SetGpuBinding(
            UINT32 slot, ID3D11ShaderResourceView* srv) override;
        HRESULT STDMETHODCALLTYPE SetDispatchDims(
            UINT32 x, UINT32 y, UINT32 z) override;

        // ---- IEngineComputeOutput ----
        HRESULT STDMETHODCALLTYPE GetAnalysisSrv(
            ID3D11ShaderResourceView** out) override;
        UINT64 STDMETHODCALLTYPE GetLastEvaluatedFrame() override
        { return m_lastEvaluatedFrame; }

    private:
        CustomComputeBridgeEffect() = default;
        ~CustomComputeBridgeEffect() = default;

        LONG m_refCount{ 1 };

        // D2D-side state (transform graph, draw info, input rect).
        winrt::com_ptr<ID2D1EffectContext>   m_effectContext;
        winrt::com_ptr<ID2D1TransformGraph>  m_transformGraph;
        winrt::com_ptr<ID2D1DrawInfo>        m_drawInfo;
        D2D1_RECT_L                          m_inputRect{};

        // The pass-through pixel shader's GUID. Loaded once by the
        // first instance to be initialized; subsequent instances
        // reuse the same shader.
        static GUID s_passThroughGuid;
        static bool s_passThroughLoaded;

        // D3D11 compute side. Owned exclusively by this effect; the
        // pre-Phase-8 `m_d3d11RunnerCache` map on `GraphEvaluator` is
        // gone -- runners live on the bridges that drive them.
        Rendering::D3D11ComputeRunner m_runner;
        std::vector<BYTE>             m_pendingBytecode;
        bool                          m_bytecodeDirty{ false };

        // Image-output side. Populated by `Dispatch` when
        // `imageOutputW > 0 && imageOutputH > 0`. The bitmap wraps a
        // dedicated D3D11 texture the compute writes to via
        // `RWTexture2D<float4> ImageOutput : register(u1)`.
        winrt::com_ptr<ID3D11Texture2D> m_imageOutputTex;
        winrt::com_ptr<ID2D1Bitmap1>    m_imageOutput;
        UINT32                          m_imageOutputW{ 0 };
        UINT32                          m_imageOutputH{ 0 };

        // Cached input bitmap. Without this the bridge allocated a
        // fresh FP32 bitmap (132 MB at 4K!) on every dispatch, which
        // dominated frame time (~30ms/frame on Intel Arc). Re-created
        // only when the upstream's dimensions change.
        winrt::com_ptr<ID2D1Bitmap1>    m_inputBitmap;
        UINT32                          m_inputBitmapW{ 0 };
        UINT32                          m_inputBitmapH{ 0 };

        UINT64 m_lastEvaluatedFrame{ 0 };

        // Phase 8 GPU-binding state, cleared at end of each Dispatch.
        std::vector<ID3D11ShaderResourceView*> m_gpuBindingSrvs;
        std::vector<UINT32>                    m_gpuBindingSlots;

        // Dispatch dimensions for the next call. Default (1,1,1).
        UINT32 m_dispatchX{ 1 };
        UINT32 m_dispatchY{ 1 };
        UINT32 m_dispatchZ{ 1 };

        // Helpers.
        HRESULT EnsurePassThroughShader();
        HRESULT EnsureImageOutputTexture(
            ID3D11Device* device,
            ID2D1DeviceContext5* dc,
            UINT32 w, UINT32 h);
    };
}
