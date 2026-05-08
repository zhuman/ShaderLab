#include "pch_engine.h"
#include "ShaderLabEffects.h"
#include "BytecodeCache.h"
#include "CustomComputeShaderEffect.h"
#include "CustomComputeBridgeEffect.h"
#include "CustomPixelShaderEffect.h"

#include <thread>

namespace ShaderLab::Effects
{
    void RegisterEngineD2DEffects(ID2D1Factory1* factory)
    {
        if (!factory) return;
        CustomPixelShaderEffect::RegisterEffect(factory);
        CustomComputeShaderEffect::RegisterEffect(factory);
        CustomComputeBridgeEffect::RegisterEffect(factory);
    }

    void ConfigureBytecodeCache(std::wstring rootPath, uint64_t staleThresholdSec)
    {
        BytecodeCache::Instance().SetDiskCacheRoot(rootPath);
        if (rootPath.empty() || staleThresholdSec == 0) return;
        // Background reap so engine init isn't blocked on a directory
        // walk. Detached jthread; the cache itself is robust to
        // concurrent access.
        std::thread([staleThresholdSec]() {
            BytecodeCache::Instance().ReapDisk(staleThresholdSec);
        }).detach();
    }

    // Shared color-math HLSL extracted to Effects/ColorMath.cpp
    // (Phase 5 partial). GetColorMathHLSL() prototype lives in
    // Effects/ShaderLabEffects.h.
    // -----------------------------------------------------------------------
    // Effect HLSL sources
    // -----------------------------------------------------------------------

    static const std::string s_luminanceHeatmapHLSL = R"HLSL(
// Luminance Heatmap - false-color visualization of luminance
Texture2D Source : register(t0);

cbuffer constants : register(b0) {
    float MinNits;     // default 0.0
    float MaxNits;     // default 10000.0
    uint ColormapMode; // 0=Turbo, 1=Inferno
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 color = Source.Load(int3(uv0.xy, 0));
    float nits = ScRGBLuminanceNits(color.rgb);
    float t = saturate((nits - MinNits) / max(MaxNits - MinNits, 0.001));
    float3 mapped = TurboColormap(t);
    // Turbo colormap outputs perceptual 0-1 values.
    // Keep as-is in scRGB (1.0 = 80 nits SDR white) for visible display.
    return float4(mapped, color.a);
}
)HLSL";

    static const std::string s_outOfGamutHLSL = R"HLSL(
// Gamut Highlight
// TargetGamut modes:
//   0 = Rec.709    (matrix conversion)
//   1 = DCI-P3     (matrix conversion)
//   2 = Rec.2020   (matrix conversion)
//   3 = Custom     (CIE xy chromaticity triangle test using
//                   RedPrimary/GreenPrimary/BluePrimary; bind these to
//                   `Working Space.RedPrimary` etc. for monitor-matched
//                   analysis, or set them manually.)
Texture2D Source : register(t0);

cbuffer constants : register(b0) {
    uint TargetGamut;
    float OverlayR;
    float OverlayG;
    float OverlayB;
    float OverlayStrength;
    uint Mode;
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 color = Source.Load(int3(uv0.xy, 0));
    if (color.a < 0.001) return color;

    // Skip near-black pixels where chromaticity is numerically unstable.
    float lum = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    if (lum < 0.001) return color;

    // Read all cbuffer vars at top so DXC keeps them resident even on
    // branches that don't directly reference every primary.
    float gamut = TargetGamut;
    float2 mr = RedPrimary;
    float2 mg = GreenPrimary;
    float2 mb = BluePrimary;

    float3 xyz = ScRGBToXYZ(color.rgb);
    float3 targetRGB = color.rgb;

    if (gamut < 0.5) {
        // Rec.709 (identity for our scRGB pipeline).
        targetRGB = color.rgb;
    } else if (gamut < 1.5) {
        targetRGB = mul(XYZ_TO_P3D65, xyz);
    } else if (gamut < 2.5) {
        targetRGB = mul(XYZ_TO_REC2020, xyz);
    } else {
        // Custom: CIE xy chromaticity triangle test against bound primaries.
        float sum = xyz.x + xyz.y + xyz.z;
        float2 xy = (sum > 0.0001) ? float2(xyz.x / sum, xyz.y / sum) : D65_WHITE;
        float2 v0 = mb - mr, v1 = mg - mr, v2 = xy - mr;
        float d00 = dot(v0, v0), d01 = dot(v0, v1), d02 = dot(v0, v2);
        float d11 = dot(v1, v1), d12 = dot(v1, v2);
        float inv = 1.0 / (d00 * d11 - d01 * d01);
        float u = (d11 * d02 - d01 * d12) * inv;
        float v = (d00 * d12 - d01 * d02) * inv;
        bool inside = (u >= 0) && (v >= 0) && (u + v <= 1.0);
        targetRGB = inside ? float3(1, 1, 1) : float3(-1, -1, -1);
    }

    bool oog = (targetRGB.r < 0.0 || targetRGB.g < 0.0 || targetRGB.b < 0.0);
    bool highlight = (Mode > 0.5) ? !oog : oog;
    if (highlight) {
        float3 overlay = float3(OverlayR, OverlayG, OverlayB);
        color.rgb = lerp(color.rgb, overlay, OverlayStrength);
    }
    return color;
}
)HLSL";

    static const std::string s_luminanceHighlightHLSL = R"HLSL(
// Luminance Highlight
// Mirrors Gamut Highlight: pixels whose luminance falls outside the
// active nit range (or inside, depending on Mode) are tinted with an
// overlay color.
//
// TargetRange selects the [min,max] nit window:
//   0 = SDR              (0 .. 80 nits)
//   1 = HDR 400          (0 .. 400 nits)
//   2 = HDR 1000         (0 .. 1000 nits)
//   3 = HDR 4000         (0 .. 4000 nits)
//   4 = HDR 10000        (0 .. 10000 nits)
//   5 = Custom           (use MinNits / MaxNits sliders directly; bind
//                         to `Working Space.MinNits/PeakNits` for
//                         monitor-matched analysis.)
Texture2D Source : register(t0);

cbuffer constants : register(b0) {
    uint TargetRange;
    float MinNits;
    float MaxNits;
    float OverlayR;
    float OverlayG;
    float OverlayB;
    float OverlayStrength;
    uint Mode; // 0 = Out-of-Range, 1 = In-Range
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 color = Source.Load(int3(uv0.xy, 0));
    if (color.a < 0.001) return color;

    // ScRGBLuminanceNits: scRGB luminance * 80 (1.0 = 80 nits SDR white).
    float nits = ScRGBLuminanceNits(color.rgb);

    // Pick effective range.
    float effMin = MinNits;
    float effMax = MaxNits;
    if (TargetRange < 0.5) {              // SDR
        effMin = 0.0;   effMax = 80.0;
    } else if (TargetRange < 1.5) {       // HDR 400
        effMin = 0.0;   effMax = 400.0;
    } else if (TargetRange < 2.5) {       // HDR 1000
        effMin = 0.0;   effMax = 1000.0;
    } else if (TargetRange < 3.5) {       // HDR 4000
        effMin = 0.0;   effMax = 4000.0;
    } else if (TargetRange < 4.5) {       // HDR 10000
        effMin = 0.0;   effMax = 10000.0;
    } else {                               // Custom
        effMin = MinNits;
        effMax = MaxNits;
    }

    // Defensive: degenerate range collapses to "everything out-of-range".
    bool outOfRange = (effMin >= effMax) || (nits < effMin) || (nits > effMax);
    bool highlight = (Mode > 0.5) ? !outOfRange : outOfRange;
    if (highlight) {
        float3 overlay = float3(OverlayR, OverlayG, OverlayB);
        color.rgb = lerp(color.rgb, overlay, OverlayStrength);
    }
    return color;
}
)HLSL";

    ShaderLabEffects& ShaderLabEffects::Instance()
    {
        static ShaderLabEffects instance;
        return instance;
    }

    ShaderLabEffects::ShaderLabEffects()
    {
        RegisterAll();
    }

    const ShaderLabEffectDescriptor* ShaderLabEffects::FindByName(std::wstring_view name) const
    {
        for (const auto& e : m_effects)
            if (_wcsicmp(e.name.c_str(), name.data()) == 0)
                return &e;
        return nullptr;
    }

    const ShaderLabEffectDescriptor* ShaderLabEffects::FindById(std::wstring_view effectId) const
    {
        for (const auto& e : m_effects)
            if (e.effectId == effectId)
                return &e;
        // Legacy ID aliases: rename history. Saved graphs may reference
        // the old name; resolve them transparently so old .effectgraph
        // files keep loading and the "Update Effect" prompt fires once
        // they're loaded.
        if (effectId == L"Perceptual Gamut Map")
            return FindById(L"ICtCp Gamut Map");
        return nullptr;
    }

    std::vector<const ShaderLabEffectDescriptor*> ShaderLabEffects::ByCategory(std::wstring_view category) const
    {
        std::vector<const ShaderLabEffectDescriptor*> result;
        for (const auto& e : m_effects)
            if (e.category == category)
                result.push_back(&e);
        return result;
    }

    std::vector<std::wstring> ShaderLabEffects::Categories() const
    {
        std::vector<std::wstring> cats;
        for (const auto& e : m_effects)
        {
            bool found = false;
            for (const auto& c : cats) if (c == e.category) { found = true; break; }
            if (!found) cats.push_back(e.category);
        }
        return cats;
    }

    Graph::EffectNode ShaderLabEffects::CreateNode(const ShaderLabEffectDescriptor& desc)
    {
        using namespace Graph;

        EffectNode node;
        node.name = desc.name;
        node.type = (desc.shaderType == CustomShaderType::PixelShader)
            ? NodeType::PixelShader : NodeType::ComputeShader;

        // Parameter nodes (no HLSL) and data-only effects have no image output pin.
        if (!desc.hlslSource.empty() && !desc.dataOnly)
            node.outputPins.push_back({ L"Output", 0 });

        CustomEffectDefinition def;
        def.shaderType = desc.shaderType;
        def.hlslSource = std::wstring(desc.hlslSource.begin(), desc.hlslSource.end());
        def.inputNames = desc.inputNames;
        def.parameters = desc.parameters;
        def.analysisFields = desc.analysisFields;
        def.analysisOutputType = desc.analysisOutputType;
        def.analysisOutputSize = 256;
        def.threadGroupX = desc.threadGroupX;
        def.threadGroupY = desc.threadGroupY;
        def.threadGroupZ = desc.threadGroupZ;
        def.shaderLabEffectId = desc.effectId;
        def.shaderLabEffectVersion = desc.effectVersion;
        CoCreateGuid(&def.shaderGuid);

        // Set up input pins from input names.
        for (uint32_t i = 0; i < desc.inputNames.size(); ++i)
            node.inputPins.push_back({ std::format(L"I{}", i), i });

        // Set default property values from parameter definitions.
        for (const auto& param : desc.parameters)
            node.properties[param.name] = param.defaultValue;

        // Set hidden default properties (cbuffer values not in Properties panel).
        for (const auto& [key, val] : desc.hiddenDefaults)
            node.properties[key] = val;

        node.customEffect = std::move(def);
        node.isClock = desc.isClock;
        return node;
    }

    void ShaderLabEffects::RegisterAll()
    {
        const auto& colorMath = GetColorMathHLSL();

        // ---- Luminance Heatmap ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Luminance Heatmap";
            desc.effectId = L"Luminance Heatmap"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.subcategory = L"Highlights";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + s_luminanceHeatmapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"MinNits",      L"float", 0.0f,    0.0f, 10000.0f, 1.0f },
                { L"MaxNits",      L"float", 10000.0f, 0.0f, 10000.0f, 100.0f },
                { L"ColormapMode", L"float", 0.0f, 0.0f, 1.0f, 1.0f, { L"Turbo", L"Inferno" } },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gamut Highlight ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Highlight";
            desc.effectId = L"Gamut Highlight"; desc.effectVersion = 4;
            desc.category = L"Analysis";
            desc.subcategory = L"Highlights";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + s_outOfGamutHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"TargetGamut",     L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Rec.709", L"DCI-P3", L"Rec.2020", L"Custom" } },
                { L"OverlayR",        L"float", 1.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayG",        L"float", 0.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayB",        L"float", 1.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayStrength", L"float", 0.7f,  0.0f, 1.0f, 0.01f },
                { L"Mode",            L"float", 0.0f,  0.0f, 1.0f, 1.0f, { L"Out-of-Gamut", L"In-Gamut" } },
                // Custom-mode primaries (sRGB/Rec.709 D65 defaults). Bind
                // these to `Working Space.RedPrimary` etc. for monitor-matched
                // analysis. Hidden in the Properties panel and graph node
                // until TargetGamut is set to "Custom".
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Luminance Highlight ----
        // Range-based companion to Gamut Highlight: tints pixels whose
        // luminance (in nits) falls outside the active nit range. The Mode
        // toggle inverts the test so the user can isolate either the
        // out-of-range or in-range pixels. TargetRange picks among common
        // HDR presets or a user-specified Custom range. For monitor-matched
        // analysis, set TargetRange to "Custom" and bind MinNits/MaxNits to
        // `Working Space.MinNits` / `Working Space.PeakNits`.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Luminance Highlight";
            desc.effectId = L"Luminance Highlight"; desc.effectVersion = 5;
            desc.category = L"Analysis";
            desc.subcategory = L"Highlights";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + s_luminanceHighlightHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"TargetRange",     L"float", 0.0f,   0.0f, 5.0f, 1.0f,
                    { L"SDR (0-80)", L"HDR 400", L"HDR 1000", L"HDR 4000",
                      L"HDR 10000", L"Custom" } },
                { L"MinNits",         L"float", 0.0f,    0.0f, 10000.0f, 1.0f, {}, L"TargetRange == 5" },
                { L"MaxNits",         L"float", 1000.0f, 0.0f, 10000.0f, 10.0f, {}, L"TargetRange == 5" },
                { L"OverlayR",        L"float", 1.0f,   0.0f, 1.0f, 0.01f },
                { L"OverlayG",        L"float", 0.0f,   0.0f, 1.0f, 0.01f },
                { L"OverlayB",        L"float", 1.0f,   0.0f, 1.0f, 0.01f },
                { L"OverlayStrength", L"float", 0.7f,   0.0f, 1.0f, 0.01f },
                { L"Mode",            L"float", 0.0f,   0.0f, 1.0f, 1.0f, { L"Out-of-Range", L"In-Range" } },
            };
            m_effects.push_back(std::move(desc));
        }

        // Future effects will be added here as they're implemented.

        // ---- CIE Histogram (D3D11 Compute) ----
        // Builds a 2D scatter histogram of source image chromaticity in CIE xy space.
        // Output: a 2D texture where R = log2(count+1) normalized, usable as input
        // to the CIE Chromaticity Plot pixel shader.
        {
            static const std::string cieHistHLSL = R"HLSL(
// CIE xy Histogram: scatter source pixels into a 2D CIE xy histogram.
// Uses groupshared tiled accumulation to avoid data races.
// Output R: log-scaled density, G: avg luminance / 10000, A: 1 if hit.

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Output : register(u1);

cbuffer Constants : register(b0) {
    uint Width;
    uint Height;
    uint OutputSize;
};

float3 ScRGBToXYZ(float3 rgb) {
    return float3(
        0.4123908 * rgb.r + 0.3575843 * rgb.g + 0.1804808 * rgb.b,
        0.2126390 * rgb.r + 0.7151687 * rgb.g + 0.0721923 * rgb.b,
        0.0193308 * rgb.r + 0.1191950 * rgb.g + 0.9505322 * rgb.b
    );
}

static const float2 CENTER = float2(0.3127, 0.3290);
static const float HALF_EXTENT = 0.50;

// Tile-based groupshared histogram.
// 48x48 = 2304 bins, well within 32KB groupshared limit.
#define TILE_SIZE 48
groupshared uint gs_counts[TILE_SIZE * TILE_SIZE];
groupshared float gs_lumSum[TILE_SIZE * TILE_SIZE];
groupshared uint gs_maxCount;

[numthreads(32, 32, 1)]
void main(uint3 GTid : SV_GroupThreadID) {
    uint tid = GTid.x + GTid.y * 32;
    uint totalThreads = 1024;
    uint totalPixels = Width * Height;
    uint outSize = max(OutputSize, 64u);

    // Clear output texture.
    for (uint ci = tid; ci < outSize * outSize; ci += totalThreads) {
        Output[int2(ci % outSize, ci / outSize)] = float4(0, 0, 0, 0);
    }

    // Clear groupshared tile.
    for (uint ti = tid; ti < TILE_SIZE * TILE_SIZE; ti += totalThreads) {
        gs_counts[ti] = 0;
        gs_lumSum[ti] = 0.0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Process all source pixels, scatter into groupshared tile.
    // The tile covers the full output range at reduced resolution.
    for (uint pi = tid; pi < totalPixels; pi += totalThreads) {
        uint px = pi % Width;
        uint py = pi / Width;
        float4 src = Source[int2(px, py)];
        if (src.a < 0.01) continue;

        float3 xyz = ScRGBToXYZ(src.rgb);  // preserve negatives for wide-gamut chromaticity
        float sum = xyz.x + xyz.y + xyz.z;
        if (sum < 1e-7) continue;

        float cieX = xyz.x / sum;
        float cieY = xyz.y / sum;

        float u = (cieX - CENTER.x) / (2.0 * HALF_EXTENT) + 0.5;
        float v = 0.5 - (cieY - CENTER.y) / (2.0 * HALF_EXTENT);
        if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;

        uint tx = min((uint)(u * TILE_SIZE), TILE_SIZE - 1);
        uint ty = min((uint)(v * TILE_SIZE), TILE_SIZE - 1);
        uint idx = ty * TILE_SIZE + tx;

        InterlockedAdd(gs_counts[idx], 1u);
        // Luminance accumulation has minor races but acceptable for avg.
        gs_lumSum[idx] += xyz.y * 80.0 / 10000.0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Find max count for log normalization.
    if (tid == 0) gs_maxCount = 1;
    GroupMemoryBarrierWithGroupSync();
    for (uint mi = tid; mi < TILE_SIZE * TILE_SIZE; mi += totalThreads) {
        InterlockedMax(gs_maxCount, gs_counts[mi]);
    }
    GroupMemoryBarrierWithGroupSync();

    float logMax = log2(float(gs_maxCount) + 1.0);

    // Write groupshared tile to output texture, upscaling to full resolution.
    // Each tile bin maps to a block of (outSize/TILE_SIZE) x (outSize/TILE_SIZE) pixels.
    float scale = float(outSize) / float(TILE_SIZE);
    for (uint oi = tid; oi < outSize * outSize; oi += totalThreads) {
        uint ox = oi % outSize;
        uint oy = oi / outSize;
        // Map output pixel back to tile bin.
        uint tx = min((uint)(float(ox) / scale), TILE_SIZE - 1);
        uint ty = min((uint)(float(oy) / scale), TILE_SIZE - 1);
        uint idx = ty * TILE_SIZE + tx;
        uint cnt = gs_counts[idx];
        if (cnt > 0) {
            float logDensity = log2(float(cnt) + 1.0) / logMax;
            float avgLum = gs_lumSum[idx] / float(cnt);
            Output[int2(ox, oy)] = float4(logDensity, avgLum, 0, 1.0);
        }
    }
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"CIE Histogram";
            desc.effectId = L"CIE Histogram"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.subcategory = L"Scopes";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hlslSource = cieHistHLSL;
            desc.inputNames = { L"Source" };
            desc.hasImageOutput = true;
            desc.parameters = {
                { L"OutputSize", L"uint", 512.0f, 64.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- CIE Chromaticity Plot ----
        {
            static const std::string ciePlotHLSL = R"HLSL(
// CIE 1931 xy Chromaticity Diagram
// Renders the spectral locus horseshoe with gamut triangle overlays.
// Input 0: CIE Histogram texture (from CIE Histogram compute effect).
//   Each pixel = log-normalized scatter density in CIE xy space.
Texture2D Histogram : register(t0);
SamplerState Sampler0 : register(s0);

cbuffer constants : register(b0) {
    uint ShowRec709;    // 1=show gamut triangle
    uint ShowP3;        // 1=show gamut triangle
    uint ShowRec2020;   // 1=show gamut triangle
    float Brightness;   // scatter dot brightness (default 2.0)
    float DiagramSize;  // output size in pixels (default 512)
    uint ShowMonitor;   // 1=show "monitor" gamut triangle drawn from
                        // RedPrimary/GreenPrimary/BluePrimary (typically
                        // bound to the Working Space node).
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
};

// CIE 1931 2-degree observer spectral locus (sampled at 10nm, 380-700nm)
// 33 xy pairs forming the horseshoe boundary.
static const float2 LOCUS[] = {
    float2(0.1741, 0.0050), // 380nm
    float2(0.1740, 0.0050), // 390nm
    float2(0.1714, 0.0049), // 400nm
    float2(0.1644, 0.0051), // 410nm
    float2(0.1566, 0.0177), // 420nm
    float2(0.1440, 0.0297), // 430nm
    float2(0.1241, 0.0578), // 440nm
    float2(0.0913, 0.1327), // 450nm
    float2(0.0687, 0.2007), // 460nm
    float2(0.0454, 0.2950), // 470nm
    float2(0.0235, 0.4127), // 480nm
    float2(0.0082, 0.5384), // 490nm
    float2(0.0039, 0.6548), // 500nm
    float2(0.0139, 0.7502), // 510nm
    float2(0.0743, 0.8338), // 520nm
    float2(0.1547, 0.8059), // 530nm
    float2(0.2296, 0.7543), // 540nm
    float2(0.3016, 0.6923), // 550nm
    float2(0.3731, 0.6245), // 560nm
    float2(0.4441, 0.5547), // 570nm
    float2(0.5125, 0.4866), // 580nm
    float2(0.5752, 0.4242), // 590nm
    float2(0.6270, 0.3725), // 600nm
    float2(0.6658, 0.3340), // 610nm
    float2(0.6915, 0.3083), // 620nm
    float2(0.7079, 0.2920), // 630nm
    float2(0.7190, 0.2809), // 640nm
    float2(0.7260, 0.2740), // 650nm
    float2(0.7300, 0.2700), // 660nm
    float2(0.7320, 0.2680), // 670nm
    float2(0.7334, 0.2666), // 680nm
    float2(0.7340, 0.2660), // 690nm
    float2(0.7347, 0.2653), // 700nm
};
#define LOCUS_COUNT 33

// Winding-number test: is point p inside the closed spectral locus + purple line?
bool IsInsideLocus(float2 p) {
    int winding = 0;
    // Test against spectral locus segments
    for (uint i = 0; i < LOCUS_COUNT; i++) {
        float2 a = LOCUS[i];
        float2 b = LOCUS[(i + 1) % LOCUS_COUNT];
        // For the last point, close with purple line back to first
        if (i == LOCUS_COUNT - 1) b = LOCUS[0];
        if (a.y <= p.y) {
            if (b.y > p.y) {
                float cross = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
                if (cross > 0) winding++;
            }
        } else {
            if (b.y <= p.y) {
                float cross = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
                if (cross < 0) winding--;
            }
        }
    }
    return winding != 0;
}

// Draw gamut triangle outline
float GamutTriangle(float2 uv, float2 r, float2 g, float2 b, float thickness) {
    float d1 = abs(dot(normalize(float2(-(g.y-r.y), g.x-r.x)), uv - r));
    float d2 = abs(dot(normalize(float2(-(b.y-g.y), b.x-g.x)), uv - g));
    float d3 = abs(dot(normalize(float2(-(r.y-b.y), r.x-b.x)), uv - b));

    // Check if on the edge (between vertices)
    float t1 = dot(uv - r, g - r) / dot(g - r, g - r);
    float t2 = dot(uv - g, b - g) / dot(b - g, b - g);
    float t3 = dot(uv - b, r - b) / dot(r - b, r - b);

    float edge = 1e10;
    if (t1 >= 0 && t1 <= 1) edge = min(edge, d1);
    if (t2 >= 0 && t2 <= 1) edge = min(edge, d2);
    if (t3 >= 0 && t3 <= 1) edge = min(edge, d3);

    return smoothstep(thickness, 0.0, edge);
}

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    // Map pixel position to CIE xy space
    // x: 0 to 0.8, y: 0 to 0.9 (standard diagram range)
    float2 pixPos = uv0.xy;
    float size = max(DiagramSize, 128.0);
    float2 xy = float2(pixPos.x / size * 0.8, (1.0 - pixPos.y / size) * 0.9);

    float4 result = float4(0.0, 0.0, 0.0, 1.0); // black background

    // Render the visible gamut region with approximate spectral colors
    if (IsInsideLocus(xy)) {
        float3 xyY = float3(xy.x, xy.y, 0.5);
        float3 xyz = xyYToXYZ(xyY);
        float3 rgb = XYZToScRGB(xyz);
        // Normalize brightness, clamp negatives to show the full visible gamut
        // (not just Rec.709). Out-of-709 colors are desaturated toward white.
        float maxC = max(max(abs(rgb.r), abs(rgb.g)), max(abs(rgb.b), 0.001));
        rgb = rgb / maxC * 0.15 * Brightness;
        rgb = max(rgb, 0.0);
        result.rgb = rgb;
    }

    // Gamut triangles
    float thickness = 0.003;
    float lineBright = 0.4 * Brightness;
    if (ShowRec709 > 0.5) {
        float e = GamutTriangle(xy, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B, thickness);
        result.rgb = lerp(result.rgb, float3(1,1,1) * lineBright, e * 0.8);
    }
    if (ShowP3 > 0.5) {
        float e = GamutTriangle(xy, GAMUT_P3_R, GAMUT_P3_G, GAMUT_P3_B, thickness);
        result.rgb = lerp(result.rgb, float3(0,1,0) * lineBright, e * 0.8);
    }
    if (ShowRec2020 > 0.5) {
        float e = GamutTriangle(xy, GAMUT_2020_R, GAMUT_2020_G, GAMUT_2020_B, thickness);
        result.rgb = lerp(result.rgb, float3(0,0.5,1) * lineBright, e * 0.8);
    }
    if (ShowMonitor > 0.5) {
        float2 mR = RedPrimary;
        float2 mG = GreenPrimary;
        float2 mB = BluePrimary;
        float e = GamutTriangle(xy, mR, mG, mB, thickness);
        result.rgb = lerp(result.rgb, float3(1, 0.8, 0) * lineBright, e * 0.9);
    }

    // D65 white point marker
    float dw = length(xy - D65_WHITE);
    if (dw < 0.008) result.rgb = float3(0.5, 0.5, 0.5) * Brightness;

    // Read scatter density from pre-computed histogram texture.
    // The histogram uses D65-centered coordinates:
    // u = (x - 0.3127) / 1.0 + 0.5, v = 0.5 - (y - 0.3290) / 1.0
    float histU = (xy.x - 0.3127) / 1.0 + 0.5;
    float histV = 0.5 - (xy.y - 0.3290) / 1.0;
    if (histU >= 0.0 && histU <= 1.0 && histV >= 0.0 && histV <= 1.0) {
        uint histW, histH;
        Histogram.GetDimensions(histW, histH);
        float4 histVal = Histogram.Load(int3(
            int(histU * float(histW)),
            int(histV * float(histH)), 0));
        float intensity = histVal.r;
        if (intensity > 0.0) {
            result.rgb += intensity * Brightness * 0.3 * float3(1, 1, 1);
        }
    }

    return result;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"CIE Chromaticity Plot";
            desc.effectId = L"CIE Chromaticity Plot"; desc.effectVersion = 5;
            desc.category = L"Analysis";
            desc.subcategory = L"Scopes";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ciePlotHLSL;
            desc.inputNames = { L"Histogram" };
            desc.parameters = {
                { L"ShowRec709",   L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"ShowP3",       L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"ShowRec2020",  L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"Brightness",   L"float", 2.0f,  0.1f, 10.0f, 0.1f },
                { L"DiagramSize",  L"float", 512.0f, 128.0f, 4096.0f, 64.0f },
                // Custom-primary triangle ("monitor" gamut). Bind these to
                // `Working Space.RedPrimary` etc. for monitor-matched plotting.
                { L"ShowMonitor",  L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"ShowMonitor == 1" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"ShowMonitor == 1" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"ShowMonitor == 1" },
            };
            m_effects.push_back(std::move(desc));
        }



        // ---- Gamut Source ----
        {
            static const std::string gamutSourceHLSL = R"HLSL(
// Gamut Source - generates all colors within a selected color gamut
// Fixed coordinate system centered on D65 white point. Scale fits Rec.2020
// so all three gamuts share the same spatial mapping.
//
// Gamut modes:
//   0 = Rec.709
//   1 = DCI-P3
//   2 = Rec.2020
//   3 = Custom  (uses RedPrimary/GreenPrimary/BluePrimary; bind to
//                Working Space.RedPrimary etc. for a monitor-matched source.)

cbuffer constants : register(b0) {
    float Gamut;
    float Luminance;    // nits (default 80.0, maps to scRGB 1.0)
    float OutputSize;   // pixels (default 1024)
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float size = max(OutputSize, 128.0);

    // Read all cbuffer vars at top to keep DXC from optimizing them out.
    float gamut = Gamut;
    float2 cR = RedPrimary;
    float2 cG = GreenPrimary;
    float2 cB = BluePrimary;

    // Select gamut primaries
    float2 r, g, b;
    if (gamut > 2.5)     { r = cR; g = cG; b = cB; }
    else if (gamut > 1.5){ r = GAMUT_2020_R; g = GAMUT_2020_G; b = GAMUT_2020_B; }
    else if (gamut > 0.5){ r = GAMUT_P3_R;   g = GAMUT_P3_G;   b = GAMUT_P3_B; }
    else                 { r = GAMUT_709_R;  g = GAMUT_709_G;  b = GAMUT_709_B; }

    float2 center = D65_WHITE;
    float halfExtent = 0.50;

    float2 uv = uv0.xy / size;
    float2 xy;
    xy.x = center.x + (uv.x - 0.5) * 2.0 * halfExtent;
    xy.y = center.y - (uv.y - 0.5) * 2.0 * halfExtent;

    if (!PointInTriangle(xy, r, g, b))
        return float4(0, 0, 0, 1.0);

    float Y = Luminance / 80.0;
    float3 xyY_val = float3(xy.x, xy.y, Y);
    float3 xyz = xyYToXYZ(xyY_val);
    float3 rgb = XYZToScRGB(xyz);

    return float4(rgb, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Source";
            desc.effectId = L"Gamut Source"; desc.effectVersion = 3;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gamutSourceHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"Gamut",      L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Rec.709", L"DCI-P3", L"Rec.2020", L"Custom" } },
                { L"Luminance",  L"float", 80.0f, 0.01f, 10000.0f, 10.0f },
                { L"OutputSize", L"float", 512.0f, 128.0f, 4096.0f, 64.0f },
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"Gamut == 3" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"Gamut == 3" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"Gamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Color Checker (Source) ----
        {
            static const std::string colorCheckerHLSL = R"HLSL(
// Macbeth ColorChecker - 24 reference patches in scRGB
// Source effect: no input required.

cbuffer constants : register(b0) {
    float PatchSize; // pixels per patch (default 64)
};

// 24 ColorChecker patches in linear sRGB (scRGB)
// Row 1: Dark Skin, Light Skin, Blue Sky, Foliage, Blue Flower, Bluish Green
// Row 2: Orange, Purplish Blue, Moderate Red, Purple, Yellow Green, Orange Yellow
// Row 3: Blue, Green, Red, Yellow, Magenta, Cyan
// Row 4: White, Neutral 8, Neutral 6.5, Neutral 5, Neutral 3.5, Black
static const float3 PATCHES[24] = {
    float3(0.0451, 0.0225, 0.0140),  // Dark Skin
    float3(0.3005, 0.1947, 0.1369),  // Light Skin
    float3(0.0600, 0.0962, 0.1622),  // Blue Sky
    float3(0.0300, 0.0410, 0.0130),  // Foliage
    float3(0.1118, 0.1018, 0.2218),  // Blue Flower
    float3(0.0940, 0.2560, 0.2180),  // Bluish Green
    float3(0.3270, 0.0935, 0.0100),  // Orange
    float3(0.0260, 0.0438, 0.2040),  // Purplish Blue
    float3(0.2180, 0.0360, 0.0310),  // Moderate Red
    float3(0.0280, 0.0130, 0.0510),  // Purple
    float3(0.1940, 0.2630, 0.0220),  // Yellow Green
    float3(0.3800, 0.1620, 0.0100),  // Orange Yellow
    float3(0.0100, 0.0180, 0.1470),  // Blue
    float3(0.0310, 0.1080, 0.0140),  // Green
    float3(0.1630, 0.0120, 0.0070),  // Red
    float3(0.4590, 0.3820, 0.0220),  // Yellow
    float3(0.2040, 0.0380, 0.1110),  // Magenta
    float3(0.0230, 0.1310, 0.2160),  // Cyan
    float3(0.8740, 0.8740, 0.8740),  // White
    float3(0.5100, 0.5100, 0.5100),  // Neutral 8
    float3(0.3040, 0.3040, 0.3040),  // Neutral 6.5
    float3(0.1610, 0.1610, 0.1610),  // Neutral 5
    float3(0.0680, 0.0680, 0.0680),  // Neutral 3.5
    float3(0.0210, 0.0210, 0.0210),  // Black
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float ps = max(PatchSize, 16.0);
    float2 pixPos = uv0.xy;
    uint col = (uint)(pixPos.x / ps);
    uint row = (uint)(pixPos.y / ps);

    if (col >= 6 || row >= 4)
        return float4(0.15, 0.15, 0.15, 1.0);

    uint idx = row * 6 + col;
    if (idx >= 24)
        return float4(0.15, 0.15, 0.15, 1.0);

    // Add thin border between patches
    float2 inPatch = fmod(pixPos, ps);
    if (inPatch.x < 1.0 || inPatch.y < 1.0)
        return float4(0.06, 0.06, 0.06, 1.0);

    return float4(PATCHES[idx], 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Color Checker";
            desc.effectId = L"Color Checker"; desc.effectVersion = 1;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + colorCheckerHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"PatchSize", L"float", 64.0f, 16.0f, 256.0f, 8.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Zone Plate (Source) ----
        {
            static const std::string zonePlateHLSL = R"HLSL(
// Zone Plate - circular resolution/aliasing test pattern
// Source effect: no input required.

cbuffer constants : register(b0) {
    float Frequency;   // default 0.5
    float PlateSize;   // pixels (default 512)
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float size = max(PlateSize, 64.0);
    float2 center = float2(size * 0.5, size * 0.5);
    float2 p = uv0.xy - center;
    float r2 = dot(p, p);
    float v = 0.5 + 0.5 * cos(Frequency * r2 / size);
    return float4(v, v, v, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Zone Plate";
            desc.effectId = L"Zone Plate"; desc.effectVersion = 1;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + zonePlateHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"Frequency", L"float", 0.5f, 0.01f, 5.0f, 0.01f },
                { L"PlateSize", L"float", 512.0f, 64.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gradient Generator (Source) ----
        {
            static const std::string gradientHLSL = R"HLSL(
// Gradient Generator - linear/radial gradients with HDR support
// Source effect: no input required.

cbuffer constants : register(b0) {
    uint GradientType;  // 0=Linear horizontal, 1=Linear vertical, 2=Radial
    float StartR;   // start color (scRGB)
    float StartG;
    float StartB;
    float EndR;     // end color (scRGB)
    float EndG;
    float EndB;
    float GradSize; // pixels (default 512)
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float size = max(GradSize, 64.0);
    float t = 0;

    if (GradientType < 0.5)
        t = saturate(uv0.x / size);
    else if (GradientType < 1.5 && GradientType > 0.5)
        t = saturate(uv0.y / size);
    else {
        float2 center = float2(size * 0.5, size * 0.5);
        float r = length(uv0.xy - center) / (size * 0.5);
        t = saturate(r);
    }

    float3 startC = float3(StartR, StartG, StartB);
    float3 endC = float3(EndR, EndG, EndB);
    float3 color = lerp(startC, endC, t);
    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Gradient Generator";
            desc.effectId = L"Gradient Generator"; desc.effectVersion = 2;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gradientHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"GradientType", L"float", 0.0f, 0.0f, 2.0f, 1.0f, { L"Linear Horizontal", L"Linear Vertical", L"Radial" } },
                { L"StartR",       L"float", 0.0f, -1.0f, 125.0f, 0.01f },
                { L"StartG",       L"float", 0.0f, -1.0f, 125.0f, 0.01f },
                { L"StartB",       L"float", 0.0f, -1.0f, 125.0f, 0.01f },
                { L"EndR",         L"float", 1.0f, -1.0f, 125.0f, 0.01f },
                { L"EndG",         L"float", 1.0f, -1.0f, 125.0f, 0.01f },
                { L"EndB",         L"float", 1.0f, -1.0f, 125.0f, 0.01f },
                { L"GradSize",     L"float", 512.0f, 64.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- HDR Test Pattern (Source) ----
        {
            static const std::string hdrTestHLSL = R"HLSL(
// HDR Test Pattern - standard patches at known luminance levels
// Source effect: no input required.

cbuffer constants : register(b0) {
    float PatternSize; // pixels (default 512)
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float size = max(PatternSize, 256.0);
    float2 uv = uv0.xy / size;

    // 4x4 grid of patches at different luminance levels
    uint col = (uint)(uv.x * 4.0);
    uint row = (uint)(uv.y * 4.0);
    col = min(col, 3u);
    row = min(row, 3u);

    // Nit levels for each patch (scRGB: nits/80)
    // Row 0: 0.1, 1, 10, 80 nits
    // Row 1: 100, 200, 400, 1000 nits
    // Row 2: 2000, 4000, 10000, primaries
    // Row 3: R, G, B, grayscale ramp
    float nitsTable[12] = {
        0.1, 1.0, 10.0, 80.0,
        100.0, 200.0, 400.0, 1000.0,
        2000.0, 4000.0, 10000.0, 0.0
    };

    float3 color = float3(0, 0, 0);
    uint idx = row * 4 + col;

    if (idx < 11) {
        float scrgb = nitsTable[idx] / 80.0;
        color = float3(scrgb, scrgb, scrgb);
    } else if (idx == 11) {
        // Rec.709 red primary at 80 nits
        color = float3(1, 0, 0);
    } else if (idx == 12) {
        // Rec.709 green primary at 80 nits
        color = float3(0, 1, 0);
    } else if (idx == 13) {
        // Rec.709 blue primary at 80 nits
        color = float3(0, 0, 1);
    } else if (idx == 14) {
        // Yellow
        color = float3(1, 1, 0);
    } else {
        // Grayscale ramp within the patch
        float2 inPatch = frac(uv * 4.0);
        float ramp = inPatch.x;
        color = float3(ramp, ramp, ramp);
    }

    // Thin grid lines
    float2 inPatch = frac(uv * 4.0);
    if (inPatch.x < 0.01 || inPatch.y < 0.01)
        color = float3(0.15, 0.15, 0.15);

    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"HDR Test Pattern";
            desc.effectId = L"HDR Test Pattern"; desc.effectVersion = 1;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + hdrTestHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"PatternSize", L"float", 512.0f, 256.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }


        // ---- Delta E Comparator ----
        {
            static const std::string deltaEHLSL = R"HLSL(
// Delta E Comparator - per-pixel color difference between two inputs
// Supports: CIE76, CIE94, CIEDE2000

cbuffer Constants : register(b0)
{
    uint Method;     // 0 = CIE76, 1 = CIE94, 2 = CIEDE2000
    float Scale;      // Multiplier for visualization (higher = more sensitive)
    float MaxDeltaE;  // Clamp for colormap (dE at this value = full red)
    uint OutputMode; // 0 = Heatmap (Turbo colormap), 1 = Grayscale dE / MaxDeltaE
};

Texture2D InputTexture : register(t0);   // Reference
Texture2D InputTexture1 : register(t1);  // Test
SamplerState InputSampler : register(s0);

// CIE76: simple Euclidean distance in L*a*b*
float DeltaE76(float3 lab1, float3 lab2) {
    float3 d = lab1 - lab2;
    return sqrt(dot(d, d));
}

// CIE94 (graphic arts)
float DeltaE94(float3 lab1, float3 lab2) {
    float dL = lab1.x - lab2.x;
    float C1 = sqrt(lab1.y * lab1.y + lab1.z * lab1.z);
    float C2 = sqrt(lab2.y * lab2.y + lab2.z * lab2.z);
    float dC = C1 - C2;
    float da = lab1.y - lab2.y;
    float db = lab1.z - lab2.z;
    float dH2 = da * da + db * db - dC * dC;
    float dH = sqrt(max(dH2, 0.0));
    float SL = 1.0;
    float SC = 1.0 + 0.045 * C1;
    float SH = 1.0 + 0.015 * C1;
    float t1 = dL / SL;
    float t2 = dC / SC;
    float t3 = dH / SH;
    return sqrt(t1*t1 + t2*t2 + t3*t3);
}

// CIEDE2000
float DeltaE2000(float3 lab1, float3 lab2) {
    float L1 = lab1.x, a1 = lab1.y, b1 = lab1.z;
    float L2 = lab2.x, a2 = lab2.y, b2 = lab2.z;

    float Cab1 = sqrt(a1*a1 + b1*b1);
    float Cab2 = sqrt(a2*a2 + b2*b2);
    float Cab_avg = (Cab1 + Cab2) * 0.5;
    float Cab_avg7 = pow(Cab_avg, 7.0);
    float G = 0.5 * (1.0 - sqrt(Cab_avg7 / (Cab_avg7 + 6103515625.0))); // 25^7

    float a1p = a1 * (1.0 + G);
    float a2p = a2 * (1.0 + G);
    float C1p = sqrt(a1p*a1p + b1*b1);
    float C2p = sqrt(a2p*a2p + b2*b2);

    // Hue angles. atan2(0, 0) is implementation-defined and on some
    // drivers returns NaN — guard against (a,b)==(0,0) by treating C==0
    // as h==0 (the hue angle of an achromatic point is meaningless and
    // any consistent value works because the C-weighted hp_avg branch
    // below short-circuits to h1p+h2p).
    float h1p = (C1p < 1e-10) ? 0.0 : atan2(b1, a1p);
    if (h1p < 0.0) h1p += 6.28318530718;
    float h2p = (C2p < 1e-10) ? 0.0 : atan2(b2, a2p);
    if (h2p < 0.0) h2p += 6.28318530718;

    float dLp = L2 - L1;
    float dCp = C2p - C1p;

    float dhp;
    if (C1p * C2p < 1e-10) dhp = 0.0;
    else {
        dhp = h2p - h1p;
        if (dhp > 3.14159265359) dhp -= 6.28318530718;
        else if (dhp < -3.14159265359) dhp += 6.28318530718;
    }
    float dHp = 2.0 * sqrt(C1p * C2p) * sin(dhp * 0.5);

    float Lp_avg = (L1 + L2) * 0.5;
    float Cp_avg = (C1p + C2p) * 0.5;

    float hp_avg;
    if (C1p * C2p < 1e-10) hp_avg = h1p + h2p;
    else {
        hp_avg = (h1p + h2p) * 0.5;
        if (abs(h1p - h2p) > 3.14159265359) hp_avg += 3.14159265359;
    }

    float T = 1.0
        - 0.17 * cos(hp_avg - 0.52359877559)       // 30 deg
        + 0.24 * cos(2.0 * hp_avg)
        + 0.32 * cos(3.0 * hp_avg + 0.10471975512)  // 6 deg
        - 0.20 * cos(4.0 * hp_avg - 1.09955742876);  // 63 deg

    float Lp50sq = (Lp_avg - 50.0) * (Lp_avg - 50.0);
    float SL = 1.0 + 0.015 * Lp50sq / sqrt(20.0 + Lp50sq);
    float SC = 1.0 + 0.045 * Cp_avg;
    float SH = 1.0 + 0.015 * Cp_avg * T;

    float Cp_avg7 = pow(Cp_avg, 7.0);
    float RC = 2.0 * sqrt(Cp_avg7 / (Cp_avg7 + 6103515625.0));
    float hp_deg = hp_avg * 57.29577951; // rad to deg
    float dtheta = 30.0 * exp(-((hp_deg - 275.0) / 25.0) * ((hp_deg - 275.0) / 25.0));
    float RT = -sin(2.0 * dtheta * 0.01745329252) * RC;

    float t1 = dLp / SL;
    float t2 = dCp / SC;
    float t3 = dHp / SH;
    return sqrt(t1*t1 + t2*t2 + t3*t3 + RT * t2 * t3);
}

float4 main(
    float4 pos      : SV_POSITION,
    float4 posScene : SCENE_POSITION,
    float4 uv0      : TEXCOORD0,
    float4 uv1      : TEXCOORD1
) : SV_Target
{
    float4 ref  = InputTexture.Sample(InputSampler, uv0.xy);
    float4 test = InputTexture1.Sample(InputSampler, uv1.xy);

    float3 labRef  = ScRGBToLab(ref.rgb);
    float3 labTest = ScRGBToLab(test.rgb);

    float dE;
    uint method = Method;
    if (method == 1)      dE = DeltaE94(labRef, labTest);
    else if (method == 2) dE = DeltaE2000(labRef, labTest);
    else                  dE = DeltaE76(labRef, labTest);

    dE *= Scale;

    // Always read OutputMode unconditionally so D3DCompile can't strip it
    // (gotcha #2 in CLAUDE.md). Branch on the read value below.
    float mode = OutputMode;

    // Map to colormap: 0 = black (exact match), MaxDeltaE = full red.
    float t = saturate(dE / max(MaxDeltaE, 0.01));

    if (mode > 0.5)
    {
        // Grayscale dE: gray = saturate(dE / MaxDeltaE), output to RGB.
        // This makes mean(R) directly readable as fractional dE — a
        // downstream Luminance Statistics node yields live mean/p95/max
        // dE values without needing CPU readback of every pixel.
        return float4(t, t, t, 1.0);
    }

    float3 color = TurboColormap(t) * smoothstep(0.0, 0.02, t);
    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Delta E Comparator";
            desc.effectId = L"Delta E Comparator"; desc.effectVersion = 5;
            desc.category = L"Analysis";
            desc.subcategory = L"Comparison";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + deltaEHLSL;
            desc.inputNames = { L"Reference", L"Test" };
            desc.parameters = {
                { L"Method",     L"float", 2.0f, 0.0f, 2.0f, 1.0f, { L"CIE76", L"CIE94", L"CIEDE2000" } },
                { L"Scale",      L"float", 1.0f, 0.1f, 10.0f, 0.1f },
                { L"MaxDeltaE",  L"float", 1.0f, 0.1f, 100.0f, 0.1f },
                { L"OutputMode", L"float", 0.0f, 0.0f, 1.0f, 1.0f, { L"Heatmap", L"Grayscale dE" } },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- False Color Luminance Map ----
        {
            static const std::string falseColorHLSL = R"HLSL(
// False Color Luminance Map - maps nit ranges to distinct colors

cbuffer Constants : register(b0)
{
    float Opacity;  // Blend with original (1.0 = full false color)
};

Texture2D Source : register(t0);

float3 FalseColor(float nits) {
    // Purple < 0.5 < Blue < 5 < Cyan < 20 < Green < 100 < Yellow < 400 < Orange < 1000 < Red < 4000 < White
    if (nits < 0.5)   return float3(0.2, 0.0, 0.4);    // Purple - crushed blacks
    if (nits < 5.0)   return float3(0.0, 0.0, 0.8);    // Blue - deep shadows
    if (nits < 20.0)  return float3(0.0, 0.6, 0.8);    // Cyan - shadow detail
    if (nits < 100.0) return float3(0.0, 0.7, 0.0);    // Green - mid-tones (SDR)
    if (nits < 400.0) return float3(0.9, 0.9, 0.0);    // Yellow - highlights
    if (nits < 1000.0) return float3(1.0, 0.5, 0.0);   // Orange - HDR specular
    if (nits < 4000.0) return float3(1.0, 0.0, 0.0);   // Red - bright HDR
    return float3(1.0, 1.0, 1.0);                       // White - peak HDR
}

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0
) : SV_Target
{
    float4 color = Source.Load(int3(uv0.xy, 0));
    float nits = ScRGBLuminanceNits(color.rgb);
    float3 fc = FalseColor(nits);
    float3 result = lerp(color.rgb, fc, Opacity);
    return float4(result, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Nit Map";
            desc.effectId = L"Nit Map"; desc.effectVersion = 1;
            desc.category = L"Analysis";
            desc.subcategory = L"Highlights";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + falseColorHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Opacity", L"float", 1.0f, 0.0f, 1.0f, 0.05f },
            };
            m_effects.push_back(std::move(desc));
        }


        // ---- Gamut Volume Coverage ----
        {
            static const std::string gamutCoverageHLSL = R"HLSL(
// Gamut Volume Coverage - visualizes which gamut regions the source occupies.
// Migrated to D3D11 compute (Phase 8c perf): each thread group scatters all
// source pixels into a 64x64 chromaticity histogram in groupshared, then
// renders the 512x512 output diagram + triangle outline by reading the
// histogram. Total cost is O(srcW*srcH + outW*outH) -- ~8M ops on a 4K
// source -- vs the previous pixel-shader gather's O(srcSamples * outW*outH)
// which was ~17B ops at the same input.

Texture2D<float4>    Source : register(t0);
RWTexture2D<float4>  Output : register(u1);

cbuffer Constants : register(b0)
{
    uint  Width;            // auto-injected by bridge
    uint  Height;
    float DiagramSize;      // output side length in pixels
    uint  TargetGamut;      // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
};

// Triangle-edge antialiased line: returns 1 inside the line band, 0 outside.
float TriangleEdge(float2 p, float2 a, float2 b, float lineW) {
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / dot(ab, ab));
    float d = length(p - a - ab * t);
    return smoothstep(lineW, 0.0, d);
}

// 64x64 chromaticity histogram. 4096 bins x 4 bytes x 2 buffers = 32 KB.
#define BINS 64
groupshared uint gs_inGamut[BINS * BINS];
groupshared uint gs_outGamut[BINS * BINS];

[numthreads(32, 32, 1)]
void main(uint3 GTid : SV_GroupThreadID)
{
    uint tid = GTid.x + GTid.y * 32;
    const uint totalThreads = 1024;

    uint outSize = max((uint)DiagramSize, 64u);

    // Read cbuffer (keep all GPU-resident on every code path so DXC
    // doesnt eliminate the slot).
    float gamutF = TargetGamut;
    float2 cR = RedPrimary;
    float2 cG = GreenPrimary;
    float2 cB = BluePrimary;

    // Resolve target gamut vertices once per group.
    float2 tR, tG, tB;
    uint gamut = (uint)gamutF;
    if (gamut == 1)      { tR = GAMUT_P3_R;   tG = GAMUT_P3_G;   tB = GAMUT_P3_B; }
    else if (gamut == 2) { tR = GAMUT_2020_R; tG = GAMUT_2020_G; tB = GAMUT_2020_B; }
    else if (gamut == 3) { tR = cR;           tG = cG;           tB = cB; }
    else                 { tR = GAMUT_709_R;  tG = GAMUT_709_G;  tB = GAMUT_709_B; }

    // Phase 1: zero groupshared.
    for (uint c = tid; c < BINS * BINS; c += totalThreads)
    {
        gs_inGamut[c]  = 0;
        gs_outGamut[c] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Phase 2: scatter every source pixel into chromaticity bins.
    uint totalPixels = Width * Height;
    for (uint pi = tid; pi < totalPixels; pi += totalThreads)
    {
        uint px = pi % Width;
        uint py = pi / Width;
        float4 s = Source[int2(px, py)];
        if (s.a < 0.001) continue;
        float3 xyz = ScRGBToXYZ(max(s.rgb, 0.0));
        float sum = xyz.x + xyz.y + xyz.z;
        if (sum < 1e-6) continue;
        float cieX = xyz.x / sum;
        float cieY = xyz.y / sum;

        // Map to bin coords matching the diagrams CIE x=[0,0.8], y=[0,0.9].
        float u = cieX / 0.8;
        float v = 1.0 - cieY / 0.9;
        if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;

        uint bx = min((uint)(u * BINS), BINS - 1);
        uint by = min((uint)(v * BINS), BINS - 1);
        uint binIdx = by * BINS + bx;

        bool inGamut = PointInTriangle(float2(cieX, cieY), tR, tG, tB);
        if (inGamut) InterlockedAdd(gs_inGamut[binIdx],  1u);
        else         InterlockedAdd(gs_outGamut[binIdx], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // Phase 3: render the 2D diagram. Each thread strides through output
    // pixels, writing bg + triangle outline + colored coverage dots.
    float lineW = 0.003;
    float dotIntensityScale = 0.05; // softens hot spots so a few hits dont saturate

    for (uint oi = tid; oi < outSize * outSize; oi += totalThreads)
    {
        uint ox = oi % outSize;
        uint oy = oi / outSize;
        float2 uvNorm = float2((ox + 0.5) / (float)outSize, (oy + 0.5) / (float)outSize);
        float2 cie_xy = float2(uvNorm.x * 0.8, (1.0 - uvNorm.y) * 0.9);

        float3 color = float3(0.05, 0.05, 0.05);
        float edge = TriangleEdge(cie_xy, tR, tG, lineW)
                   + TriangleEdge(cie_xy, tG, tB, lineW)
                   + TriangleEdge(cie_xy, tB, tR, lineW);
        color += float3(0.3, 0.3, 0.3) * saturate(edge);

        // Look up the bin this output pixel maps to.
        uint bx = min((uint)(uvNorm.x * BINS), BINS - 1);
        uint by = min((uint)(uvNorm.y * BINS), BINS - 1);
        uint binIdx = by * BINS + bx;
        uint inHits  = gs_inGamut[binIdx];
        uint outHits = gs_outGamut[binIdx];

        if (inHits > 0)
            color += float3(0.0, 0.8, 0.0) * saturate(float(inHits) * dotIntensityScale);
        if (outHits > 0)
            color += float3(1.0, 0.2, 0.0) * saturate(float(outHits) * dotIntensityScale);

        Output[int2(ox, oy)] = float4(color, 1.0);
    }
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Coverage";
            desc.effectId = L"Gamut Coverage"; desc.effectVersion = 6;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 32;
            desc.threadGroupY = 32;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + gamutCoverageHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"DiagramSize",  L"float", 512.0f, 128.0f, 4096.0f, 64.0f },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" } },
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gamut Map ----
        {
            static const std::string gamutMapHLSL = R"HLSL(
// Gamut Map - constrains input colors to a target gamut.
// Four modes:
//   0: Clip - hard clamp RGB components to [0,1] in target space
//   1: Nearest - project out-of-gamut CIE xy to nearest point on gamut triangle
//   2: Compress to White - move out-of-gamut xy toward D65 white until inside
//   3: Fit Gamut - uniformly scale all chromaticities to fit source gamut inside target

cbuffer Constants : register(b0)
{
    uint Mode;        // 0=Clip, 1=Nearest, 2=Compress, 3=Fit Gamut
    uint TargetGamut; // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float Strength;    // 0=bypass, 1=full mapping
    uint SourceGamut; // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float2 TargetRedPrimary;
    float2 TargetGreenPrimary;
    float2 TargetBluePrimary;
    float2 SourceRedPrimary;
    float2 SourceGreenPrimary;
    float2 SourceBluePrimary;
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

// Nearest point on line segment AB to point P
float2 NearestOnSegment(float2 p, float2 a, float2 b) {
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-10));
    return a + ab * t;
}

// Find the nearest point on triangle (a,b,c) boundary to point p
float2 NearestOnTriangle(float2 p, float2 a, float2 b, float2 c) {
    float2 p0 = NearestOnSegment(p, a, b);
    float2 p1 = NearestOnSegment(p, b, c);
    float2 p2 = NearestOnSegment(p, c, a);
    float d0 = dot(p - p0, p - p0);
    float d1 = dot(p - p1, p - p1);
    float d2 = dot(p - p2, p - p2);
    float2 best = p0;
    float bestD = d0;
    if (d1 < bestD) { best = p1; bestD = d1; }
    if (d2 < bestD) { best = p2; }
    return best;
}

// Line-segment intersection: find t where ray from P in direction dir crosses segment AB.
float RaySegmentIntersect(float2 p, float2 dir, float2 a, float2 b) {
    float2 ab = b - a;
    float denom = dir.x * ab.y - dir.y * ab.x;
    float result = -1.0;
    if (abs(denom) >= 1e-10)
    {
        float2 pa = a - p;
        float t = (pa.x * ab.y - pa.y * ab.x) / denom;
        float u = (pa.x * dir.y - pa.y * dir.x) / denom;
        if (t > 0.0 && u >= 0.0 && u <= 1.0)
            result = t;
    }
    return result;
}

// Move point toward white (D65) until it hits the gamut triangle boundary
float2 CompressToWhite(float2 p, float2 a, float2 b, float2 c) {
    float2 white = float2(0.3127, 0.3290);
    float2 dir = white - p;
    float tMin = 1e10;
    float t0 = RaySegmentIntersect(p, dir, a, b);
    float t1 = RaySegmentIntersect(p, dir, b, c);
    float t2 = RaySegmentIntersect(p, dir, c, a);
    if (t0 > 0 && t0 < tMin) tMin = t0;
    if (t1 > 0 && t1 < tMin) tMin = t1;
    if (t2 > 0 && t2 < tMin) tMin = t2;
    return (tMin < 1.0) ? p + dir * tMin : white;
}

// Compute how far a primary extends beyond the target boundary (ratio > 1 = outside)
float PrimaryExcursion(float2 primary, float2 white, float2 tR, float2 tG, float2 tB) {
    float2 dir = primary - white;
    float tMin = 1e10;
    float t0 = RaySegmentIntersect(white, dir, tR, tG);
    float t1 = RaySegmentIntersect(white, dir, tG, tB);
    float t2 = RaySegmentIntersect(white, dir, tB, tR);
    if (t0 > 0 && t0 < tMin) tMin = t0;
    if (t1 > 0 && t1 < tMin) tMin = t1;
    if (t2 > 0 && t2 < tMin) tMin = t2;
    // tMin is where ray hits boundary; primary is at t=1. Ratio = 1/tMin.
    return (tMin > 0 && tMin < 1e9) ? 1.0 / tMin : 1.0;
}

// Compute uniform scale factor to fit source gamut inside target gamut
float ComputeFitScale(float2 sR, float2 sG, float2 sB,
                      float2 tR, float2 tG, float2 tB) {
    float2 white = float2(0.3127, 0.3290);
    float maxExcursion = max(max(
        PrimaryExcursion(sR, white, tR, tG, tB),
        PrimaryExcursion(sG, white, tR, tG, tB)),
        PrimaryExcursion(sB, white, tR, tG, tB));
    return (maxExcursion > 1.0) ? 1.0 / maxExcursion : 1.0;
}

float4 main(
    float4 pos      : SV_POSITION,
    float4 posScene : SCENE_POSITION,
    float4 uv0      : TEXCOORD0
) : SV_Target
{
    float4 color = InputTexture.Sample(InputSampler, uv0.xy);
    if (Strength < 0.001) return color;

    // Near-black pixels have unreliable chromaticity — pass through unchanged.
    float lum = dot(max(color.rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
    if (lum < 1e-5) return color;

    // Read all cbuffer vars at top so DXC keeps them resident.
    float targetF = TargetGamut;
    float sourceF = SourceGamut;
    float2 ctR = TargetRedPrimary;
    float2 ctG = TargetGreenPrimary;
    float2 ctB = TargetBluePrimary;
    float2 csR = SourceRedPrimary;
    float2 csG = SourceGreenPrimary;
    float2 csB = SourceBluePrimary;

    // Get target gamut primaries
    float2 gR, gG, gB;
    uint gamut = (uint)targetF;
    if (gamut == 1)      { gR = GAMUT_P3_R; gG = GAMUT_P3_G; gB = GAMUT_P3_B; }
    else if (gamut == 2) { gR = GAMUT_2020_R; gG = GAMUT_2020_G; gB = GAMUT_2020_B; }
    else if (gamut == 3) { gR = ctR; gG = ctG; gB = ctB; }
    else                 { gR = GAMUT_709_R; gG = GAMUT_709_G; gB = GAMUT_709_B; }

    uint mode = Mode;

    if (mode == 0)
    {
        // Clip mode: transform to target gamut RGB, clamp, transform back.
        // For sRGB (gamut 0), the pipeline is already in Rec.709, so just clamp.
        float3 rgb = color.rgb;

        if (gamut == 0) {
            // sRGB = Rec.709 primaries = scRGB primaries. Just clamp negatives.
            float3 clamped = max(rgb, 0.0);
            color.rgb = lerp(rgb, clamped, Strength);
        }
        else if (gamut == 1) {
            // scRGB -> XYZ -> P3 -> clamp -> XYZ -> scRGB
            float3 xyz = ScRGBToXYZ(rgb);
            float3 p3 = mul(XYZ_TO_P3D65, xyz);
            float3 clamped = max(p3, 0.0);
            float3 xyzBack = mul(P3D65_TO_XYZ, clamped);
            float3 result = XYZToScRGB(xyzBack);
            color.rgb = lerp(rgb, result, Strength);
        }
        else {
            // scRGB -> XYZ -> BT.2020 -> clamp -> XYZ -> scRGB
            float3 xyz = ScRGBToXYZ(rgb);
            float3 bt2020 = mul(XYZ_TO_REC2020, xyz);
            float3 clamped = max(bt2020, 0.0);
            float3 xyzBack = mul(REC2020_TO_XYZ, clamped);
            float3 result = XYZToScRGB(xyzBack);
            color.rgb = lerp(rgb, result, Strength);
        }
    }
    else if (mode == 3)
    {
        // Fit Gamut: uniformly scale all chromaticities toward D65 white.
        float2 sR, sG, sB;
        uint sg = SourceGamut;
        if (sg == 1)      { sR = GAMUT_P3_R; sG = GAMUT_P3_G; sB = GAMUT_P3_B; }
        else if (sg == 2) { sR = GAMUT_2020_R; sG = GAMUT_2020_G; sB = GAMUT_2020_B; }
        else if (sg == 3) { sR = csR; sG = csG; sB = csB; }
        else              { sR = GAMUT_709_R; sG = GAMUT_709_G; sB = GAMUT_709_B; }

        float scale = ComputeFitScale(sR, sG, sB, gR, gG, gB);
        if (scale >= 1.0) return color;

        float3 xyz = ScRGBToXYZ(color.rgb);
        float sum = xyz.x + xyz.y + xyz.z;
        if (sum < 1e-6) return color;

        float2 xy = float2(xyz.x / sum, xyz.y / sum);
        float Y = max(xyz.y, 0.0);
        float2 white = float2(0.3127, 0.3290);

        float s = lerp(1.0, scale, Strength);
        xy = white + (xy - white) * s;

        float X = (xy.y > 1e-6) ? xy.x * Y / xy.y : 0.0;
        float Z = (xy.y > 1e-6) ? (1.0 - xy.x - xy.y) * Y / xy.y : 0.0;
        color.rgb = XYZToScRGB(float3(X, Y, Z));
    }
    else
    {
        // Use raw scRGB (with negatives) for XYZ to preserve true chromaticity.
        float3 xyz = ScRGBToXYZ(color.rgb);
        float sum = xyz.x + xyz.y + xyz.z;
        if (sum < 1e-6) return color;

        float2 xy = float2(xyz.x / sum, xyz.y / sum);
        float Y = max(xyz.y, 0.0);  // Preserve luminance (clamp negative Y)

        // Check if inside gamut triangle
        bool inside = PointInTriangle(xy, gR, gG, gB);
        if (inside) return color;  // In-gamut: pass through unchanged

        float2 mapped;
        if (mode == 1)
            mapped = NearestOnTriangle(xy, gR, gG, gB);
        else
            mapped = CompressToWhite(xy, gR, gG, gB);

        xy = lerp(xy, mapped, Strength);

        // Reconstruct XYZ from mapped xy + original Y
        float X = (xy.y > 1e-6) ? xy.x * Y / xy.y : 0.0;
        float Z = (xy.y > 1e-6) ? (1.0 - xy.x - xy.y) * Y / xy.y : 0.0;
        color.rgb = XYZToScRGB(float3(X, Y, Z));
    }

    return color;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Map";
            desc.effectId = L"Gamut Map"; desc.effectVersion = 5;
            desc.category = L"Analysis";
            desc.subcategory = L"Gamut Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gamutMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Mode",        L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Clip", L"Nearest Point", L"Compress to White", L"Fit Gamut" } },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" } },
                { L"Strength",    L"float", 1.0f, 0.0f, 1.0f, 0.05f },
                { L"SourceGamut", L"float", 2.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" }, L"Mode == 3" },
                // Custom-mode primaries. Bind to Working Space.RedPrimary etc.
                // Target primaries appear when TargetGamut == 3 (Custom).
                { L"TargetRedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"TargetGreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"TargetBluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                // Source primaries appear when Mode == 3 (Fit Gamut) AND
                // SourceGamut == 3 (Custom). Currently only one visibleWhen
                // condition is supported per parameter, so we gate on the
                // narrower SourceGamut == 3 — this is correct because the
                // SourceGamut parameter itself is gated on Mode == 3.
                { L"SourceRedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.708f, 0.292f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
                { L"SourceGreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.170f, 0.797f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
                { L"SourceBluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.131f, 0.046f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Gamut Map ----
        {
            static const std::string perceptualGamutMapHLSL = R"HLSL(
// ICtCp Gamut Map - perceptual gamut mapping in BT.2100 ICtCp space.
// Samples the target gamut boundary as a polygon in the Ct/Cp plane
// at the pixel's intensity level, then maps out-of-gamut pixels.

cbuffer Constants : register(b0)
{
    uint Mode;          // 0=Nearest on Shell, 1=Compress to Neutral, 2=Fit to Shell
    uint TargetGamut;   // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float Strength;      // 0=bypass, 1=full
    uint SourceGamut;   // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float2 TargetRedPrimary;
    float2 TargetGreenPrimary;
    float2 TargetBluePrimary;
    float2 SourceRedPrimary;
    float2 SourceGreenPrimary;
    float2 SourceBluePrimary;
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

#define NBP 48

void SampleBoundary(float2 gR, float2 gG, float2 gB, float iVal, out float2 bnd[NBP])
{
    float nits = PQ_EOTF(iVal);
    float Ys = max(nits / 80.0, 0.0001);
    uint ppe = NBP / 3;
    for (uint i = 0; i < NBP; i++)
    {
        float2 xy;
        uint e = i / ppe;
        float t = (float)(i % ppe) / (float)ppe;
        if (e == 0)      xy = lerp(gR, gG, t);
        else if (e == 1) xy = lerp(gG, gB, t);
        else             xy = lerp(gB, gR, t);
        float X = (xy.y > 1e-6) ? xy.x * Ys / xy.y : 0;
        float Z = (xy.y > 1e-6) ? (1.0 - xy.x - xy.y) * Ys / xy.y : 0;
        float3 ic = ScRGBToICtCp(XYZToScRGB(float3(X, Ys, Z)));
        bnd[i] = float2(ic.y, ic.z);
    }
}

bool PtInPoly(float2 p, float2 poly[NBP])
{
    bool inside = false;
    for (uint i = 0, j = NBP - 1; i < NBP; j = i++)
    {
        if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
            (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
            inside = !inside;
    }
    return inside;
}

float2 NearestOnPoly(float2 p, float2 poly[NBP])
{
    float bestD2 = 1e10;
    float2 bestPt = p;
    for (uint i = 0; i < NBP; i++)
    {
        uint j = (i + 1) % NBP;
        float2 ab = poly[j] - poly[i];
        float t = saturate(dot(p - poly[i], ab) / max(dot(ab, ab), 1e-10));
        float2 proj = poly[i] + ab * t;
        float d2 = dot(p - proj, p - proj);
        if (d2 < bestD2) { bestD2 = d2; bestPt = proj; }
    }
    return bestPt;
}

float2 CompressNeutral(float2 p, float2 poly[NBP])
{
    float2 dir = float2(0,0) - p;
    float bestT = 1e10;
    for (uint i = 0; i < NBP; i++)
    {
        uint j = (i + 1) % NBP;
        float2 a = poly[i], b = poly[j];
        float2 ab = b - a;
        float denom = dir.x * ab.y - dir.y * ab.x;
        if (abs(denom) < 1e-10) continue;
        float2 pa = a - p;
        float t = (pa.x * ab.y - pa.y * ab.x) / denom;
        float u = (pa.x * dir.y - pa.y * dir.x) / denom;
        if (t > 0.0 && u >= 0.0 && u <= 1.0 && t < bestT)
            bestT = t;
    }
    float2 result = NearestOnPoly(p, poly);
    if (bestT < 1.0)
        result = p + dir * bestT;
    return result;
}

// Compute uniform ICtCp scale factor: for each source boundary vertex,
// find how far it extends beyond the target boundary (ray from neutral).
float ComputeICtCpFitScale(float2 srcBnd[NBP], float2 tgtBnd[NBP])
{
    float maxRatio = 1.0;
    for (uint i = 0; i < NBP; i++)
    {
        float2 p = srcBnd[i];
        float distSrc = length(p);
        if (distSrc < 1e-8) continue;

        // Ray from neutral (0,0) through source boundary point
        float2 dir = p;
        float bestT = 1e10;
        for (uint j = 0; j < NBP; j++)
        {
            uint k = (j + 1) % NBP;
            float2 a = tgtBnd[j], b = tgtBnd[k];
            float2 ab = b - a;
            float denom = dir.x * ab.y - dir.y * ab.x;
            if (abs(denom) < 1e-10) continue;
            float2 pa = a; // a - origin(0,0)
            float t = (pa.x * ab.y - pa.y * ab.x) / denom;
            float u = (pa.x * dir.y - pa.y * dir.x) / denom;
            if (t > 0.0 && u >= 0.0 && u <= 1.0 && t < bestT)
                bestT = t;
        }
        // Target boundary is at t*dir from neutral; source vertex is at 1.0*dir.
        // Ratio = 1/bestT: if bestT < 1, source extends beyond target.
        if (bestT > 0 && bestT < 1e9)
            maxRatio = max(maxRatio, 1.0 / bestT);
    }
    return (maxRatio > 1.0) ? 1.0 / maxRatio : 1.0;
}

float4 main(
    float4 pos : SV_POSITION, float4 ps : SCENE_POSITION, float4 uv0 : TEXCOORD0
) : SV_Target
{
    float4 color = InputTexture.Sample(InputSampler, uv0.xy);
    if (Strength < 0.001) return color;

    // Near-black pixels have unreliable chromaticity — pass through unchanged.
    float lum = dot(max(color.rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
    if (lum < 1e-5) return color;

    // Read all cbuffer vars at top so DXC keeps them resident.
    float targetF = TargetGamut;
    float sourceF = SourceGamut;
    float2 ctR = TargetRedPrimary;
    float2 ctG = TargetGreenPrimary;
    float2 ctB = TargetBluePrimary;
    float2 csR = SourceRedPrimary;
    float2 csG = SourceGreenPrimary;
    float2 csB = SourceBluePrimary;

    float2 gR, gG, gB;
    uint g = (uint)targetF;
    if (g == 1)      { gR = GAMUT_P3_R; gG = GAMUT_P3_G; gB = GAMUT_P3_B; }
    else if (g == 2) { gR = GAMUT_2020_R; gG = GAMUT_2020_G; gB = GAMUT_2020_B; }
    else if (g == 3) { gR = ctR; gG = ctG; gB = ctB; }
    else             { gR = GAMUT_709_R; gG = GAMUT_709_G; gB = GAMUT_709_B; }

    // Use CIE xy triangle test for reliable in/out-of-gamut detection,
    // then do the actual mapping in ICtCp for perceptual quality.
    float3 xyz = ScRGBToXYZ(max(color.rgb, 0.0));
    float xyzSum = xyz.x + xyz.y + xyz.z;
    if (xyzSum < 1e-6) return color;
    float2 cieXY = float2(xyz.x / xyzSum, xyz.y / xyzSum);

    uint mode = Mode;

    if (mode == 2)
    {
        // Fit to Shell: uniformly scale all Ct/Cp toward neutral axis.
        float2 sR, sG, sB;
        uint sg = SourceGamut;
        if (sg == 1)      { sR = GAMUT_P3_R; sG = GAMUT_P3_G; sB = GAMUT_P3_B; }
        else if (sg == 2) { sR = GAMUT_2020_R; sG = GAMUT_2020_G; sB = GAMUT_2020_B; }
        else if (sg == 3) { sR = csR; sG = csG; sB = csB; }
        else              { sR = GAMUT_709_R; sG = GAMUT_709_G; sB = GAMUT_709_B; }

        float3 ictcp = ScRGBToICtCp(max(color.rgb, 0.0));
        float origY = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));

        // Sample both source and target boundaries at this I level
        float2 srcBnd[NBP], tgtBnd[NBP];
        SampleBoundary(sR, sG, sB, ictcp.x, srcBnd);
        SampleBoundary(gR, gG, gB, ictcp.x, tgtBnd);

        float scale = ComputeICtCpFitScale(srcBnd, tgtBnd);
        if (scale >= 1.0) return color;

        float2 ctcp = float2(ictcp.y, ictcp.z);
        float s = lerp(1.0, scale, Strength);
        ctcp *= s;

        float3 mappedRGB = ICtCpToScRGB(float3(ictcp.x, ctcp.x, ctcp.y));
        float mappedY = dot(max(mappedRGB, 0.0), float3(0.2126, 0.7152, 0.0722));
        if (mappedY > 1e-6)
            mappedRGB *= origY / mappedY;
        color.rgb = mappedRGB;
    }
    else
    {
        // Modes 0 and 1: per-pixel nearest/compress (only out-of-gamut pixels).
        // Use CIE xy detection with slight inset to avoid boundary jitter.
        // Inset the triangle slightly toward white so boundary pixels consistently
        // classify as out-of-gamut rather than randomly flickering.
        float2 white = float2(0.3127, 0.3290);
        float inset = 0.002;
        float2 iR = lerp(gR, white, inset);
        float2 iG = lerp(gG, white, inset);
        float2 iB = lerp(gB, white, inset);
        bool insideGamut = PointInTriangle(cieXY, iR, iG, iB);
        if (!insideGamut)
        {
            float3 ictcp = ScRGBToICtCp(max(color.rgb, 0.0));
            float2 ctcp = float2(ictcp.y, ictcp.z);
            float origY = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));

            float2 bnd[NBP];
            SampleBoundary(gR, gG, gB, ictcp.x, bnd);

            float2 mapped = (mode == 1)
                ? CompressNeutral(ctcp, bnd)
                : NearestOnPoly(ctcp, bnd);
            ctcp = lerp(ctcp, mapped, Strength);
            float3 mappedRGB = ICtCpToScRGB(float3(ictcp.x, ctcp.x, ctcp.y));
            float mappedY = dot(max(mappedRGB, 0.0), float3(0.2126, 0.7152, 0.0722));
            if (mappedY > 1e-6)
                mappedRGB *= origY / mappedY;
            color.rgb = mappedRGB;
        }
    }
    return color;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Gamut Map";
            desc.effectId = L"ICtCp Gamut Map"; desc.effectVersion = 10;
            desc.category = L"Analysis";
            desc.subcategory = L"Gamut Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + perceptualGamutMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Mode",        L"float", 0.0f, 0.0f, 2.0f, 1.0f, { L"Nearest on Shell", L"Compress to Neutral", L"Fit to Shell" } },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" } },
                { L"Strength",    L"float", 1.0f, 0.0f, 1.0f, 0.05f },
                { L"SourceGamut", L"float", 2.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" }, L"Mode == 2" },
                { L"TargetRedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"TargetGreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"TargetBluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"SourceRedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.708f, 0.292f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
                { L"SourceGreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.170f, 0.797f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
                { L"SourceBluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.131f, 0.046f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Boundary Viewer ----
        {
            static const std::string ictcpBoundaryHLSL = R"HLSL(
// ICtCp Boundary Viewer - visualizes the gamut boundary in ICtCp Ct/Cp space
// at multiple intensity (I) levels.

cbuffer Constants : register(b0)
{
    float DiagramSize;
    uint TargetGamut;
    float Intensity;   // Which I level to highlight (PQ domain, 0-1)
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

#define NBP 24

void SampleBnd(float2 gR, float2 gG, float2 gB, float iVal, out float2 bnd[NBP])
{
    float nits = PQ_EOTF(iVal);
    float Ys = max(nits / 80.0, 0.0001);
    uint ppe = NBP / 3;
    for (uint i = 0; i < NBP; i++)
    {
        float2 xy;
        uint e = i / ppe;
        float t = (float)(i % ppe) / (float)ppe;
        if (e == 0)      xy = lerp(gR, gG, t);
        else if (e == 1) xy = lerp(gG, gB, t);
        else             xy = lerp(gB, gR, t);
        float X = (xy.y > 1e-6) ? xy.x * Ys / xy.y : 0;
        float Z = (xy.y > 1e-6) ? (1.0 - xy.x - xy.y) * Ys / xy.y : 0;
        float3 ic = ScRGBToICtCp(XYZToScRGB(float3(X, Ys, Z)));
        bnd[i] = float2(ic.y, ic.z);
    }
}

float4 main(
    float4 pos : SV_POSITION, float4 ps : SCENE_POSITION, float4 uv0 : TEXCOORD0
) : SV_Target
{
    float size = max(DiagramSize, 256.0);
    float2 ctcp = (uv0.xy - 0.5) * 1.0;

    // Read all cbuffer vars at top so DXC keeps them resident.
    float gamutF = TargetGamut;
    float2 cR = RedPrimary;
    float2 cG = GreenPrimary;
    float2 cB = BluePrimary;

    float2 gR, gG, gB;
    uint g = (uint)gamutF;
    if (g == 1)      { gR = GAMUT_P3_R; gG = GAMUT_P3_G; gB = GAMUT_P3_B; }
    else if (g == 2) { gR = GAMUT_2020_R; gG = GAMUT_2020_G; gB = GAMUT_2020_B; }
    else if (g == 3) { gR = cR; gG = cG; gB = cB; }
    else             { gR = GAMUT_709_R; gG = GAMUT_709_G; gB = GAMUT_709_B; }

    float3 color = float3(0.01, 0.01, 0.01);
    float lineW = 1.5 / size;
    float dotR = 3.0 / size;

    // Draw boundaries at 5 fixed I levels + the user-selected one
    float iLevels[6] = { 0.15, 0.3, 0.45, 0.6, 0.75, Intensity };
    float3 lColors[6] = {
        float3(0.15, 0.05, 0.05),
        float3(0.15, 0.1, 0.05),
        float3(0.05, 0.15, 0.05),
        float3(0.05, 0.1, 0.15),
        float3(0.1, 0.05, 0.15),
        float3(0.4, 0.4, 0.0)   // User-selected level in yellow
    };

    for (int lv = 0; lv < 6; lv++)
    {
        float2 bnd[NBP];
        SampleBnd(gR, gG, gB, iLevels[lv], bnd);

        for (uint i = 0; i < NBP; i++)
        {
            uint j = (i + 1) % NBP;
            float2 ab = bnd[j] - bnd[i];
            float t = saturate(dot(ctcp - bnd[i], ab) / max(dot(ab, ab), 1e-10));
            float d = length(ctcp - bnd[i] - ab * t);
            if (d < lineW)
                color = max(color, lColors[lv] * (1.0 - d / lineW));

            if (length(ctcp - bnd[i]) < dotR)
                color = max(color, lColors[lv] * 1.5);
        }
    }

    // Axes
    if (abs(ctcp.x) < 0.4 / size || abs(ctcp.y) < 0.4 / size)
        color = max(color, float3(0.08, 0.08, 0.08));

    // Center dot (neutral)
    if (length(ctcp) < dotR * 1.5)
        color = float3(0.3, 0.3, 0.3);

    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Boundary";
            desc.effectId = L"ICtCp Boundary"; desc.effectVersion = 4;
            desc.category = L"Analysis";
            desc.subcategory = L"Gamut Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ictcpBoundaryHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"DiagramSize", L"float", 512.0f, 128.0f, 2048.0f, 64.0f },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" } },
                { L"Intensity",   L"float", 0.5f, 0.05f, 0.95f, 0.05f },
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Round-Trip Validator ----
        // Diagnostic effect: passes input through scRGB -> ICtCp -> scRGB
        // and outputs |out - in| * Gain. A correct implementation renders
        // black (modulo FP16 precision); any non-trivial color means the
        // ICtCp matrices or PQ helpers are wrong, and downstream tone-
        // mapping effects are untrustworthy.
        {
            static const std::string ictcpRoundTripHLSL = R"HLSL(
// ICtCp Round-Trip Validator
Texture2D Source : register(t0);
SamplerState Sampler : register(s0);

cbuffer constants : register(b0) {
    float Gain;       // default 1000
};

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = Source.Load(int3(uv, 0));
    float3 ictcp = ScRGBToICtCp(color.rgb);
    float3 back  = ICtCpToScRGB(ictcp);
    float3 diff  = abs(back - color.rgb) * Gain;
    return float4(diff, 1.0);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Round-Trip Validator";
            desc.effectId = L"ICtCp Round-Trip Validator"; desc.effectVersion = 3;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ictcpRoundTripHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Gain", L"float", 1000.0f, 1.0f, 100000.0f, 10.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Tone Map (HDR -> SDR) ----
        // Compresses I-channel only via Reinhard, leaving Ct/Cp untouched
        // so HUE is preserved by construction. Saturation may shift slightly
        // when ToneLift > 0 because lifting I without rescaling chroma
        // reduces apparent colorfulness. Both peaks are in nits and converted
        // to I-coordinates (PQ values) before the curve is applied.
        //
        // ToneLift adds a configurable mid-bump on top of the compression so
        // dark/mid scenes can be brightened toward "D2D HDR Tone Map"-style
        // output. ToneLift = 0 is pure peak-only compression (preserves HDR
        // creative intent). ToneLift ~= 0.6 approximates D2D HDR Tone Map's
        // dark-end lift on typical HDR content.
        {
            static const std::string ictcpToneMapHLSL = R"HLSL(
// ICtCp Tone Map (HDR -> SDR), I-channel Reinhard with optional mid-bump.
// Migrated to D3D11 compute (Phase 8) so it can consume upstream
// IEngineComputeOutput SRVs directly via the SHADERLAB_GPU_BUFFER macros
// when SourcePeakNits / TargetPeakNits are bound from a Luminance
// Statistics or similar producer.
#include "shaderlab_params.hlsli"

// Inputs / outputs (bridge-provided):
//   t0 -> input image (FP32 RGBA, scRGB linear)
//   u1 -> output image (RWTexture2D<float4>, FP32 RGBA, scRGB linear)
// Plus auto-injected Width/Height in the cbuffer + GPU-bindable params
// at t1 (SourcePeakNits) and t2 (TargetPeakNits) when GPU mode is on.
Texture2D<float4>        Source : register(t0);
RWTexture2D<float4>      ImageOutput : register(u1);

SHADERLAB_GPU_BUFFER(SourcePeakNits, t1)
SHADERLAB_GPU_BUFFER(TargetPeakNits, t2)

cbuffer constants : register(b0) {
    uint   Width;
    uint   Height;
    SHADERLAB_PARAM(float, SourcePeakNits)        // typical 1000-10000
    SHADERLAB_PARAM(float, TargetPeakNits)        // SDR target peak (e.g. 80, 203)
    float  Strength;                              // 0..1 lerp from identity to compressed+lifted
    float  ToneLift;                              // 0..1 mid-tone lift
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= Width || dtid.y >= Height) return;

    SHADERLAB_LOAD_PARAM(float, SourcePeakNits)
    SHADERLAB_LOAD_PARAM(float, TargetPeakNits)

    float4 color = Source.Load(int3(dtid.xy, 0));
    float3 ictcp = ScRGBToICtCp(color.rgb);

    float peakIn  = NitsToI(SourcePeakNits);
    float peakOut = NitsToI(TargetPeakNits);
    float compressed = ReinhardCompressI(ictcp.x, peakIn, peakOut);

    // Anchored polynomial lift in I-space, applied AFTER compression.
    // Curve: f(x) = x + a*x*(1-x), evaluated in normalized [0, peakOut]
    // space then scaled back to I units. Properties:
    //   f(0)   = 0           (preserves true black)
    //   f(1)   = 1           (preserves peakOut anchor)
    //   f'(0)  = 1 + a       (controlled finite toe slope; no shadow blow-up)
    //   max bump occurs near x = 0.5 (mid-tone region), magnitude = a/4
    float a = saturate(ToneLift);
    float invPeakOut = 1.0 / max(peakOut, 1e-6);
    float xn = saturate(compressed * invPeakOut);
    float lifted = (xn + a * xn * (1.0 - xn)) * peakOut;

    ictcp.x = lerp(ictcp.x, lifted, saturate(Strength));

    float3 outRgb = ICtCpToScRGB(ictcp);
    ImageOutput[dtid.xy] = float4(outRgb, color.a);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Tone Map (HDR -> SDR)";
            desc.effectId = L"ICtCp Tone Map"; desc.effectVersion = 12;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 8;
            desc.threadGroupY = 8;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + ictcpToneMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                // SourcePeakNits + TargetPeakNits remain gpuBindable.
                // Now in D3D11 compute, the host can route the upstream
                // SRV directly into the consumer's t-slot via the
                // bridge's SetGpuBinding -- no CPU readback round-trip.
                Graph::ParameterDefinition{ L"SourcePeakNits", L"float", 1000.0f, 100.0f, 10000.0f, 50.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"TargetPeakNits", L"float",  203.0f,  80.0f,   500.0f,  1.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"Strength",       L"float",    1.0f,   0.0f,     1.0f, 0.05f },
                Graph::ParameterDefinition{ L"ToneLift",       L"float",    0.0f,   0.0f,     1.0f, 0.05f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Inverse Tone Map (SDR -> HDR) ----
        // Inverse Reinhard on I; expands SDR-anchored content into the
        // HDR peak. Mirror of ICtCp Tone Map; Ct/Cp unchanged.
        {
            static const std::string ictcpInverseToneMapHLSL = R"HLSL(
// ICtCp Inverse Tone Map (SDR -> HDR), I-channel inverse Reinhard
Texture2D Source : register(t0);
SamplerState Sampler : register(s0);

cbuffer constants : register(b0) {
    float SourcePeakNits;        // SDR source peak (e.g. 80, 203)
    float TargetPeakNits;        // typical 1000-10000
    float Strength;              // 0..1 lerp from identity to expanded
};

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = Source.Load(int3(uv, 0));
    float3 ictcp = ScRGBToICtCp(color.rgb);

    // Inverse of ReinhardCompressI(x, peakIn=HDR, peakOut=SDR):
    // for SDR -> HDR expansion we feed the *compressed* (SDR) range as
    // peakOut and the *expanded* (HDR) range as peakIn. The input I lives
    // in [0, peakOut_I] (SDR range) and the helper returns an I in
    // [0, peakIn_I] (HDR range).
    float sdrI = NitsToI(SourcePeakNits);
    float hdrI = NitsToI(TargetPeakNits);
    float expanded = ReinhardExpandI(ictcp.x, hdrI, sdrI);
    ictcp.x = lerp(ictcp.x, expanded, saturate(Strength));

    float3 outRgb = ICtCpToScRGB(ictcp);
    return float4(outRgb, color.a);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Inverse Tone Map (SDR -> HDR)";
            desc.effectId = L"ICtCp Inverse Tone Map"; desc.effectVersion = 10;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ictcpInverseToneMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"SourcePeakNits", L"float", 203.0f, 80.0f, 500.0f, 1.0f },
                { L"TargetPeakNits", L"float", 1000.0f, 100.0f, 10000.0f, 50.0f },
                { L"Strength",       L"float", 1.0f, 0.0f, 1.0f, 0.05f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Saturation ----
        // Scale Ct/Cp uniformly. Hue and luminance are unchanged because
        // the (Ct, Cp) chromaticity vector is just multiplied; this is
        // the reason ICtCp exists. Saturation = 1 is identity, > 1
        // boosts, < 1 desaturates, 0 -> grayscale.
        {
            static const std::string ictcpSaturationHLSL = R"HLSL(
// ICtCp Saturation — uniform Ct/Cp scale (hue + luminance preserved)
Texture2D Source : register(t0);
SamplerState Sampler : register(s0);

cbuffer constants : register(b0) {
    float Saturation;   // 1.0 = identity, 0.0 = grayscale, >1 = boosted
};

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = Source.Load(int3(uv, 0));
    float3 ictcp = ScRGBToICtCp(color.rgb);
    ictcp.y *= Saturation;
    ictcp.z *= Saturation;
    float3 outRgb = ICtCpToScRGB(ictcp);
    return float4(outRgb, color.a);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Saturation";
            desc.effectId = L"ICtCp Saturation"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ictcpSaturationHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Saturation", L"float", 1.0f, 0.0f, 4.0f, 0.05f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Highlight Desaturation ----
        // Smooth desaturation as I rises — the standard "filmic
        // highlight rolloff" trick. Below KneeNits the image is
        // unchanged; between KneeNits and PeakNits the saturation
        // multiplier ramps from 1 toward (1 - Amount); above PeakNits
        // the saturation is fully attenuated. Pairs naturally with
        // ICtCp Tone Map: tone-map first, then desat the highlights.
        {
            static const std::string ictcpHighlightDesatHLSL = R"HLSL(
// ICtCp Highlight Desaturation — smooth Ct/Cp rolloff vs. I
Texture2D Source : register(t0);
SamplerState Sampler : register(s0);

cbuffer constants : register(b0) {
    float KneeNits;     // start of the rolloff (e.g. 200)
    float PeakNits;     // end of the rolloff       (e.g. 1000)
    float Amount;       // [0..1], how far to desaturate at peak (1 = grayscale at peak)
};

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = Source.Load(int3(uv, 0));
    float3 ictcp = ScRGBToICtCp(color.rgb);

    // Map I (PQ) back to nits so the user's parameters mean what they say
    // even though the I axis is non-linear.
    float nits = IToNits(ictcp.x);
    float t = saturate((nits - KneeNits) / max(PeakNits - KneeNits, 1e-3));
    float scale = 1.0 - saturate(Amount) * smoothstep(0.0, 1.0, t);

    ictcp.y *= scale;
    ictcp.z *= scale;
    float3 outRgb = ICtCpToScRGB(ictcp);
    return float4(outRgb, color.a);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Highlight Desaturation";
            desc.effectId = L"ICtCp Highlight Desaturation"; desc.effectVersion = 3;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ictcpHighlightDesatHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"KneeNits",       L"float", 200.0f,  10.0f, 5000.0f, 10.0f },
                { L"PeakNits",       L"float", 1000.0f, 50.0f, 10000.0f, 50.0f },
                { L"Amount",         L"float", 1.0f,    0.0f,  1.0f,    0.05f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Split Comparison ----
        {
            static const std::string splitCompareHLSL = R"HLSL(
// Split Comparison - oriented wipe between two inputs.
// Works with arbitrary `Angle` (degrees) and `SplitPosition` (0..1).
//
// The wipe boundary is a line through the image perpendicular to the
// direction (cos(Angle), sin(Angle)). SplitPosition sweeps the line
// across the image: 0 = entire image is B, 1 = entire image is A.
// At 0.5 the line passes through the image center.
//
// Coordinates are normalized so that "1.0" of travel corresponds to
// the full extent of the image *along the wipe direction* -- i.e.
// |cos(Angle)| * W + |sin(Angle)| * H. This makes the wipe sweep the
// entire image regardless of angle: at SplitPosition=0 nothing of A
// shows; at 1 nothing of B shows; in between you get a clean
// diagonal line.
//
// LineWidth controls the dividing line thickness in pixels.

Texture2D ImageA : register(t0);
Texture2D ImageB : register(t1);

cbuffer Constants : register(b0)
{
    float SplitPosition;   // 0..1 sweep along wipe direction
    float LineWidth;       // dividing line thickness in pixels
    float Angle;           // degrees; 0 = horizontal wipe (vertical line),
                           // 90 = vertical wipe (horizontal line),
                           // 45 = top-left-to-bottom-right diagonal
    float OutputW;         // host-injected: actual output rect width in
                           // pixels. Cant use Texture2D::GetDimensions()
                           // because D2D pads intermediates to atlas
                           // allocation sizes (typically 4096x4096).
    float OutputH;         // host-injected: actual output rect height.
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 a = ImageA.Load(int3(uv0.xy, 0));
    float4 b = ImageB.Load(int3(uv0.xy, 0));

    // Use the host-supplied output dimensions, not GetDimensions(), since
    // D2D pads input textures to atlas allocation sizes (e.g. 4096x4096
    // when the actual output rect is 3840x2160). uv0 is in true pixel
    // coords thanks to D2D1_PIXEL_OPTIONS_TRIVIAL_SAMPLING; we just need
    // the matching output-rect dimensions to recenter on.
    float W = max(OutputW, 1.0);
    float H = max(OutputH, 1.0);

    // Direction vector along which we project pixel positions.
    float radians = Angle * 3.14159265 / 180.0;
    float2 dir = float2(cos(radians), sin(radians));

    // Project the pixel coord (relative to image center) onto dir.
    // SplitPosition = 0..1 sweeps the wipe across the full extent of
    // the image *along* the dir vector, with 0.5 always pivoting on
    // the geometric image center regardless of angle.
    float2 p = uv0.xy - float2(W * 0.5, H * 0.5);
    float projPx  = dot(p, dir);
    float halfMax = 0.5 * (abs(dir.x) * W + abs(dir.y) * H);
    float projNorm = saturate(projPx / max(halfMax * 2.0, 1.0) + 0.5);

    float threshold = SplitPosition;
    float dist = abs(projNorm - threshold) * (halfMax * 2.0);

    // Dividing line.
    float halfLine = max(LineWidth * 0.5, 0.5);
    if (dist < halfLine)
        return float4(1, 1, 1, 1);

    if (projNorm < threshold)
        return a;
    return b;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Split Comparison";
            desc.effectId = L"Split Comparison"; desc.effectVersion = 5;
            desc.category = L"Analysis";
            desc.subcategory = L"Comparison";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + splitCompareHLSL;
            desc.inputNames = { L"ImageA", L"ImageB" };
            desc.parameters = {
                { L"SplitPosition", L"float",   0.5f,    0.0f,   1.0f,  0.01f },
                { L"LineWidth",     L"float",   2.0f,    0.0f,  10.0f,  0.5f },
                { L"Angle",         L"float",   0.0f, -360.0f, 360.0f,  1.0f },
                // Hidden: host writes actual output-rect dimensions
                // each frame (see GraphEvaluator's pixel-shader eval).
                Graph::ParameterDefinition{ L"OutputW", L"float", 1.0f, 1.0f, 16384.0f, 1.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"OutputH", L"float", 1.0f, 1.0f, 16384.0f, 1.0f, {}, L"", true },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Image Statistics ----
        // D3D11 compute shader with stride-based reduction + 256-bin histogram.
        // Built-in path uses GpuReduction (RWBuffer<uint>); user-modified path
        // uses D3D11ComputeRunner (RWStructuredBuffer<float4>) via Effect Designer.
        {
            static const std::string channelStatsHLSL = R"HLSL(
// Channel Statistics - D3D11 Compute Shader
// Population stats for a single colour channel (R/G/B/A). For
// per-luminance stats use Luminance Statistics; for chromaticity stats
// use Chromaticity Statistics.
//
// 32x32 = 1024 threads, dispatched as (1,1,1).
// Each thread strides across the full image, reduces in groupshared,
// then thread 0 computes median/P95 from a 256-bin histogram CDF.
//
// Result[0..6]: Min, Max, Mean, Median, P95, Samples, Nonzero%

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);

cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    uint Channel;      // 0=R, 1=G, 2=B, 3=A
    uint NonzeroOnly;  // 0=all, 1=nonzero only
};

#define GROUP_SIZE 32
#define THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
#define HIST_BINS 256
#define HIST_MAX 100.0

groupshared float gs_min[THREAD_COUNT];
groupshared float gs_max[THREAD_COUNT];
groupshared float gs_sum[THREAD_COUNT];
groupshared uint  gs_count[THREAD_COUNT];
groupshared uint  gs_total[THREAD_COUNT];
groupshared uint  gs_nonzero[THREAD_COUNT];
groupshared uint  gs_hist[HIST_BINS];

float GetValue(float4 pix, uint ch)
{
    if (ch == 0) return pix.r;
    if (ch == 1) return pix.g;
    if (ch == 2) return pix.b;
    return pix.a;
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID)
{
    uint tid = GTid.x + GTid.y * GROUP_SIZE;

    for (uint bi = tid; bi < HIST_BINS; bi += THREAD_COUNT)
        gs_hist[bi] = 0;
    GroupMemoryBarrierWithGroupSync();

    float tMin = 1e30;
    float tMax = -1e30;
    float tSum = 0;
    uint  tCount = 0;
    uint  tTotal = 0;
    uint  tNonzero = 0;

    for (uint y = GTid.y; y < Height; y += GROUP_SIZE)
    {
        for (uint x = GTid.x; x < Width; x += GROUP_SIZE)
        {
            float4 pix = Source.Load(int3(x, y, 0));
            float v = GetValue(pix, Channel);

            tTotal++;
            bool nz = abs(v) > 0.0001;
            if (nz) tNonzero++;
            if (NonzeroOnly == 1 && !nz) continue;

            tMin = min(tMin, v);
            tMax = max(tMax, v);
            tSum += v;
            tCount++;

            float normalized = saturate(v / HIST_MAX);
            uint bin = min((uint)(normalized * 255.0), 255u);
            InterlockedAdd(gs_hist[bin], 1u);
        }
    }

    gs_min[tid] = tMin;
    gs_max[tid] = tMax;
    gs_sum[tid] = tSum;
    gs_count[tid] = tCount;
    gs_total[tid] = tTotal;
    gs_nonzero[tid] = tNonzero;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = THREAD_COUNT / 2; stride > 0; stride >>= 1)
    {
        if (tid < stride)
        {
            gs_min[tid] = min(gs_min[tid], gs_min[tid + stride]);
            gs_max[tid] = max(gs_max[tid], gs_max[tid + stride]);
            gs_sum[tid] += gs_sum[tid + stride];
            gs_count[tid] += gs_count[tid + stride];
            gs_total[tid] += gs_total[tid + stride];
            gs_nonzero[tid] += gs_nonzero[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid == 0)
    {
        float fMin = gs_min[0];
        float fMax = gs_max[0];
        uint  totalSamples = gs_count[0];
        uint  totalPixels = gs_total[0];
        uint  nonzeroPixels = gs_nonzero[0];
        float fMean = (totalSamples > 0) ? (gs_sum[0] / float(totalSamples)) : 0.0;

        float fMedian = 0.0;
        float fP95 = 0.0;
        uint medianTarget = totalSamples / 2;
        uint p95Target = (uint)(totalSamples * 0.95);
        uint cumulative = 0;
        bool foundMedian = false;
        bool foundP95 = false;

        for (uint b = 0; b < HIST_BINS; b++)
        {
            cumulative += gs_hist[b];
            float binValue = (float(b) + 0.5) / 255.0 * HIST_MAX;
            if (!foundMedian && cumulative >= medianTarget) { fMedian = binValue; foundMedian = true; }
            if (!foundP95    && cumulative >= p95Target)    { fP95 = binValue;    foundP95 = true; }
        }

        float nonzeroPct = (totalPixels > 0)
            ? float(nonzeroPixels) / float(totalPixels) : 0.0;

        Result[0] = float4(fMin, 0, 0, 0);
        Result[1] = float4(fMax, 0, 0, 0);
        Result[2] = float4(fMean, 0, 0, 0);
        Result[3] = float4(fMedian, 0, 0, 0);
        Result[4] = float4(fP95, 0, 0, 0);
        Result[5] = float4(float(totalSamples), 0, 0, 0);
        Result[6] = float4(nonzeroPct, 0, 0, 0);
    }
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"Channel Statistics";
            desc.effectId = L"Channel Statistics"; desc.effectVersion = 1;
            desc.category = L"Analysis";
            desc.subcategory = L"Statistics";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hlslSource = channelStatsHLSL;
            desc.dataOnly = true;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Channel",     L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Red", L"Green", L"Blue", L"Alpha" } },
                { L"NonzeroOnly", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"All Pixels", L"Nonzero Only" } },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Min",      Graph::AnalysisFieldType::Float },
                { L"Max",      Graph::AnalysisFieldType::Float },
                { L"Mean",     Graph::AnalysisFieldType::Float },
                { L"Median",   Graph::AnalysisFieldType::Float },
                { L"P95",      Graph::AnalysisFieldType::Float },
                { L"Samples",  Graph::AnalysisFieldType::Float },
                { L"Nonzero%", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Luminance Statistics ----
        // Population stats on BT.709 luminance, with HDR-aware extras
        // for adaptive tone mapping. Histogram is log-spaced (8 decades
        // covering ~0.001..10000 nits) so percentiles stay accurate at
        // the bright end where adaptive tone mapping cares the most.
        {
            static const std::string luminanceStatsHLSL = R"HLSL(
// Luminance Statistics - D3D11 Compute Shader
// Outputs (in nits when Units = 1, normalized otherwise):
//   Min, Max, Mean, Median, P95, P99, AvgLog, ClippedFraction, Samples
//
// Units enum:
//   0 = Normalized (scRGB Y, where 1.0 = SDR white = 80 nits)
//   1 = Nits (Y * 80)
//
// ClippedFraction: fraction of pixels whose luminance >= ClipNits.

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);

cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    uint Units;        // 0 = Normalized, 1 = Nits
    float ClipNits;
};

#define GROUP_SIZE 32
#define THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
#define HIST_BINS 256

// Log-spaced histogram covers 8 decades: 1e-2 .. 1e6 nits.
// nits = 10^(LOG_MIN + (bin / HIST_BINS) * LOG_RANGE)
#define LOG_MIN   -2.0
#define LOG_RANGE 8.0

groupshared float gs_min[THREAD_COUNT];
groupshared float gs_max[THREAD_COUNT];
groupshared float gs_sum[THREAD_COUNT];
groupshared float gs_logsum[THREAD_COUNT];
groupshared uint  gs_count[THREAD_COUNT];
groupshared uint  gs_clipped[THREAD_COUNT];
groupshared uint  gs_hist[HIST_BINS];

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID)
{
    uint tid = GTid.x + GTid.y * GROUP_SIZE;

    for (uint bi = tid; bi < HIST_BINS; bi += THREAD_COUNT)
        gs_hist[bi] = 0;
    GroupMemoryBarrierWithGroupSync();

    // Resolve clip threshold (always in nits internally).
    float clipNitsResolved = ClipNits;

    float tMin = 1e30;
    float tMax = -1e30;
    float tSum = 0;       // sum of nit values
    float tLogSum = 0;    // sum of log10(nits) for AvgLog
    uint  tCount = 0;
    uint  tClipped = 0;

    for (uint y = GTid.y; y < Height; y += GROUP_SIZE)
    {
        for (uint x = GTid.x; x < Width; x += GROUP_SIZE)
        {
            float4 pix = Source.Load(int3(x, y, 0));
            float Y = max(0.0, dot(pix.rgb, float3(0.2126, 0.7152, 0.0722)));
            float nits = Y * 80.0;

            tCount++;
            tMin = min(tMin, nits);
            tMax = max(tMax, nits);
            tSum += nits;
            tLogSum += log10(max(nits, 1e-4));
            if (nits >= clipNitsResolved) tClipped++;

            float lv = log10(max(nits, 1e-4));
            float t = (lv - LOG_MIN) / LOG_RANGE;
            uint bin = clamp((uint)(saturate(t) * (HIST_BINS - 1)), 0u, (uint)(HIST_BINS - 1));
            InterlockedAdd(gs_hist[bin], 1u);
        }
    }

    gs_min[tid] = tMin;
    gs_max[tid] = tMax;
    gs_sum[tid] = tSum;
    gs_logsum[tid] = tLogSum;
    gs_count[tid] = tCount;
    gs_clipped[tid] = tClipped;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = THREAD_COUNT / 2; stride > 0; stride >>= 1)
    {
        if (tid < stride)
        {
            gs_min[tid] = min(gs_min[tid], gs_min[tid + stride]);
            gs_max[tid] = max(gs_max[tid], gs_max[tid + stride]);
            gs_sum[tid] += gs_sum[tid + stride];
            gs_logsum[tid] += gs_logsum[tid + stride];
            gs_count[tid] += gs_count[tid + stride];
            gs_clipped[tid] += gs_clipped[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid == 0)
    {
        uint  totalSamples = gs_count[0];
        float fMin = gs_min[0];
        float fMax = gs_max[0];
        float fMean   = (totalSamples > 0) ? (gs_sum[0]    / float(totalSamples)) : 0.0;
        float fAvgLogLog = (totalSamples > 0) ? (gs_logsum[0] / float(totalSamples)) : 0.0;
        float fAvgLog = pow(10.0, fAvgLogLog);  // geometric mean in nits

        // Histogram CDF -> percentiles (in log-domain bin centres).
        float fMedian = 0.0;
        float fP95 = 0.0;
        float fP99 = 0.0;
        uint medianTarget = totalSamples / 2;
        uint p95Target = (uint)(totalSamples * 0.95);
        uint p99Target = (uint)(totalSamples * 0.99);
        uint cumulative = 0;
        bool foundMedian = false, foundP95 = false, foundP99 = false;

        for (uint b = 0; b < HIST_BINS; b++)
        {
            cumulative += gs_hist[b];
            float binLog = LOG_MIN + (float(b) + 0.5) / float(HIST_BINS) * LOG_RANGE;
            float binNits = pow(10.0, binLog);
            if (!foundMedian && cumulative >= medianTarget) { fMedian = binNits; foundMedian = true; }
            if (!foundP95    && cumulative >= p95Target)    { fP95 = binNits;    foundP95 = true; }
            if (!foundP99    && cumulative >= p99Target)    { fP99 = binNits;    foundP99 = true; }
        }

        float clippedFrac = (totalSamples > 0)
            ? float(gs_clipped[0]) / float(totalSamples) : 0.0;

        // Convert to normalized (scRGB Y) if requested.
        float scale = (Units == 1) ? 1.0 : (1.0 / 80.0);

        Result[0] = float4(fMin    * scale, 0, 0, 0);
        Result[1] = float4(fMax    * scale, 0, 0, 0);
        Result[2] = float4(fMean   * scale, 0, 0, 0);
        Result[3] = float4(fMedian * scale, 0, 0, 0);
        Result[4] = float4(fP95    * scale, 0, 0, 0);
        Result[5] = float4(fP99    * scale, 0, 0, 0);
        Result[6] = float4(fAvgLog * scale, 0, 0, 0);
        Result[7] = float4(clippedFrac, 0, 0, 0);
        Result[8] = float4(float(totalSamples), 0, 0, 0);
    }
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"Luminance Statistics";
            desc.effectId = L"Luminance Statistics"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.subcategory = L"Statistics";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hlslSource = luminanceStatsHLSL;
            desc.dataOnly = true;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Units",      L"float", 1.0f,    0.0f, 1.0f,    1.0f, { L"Normalized", L"Nits" } },
                { L"ClipNits",   L"float", 203.0f, 10.0f, 10000.0f, 1.0f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Min",             Graph::AnalysisFieldType::Float },
                { L"Max",             Graph::AnalysisFieldType::Float },
                { L"Mean",            Graph::AnalysisFieldType::Float },
                { L"Median",          Graph::AnalysisFieldType::Float },
                { L"P95",             Graph::AnalysisFieldType::Float },
                { L"P99",             Graph::AnalysisFieldType::Float },
                { L"AvgLog",          Graph::AnalysisFieldType::Float },
                { L"ClippedFraction", Graph::AnalysisFieldType::Float },
                { L"Samples",         Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Chromaticity Statistics ----
        // ICtCp-domain stats: average and peak chroma, mean hue. Free in
        // ICtCp and meaningful precisely because Ct/Cp distance is
        // perceptually uniform — so MeanChroma corresponds to "how
        // saturated the image looks" in a way no RGB-domain stat does.
        {
            static const std::string chromaStatsHLSL = R"HLSL(
// Chromaticity Statistics - D3D11 Compute Shader
// All stats computed in BT.2100 ICtCp.
// Result fields:
//   [0] MeanCt
//   [1] MeanCp
//   [2] MeanChroma   = mean of length(Ct, Cp)
//   [3] MaxChroma
//   [4] MeanHueDeg   = degrees from atan2(Cp, Ct), wrapped 0..360
//   [5] Samples

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);

cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
};

#define GROUP_SIZE 32
#define THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)

groupshared float gs_ct[THREAD_COUNT];
groupshared float gs_cp[THREAD_COUNT];
groupshared float gs_chroma[THREAD_COUNT];
groupshared float gs_chromaMax[THREAD_COUNT];
groupshared float gs_huex[THREAD_COUNT];   // accumulator for mean-of-angles via unit vectors
groupshared float gs_huey[THREAD_COUNT];
groupshared uint  gs_count[THREAD_COUNT];

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID)
{
    uint tid = GTid.x + GTid.y * GROUP_SIZE;

    float tCt = 0, tCp = 0;
    float tChroma = 0;
    float tChromaMax = 0;
    float tHx = 0, tHy = 0;
    uint  tCount = 0;

    for (uint y = GTid.y; y < Height; y += GROUP_SIZE)
    {
        for (uint x = GTid.x; x < Width; x += GROUP_SIZE)
        {
            float4 pix = Source.Load(int3(x, y, 0));
            float3 ictcp = ScRGBToICtCp(pix.rgb);
            float ct = ictcp.y;
            float cp = ictcp.z;
            float chroma = sqrt(ct * ct + cp * cp);

            tCt += ct;
            tCp += cp;
            tChroma += chroma;
            tChromaMax = max(tChromaMax, chroma);
            // Mean of angles via unit-vector sum (the only correct way).
            if (chroma > 1e-6) {
                tHx += ct / chroma;
                tHy += cp / chroma;
            }
            tCount++;
        }
    }

    gs_ct[tid] = tCt;
    gs_cp[tid] = tCp;
    gs_chroma[tid] = tChroma;
    gs_chromaMax[tid] = tChromaMax;
    gs_huex[tid] = tHx;
    gs_huey[tid] = tHy;
    gs_count[tid] = tCount;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = THREAD_COUNT / 2; stride > 0; stride >>= 1)
    {
        if (tid < stride)
        {
            gs_ct[tid] += gs_ct[tid + stride];
            gs_cp[tid] += gs_cp[tid + stride];
            gs_chroma[tid] += gs_chroma[tid + stride];
            gs_chromaMax[tid] = max(gs_chromaMax[tid], gs_chromaMax[tid + stride]);
            gs_huex[tid] += gs_huex[tid + stride];
            gs_huey[tid] += gs_huey[tid + stride];
            gs_count[tid] += gs_count[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid == 0)
    {
        uint  N = gs_count[0];
        float invN = (N > 0) ? (1.0 / float(N)) : 0.0;
        float meanCt = gs_ct[0] * invN;
        float meanCp = gs_cp[0] * invN;
        float meanChroma = gs_chroma[0] * invN;
        float maxChroma = gs_chromaMax[0];

        float angle = atan2(gs_huey[0], gs_huex[0]);  // radians, [-pi, pi]
        float deg = degrees(angle);
        if (deg < 0.0) deg += 360.0;

        Result[0] = float4(meanCt, 0, 0, 0);
        Result[1] = float4(meanCp, 0, 0, 0);
        Result[2] = float4(meanChroma, 0, 0, 0);
        Result[3] = float4(maxChroma, 0, 0, 0);
        Result[4] = float4(deg, 0, 0, 0);
        Result[5] = float4(float(N), 0, 0, 0);
    }
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"Chromaticity Statistics";
            desc.effectId = L"Chromaticity Statistics"; desc.effectVersion = 1;
            desc.category = L"Analysis";
            desc.subcategory = L"Statistics";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hlslSource = colorMath + chromaStatsHLSL;
            desc.dataOnly = true;
            desc.inputNames = { L"Source" };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"MeanCt",     Graph::AnalysisFieldType::Float },
                { L"MeanCp",     Graph::AnalysisFieldType::Float },
                { L"MeanChroma", Graph::AnalysisFieldType::Float },
                { L"MaxChroma",  Graph::AnalysisFieldType::Float },
                { L"MeanHueDeg", Graph::AnalysisFieldType::Float },
                { L"Samples",    Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Float Slider ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Float Parameter";
            desc.effectId = L"Float Parameter"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            // No HLSL — evaluator handles parameter nodes directly.
            desc.parameters = {
                { L"Value", L"float", 0.5f, 0.0f, 1.0f, 0.01f },
                { L"Min",   L"float", 0.0f, -10000.0f, 10000.0f, 0.1f },
                { L"Max",   L"float", 1.0f, -10000.0f, 10000.0f, 0.1f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Integer Slider ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Integer Parameter";
            desc.effectId = L"Integer Parameter"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Value", L"float", 0.0f, 0.0f, 10.0f, 1.0f },
                { L"Min",   L"float", 0.0f, -10000.0f, 10000.0f, 1.0f },
                { L"Max",   L"float", 10.0f, -10000.0f, 10000.0f, 1.0f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Toggle ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Toggle Parameter";
            desc.effectId = L"Toggle Parameter"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Value", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Gamut Selector ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Parameter";
            desc.effectId = L"Gamut Parameter"; desc.effectVersion = 2;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Value", L"float", 0.0f, 0.0f, 2.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020" } },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Clock ----
        // Time-based animation source. Outputs elapsed Time (seconds) and
        // Progress (0→1 normalized). Has on-node Play/Pause and seek slider.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Clock";
            desc.effectId = L"Clock"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"AutoDuration", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
                { L"StartTime", L"float", 0.0f, 0.0f, 3600.0f, 0.1f },
                { L"StopTime",  L"float", 10.0f, 0.0f, 3600.0f, 0.1f },
                { L"Speed",     L"float", 1.0f, -10.0f, 10.0f, 0.1f },
                { L"Loop",      L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Time",     Graph::AnalysisFieldType::Float },
                { L"Progress", Graph::AnalysisFieldType::Float },
            };
            // Mark as clock node for special render loop handling.
            desc.isClock = true;
            m_effects.push_back(std::move(desc));
        }

        // ---- Numeric Expression Parameter Node ----
        // A single configurable math node that evaluates a user-provided
        // expression with up to 5 named scalar inputs. Replaces the older
        // Add/Subtract/Multiply/Divide/Min/Max nodes — all of those are
        // expressible as a one-line formula here (e.g. "max(A,B)", "A*B+C").
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Numeric Expression";
            desc.effectId = L"Math Expression"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Expression", L"string", std::wstring(L"A"), 0.0f, 0.0f, 0.0f },
                { L"A", L"float", 0.0f, -100000.0f, 100000.0f, 0.1f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Result", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Random Parameter Node ----
        // Takes a single float Seed and produces a deterministic, well-mixed
        // float in [0, 1]. The output is a pure function of the seed value:
        // whenever the upstream value changes (e.g. driven by a Clock or
        // Numeric Expression), a fresh random number is produced; identical
        // seeds always reproduce the same output, which keeps graphs and
        // pixel-trace results deterministic.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Random";
            desc.effectId = L"Random"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Seed", L"float", 0.0f, -1000000.0f, 1000000.0f, 0.01f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Result", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Working Space Parameter Node ----
        // A first-class, host-driven parameter node that mirrors the active
        // display profile (live or simulated) as 14 typed analysis output
        // fields. Lets graphs wire other nodes (tone mapper, OOG, etc.) to
        // the live working space via the property-binding system instead of
        // duplicating per-effect "follow the display" toggles. Updated by
        // MainWindow::UpdateWorkingSpaceNodes() whenever the display profile
        // changes; values are written into node->properties keyed by field
        // name and unpacked into the analysis output by the regular
        // parameter-node branch in GraphEvaluator.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Working Space";
            desc.effectId = L"Working Space"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            // No parameters — values are entirely host-driven. Properties
            // panel will be empty for this node, exactly the desired UX.
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                // Display mode (Float, encoded as 0=SDR, 1=WCG/ACM, 2=HDR).
                { L"ActiveColorMode",   Graph::AnalysisFieldType::Float },
                // Capability + user-toggle flags (Float-as-bool, 0/1).
                { L"HdrSupported",      Graph::AnalysisFieldType::Float },
                { L"HdrUserEnabled",    Graph::AnalysisFieldType::Float },
                { L"WcgSupported",      Graph::AnalysisFieldType::Float },
                { L"WcgUserEnabled",    Graph::AnalysisFieldType::Float },
                { L"IsSimulated",       Graph::AnalysisFieldType::Float },
                // Luminance levels in nits.
                { L"SdrWhiteNits",      Graph::AnalysisFieldType::Float },
                { L"PeakNits",          Graph::AnalysisFieldType::Float },
                { L"MinNits",           Graph::AnalysisFieldType::Float },
                { L"MaxFullFrameNits",  Graph::AnalysisFieldType::Float },
                // Color primaries as CIE xy (Float2).
                { L"RedPrimary",        Graph::AnalysisFieldType::Float2 },
                { L"GreenPrimary",      Graph::AnalysisFieldType::Float2 },
                { L"BluePrimary",       Graph::AnalysisFieldType::Float2 },
                { L"WhitePoint",        Graph::AnalysisFieldType::Float2 },
            };
            // Bootstrap defaults — sRGB SDR. Lets the node show meaningful
            // values immediately on creation, even before MainWindow's
            // UpdateWorkingSpaceNodes() has had a chance to run.
            using winrt::Windows::Foundation::Numerics::float2;
            desc.hiddenDefaults = {
                { L"ActiveColorMode",   Graph::PropertyValue{ 0.0f } },
                { L"HdrSupported",      Graph::PropertyValue{ 0.0f } },
                { L"HdrUserEnabled",    Graph::PropertyValue{ 0.0f } },
                { L"WcgSupported",      Graph::PropertyValue{ 0.0f } },
                { L"WcgUserEnabled",    Graph::PropertyValue{ 0.0f } },
                { L"IsSimulated",       Graph::PropertyValue{ 0.0f } },
                { L"SdrWhiteNits",      Graph::PropertyValue{ 80.0f } },
                { L"PeakNits",          Graph::PropertyValue{ 80.0f } },
                { L"MinNits",           Graph::PropertyValue{ 0.5f } },
                { L"MaxFullFrameNits",  Graph::PropertyValue{ 80.0f } },
                { L"RedPrimary",        Graph::PropertyValue{ float2{ 0.640f, 0.330f } } },
                { L"GreenPrimary",      Graph::PropertyValue{ float2{ 0.300f, 0.600f } } },
                { L"BluePrimary",       Graph::PropertyValue{ float2{ 0.150f, 0.060f } } },
                { L"WhitePoint",        Graph::PropertyValue{ float2{ 0.3127f, 0.3290f } } },
            };
            m_effects.push_back(std::move(desc));
        }
    }
}
