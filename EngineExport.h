#pragma once
#ifdef SHADERLAB_ENGINE_EXPORTS
#define SHADERLAB_API __declspec(dllexport)
#else
#define SHADERLAB_API __declspec(dllimport)
#endif

// Engine ABI version. Bumped manually whenever a public engine symbol's
// signature or behavior changes in a way that would break a host built
// against an older header. Independent of `Version.h::VersionMajor` (the
// app version) and `Version.h::GraphFormatVersion` (the JSON schema).
//
// Hosts (the WinUI app, ShaderLabHeadless when it lands, third-party
// consumers if any) should call `ShaderLab_GetAbiVersion()` at startup
// and compare against this constant. Mismatch means the DLL is from a
// different build than the headers and is unsafe to use.
//
// **History** (compatibility-breaking changes):
//   1: Initial. Phase 6.
#define SHADERLAB_ENGINE_ABI_VERSION 1

// C-linkage entry so it can be GetProcAddress'd if a host wants to do a
// version check before dynamically loading the DLL.
extern "C" SHADERLAB_API uint32_t ShaderLab_GetAbiVersion();

