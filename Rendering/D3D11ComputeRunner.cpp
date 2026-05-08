#include "pch_engine.h"
#include "D3D11ComputeRunner.h"

namespace ShaderLab::Rendering
{
    HRESULT __stdcall D3D11ComputeRunner::QueryInterface(REFIID iid, void** out) noexcept
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) ||
            iid == Effects::IID_IEngineComputeOutput)
        {
            *out = static_cast<Effects::IEngineComputeOutput*>(this);
            // No-op AddRef -- runner lifetime owned by GraphEvaluator's cache.
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT __stdcall D3D11ComputeRunner::GetAnalysisSrv(ID3D11ShaderResourceView** out)
    {
        if (!out) return E_POINTER;
        if (!m_resultSRV) return E_NOT_VALID_STATE;
        *out = m_resultSRV.get();
        (*out)->AddRef();
        return S_OK;
    }

    void D3D11ComputeRunner::Initialize(ID3D11Device* device)
    {
        if (!device) return;
        m_device.copy_from(device);
        m_device->GetImmediateContext(m_context.put());
    }

    bool D3D11ComputeRunner::CompileShader(const std::string& hlslSource)
    {
        m_shader = nullptr;
        m_bytecode.clear();
        m_compileError.clear();

        winrt::com_ptr<ID3DBlob> blob, errors;
        HRESULT hr = D3DCompile(
            hlslSource.c_str(), hlslSource.size(),
            "D3D11ComputeEffect", nullptr, nullptr,
            "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, blob.put(), errors.put());

        if (FAILED(hr))
        {
            if (errors)
            {
                std::string msg(static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
                m_compileError = std::wstring(msg.begin(), msg.end());
            }
            else
            {
                m_compileError = L"D3DCompile failed with unknown error.";
            }
            return false;
        }

        hr = m_device->CreateComputeShader(
            blob->GetBufferPointer(), blob->GetBufferSize(),
            nullptr, m_shader.put());
        if (FAILED(hr))
        {
            m_compileError = L"CreateComputeShader failed.";
            return false;
        }

        // Stash bytecode so callers can run D3DReflect for cbuffer layout.
        const auto* src = static_cast<const uint8_t*>(blob->GetBufferPointer());
        m_bytecode.assign(src, src + blob->GetBufferSize());

        return true;
    }

    void D3D11ComputeRunner::EnsureBuffers(uint32_t resultCount)
    {
        if (m_resultCount == resultCount && m_resultBuffer) return;
        m_resultCount = resultCount;
        m_resultBuffer = nullptr;
        m_stagingBuffer = nullptr;
        m_resultUAV = nullptr;
        m_resultSRV = nullptr;
        m_cbuffer = nullptr;

        uint32_t byteSize = (std::max)(resultCount * 16u, 16u); // 16 bytes per float4

        // Structured buffer for results. Phase 8: BindFlags also include
        // SHADER_RESOURCE so a downstream consumer effect can read the
        // analysis values directly off the GPU through an SRV (the
        // IEngineComputeOutput path) without going through Map().
        D3D11_BUFFER_DESC bufDesc{};
        bufDesc.ByteWidth = byteSize;
        bufDesc.Usage = D3D11_USAGE_DEFAULT;
        bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufDesc.StructureByteStride = 16; // sizeof(float4)
        m_device->CreateBuffer(&bufDesc, nullptr, m_resultBuffer.put());

        // UAV.
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = resultCount;
        m_device->CreateUnorderedAccessView(m_resultBuffer.get(), &uavDesc, m_resultUAV.put());

        // SRV — same layout, read-only. Used by IEngineComputeOutput
        // consumers; downstream effects bind this directly to a t-slot
        // and Load() the analysis values without a CPU round-trip.
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = resultCount;
        m_device->CreateShaderResourceView(m_resultBuffer.get(), &srvDesc, m_resultSRV.put());

        // Staging buffer for readback.
        bufDesc.Usage = D3D11_USAGE_STAGING;
        bufDesc.BindFlags = 0;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        m_device->CreateBuffer(&bufDesc, nullptr, m_stagingBuffer.put());

        // Constant buffer (256 bytes max — room for Width, Height + user params).
        bufDesc.ByteWidth = 256;
        bufDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufDesc.MiscFlags = 0;
        bufDesc.StructureByteStride = 0;
        m_device->CreateBuffer(&bufDesc, nullptr, m_cbuffer.put());
    }

    void D3D11ComputeRunner::InstallPrecompiledShader(
        const std::vector<uint8_t>& bytecode,
        winrt::com_ptr<ID3D11ComputeShader> shader)
    {
        // Install pre-compiled bytecode + the live shader handle. Used
        // by CustomComputeBridgeEffect to bypass the runtime D3DCompile
        // path when the bytecode is already produced by the host (e.g.
        // an MCP /effect/compile call). Subsequent Dispatch calls use
        // the installed shader directly.
        m_bytecode = bytecode;
        m_shader = std::move(shader);
        m_compileError.clear();
    }

    std::vector<float> D3D11ComputeRunner::Dispatch(
        ID3D11Texture2D* inputTexture,
        const std::vector<BYTE>& cbufferData,
        uint32_t resultCount)
    {
        return DispatchWithImageOutput(inputTexture, cbufferData, resultCount, nullptr);
    }

    std::vector<float> D3D11ComputeRunner::DispatchWithImageOutput(
        ID3D11Texture2D* inputTexture,
        const std::vector<BYTE>& cbufferData,
        uint32_t resultCount,
        ID3D11Texture2D* imageOutputTexture,
        uint32_t dispatchX, uint32_t dispatchY, uint32_t dispatchZ,
        const std::vector<ID3D11ShaderResourceView*>& extraSrvs,
        const std::vector<uint32_t>& extraSrvSlots,
        bool readbackToCpu)
    {
        std::vector<float> result;
        if (!m_shader || !m_context || !inputTexture)
            return result;

        // Get texture dimensions.
        D3D11_TEXTURE2D_DESC texDesc{};
        inputTexture->GetDesc(&texDesc);

        // Ensure buffers are the right size. Use 1 as the floor so
        // analysis-only shaders that emit no analysis output (rare,
        // but possible in the image-producing case) still get a valid
        // SRV bound.
        uint32_t bufferSlots = (resultCount > 0) ? resultCount : 1;
        EnsureBuffers(bufferSlots);
        if (!m_resultBuffer || !m_resultUAV || !m_cbuffer) return result;

        // Create SRV for input.
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        HRESULT hr = m_device->CreateShaderResourceView(inputTexture, &srvDesc, srv.put());
        if (FAILED(hr)) return result;

        // Image-output UAV (optional).
        winrt::com_ptr<ID3D11UnorderedAccessView> imageUAV;
        if (imageOutputTexture)
        {
            D3D11_TEXTURE2D_DESC outDesc{};
            imageOutputTexture->GetDesc(&outDesc);
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = outDesc.Format;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            hr = m_device->CreateUnorderedAccessView(imageOutputTexture, &uavDesc, imageUAV.put());
            if (FAILED(hr)) return result;
            // Clear the image-output UAV so previous-frame contents
            // don't leak through when the shader writes selectively.
            float clearF[4] = { 0, 0, 0, 0 };
            m_context->ClearUnorderedAccessViewFloat(imageUAV.get(), clearF);
        }

        // Pack cbuffer: Width, Height (8 bytes) + user data.
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = m_context->Map(m_cbuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            memset(mapped.pData, 0, 256);
            auto* cb = static_cast<BYTE*>(mapped.pData);
            // Auto-inject Width, Height.
            uint32_t dims[2] = { texDesc.Width, texDesc.Height };
            memcpy(cb, dims, 8);
            // Copy user params starting at offset 8.
            if (!cbufferData.empty())
            {
                uint32_t userSize = (std::min)(static_cast<uint32_t>(cbufferData.size()), 248u);
                memcpy(cb + 8, cbufferData.data(), userSize);
            }
            m_context->Unmap(m_cbuffer.get(), 0);
        }

        // Clear analysis result buffer.
        uint32_t clearValues[4] = { 0, 0, 0, 0 };
        m_context->ClearUnorderedAccessViewUint(m_resultUAV.get(), clearValues);

        // Dispatch.
        m_context->CSSetShader(m_shader.get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { srv.get() };
        m_context->CSSetShaderResources(0, 1, srvs);

        // Phase 8: GPU-bound parameters arrive as extra SRVs. Each
        // (slot, srv) pair binds the upstream IEngineComputeOutput's
        // structured-buffer SRV at the consumer's t-slot. Bind
        // individually so non-contiguous slots (e.g. t1 and t3)
        // work without a contiguous-array constraint.
        size_t maxExtra = (std::min)(extraSrvs.size(), extraSrvSlots.size());
        for (size_t i = 0; i < maxExtra; ++i)
        {
            ID3D11ShaderResourceView* one[1] = { extraSrvs[i] };
            m_context->CSSetShaderResources(extraSrvSlots[i], 1, one);
        }

        ID3D11UnorderedAccessView* uavs[2] = { m_resultUAV.get(), imageUAV.get() };
        UINT uavCount = imageUAV ? 2u : 1u;
        m_context->CSSetUnorderedAccessViews(0, uavCount, uavs, nullptr);
        ID3D11Buffer* cbs[] = { m_cbuffer.get() };
        m_context->CSSetConstantBuffers(0, 1, cbs);

        m_context->Dispatch(
            (std::max)(dispatchX, 1u),
            (std::max)(dispatchY, 1u),
            (std::max)(dispatchZ, 1u));

        // Clear shader state.
        ID3D11ShaderResourceView* nullSRV[] = { nullptr };
        m_context->CSSetShaderResources(0, 1, nullSRV);
        for (size_t i = 0; i < maxExtra; ++i)
        {
            ID3D11ShaderResourceView* none[1] = { nullptr };
            m_context->CSSetShaderResources(extraSrvSlots[i], 1, none);
        }
        ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
        m_context->CSSetUnorderedAccessViews(0, uavCount, nullUAVs, nullptr);
        m_context->CSSetShader(nullptr, nullptr, 0);

        // Readback (only if caller asked for analysis values; image
        // output stays GPU-resident). Phase 8c: also gated by
        // `readbackToCpu` so the host can keep the result GPU-only when
        // no CPU consumer needs the values this frame -- the SRV at
        // `m_resultSRV` is unaffected and downstream GPU bindings still
        // see the buffer's freshly-written contents.
        if (resultCount > 0 && readbackToCpu)
        {
            m_context->CopyResource(m_stagingBuffer.get(), m_resultBuffer.get());
            hr = m_context->Map(m_stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr))
            {
                const float* data = static_cast<const float*>(mapped.pData);
                result.assign(data, data + resultCount * 4);
                m_context->Unmap(m_stagingBuffer.get(), 0);
            }
        }

        // Phase 8: bump the dispatch counter so downstream
        // IEngineComputeOutput consumers can detect freshness.
        ++m_lastEvaluatedFrame;

        return result;
    }
}
