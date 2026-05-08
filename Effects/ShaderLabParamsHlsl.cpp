#include "pch_engine.h"
#include "ShaderLabParamsHlsl.h"

#include <cstring>

namespace ShaderLab::Effects
{
    // The macro file content. See ShaderLabParamsHlsl.h for usage notes.
    //
    // Per-parameter binding mode is injected by the host as a numeric
    // macro: _SLPARAM_<name>_GPU == 0 (cbuffer) or 1 (texture-bound).
    // The host always injects the macro for every gpu-bindable param,
    // even when no binding is active, so the effect's source compiles
    // identically regardless of binding state.
    //
    // Authors write three pieces (file scope, cbuffer body, function body):
    //   SHADERLAB_GPU_BUFFER(TargetPeakNits, t1)        // file scope
    //   SHADERLAB_PARAM(float, TargetPeakNits)          // inside cbuffer
    //   SHADERLAB_LOAD_PARAM(float, TargetPeakNits)     // inside main()
    // and use TargetPeakNits as a normal local downstream. The host
    // does the rest: emits _SLPARAM_TargetPeakNits_GPU=0|1 and, when 1,
    // binds an SRV to the right t-slot.
    static constexpr const char* kSource = R"HLSL(
// shaderlab_params.hlsli — engine-embedded macro library.
// See Effects/ShaderLabParamsHlsl.h for documentation.
#ifndef SHADERLAB_PARAMS_HLSLI_INCLUDED
#define SHADERLAB_PARAMS_HLSLI_INCLUDED

// Token-paste indirection for proper macro argument expansion.
#define _SLPARAM_CAT(a, b) _SLPARAM_CAT_(a, b)
#define _SLPARAM_CAT_(a, b) a##b

// Type-aware swizzle helpers: pull the right components from a float4
// storage slot for any of float / float2 / float3 / float4.
#define _SL_VEC_float(v)  (v).x
#define _SL_VEC_float2(v) (v).xy
#define _SL_VEC_float3(v) (v).xyz
#define _SL_VEC_float4(v) (v)

// File-scope buffer declaration. Expands to a StructuredBuffer<float4>
// when the parameter is GPU-bound; expands to nothing when the value
// lives in the cbuffer.
#define SHADERLAB_GPU_BUFFER(name, slot) \
    _SLPARAM_CAT(_SLBUF_, _SLPARAM_##name##_GPU)(name, slot)
#define _SLBUF_0(name, slot)
#define _SLBUF_1(name, slot) StructuredBuffer<float4> _SLBuf_##name : register(slot);

// Inside cbuffer block. Expands to a value slot when cbuffer-bound;
// expands to a uint INDEX slot when GPU-bound (the host packs the
// upstream field's index in the analysis SRV at this offset).
#define SHADERLAB_PARAM(type, name) \
    _SLPARAM_CAT(_SLPARAM_, _SLPARAM_##name##_GPU)(type, name)
#define _SLPARAM_0(type, name) type name;
#define _SLPARAM_1(type, name) uint _SLIdx_##name;

// Inside function body, before any use of the parameter. Expands to
// a local-variable assignment when GPU-bound (read float4 at the
// host-supplied index, swizzle to the requested type); expands to
// nothing when cbuffer-bound (the cbuffer slot is already in scope
// as a global).
#define SHADERLAB_LOAD_PARAM(type, name) \
    _SLPARAM_CAT(_SLLOAD_, _SLPARAM_##name##_GPU)(type, name)
#define _SLLOAD_0(type, name)
#define _SLLOAD_1(type, name) type name = _SL_VEC_##type(_SLBuf_##name[_SLIdx_##name]);

#endif // SHADERLAB_PARAMS_HLSLI_INCLUDED
)HLSL";

    const char* GetShaderLabParamsHLSL() { return kSource; }
    size_t      GetShaderLabParamsHLSLLength() { return std::strlen(kSource); }
}
