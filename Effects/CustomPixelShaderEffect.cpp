#include "pch_engine.h"
#include "CustomPixelShaderEffect.h"

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Property bindings table (used by D2D effect registration)
    // -----------------------------------------------------------------------

    // Custom property index (only InputCount is exposed to D2D).
    // Shader bytecode and constant buffer are managed internally
    // via direct pointer access from GraphEvaluator.
    enum : UINT32
    {
        PROP_INPUT_COUNT = 0,
    };

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    HRESULT CustomPixelShaderEffect::RegisterEffect(ID2D1Factory1* factory)
    {
        // Default registration with 1 input (used as fallback).
        return RegisterWithInputCount(factory, CLSID_CustomPixelShader, 1);
    }

    HRESULT CustomPixelShaderEffect::RegisterWithInputCount(
        ID2D1Factory1* factory, REFCLSID clsid, UINT32 inputCount)
    {
        if (!factory || inputCount == 0 || inputCount > 8)
            return E_INVALIDARG;

        // Build XML with the exact number of fixed inputs.
        std::wstring xml =
            L"<?xml version='1.0'?>\r\n"
            L"<Effect>\r\n"
            L"  <Property name='DisplayName' type='string' value='Custom Pixel Shader'/>\r\n"
            L"  <Property name='Author'      type='string' value='ShaderLab'/>\r\n"
            L"  <Property name='Category'    type='string' value='Custom'/>\r\n"
            L"  <Property name='Description' type='string' value='Runs a user-supplied pixel shader.'/>\r\n"
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
            &CustomPixelShaderEffect::CreateFactory);
    }

    HRESULT CustomPixelShaderEffect::UnregisterEffect(ID2D1Factory1* factory)
    {
        if (!factory)
            return E_INVALIDARG;
        return factory->UnregisterEffect(CLSID_CustomPixelShader);
    }

    // Thread-local for capturing the last-created impl pointer.
    thread_local CustomPixelShaderEffect* CustomPixelShaderEffect::s_lastCreated = nullptr;
    thread_local UINT32 CustomPixelShaderEffect::s_pendingInputCount = 0;

    HRESULT __stdcall CustomPixelShaderEffect::CreateFactory(IUnknown** effect)
    {
        auto* impl = new (std::nothrow) CustomPixelShaderEffect();
        *effect = static_cast<ID2D1EffectImpl*>(impl);
        s_lastCreated = impl;
        return *effect ? S_OK : E_OUTOFMEMORY;
    }

    CustomPixelShaderEffect::CustomPixelShaderEffect()
    {
        // Use the pending input count set before CreateEffect, so that
        // Initialize → SetSingleTransformNode sees the correct value
        // from GetInputCount() immediately.
        if (s_pendingInputCount > 0)
            m_inputCount = s_pendingInputCount;
    }

    // -----------------------------------------------------------------------
    // IUnknown
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
            return E_POINTER;

        *ppv = nullptr;

        if (riid == __uuidof(ID2D1EffectImpl))
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1DrawTransform))
            *ppv = static_cast<ID2D1DrawTransform*>(this);
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

    IFACEMETHODIMP_(ULONG) CustomPixelShaderEffect::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) CustomPixelShaderEffect::Release()
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0)
            delete this;
        return count;
    }

    // -----------------------------------------------------------------------
    // ID2D1EffectImpl
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::Initialize(
        ID2D1EffectContext* effectContext,
        ID2D1TransformGraph* transformGraph)
    {
        m_effectContext.copy_from(effectContext);
        m_transformGraph.copy_from(transformGraph);

        // Set ourselves as the single transform node in the graph.
        return transformGraph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
    }

    IFACEMETHODIMP CustomPixelShaderEffect::PrepareForRender(D2D1_CHANGE_TYPE /*changeType*/)
    {
        if (!m_drawInfo)
            return E_FAIL;

        // If the shader bytecode changed, load it into D2D.
        if (m_shaderDirty && !m_shaderBytecode.empty())
        {
            if (m_shaderGuid == GUID{})
                CoCreateGuid(&m_shaderGuid);

            HRESULT hr = m_effectContext->LoadPixelShader(
                m_shaderGuid,
                m_shaderBytecode.data(),
                static_cast<UINT32>(m_shaderBytecode.size()));

            if (SUCCEEDED(hr))
                hr = m_drawInfo->SetPixelShader(m_shaderGuid,
                    D2D1_PIXEL_OPTIONS_TRIVIAL_SAMPLING);

            if (FAILED(hr))
                return hr;

            m_shaderDirty = false;
        }

        // If the constant buffer data changed, push it to the GPU.
        if (m_cbDirty && !m_constantBuffer.empty())
        {
            HRESULT hr = m_drawInfo->SetPixelShaderConstantBuffer(
                m_constantBuffer.data(),
                static_cast<UINT32>(m_constantBuffer.size()));

            if (FAILED(hr))
                return hr;

            m_cbDirty = false;
        }

        return S_OK;
    }

    IFACEMETHODIMP CustomPixelShaderEffect::SetGraph(ID2D1TransformGraph* transformGraph)
    {
        m_transformGraph.copy_from(transformGraph);
        return transformGraph->SetSingleTransformNode(static_cast<ID2D1DrawTransform*>(this));
    }

    // -----------------------------------------------------------------------
    // ID2D1DrawTransform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::SetDrawInfo(ID2D1DrawInfo* drawInfo)
    {
        m_drawInfo.copy_from(drawInfo);

        if (!m_shaderBytecode.empty())
            m_shaderDirty = true;

        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1Transform
    // -----------------------------------------------------------------------

    IFACEMETHODIMP CustomPixelShaderEffect::MapInputRectsToOutputRect(
        const D2D1_RECT_L* inputRects,
        const D2D1_RECT_L* /*inputOpaqueSubRects*/,
        UINT32 inputRectCount,
        D2D1_RECT_L* outputRect,
        D2D1_RECT_L* outputOpaqueSubRect)
    {
        // Source effects with a fixed output size always use it,
        // regardless of input rect (which may be a dummy bitmap).
        if (m_fixedOutputWidth > 0 && m_fixedOutputHeight > 0)
        {
            *outputRect = D2D1_RECT_L{ 0, 0,
                static_cast<LONG>(m_fixedOutputWidth),
                static_cast<LONG>(m_fixedOutputHeight) };
            m_inputRect = *outputRect;
            m_lastOutputRect = *outputRect;
        }
        else if (inputRectCount > 0 && inputRects)
        {
            m_inputRect = inputRects[0];

            // Compute union of input rects.
            *outputRect = inputRects[0];
            for (UINT32 i = 1; i < inputRectCount; ++i)
            {
                outputRect->left   = (std::min)(outputRect->left,   inputRects[i].left);
                outputRect->top    = (std::min)(outputRect->top,    inputRects[i].top);
                outputRect->right  = (std::max)(outputRect->right,  inputRects[i].right);
                outputRect->bottom = (std::max)(outputRect->bottom, inputRects[i].bottom);
            }

            outputRect->left   = (std::max)(outputRect->left,   0L);
            outputRect->top    = (std::max)(outputRect->top,    0L);
            outputRect->right  = (std::min)(outputRect->right,  4096L);
            outputRect->bottom = (std::min)(outputRect->bottom, 4096L);

            m_lastOutputRect = *outputRect;
        }
        else
        {
            *outputRect = D2D1_RECT_L{ 0, 0, 0, 0 };
            m_lastOutputRect = *outputRect;
        }

        *outputOpaqueSubRect = D2D1_RECT_L{ 0, 0, 0, 0 };
        return S_OK;
    }

    IFACEMETHODIMP CustomPixelShaderEffect::MapOutputRectToInputRects(
        const D2D1_RECT_L* /*outputRect*/,
        D2D1_RECT_L* inputRects,
        UINT32 inputRectCount) const
    {
        // Always return the FULL output rect as the input demand, ignoring
        // the queried sub-rect. This is the safe-with-TRIVIAL_SAMPLING
        // baseline: it ensures the intermediate textures match the full
        // effect output rect so TEXCOORD maps 1:1 with GetDimensions()-
        // normalized math, and so thumbnail / sub-rect previews show the
        // *whole* effect output rather than just the queried region.
        //
        // An earlier attempt at honoring the queried sub-rect to enable
        // D2Ds lazy-eval propagation (for preview-resolution scaling and
        // per-input-rect routing) caused the symptom that thumbnail-
        // sized preview panes rendered only a small sub-rect of the
        // chain into the upper-left of the canvas. Reverted; preview-
        // resolution scaling is shipped as the explicit Scale catalog
        // node instead, which constrains the source-side rect rather
        // than relying on leaf-side sub-rect demand propagation.
        for (UINT32 i = 0; i < inputRectCount; ++i)
        {
            inputRects[i] = m_lastOutputRect;
        }
        return S_OK;
    }

    IFACEMETHODIMP CustomPixelShaderEffect::MapInvalidRect(
        UINT32 /*inputIndex*/,
        D2D1_RECT_L invalidInputRect,
        D2D1_RECT_L* invalidOutputRect) const
    {
        // Any change in an input invalidates the same region of the output.
        *invalidOutputRect = invalidInputRect;
        return S_OK;
    }

    // -----------------------------------------------------------------------
    // ID2D1TransformNode
    // -----------------------------------------------------------------------

    IFACEMETHODIMP_(UINT32) CustomPixelShaderEffect::GetInputCount() const
    {
        return m_inputCount;
    }

    // -----------------------------------------------------------------------
    // Custom property accessors
    // -----------------------------------------------------------------------

    HRESULT CustomPixelShaderEffect::SetInputCount(UINT32 count)
    {
        if (count == m_inputCount)
            return S_OK;

        m_inputCount = count;

        // Update the D2D transform graph to reflect the new input count.
        if (m_transformGraph)
        {
            return m_transformGraph->SetSingleTransformNode(
                static_cast<ID2D1DrawTransform*>(this));
        }
        return S_OK;
    }

    UINT32 CustomPixelShaderEffect::GetInputCountProp() const
    {
        return m_inputCount;
    }

    // -----------------------------------------------------------------------
    // Host-facing API (outside D2D property system)
    // -----------------------------------------------------------------------

    HRESULT CustomPixelShaderEffect::LoadShaderBytecode(ID3DBlob* bytecode)
    {
        if (!bytecode)
            return E_INVALIDARG;
        return LoadShaderBytecode(
            static_cast<const BYTE*>(bytecode->GetBufferPointer()),
            static_cast<UINT32>(bytecode->GetBufferSize()));
    }

    HRESULT CustomPixelShaderEffect::LoadShaderBytecode(const BYTE* data, UINT32 dataSize)
    {
        if (!data || dataSize == 0)
            return E_INVALIDARG;
        m_shaderBytecode.assign(data, data + dataSize);
        m_shaderDirty = true;
        return S_OK;
    }

    void CustomPixelShaderEffect::SetConstantBufferData(const BYTE* data, UINT32 dataSize)
    {
        m_constantBuffer.assign(data, data + dataSize);
        m_cbDirty = true;
    }

    HRESULT CustomPixelShaderEffect::ForceUploadConstantBuffer()
    {
        if (!m_drawInfo || m_constantBuffer.empty())
            return E_FAIL;
        m_cbDirty = false;
        return m_drawInfo->SetPixelShaderConstantBuffer(
            m_constantBuffer.data(),
            static_cast<UINT32>(m_constantBuffer.size()));
    }

    // -----------------------------------------------------------------------
    // Convenience: pack PropertyValue map into constant buffer
    // -----------------------------------------------------------------------

    void CustomPixelShaderEffect::PackConstantBuffer(
        const std::map<std::wstring, Graph::PropertyValue>& properties,
        const std::vector<ShaderVariable>& variables,
        uint32_t cbSizeBytes)
    {
        // Allocate and zero the constant buffer.
        m_constantBuffer.resize(cbSizeBytes, 0);

        for (const auto& var : variables)
        {
            auto it = properties.find(var.name);
            if (it == properties.end())
                continue;
            if (var.offset >= cbSizeBytes)
                continue;

            BYTE* dest = m_constantBuffer.data() + var.offset;
            uint32_t remaining = cbSizeBytes - var.offset;

            // Typed pack: converts float -> uint/int/bool and respects
            // the declared HLSL cbuffer slot type. See
            // ShaderCompiler.cpp::PackPropertyToCBuffer (Phase 3).
            PackPropertyToCBuffer(dest, remaining, var.type, var.columns, it->second);
        }

        m_cbDirty = true;
    }
}
