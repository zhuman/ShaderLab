#pragma once

#include "pch_engine.h"
#include "NodeType.h"
#include "PropertyValue.h"

namespace ShaderLab::Graph
{
    // Describes a single input or output pin on a node.
    struct PinDescriptor
    {
        std::wstring name;
        uint32_t     index{ 0 };
    };

    // Shader type for custom effects.
    enum class CustomShaderType
    {
        PixelShader,
        ComputeShader,
        D3D11ComputeShader,  // Raw D3D11 compute (not D2D-tiled, full atomics/groupshared)
    };

    // Describes a user-defined parameter for a custom effect.
    struct ParameterDefinition
    {
        std::wstring name;
        std::wstring typeName;       // "float", "float2", "float3", "float4", "int", "uint", "bool"
        PropertyValue defaultValue;
        float minValue{ 0.0f };
        float maxValue{ 1.0f };
        float step{ 0.01f };

        // Named labels for uint enum values (index → display name).
        // When non-empty, the Properties panel renders a ComboBox instead of a NumberBox.
        std::vector<std::wstring> enumLabels;

        // Conditional visibility: "ParamName == value" hides this parameter
        // unless the condition is met. Empty = always visible.
        // Supports ==, !=, <, <=, >, >= with numeric comparison.
        std::wstring visibleWhen;

        // GPU-binding eligibility (Phase 8, opt-in). When true, this
        // parameter's HLSL declaration uses the SHADERLAB_PARAM /
        // SHADERLAB_LOAD_PARAM macros from `shaderlab_params.hlsli`,
        // and a property binding from an upstream IEngineComputeOutput
        // can route the value GPU→GPU. The host injects the
        // `_SLPARAM_<name>_GPU=0|1` macro at compile time and (when 1)
        // binds the upstream SRV to the corresponding t-slot.
        // Default false: parameter is cbuffer-only, current behavior.
        bool gpuBindable{ false };
    };

    // Evaluate a visibleWhen condition against current property values.
    // Returns true (visible) if the condition is empty, unparseable, or satisfied.
    inline bool EvaluateVisibleWhen(
        const std::wstring& condition,
        const std::map<std::wstring, PropertyValue>& props)
    {
        if (condition.empty()) return true;

        // Parse: "ParamName op value"
        // Find operator position
        size_t opPos = std::wstring::npos;
        size_t opLen = 0;
        struct { const wchar_t* str; size_t len; } ops[] = {
            { L"==", 2 }, { L"!=", 2 }, { L"<=", 2 }, { L">=", 2 }, { L"<", 1 }, { L">", 1 }
        };
        for (auto& op : ops)
        {
            size_t p = condition.find(op.str);
            if (p != std::wstring::npos) { opPos = p; opLen = op.len; break; }
        }
        if (opPos == std::wstring::npos) return true; // Unparseable, fail open.

        // Extract param name (trim spaces).
        std::wstring paramName = condition.substr(0, opPos);
        while (!paramName.empty() && paramName.back() == L' ') paramName.pop_back();
        std::wstring op = condition.substr(opPos, opLen);

        // Extract value (trim spaces).
        std::wstring valStr = condition.substr(opPos + opLen);
        while (!valStr.empty() && valStr.front() == L' ') valStr.erase(valStr.begin());

        float rhs = 0;
        try { rhs = std::stof(valStr); } catch (...) { return true; }

        // Look up the property value.
        auto it = props.find(paramName);
        if (it == props.end()) return true; // Missing param, fail open.

        float lhs = 0;
        if (auto* f = std::get_if<float>(&it->second)) lhs = *f;
        else if (auto* i = std::get_if<int32_t>(&it->second)) lhs = static_cast<float>(*i);
        else if (auto* u = std::get_if<uint32_t>(&it->second)) lhs = static_cast<float>(*u);
        else if (auto* b = std::get_if<bool>(&it->second)) lhs = *b ? 1.0f : 0.0f;
        else return true; // Non-numeric, fail open.

        if (op == L"==") return std::abs(lhs - rhs) < 0.001f;
        if (op == L"!=") return std::abs(lhs - rhs) >= 0.001f;
        if (op == L"<")  return lhs < rhs;
        if (op == L"<=") return lhs <= rhs;
        if (op == L">")  return lhs > rhs;
        if (op == L">=") return lhs >= rhs;
        return true;
    }

    // Identifies the type of non-image data produced by analysis/compute effects.
    enum class AnalysisOutputType
    {
        None,
        Histogram,      // Built-in D2D histogram (256 float bins).
        FloatBuffer,    // Custom compute: array of float4 values read from output pixels.
        Typed,          // Custom compute: typed named fields (replaces old KeyValue).
    };

    // Type of a single analysis output field.
    enum class AnalysisFieldType
    {
        Float,          // 1 component, 1 pixel
        Float2,         // 2 components, 1 pixel
        Float3,         // 3 components, 1 pixel
        Float4,         // 4 components, 1 pixel
        FloatArray,     // N floats packed 4-per-pixel (ceil(N/4) pixels)
        Float2Array,    // N×float2, 1 pixel each (.xy used)
        Float3Array,    // N×float3, 1 pixel each (.xyz used)
        Float4Array,    // N×float4, 1 pixel each
    };

    // Returns the number of scalar components per element for a field type.
    inline uint32_t AnalysisFieldComponentCount(AnalysisFieldType type)
    {
        switch (type)
        {
        case AnalysisFieldType::Float:       return 1;
        case AnalysisFieldType::Float2:      return 2;
        case AnalysisFieldType::Float3:      return 3;
        case AnalysisFieldType::Float4:      return 4;
        case AnalysisFieldType::FloatArray:  return 1;
        case AnalysisFieldType::Float2Array: return 2;
        case AnalysisFieldType::Float3Array: return 3;
        case AnalysisFieldType::Float4Array: return 4;
        default: return 4;
        }
    }

    // Returns true if the field type is an array type.
    inline bool AnalysisFieldIsArray(AnalysisFieldType type)
    {
        return type >= AnalysisFieldType::FloatArray;
    }

    // Returns the number of UAV pixels consumed by one field.
    inline uint32_t AnalysisFieldPixelCount(AnalysisFieldType type, uint32_t arrayLength)
    {
        switch (type)
        {
        case AnalysisFieldType::Float:
        case AnalysisFieldType::Float2:
        case AnalysisFieldType::Float3:
        case AnalysisFieldType::Float4:
            return 1;
        case AnalysisFieldType::FloatArray:
            return (arrayLength + 3) / 4;  // pack 4 floats per pixel
        case AnalysisFieldType::Float2Array:
        case AnalysisFieldType::Float3Array:
        case AnalysisFieldType::Float4Array:
            return arrayLength;  // 1 pixel per element
        default: return 1;
        }
    }

    // Describes one output field of an analysis compute shader.
    struct AnalysisFieldDescriptor
    {
        std::wstring name;
        AnalysisFieldType type{ AnalysisFieldType::Float4 };
        uint32_t arrayLength{ 0 };  // only for array types; max 4096 elements

        // GPU-binding publication (Phase 8, opt-in). When true, the
        // upstream effect implementing IEngineComputeOutput exposes
        // this field's slot as readable from a downstream
        // SHADERLAB_GPU_BUFFER binding. The slot index in the
        // structured-buffer SRV is the field's position in the
        // analysisFields vector, so the field's order is part of the
        // binding ABI -- reordering an existing effect's analysisFields
        // requires bumping its effectVersion.
        // Default false: field is CPU-only, current behavior.
        bool gpuPublish{ false };

        uint32_t pixelCount() const { return AnalysisFieldPixelCount(type, arrayLength); }
    };

    // Runtime value of a single analysis output field (read back from GPU).
    struct AnalysisFieldValue
    {
        std::wstring name;
        AnalysisFieldType type{ AnalysisFieldType::Float4 };

        // For scalar types (Float/Float2/Float3/Float4).
        std::array<float, 4> components{ 0, 0, 0, 0 };

        // For array types. Flat storage, stride = AnalysisFieldComponentCount(type).
        std::vector<float> arrayData;
    };

    // Complete definition of a custom effect authored in the Effect Designer.
    // Stored per-node and serialized with the graph JSON.
    struct CustomEffectDefinition
    {
        CustomShaderType shaderType{ CustomShaderType::PixelShader };
        std::wstring hlslSource;                        // Full HLSL source code.
        std::vector<std::wstring> inputNames;           // Named inputs (textures).
        std::vector<ParameterDefinition> parameters;    // Declared cbuffer parameters.

        // ShaderLab built-in effect identity (empty for user-authored effects).
        std::wstring shaderLabEffectId;
        uint32_t shaderLabEffectVersion{ 0 };

        // Compute shader settings.
        uint32_t threadGroupX{ 8 };
        uint32_t threadGroupY{ 8 };
        uint32_t threadGroupZ{ 1 };

        // Analysis output metadata (for compute effects).
        AnalysisOutputType analysisOutputType{ AnalysisOutputType::None };
        uint32_t analysisOutputSize{ 256 };

        // Typed analysis output field descriptors.
        // The compute shader writes fields sequentially to Output row 0.
        // Each field's pixel offset = sum of prior fields' pixelCount().
        std::vector<AnalysisFieldDescriptor> analysisFields;

        // Runtime: compiled bytecode (not serialized — recompiled from hlslSource on load).
        std::vector<uint8_t> compiledBytecode;

        // Runtime: unique shader GUID for this instance (avoids D2D shader ID collisions).
        GUID shaderGuid{};

        bool isCompiled() const { return !compiledBytecode.empty(); }

        // Total UAV pixels needed for all analysis fields.
        uint32_t totalAnalysisPixels() const
        {
            uint32_t total = 0;
            for (const auto& f : analysisFields)
                total += f.pixelCount();
            return total;
        }
    };

    // Stores non-image output data from analysis/compute effects.
    // Read back from the GPU after the effect graph is evaluated.
    struct AnalysisOutput
    {
        AnalysisOutputType type{ AnalysisOutputType::None };

        // Typed field values (for AnalysisOutputType::Typed).
        std::vector<AnalysisFieldValue> fields;

        // Legacy/histogram support.
        std::vector<float> data;         // Bin values, buffer contents, etc.
        std::wstring       label;        // Human-readable description.
        uint32_t           channelIndex{ 0 }; // For per-channel data (R=0, G=1, B=2, A=3).
    };

    // Specifies the source of a single scalar value in a property binding.
    struct ComponentSource
    {
        uint32_t sourceNodeId{ 0 };
        std::wstring sourceFieldName;   // analysis field name on source node
        uint32_t sourceIndex{ 0 };      // array element index (0 for scalar fields)
        uint32_t sourceComponent{ 0 };  // 0-3 for .xyzw within that element
    };

    // Binds a downstream node's property to upstream analysis output values.
    // Supports per-component routing: each destination component can come from
    // a different source field/node/component.
    struct PropertyBinding
    {
        // Per-component sources. Size matches destination component count:
        //   float → 1, float2 → 2, float3 → 3, float4 → 4
        // Use nullopt for unbound components (keeps authored default).
        std::vector<std::optional<ComponentSource>> sources;

        // Whole-array mode: for vector<float> destination properties.
        // When set, ignores per-component sources and copies entire array.
        bool wholeArray{ false };
        uint32_t wholeArraySourceNodeId{ 0 };
        std::wstring wholeArraySourceFieldName;
    };

    // A node in the effect graph DAG.
    // Each node represents a D2D image source, a built-in D2D effect,
    // a custom pixel/compute shader, or the final output.
    struct EffectNode
    {
        uint32_t   id{ 0 };
        std::wstring name;
        NodeType   type{ NodeType::Source };

        // Canvas position for the visual graph editor (in DIPs).
        winrt::Windows::Foundation::Numerics::float2 position{ 0.0f, 0.0f };

        // Effect-specific properties (e.g., Gaussian blur StdDeviation, Flood Color).
        // These are the "authored" defaults — never mutated by bindings at runtime.
        std::map<std::wstring, PropertyValue> properties;

        // Property bindings: property name → upstream analysis field source.
        // During evaluation, bound values override authored properties.
        std::map<std::wstring, PropertyBinding> propertyBindings;

        // Pin descriptors — filled when the node type is known.
        std::vector<PinDescriptor> inputPins;
        std::vector<PinDescriptor> outputPins;

        // For built-in D2D effects: the CLSID used to create the effect.
        std::optional<GUID> effectClsid;

        // For custom shaders: path to the HLSL source file.
        std::optional<std::wstring> shaderPath;

        // For custom effects authored in the Effect Designer.
        std::optional<CustomEffectDefinition> customEffect;

        // Cached output after evaluation (non-owning, managed by the render engine).
        // Not serialized.
        ID2D1Image* cachedOutput{ nullptr };

        // Analysis/compute output data (read back after evaluation). Not serialized.
        AnalysisOutput analysisOutput;

        // Runtime error message (e.g., effect creation failure). Not serialized.
        std::wstring runtimeError;

        // Dirty flag -- set when properties change, cleared after evaluation.
        bool dirty{ true };

        // Clock node: time-based animation source.
        bool isClock{ false };
        bool isPlaying{ false };   // runtime: toggled by on-node Play/Pause
        double clockTime{ 0.0 };   // runtime: accumulated time in seconds

        // Set by the host before evaluation: true if this node feeds a visible
        // output (Output node, output window, preview, or data-only analysis).
        // The evaluator skips nodes where needed == false.
        bool needed{ true };
    };
}
