#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Effects
{
    // Describes a pre-built ShaderLab effect with embedded HLSL.
    struct ShaderLabEffectDescriptor
    {
        std::wstring name;
        std::wstring category;      // "Analysis", "Source", "Parameter"
        std::wstring subcategory;   // Optional second-tier grouping under
                                    // category (e.g. "Highlights", "Scopes",
                                    // "Gamut Mapping"). Empty = ungrouped,
                                    // shown directly under the category.
        Graph::CustomShaderType shaderType{ Graph::CustomShaderType::PixelShader };

        // Stable identifier and version for upgrade detection.
        std::wstring effectId;          // Stable ID (survives renames)
        uint32_t effectVersion{ 1 };    // Increment when HLSL or params change

        // Embedded HLSL source — compiled on first use.
        std::string hlslSource;

        // Named texture inputs.
        std::vector<std::wstring> inputNames;

        // Declared parameters with defaults + UI metadata.
        std::vector<Graph::ParameterDefinition> parameters;

        // Analysis output fields (for compute shaders).
        std::vector<Graph::AnalysisFieldDescriptor> analysisFields;
        Graph::AnalysisOutputType analysisOutputType{ Graph::AnalysisOutputType::None };

        // Compute shader thread group dimensions.
        uint32_t threadGroupX{ 8 };
        uint32_t threadGroupY{ 8 };
        uint32_t threadGroupZ{ 1 };

        // Hidden default properties (in cbuffer but not shown in Properties panel).
        // Set on node creation; auto-updated by the evaluator for dynamic values.
        std::map<std::wstring, Graph::PropertyValue> hiddenDefaults;

        // Data-only effects have no visible image output pin. They produce
        // analysis output fields but their image output is internal only.
        bool dataOnly{ false };

        // Image-producing compute effects output a texture (not just analysis data).
        // Creates an output pin so downstream pixel shaders can read the result.
        bool hasImageOutput{ false };

        // Clock nodes are time-based animation sources (special render loop handling).
        bool isClock{ false };
    };

    // Registry of all ShaderLab pre-built effects.
    class SHADERLAB_API ShaderLabEffects
    {
    public:
        static ShaderLabEffects& Instance();

        const ShaderLabEffectDescriptor* FindByName(std::wstring_view name) const;
        const ShaderLabEffectDescriptor* FindById(std::wstring_view effectId) const;
        const std::vector<ShaderLabEffectDescriptor>& All() const { return m_effects; }

        // Computed library version: sum of all effect versions.
        uint32_t LibraryVersion() const
        {
            uint32_t v = 0;
            for (const auto& e : m_effects) v += e.effectVersion;
            return v;
        }
        std::vector<const ShaderLabEffectDescriptor*> ByCategory(std::wstring_view category) const;
        std::vector<std::wstring> Categories() const;

        // Create a fully-configured EffectNode from a descriptor.
        static Graph::EffectNode CreateNode(const ShaderLabEffectDescriptor& desc);

    private:
        ShaderLabEffects();
        void RegisterAll();

        std::vector<ShaderLabEffectDescriptor> m_effects;
    };

    SHADERLAB_API void RegisterEngineD2DEffects(ID2D1Factory1* factory);

    // Configure the on-disk bytecode cache for this process. Pass an
    // empty `rootPath` to disable disk persistence (tests, headless
    // ad-hoc runs). When non-empty, the directory is created if
    // missing, and a background thread reaps entries older than
    // `staleThresholdSec` (default 90 days) so the cache doesn't
    // grow without bound. Pass 0 for staleThresholdSec to skip the
    // background reap (the user can still trigger it manually via
    // the status-bar broom button).
    //
    // Recommended: pass `%LOCALAPPDATA%\ShaderLab\bytecode\`. The
    // engine does NOT pick a default automatically -- the host owns
    // the path policy so headless/test runs can opt out cleanly.
    SHADERLAB_API void ConfigureBytecodeCache(
        std::wstring rootPath,
        uint64_t staleThresholdSec = 90ull * 24 * 60 * 60);

    // Shared HLSL color math functions (prepended to all ShaderLab shaders).
    SHADERLAB_API const std::string& GetColorMathHLSL();
}
