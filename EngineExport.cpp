#include "pch_engine.h"
#include "EngineExport.h"

extern "C" SHADERLAB_API uint32_t ShaderLab_GetAbiVersion()
{
    return SHADERLAB_ENGINE_ABI_VERSION;
}
