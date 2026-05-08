#pragma once

#include "pch_engine.h"
#include "../Effects/IEngineComputeOutput.h"

namespace ShaderLab::Rendering
{
    // Generic D3D11 compute shader dispatch runner for user-authored shaders.
    // Handles: shader compilation, SRV/UAV/CB creation, dispatch, and readback.
    //
    // Output uses RWStructuredBuffer<float4> matching the analysisFields ABI:
    //   - One float4 per analysis pixel (field.pixelCount())
    //   - Float → .x, Float2 → .xy, Float3 → .xyz, Float4 → all
    //   - Compatible with existing ReadCustomAnalysisOutput readback
    //
    // cbuffer contract: first 8 bytes = uint Width, uint Height (auto-injected).
    // User parameters start at offset 8.
    //
    // Phase 8: implements IEngineComputeOutput so downstream effects can
    // bind the result SRV directly into a SHADERLAB_GPU_BUFFER slot,
    // avoiding the Map() round-trip. The COM impl is no-op-refcounted
    // -- the runner's lifetime is owned by the GraphEvaluator's
    // unique_ptr cache, callers are not allowed to AddRef beyond that.
    class D3D11ComputeRunner : public Effects::IEngineComputeOutput
    {
    public:
        D3D11ComputeRunner() = default;
        virtual ~D3D11ComputeRunner() = default;

        // Initialize with a D3D11 device (caches device + immediate context).
        void Initialize(ID3D11Device* device);

        // Compile a compute shader from HLSL source.
        // Returns true on success. Call GetCompileError() on failure.
        // On success, GetCompiledBytecode() returns the bytecode blob —
        // the caller should store it on the CustomEffectDefinition so
        // downstream cbuffer reflection (in DispatchUserD3D11Compute)
        // can pack user properties at the correct offsets.
        bool CompileShader(const std::string& hlslSource);

        // Install pre-compiled shader bytecode (Phase 8 bridge effect
        // path). Bypasses D3DCompile -- used when the host already
        // has the bytecode (e.g. after an MCP /effect/compile call).
        // The runner stashes the bytecode blob and the live
        // ID3D11ComputeShader; subsequent Dispatch calls use it like
        // they would the CompileShader output.
        void InstallPrecompiledShader(
            const std::vector<uint8_t>& bytecode,
            winrt::com_ptr<ID3D11ComputeShader> shader);

        // Get the last compile error message.
        const std::wstring& GetCompileError() const { return m_compileError; }

        // Get the compiled bytecode (empty until CompileShader succeeds).
        const std::vector<uint8_t>& GetCompiledBytecode() const { return m_bytecode; }

        // Dispatch the compiled shader on an input texture.
        // cbufferData: user constant buffer bytes (Width/Height prepended automatically).
        // resultCount: number of float4 elements in the result buffer.
        // Returns the readback data (resultCount * 4 floats).
        std::vector<float> Dispatch(
            ID3D11Texture2D* inputTexture,
            const std::vector<BYTE>& cbufferData,
            uint32_t resultCount);

        // Dispatch with an optional secondary image-output texture
        // bound at u1 as `RWTexture2D<float4>`. Used by
        // CustomComputeBridgeEffect: image-producing D3D11 compute
        // effects (CIE Histogram et al.) write their image output
        // here while still publishing analysis values to the
        // structured buffer at u0. Pass nullptr for analysis-only.
        // Same readback contract as the regular Dispatch.
        //
        // dispatchX/Y/Z: number of thread groups. Defaults to (1,1,1)
        // for analysis-only effects whose [numthreads] is sized to
        // cover the analysis output internally (e.g. numthreads(64,1,1)
        // running a per-thread loop over Width*Height pixels). Image-
        // producing per-pixel effects pass (W/tx, H/ty, 1) where tx,ty
        // are the [numthreads] dimensions.
        //
        // extraSrvs / extraSrvSlots: Phase 8 GPU-binding extension.
        // For each entry, the SRV is bound at the named t-slot before
        // dispatch. Used by the bridge to wire upstream
        // IEngineComputeOutput SRVs into a consumer's
        // SHADERLAB_GPU_BUFFER slots without a CPU readback round-trip.
        // Pass empty vectors for the no-binding case.
        std::vector<float> DispatchWithImageOutput(
            ID3D11Texture2D* inputTexture,
            const std::vector<BYTE>& cbufferData,
            uint32_t resultCount,
            ID3D11Texture2D* imageOutputTexture,
            uint32_t dispatchX = 1, uint32_t dispatchY = 1, uint32_t dispatchZ = 1,
            const std::vector<ID3D11ShaderResourceView*>& extraSrvs = {},
            const std::vector<uint32_t>& extraSrvSlots = {},
            bool readbackToCpu = true);

        bool IsInitialized() const { return m_device != nullptr; }
        bool HasShader() const { return m_shader != nullptr; }

        // SRV onto the result structured buffer. Used by upstream
        // effects implementing IEngineComputeOutput to expose their
        // analysis output for direct GPU consumption by downstream
        // SHADERLAB_GPU_BUFFER bindings -- no Map() round-trip. Returns
        // nullptr until the first Dispatch (the SRV is created lazily
        // alongside the UAV in EnsureBuffers).
        ID3D11ShaderResourceView* GetResultSRV() const { return m_resultSRV.get(); }

        // ---- IUnknown (no-op refcount) ----------------------------------
        // The runner's lifetime is owned by the GraphEvaluator's cache;
        // AddRef / Release are no-ops returning a constant. This is
        // valid for "interior" COM objects whose lifetime is managed
        // by an external owner -- callers are required not to outlive
        // the evaluator that vends the pointer.
        HRESULT __stdcall QueryInterface(REFIID iid, void** out) noexcept override;
        ULONG   __stdcall AddRef() noexcept override { return 1; }
        ULONG   __stdcall Release() noexcept override { return 1; }

        // ---- IEngineComputeOutput ---------------------------------------
        HRESULT __stdcall GetAnalysisSrv(ID3D11ShaderResourceView** out) override;
        UINT64  __stdcall GetLastEvaluatedFrame() override { return m_lastEvaluatedFrame; }

        // Called by the evaluator immediately after a successful Dispatch
        // so consumers can detect freshness via GetLastEvaluatedFrame.
        void SetLastEvaluatedFrame(uint64_t frame) { m_lastEvaluatedFrame = frame; }

    private:
        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        winrt::com_ptr<ID3D11ComputeShader> m_shader;
        std::vector<uint8_t> m_bytecode;

        // Result buffer (RWStructuredBuffer<float4>) -- bound as both
        // UAV (writer) and SRV (downstream Phase 8 consumer).
        winrt::com_ptr<ID3D11Buffer> m_resultBuffer;
        winrt::com_ptr<ID3D11Buffer> m_stagingBuffer;
        winrt::com_ptr<ID3D11Buffer> m_cbuffer;
        winrt::com_ptr<ID3D11UnorderedAccessView> m_resultUAV;
        winrt::com_ptr<ID3D11ShaderResourceView>  m_resultSRV;
        uint32_t m_resultCount{ 0 };
        uint64_t m_lastEvaluatedFrame{ 0 };

        std::wstring m_compileError;

        void EnsureBuffers(uint32_t resultCount);
    };
}
