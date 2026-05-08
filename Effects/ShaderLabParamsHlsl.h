#pragma once

// shaderlab_params.hlsli — content embedded as a string constant.
//
// This header defines a small set of macros that ShaderLab's built-in
// effects and Effect Designer-authored shaders use to declare
// parameters that may be either CPU-driven (cbuffer) or GPU-driven
// (StructuredBuffer<float4> bound by the host from an upstream
// IEngineComputeOutput effect).
//
// The macro file lives engine-side as a `const char*` so the GUI app
// and the headless host don't need to ship a separate file. The
// ShaderCompiler's ID3DInclude implementation resolves
//   #include "shaderlab_params.hlsli"
// to this string, no on-disk lookup needed.

#include "../EngineExport.h"

namespace ShaderLab::Effects
{
    // Filename ShaderCompiler's include handler responds to. Embedded
    // shaders use #include "shaderlab_params.hlsli" — case-sensitive,
    // matches by filename only (no path prefix).
    inline constexpr const char* kShaderLabParamsIncludeName = "shaderlab_params.hlsli";

    // Returns the embedded macro library content. Pointer remains
    // valid for the process lifetime; do not free.
    SHADERLAB_API const char* GetShaderLabParamsHLSL();

    // Length of the embedded content in bytes (excluding any trailing
    // null terminator). Matches strlen(GetShaderLabParamsHLSL()).
    SHADERLAB_API size_t GetShaderLabParamsHLSLLength();
}
