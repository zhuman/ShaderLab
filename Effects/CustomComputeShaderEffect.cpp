#include "pch_engine.h"
#include "CustomComputeShaderEffect.h"

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    HRESULT CustomComputeShaderEffect::RegisterEffect(ID2D1Factory1* factory)
    {
        return RegisterWithInputCount(factory, CLSID_CustomComputeShader, 1);
    }

    HRESULT CustomComputeShaderEffect::RegisterWithInputCount(
        ID2D1Factory1* factory, REFCLSID clsid, UINT32 inputCount)
    {
        if (!factory || inputCount == 0 || inputCount > 8)
            return E_INVALIDARG;

        std::wstring xml =
            L"<?xml version='1.0'?>\r\n"
            L"<Effect>\r\n"
            L"  <Property name='DisplayName' type='string' value='Custom Compute Shader'/>\r\n"
            L"  <Property name='Author'      type='string' value='ShaderLab'/>\r\n"
            L"  <Property name='Category'    type='string' value='Custom'/>\r\n"
            L"  <Property name='Description' type='string' value='Runs a user-supplied compute shader.'/>\r\n"
            L"  <Inputs>\r\n";
        for (UINT32 i = 0; i < inputCount; ++i)
            xml += std::format(L"    <Input name='I{}'/>\r\n", i);
        xml += L"  </Inputs>\r\n"
               L"</Effect>\r\n";

        return factory->RegisterEffectFromString(
            clsid,
            xml.c_str(),
            nullptr,
            0,
            &CustomComputeShaderEffect::CreateFactory);
    }

    HRESULT CustomComputeShaderEffect::UnregisterEffect(ID2D1Factory1* factory)
    {
        if (!factory)
            return E_INVALIDARG;
        return factory->UnregisterEffect(CLSID_CustomComputeShader);
    }

    thread_local CustomComputeShaderEffect* CustomComputeShaderEffect::s_lastCreated = nullptr;
    thread_local UINT32 CustomComputeShaderEffect::s_pendingInputCount = 0;

    HRESULT __stdcall CustomComputeShaderEffect::CreateFactory(IUnknown** effect)
    {
        auto* impl = new (std::nothrow) CustomComputeShaderEffect();
        *effect = static_cast<ID2D1EffectImpl*>(impl);
        s_lastCreated = impl;
        return *effect ? S_OK : E_OUTOFMEMORY;
    }

    CustomComputeShaderEffect::CustomComputeShaderEffect()
    {
        if (s_pendingInputCount > 0)
            m_inputCount = s_pendingInputCount;
        // Sentinel value to detect if MapInputRectsToOutputRect was called.
        m_inputRect = { 0, 0, 0, 0 };
    }

    // -----------------------------------------------------------------------
    // IUnknown
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
            return E_POINTER;

        *ppv = nullptr;

        if (riid == __uuidof(ID2D1EffectImpl))
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1ComputeTransform))
            *ppv = static_cast<ID2D1ComputeTransform*>(this);
        else if (riid == __uuidof(ID2D1Transform))
            *ppv = static_cast<ID2D1Transform*>(this);
        else if (riid == __uuidof(ID2D1TransformNode))
            *ppv = static_cast<ID2D1TransformNode*>(this);
        else if (riid == __uuidof(IUnknown))
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else
            return E_NOINTERFACE;

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) CustomComputeShaderEffect::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) CustomComputeShaderEffect::Release()
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0)
            delete this;
        return count;
    }

    // -----------------------------------------------------------------------
    // ID2D1EffectImpl
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);
        m_transformGraph.copy_from(transformGraph);

        // Check compute shader support (feature level 11_0+ required).
        if (!effectContext->IsShaderLoaded(CLSID_CustomComputeShader))
        {
            D2D1_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS hardwareOptions{};
            HRESULT hr = effectContext->CheckFeatureSupport(
                D2D1_FEATURE_D3D10_X_HARDWARE_OPTIONS,
                &hardwareOptions,
                sizeof(hardwareOptions));

            if (FAILED(hr) || !hardwareOptions.computeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x)
            {
                // Compute shaders not supported on this hardware.
                return D2DERR_INSUFFICIENT_DEVICE_CAPABILITIES;
            }
        }

        return transformGraph->SetSingleTransformNode(
            static_cast<ID2D1ComputeTransform*>(this));
    }

    IFACEMETHODIMP CustomComputeShaderEffect::PrepareForRender(D2D1_CHANGE_TYPE /*changeType*/)
    {
        if (!m_computeInfo)
            return E_FAIL;

        // If no shader has been loaded yet, nothing to render.
        if (m_shaderBytecode.empty() && !m_shaderLoaded)
            return E_FAIL;

        // If the shader bytecode changed, load it into D2D.
        if (m_shaderDirty && !m_shaderBytecode.empty())
        {
            if (m_shaderGuid == GUID{})
                CoCreateGuid(&m_shaderGuid);

            HRESULT hr = m_effectContext->LoadComputeShader(
                m_shaderGuid,
                m_shaderBytecode.data(),
                static_cast<UINT32>(m_shaderBytecode.size()));

            if (SUCCEEDED(hr))
                hr = m_computeInfo->SetComputeShader(m_shaderGuid);

            if (FAILED(hr))
                return hr;

            m_shaderDirty = false;
            m_shaderLoaded = true;
        }

        // DO NOT upload cbuffer here — D2D latches the cbuffer state at this
        // point, preventing per-tile updates in CalculateThreadgroups from
        // taking effect. We upload exclusively in CalculateThreadgroups where
        // we can inject correct _TileOffset and _ImageSize per tile.

        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::SetGraph(ID2D1TransformGraph* transformGraph)
    {
        m_transformGraph.copy_from(transformGraph);
        return transformGraph->SetSingleTransformNode(
            static_cast<ID2D1ComputeTransform*>(this));
    }

    // -----------------------------------------------------------------------
    // ID2D1ComputeTransform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::SetComputeInfo(ID2D1ComputeInfo* computeInfo)
    {
        m_computeInfo.copy_from(computeInfo);

        // Set output buffer to FP16 for HDR/WCG support.
        computeInfo->SetOutputBuffer(D2D1_BUFFER_PRECISION_16BPC_FLOAT, D2D1_CHANNEL_DEPTH_4);

        if (!m_shaderBytecode.empty())
            m_shaderDirty = true;

        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::CalculateThreadgroups(
        const D2D1_RECT_L* outputRect,
        UINT32* dimensionX,
        UINT32* dimensionY,
        UINT32* dimensionZ)
    {
        UINT32 width  = static_cast<UINT32>(outputRect->right - outputRect->left);
        UINT32 height = static_cast<UINT32>(outputRect->bottom - outputRect->top);
        if (width == 0) width = 1;
        if (height == 0) height = 1;

        // Upload the constant buffer here (NOT in PrepareForRender) so that
        // per-tile system constants are correct for each tile dispatch.
        // D2D calls CalculateThreadgroups once per tile, after
        // MapInputRectsToOutputRect has set the tile region.
        if (m_computeInfo && !m_constantBuffer.empty())
        {
            // Inject system constant into reserved cbuffer prefix:
            //   offset  0: int2 _TileOffset  (tile origin in full image)
            // User properties start at offset 8+.
            // NOTE: _ImageSize is NOT injected — shaders must use
            // Source.GetDimensions() which returns the full source size.
            if (m_constantBuffer.size() >= 8)
            {
                int32_t offX = outputRect->left;
                int32_t offY = outputRect->top;
                memcpy(m_constantBuffer.data() + 0, &offX, sizeof(int32_t));
                memcpy(m_constantBuffer.data() + 4, &offY, sizeof(int32_t));
            }

            m_computeInfo->SetComputeShaderConstantBuffer(
                m_constantBuffer.data(),
                static_cast<UINT32>(m_constantBuffer.size()));
            m_cbDirty = false;
        }

        *dimensionX = (width  + m_threadGroupX - 1) / m_threadGroupX;
        *dimensionY = (height + m_threadGroupY - 1) / m_threadGroupY;
        *dimensionZ = 1;

        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1Transform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomComputeShaderEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects,
        const D2D1_RECT_L* /*inputOpaqueSubRects*/,
        UINT32 inputRectCount,
        D2D1_RECT_L* outputRect,
        D2D1_RECT_L* outputOpaqueSubRect)
    {
        if (inputRectCount > 0 && inputRects)
        {
            m_inputRect = inputRects[0];
            *outputRect = inputRects[0];
        }
        else
        {
            m_inputRect = D2D1_RECT_L{ 0, 0, 0, 0 };
            *outputRect = m_inputRect;
        }

        *outputOpaqueSubRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* outputRect,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        // If MapInputRectsToOutputRect hasn't been called yet (no valid input rect),
        // fall back to the requested output rect so D2D gets reasonable bounds.
        D2D1_RECT_L rect = m_inputRect;
        if (rect.left == 0 && rect.top == 0 && rect.right == 0 && rect.bottom == 0 && outputRect)
            rect = *outputRect;

        for (UINT32 i = 0; i < inputRectCount; ++i)
        {
            inputRects[i] = rect;
        }
        return S_OK;
    }

    IFACEMETHODIMP CustomComputeShaderEffect::MapInvalidRect(
        UINT32 /*inputIndex*/,
        D2D1_RECT_L /*invalidInputRect*/,
        D2D1_RECT_L* invalidOutputRect) const
    {
        // Return the stored input rect, or empty if not yet initialized.
        *invalidOutputRect = m_inputRect;
        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1TransformNode
    // -----------------------------------------------------------------------

    IFACEMETHODIMP_(UINT32) CustomComputeShaderEffect::GetInputCount() const
    {
        return m_inputCount;
    }

    // -----------------------------------------------------------------------
    // Custom property accessors
    // -----------------------------------------------------------------------

    HRESULT CustomComputeShaderEffect::SetInputCount(UINT32 count)
    {
        if (count == m_inputCount)
            return S_OK;

        m_inputCount = count;

        if (m_transformGraph)
        {
            return m_transformGraph->SetSingleTransformNode(
                static_cast<ID2D1ComputeTransform*>(this));
        }
        return S_OK;
    }

    UINT32 CustomComputeShaderEffect::GetInputCountProp() const
    {
        return m_inputCount;
    }

    // -----------------------------------------------------------------------
    // Host-facing API (outside D2D property system)
    // -----------------------------------------------------------------------

    HRESULT CustomComputeShaderEffect::LoadShaderBytecode(ID3DBlob* bytecode)
    {
        if (!bytecode)
            return E_INVALIDARG;
        return LoadShaderBytecode(
            static_cast<const BYTE*>(bytecode->GetBufferPointer()),
            static_cast<UINT32>(bytecode->GetBufferSize()));
    }

    HRESULT CustomComputeShaderEffect::LoadShaderBytecode(const BYTE* data, UINT32 dataSize)
    {
        if (!data || dataSize == 0)
            return E_INVALIDARG;
        m_shaderBytecode.assign(data, data + dataSize);
        m_shaderDirty = true;
        return S_OK;
    }

    void CustomComputeShaderEffect::SetConstantBufferData(const BYTE* data, UINT32 dataSize)
    {
        // D3D11 constant buffers must be multiples of 16 bytes.
        UINT32 paddedSize = (dataSize + 15) & ~15u;
        m_constantBuffer.assign(paddedSize, 0);
        if (data && dataSize > 0)
            memcpy(m_constantBuffer.data(), data, dataSize);
        m_cbDirty = true;
    }

    HRESULT CustomComputeShaderEffect::ForceUploadConstantBuffer()
    {
        if (!m_computeInfo || m_constantBuffer.empty())
            return E_FAIL;
        m_cbDirty = false;
        return m_computeInfo->SetComputeShaderConstantBuffer(
            m_constantBuffer.data(),
            static_cast<UINT32>(m_constantBuffer.size()));
    }

    void CustomComputeShaderEffect::PackConstantBuffer(
        const std::map<std::wstring, Graph::PropertyValue>& properties,
        const std::vector<ShaderVariable>& variables,
        uint32_t cbSizeBytes)
    {
        // Pad to 16-byte boundary for D3D11 constant buffer alignment.
        uint32_t paddedSize = (cbSizeBytes + 15) & ~15u;
        m_constantBuffer.resize(paddedSize, 0);

        for (const auto& var : variables)
        {
            auto it = properties.find(var.name);
            if (it == properties.end())
                continue;
            if (var.offset >= cbSizeBytes)
                continue;

            BYTE* dest = m_constantBuffer.data() + var.offset;
            uint32_t remaining = cbSizeBytes - var.offset;

            // Typed pack: see ShaderCompiler.cpp::PackPropertyToCBuffer (Phase 3).
            PackPropertyToCBuffer(dest, remaining, var.type, var.columns, it->second);
        }

        m_cbDirty = true;
    }

    void CustomComputeShaderEffect::SetThreadGroupSize(UINT32 x, UINT32 y, UINT32 z)
    {
        m_threadGroupX = (std::max)(x, 1u);
        m_threadGroupY = (std::max)(y, 1u);
        m_threadGroupZ = (std::max)(z, 1u);
    }
}
