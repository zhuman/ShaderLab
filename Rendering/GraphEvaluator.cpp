#include "pch_engine.h"
#include "GraphEvaluator.h"
#include "../Effects/ShaderCompiler.h"
#include "../Effects/BytecodeCache.h"
#include "../Effects/Performance.h"
#include "../Effects/IEngineComputeOutput.h"
#include "MathExpression.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering
{
    // ---- Phase 8 cache routing helpers (file-scope) -----------------------
    //
    // Build a BytecodeCompileRequest from a node's CustomEffectDefinition,
    // then synchronously fetch-or-compile via BytecodeCache. Centralizes
    // the canonicalize -> hash -> request pattern shared by:
    //   * ShaderLab built-in effects (PixelShader / D2D-tiled compute), and
    //   * D3D11 compute custom effects (the bridge's lazy compile).
    //
    // gpuBindableNames is filtered from def.parameters in stable index
    // order; today no built-in effect sets gpuBindable=true so the list is
    // empty and the bitset is 0 (= the baseline variant).
    namespace {
        Effects::BytecodeCacheResult CompileViaCache(
            const std::wstring& effectId,
            uint32_t            effectVersion,
            const std::wstring& hlslSource,
            const std::string&  target,
            uint32_t            macroBitset,
            const std::vector<std::string>& gpuBindableNames)
        {
            std::string canonical = Effects::CanonicalizeHlslSource(hlslSource);

            Effects::BytecodeCompileRequest req;
            req.key.sourceHash         = Effects::HashCanonicalSource(canonical);
            req.key.paramSignatureHash = Effects::HashParamSignature(gpuBindableNames);
            req.key.includeLibraryHash = Effects::IncludeLibraryHash();
            req.key.macroBitset        = macroBitset;
            req.key.entryPoint         = "main";
            req.key.target             = target;
            req.metadata.effectId      = effectId;
            req.metadata.version       = effectVersion;
            req.hlslSource             = std::move(canonical);
            req.gpuBindableParamNames  = gpuBindableNames;

            return Effects::BytecodeCache::Instance().GetOrCompile(std::move(req));
        }

        // Filter a definition's parameters to those flagged gpuBindable,
        // returning their UTF-8 names in declared order. Empty today
        // (no built-in effect has gpuBindable=true) but ready for
        // p8-migrate-ictcp.
        std::vector<std::string> ExtractGpuBindableNames(
            const ShaderLab::Graph::CustomEffectDefinition& def)
        {
            std::vector<std::string> names;
            names.reserve(def.parameters.size());
            for (const auto& p : def.parameters)
            {
                if (!p.gpuBindable) continue;
                std::string utf8;
                int needed = ::WideCharToMultiByte(
                    CP_UTF8, 0, p.name.data(), static_cast<int>(p.name.size()),
                    nullptr, 0, nullptr, nullptr);
                if (needed > 0)
                {
                    utf8.resize(static_cast<size_t>(needed));
                    ::WideCharToMultiByte(
                        CP_UTF8, 0, p.name.data(), static_cast<int>(p.name.size()),
                        utf8.data(), needed, nullptr, nullptr);
                }
                names.push_back(std::move(utf8));
            }
            return names;
        }
    }
    // -----------------------------------------------------------------------
    // Main evaluation entry point
    // -----------------------------------------------------------------------

    ID2D1Image* GraphEvaluator::Evaluate(EffectGraph& graph, ID2D1DeviceContext5* dc)
    {
        if (graph.IsEmpty() || !dc)
            return nullptr;

        // Effects created on the previous frame are now fully initialized.
        m_justCreated.clear();

        // NOTE on deferred-compute ownership:
        //   `DeferredCompute::inputImages` holds owning `winrt::com_ptr<ID2D1Image>`
        //   entries (not raw pointers), so even if a subsequent Evaluate
        //   pass within the same render iteration rebuilds the producing
        //   effect, this pass's entry keeps the image chain alive. We do
        //   NOT clear m_deferredCompute here.
        //
        //   We used to clear at the top of Evaluate to prevent a UAF from
        //   stale raw pointers across a two-pass render iteration. With
        //   owning refs in place, that defence is moot -- and clearing here
        //   actually breaks the steady-state flow: pass 1 pushes a deferred
        //   entry while the node is dirty + fields are stale, pass 2 finds
        //   `dirty=false` and `analysisOutput.fields` already populated
        //   from a previous runEval, so it does NOT re-push. Clearing
        //   between the passes then leaves m_deferredCompute empty and
        //   ProcessDeferredCompute dispatches nothing -- set-property on
        //   an upstream Source has no visible effect on a downstream
        //   compute analysis node.
        //
        //   m_deferredCompute is drained + cleared at the bottom of
        //   ProcessDeferredCompute; the source-not-ready early return
        //   above (in ProcessDeferredCompute) also clears it.

        // Get topological ordering (sources first → output last).
        std::vector<uint32_t> order;
        try
        {
            order = graph.TopologicalSort();
        }
        catch (const std::logic_error&)
        {
            // Graph has a cycle -- cannot evaluate.
            return nullptr;
        }

        ID2D1Image* finalOutput = nullptr;

        // Propagate dirty downstream in topological order. If any node is
        // dirty at the start of this frame (e.g. a Source whose video frame
        // just uploaded, or a node whose property changed), every node it
        // feeds is also dirty — they need to re-evaluate against the new
        // upstream output. Without this, analysis-only compute nodes
        // (Channel/Luminance/Chromaticity Statistics) freeze their fields
        // on the first frame because their own properties never change
        // even as the input image content does.
        for (uint32_t nodeId : order)
        {
            auto* n = graph.FindNode(nodeId);
            if (!n || !n->dirty) continue;
            for (const auto* edge : graph.GetOutputEdges(nodeId))
            {
                if (auto* dn = graph.FindNode(edge->destNodeId))
                    dn->dirty = true;
            }
        }

        for (uint32_t nodeId : order)
        {
            EffectNode* node = graph.FindNode(nodeId);
            if (!node)
                continue;

            // Skip nodes not needed by any visible output.
            if (!node->needed)
            {
                node->dirty = false;
                continue;
            }

            switch (node->type)
            {
            case NodeType::Source:
            {
                // Source nodes have their cachedOutput set externally
                // (by WIC image loading, Flood effect, or video provider).
                // Resolve property bindings (e.g., Clock → Video.Time).
                if (!node->propertyBindings.empty())
                {
                    std::map<std::wstring, PropertyValue> effectiveProps;
                    bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                    if (bindingsChanged)
                    {
                        for (const auto& [propName, binding] : node->propertyBindings)
                        {
                            auto eit = effectiveProps.find(propName);
                            if (eit != effectiveProps.end())
                                node->properties[propName] = eit->second;
                        }
                        node->dirty = true;
                    }
                }
                node->dirty = false;
                break;
            }

            case NodeType::BuiltInEffect:
            {
                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                if (effect)
                {
                    WireInputs(effect, *node, graph);

                    // Resolve property bindings every frame.
                    std::map<std::wstring, PropertyValue> effectiveProps;
                    bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);

                    // Write resolved binding values back for UI display.
                    if (bindingsChanged)
                    {
                        for (const auto& [propName, binding] : node->propertyBindings)
                        {
                            auto eit = effectiveProps.find(propName);
                            if (eit != effectiveProps.end())
                                node->properties[propName] = eit->second;
                        }
                    }

                    if (node->dirty || bindingsChanged)
                    {
                        ApplyProperties(effect, *node, effectiveProps);
                        node->dirty = false;
                    }

                    // The effect's output is an ID2D1Image. Take ownership so the
                    // image survives D2D's internal pipeline churn (input toggles,
                    // property reapply) until the effect itself is invalidated.
                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    m_outputCache[nodeId] = output;
                    node->cachedOutput = output.get();

                    // For analysis effects (e.g., Histogram), force computation
                    // by drawing the output, then read back the result property.
                    if (node->effectClsid.has_value() &&
                        IsEqualGUID(node->effectClsid.value(), CLSID_D2D1Histogram))
                    {
                        ReadHistogramOutput(dc, effect, *node);
                    }
                }
                break;
            }

            case NodeType::PixelShader:
            case NodeType::ComputeShader:
            {
                // D3D11 Compute Shader: defer dispatch until after all D2D effects are initialized.
                if (node->customEffect.has_value() &&
                    node->customEffect->shaderType == CustomShaderType::D3D11ComputeShader)
                {
                    // Resolve property bindings for D3D11 compute nodes.
                    if (!node->propertyBindings.empty())
                    {
                        std::map<std::wstring, PropertyValue> effectiveProps;
                        bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                        if (bindingsChanged)
                        {
                            for (const auto& [propName, binding] : node->propertyBindings)
                            {
                                auto eit = effectiveProps.find(propName);
                                if (eit != effectiveProps.end())
                                    node->properties[propName] = eit->second;
                            }
                            node->dirty = true;
                        }
                    }

                    auto inputs = graph.GetInputEdges(nodeId);
                    // Collect all upstream input images, in pin-index order
                    // (matching customEffect.inputNames). The bridge binds
                    // them at t0..t(N-1).
                    std::vector<winrt::com_ptr<ID2D1Image>> inputImages;
                    {
                        // Find max destPin so we can size by declared input
                        // count. Slots not connected get nullptr (the bridge
                        // returns E_INVALIDARG, surfaced as a runtime error).
                        uint32_t maxPin = 0;
                        for (const auto* e : inputs)
                            if (e->destPin >= maxPin) maxPin = e->destPin + 1;
                        // Also bound by declared input count if known.
                        uint32_t declaredCount = node->customEffect.has_value()
                            ? static_cast<uint32_t>(node->customEffect->inputNames.size())
                            : 0;
                        uint32_t totalSlots = (std::max)(maxPin, declaredCount);
                        if (totalSlots == 0 && !inputs.empty()) totalSlots = 1;
                        inputImages.assign(totalSlots, {});
                        for (const auto* e : inputs)
                        {
                            auto* srcNode = graph.FindNode(e->sourceNodeId);
                            if (srcNode && srcNode->cachedOutput &&
                                e->destPin < inputImages.size())
                            {
                                inputImages[e->destPin].copy_from(srcNode->cachedOutput);
                            }
                        }
                    }
                    ID2D1Image* primaryInput = inputImages.empty() ? nullptr : inputImages[0].get();
                    bool hasImageOutput = !node->outputPins.empty();
                    // Image-producing compute: recompute when dirty or no cached output.
                    // Analysis-only compute: also recompute if no analysis fields yet.
                    //
                    // Phase 8c regression fix: when skip-readback is on, ALL
                    // compute nodes must redispatch every frame regardless
                    // of dirty state. Pre-skip-readback the per-frame
                    // upstream dirty-propagation in OnRenderTick lit up
                    // compute consumers via image edges, but inside this
                    // EvaluateNode the upstream's `dirty` has already been
                    // cleared (Source case clears it before downstream eval
                    // runs) so we can't use it here. The host's upstream
                    // propagation does set node->dirty for us when Source
                    // ticks, but ResolveBindings on the consumer of an
                    // analysis source (e.g. ICtCp <- LumStats.Mean) only
                    // dirties the consumer when CPU value changes -- which
                    // never happens when LumStats's Map() was throttled.
                    // Force redispatch for every compute node so the GPU
                    // dispatch fires and the structured-buffer / image-
                    // output texture are kept fresh.
                    const bool skipReadbackForcesRedispatch =
                        Performance::IsSkipUnneededCpuReadbackEnabled();
                    bool needsCompute = node->dirty ||
                        skipReadbackForcesRedispatch ||
                        (hasImageOutput && !node->cachedOutput) ||
                        (!hasImageOutput && node->analysisOutput.fields.empty());
                    if (primaryInput && needsCompute && !m_deferredComputeFrozen)
                    {
                        // Phase 8: ensure the bridge effect exists for this
                        // node so ProcessDeferredCompute can drive it via
                        // ICustomComputeBridge. CreateOrGetEffect captures
                        // the impl into m_bridgeImplCache.
                        GetOrCreateEffect(dc, *node);

                        // For image-producing compute, pre-render every upstream
                        // input to its own FP32 bitmap NOW while properties are
                        // fresh. This avoids D2D GPU caching returning stale data
                        // when ProcessDeferredCompute renders later inside BeginDraw.
                        std::vector<winrt::com_ptr<ID2D1Bitmap1>> preRendered(inputImages.size());
                        if (hasImageOutput)
                        {
                            for (size_t i = 0; i < inputImages.size(); ++i)
                            {
                                if (inputImages[i])
                                    preRendered[i] = PreRenderInputBitmap(dc, inputImages[i].get());
                            }
                        }
                        m_deferredCompute.push_back({ nodeId, std::move(inputImages), std::move(preRendered) });
                    }
                    node->dirty = false;
                    if (!hasImageOutput)
                        node->cachedOutput = nullptr;
                    break;
                }

                // Parameter nodes: no HLSL, just expose property values as analysis output.
                if (node->customEffect.has_value() &&
                    node->customEffect->hlslSource.empty() &&
                    node->customEffect->analysisOutputType == AnalysisOutputType::Typed &&
                    !node->customEffect->analysisFields.empty())
                {
                    // Resolve property bindings for parameter/math/clock nodes.
                    if (!node->propertyBindings.empty())
                    {
                        std::map<std::wstring, PropertyValue> effectiveProps;
                        bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                        if (bindingsChanged)
                        {
                            for (const auto& [propName, binding] : node->propertyBindings)
                            {
                                auto eit = effectiveProps.find(propName);
                                if (eit != effectiveProps.end())
                                    node->properties[propName] = eit->second;
                            }
                        }
                    }

                    node->analysisOutput.type = AnalysisOutputType::Typed;
                    node->analysisOutput.fields.clear();

                    if (node->isClock)
                    {
                        // Clock nodes compute Time/Progress from internal clock state.
                        float startTime = 0.0f, stopTime = 10.0f;
                        auto stIt = node->properties.find(L"StartTime");
                        if (stIt != node->properties.end())
                            if (auto* f = std::get_if<float>(&stIt->second)) startTime = *f;
                        auto etIt = node->properties.find(L"StopTime");
                        if (etIt != node->properties.end())
                            if (auto* f = std::get_if<float>(&etIt->second)) stopTime = *f;
                        double duration = static_cast<double>(stopTime - startTime);
                        if (duration <= 0.0) duration = 1.0;

                        float currentTime = startTime + static_cast<float>(node->clockTime);
                        float progress = static_cast<float>(node->clockTime / duration);

                        for (const auto& fd : node->customEffect->analysisFields)
                        {
                            AnalysisFieldValue fv;
                            fv.name = fd.name;
                            fv.type = fd.type;
                            if (fd.name == L"Time") fv.components[0] = currentTime;
                            else if (fd.name == L"Progress") fv.components[0] = progress;
                            node->analysisOutput.fields.push_back(std::move(fv));
                        }
                    }
                    else if (node->customEffect.has_value() &&
                            !node->customEffect->shaderLabEffectId.empty() &&
                            node->customEffect->shaderLabEffectId == L"Math Expression")
                    {
                        // Numeric Expression node: evaluate the user-supplied
                        // formula with each declared float parameter (A, B, C, ...)
                        // bound by name. Inputs are dynamic — the parameter list
                        // on the node defines which variables exist.
                        std::wstring expr;
                        auto eIt = node->properties.find(L"Expression");
                        if (eIt != node->properties.end())
                            if (auto* s = std::get_if<std::wstring>(&eIt->second)) expr = *s;

                        std::vector<std::wstring> nameStrs;
                        std::vector<const wchar_t*> namePtrs;
                        std::vector<float> vals;
                        nameStrs.reserve(node->customEffect->parameters.size());
                        namePtrs.reserve(node->customEffect->parameters.size());
                        vals.reserve(node->customEffect->parameters.size());
                        for (const auto& p : node->customEffect->parameters)
                        {
                            if (p.name == L"Expression") continue;
                            if (p.typeName != L"float") continue;
                            float v = 0.0f;
                            auto it = node->properties.find(p.name);
                            if (it != node->properties.end())
                                if (auto* f = std::get_if<float>(&it->second)) v = *f;
                            nameStrs.push_back(p.name);
                            vals.push_back(v);
                        }
                        for (auto& n : nameStrs) namePtrs.push_back(n.c_str());

                        float result = 0.0f;
                        std::wstring err;
                        bool ok = EvaluateMathExpression(
                            expr, namePtrs.data(), vals.data(), vals.size(),
                            result, &err);
                        node->runtimeError = ok ? std::wstring{} : err;

                        AnalysisFieldValue fv;
                        fv.name = L"Result";
                        fv.type = AnalysisFieldType::Float;
                        fv.components[0] = result;
                        node->analysisOutput.fields.push_back(std::move(fv));
                    }
                    else if (node->customEffect.has_value() &&
                            !node->customEffect->shaderLabEffectId.empty() &&
                            node->customEffect->shaderLabEffectId == L"Random")
                    {
                        // Random Parameter Node: hash the Seed float into a
                        // uniform float in [0, 1). Pure function of the seed,
                        // so identical seeds reproduce identical output and
                        // any change to the seed (e.g. tick of an upstream
                        // Clock) yields a new well-mixed value. Uses a
                        // SplitMix64-style integer mixer on the bit pattern
                        // of the float — much better distribution than a
                        // raw `sinf(seed)` trick and stays deterministic
                        // across builds / architectures.
                        float seed = 0.0f;
                        auto sIt = node->properties.find(L"Seed");
                        if (sIt != node->properties.end())
                            if (auto* f = std::get_if<float>(&sIt->second)) seed = *f;

                        uint32_t bits = 0;
                        std::memcpy(&bits, &seed, sizeof(bits));
                        uint64_t z = static_cast<uint64_t>(bits) * 0x9E3779B97F4A7C15ULL
                                     + 0xBF58476D1CE4E5B9ULL;
                        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                        z =  z ^ (z >> 31);
                        // Take the top 24 bits and divide into [0, 1) so the
                        // result has the full mantissa precision of a float.
                        const uint32_t mantissa = static_cast<uint32_t>(z >> 40);
                        const float result01 = static_cast<float>(mantissa) / 16777216.0f;

                        AnalysisFieldValue fv;
                        fv.name = L"Result";
                        fv.type = AnalysisFieldType::Float;
                        fv.components[0] = result01;
                        node->analysisOutput.fields.push_back(std::move(fv));
                    }
                    else
                    {
                        // Regular parameter nodes: copy property values into
                        // the analysis output fields. Supports the full
                        // PropertyValue variant — host-managed nodes (e.g.
                        // Working Space) write float2/float3/float4 plus
                        // bool / int / uint into properties keyed by field
                        // name and the evaluator unpacks them here.
                        for (const auto& fd : node->customEffect->analysisFields)
                        {
                            AnalysisFieldValue fv;
                            fv.name = fd.name;
                            fv.type = fd.type;
                            auto propIt = node->properties.find(fd.name);
                            if (propIt != node->properties.end())
                            {
                                const auto& pv = propIt->second;
                                if (auto* f = std::get_if<float>(&pv))
                                {
                                    fv.components[0] = *f;
                                }
                                else if (auto* b = std::get_if<bool>(&pv))
                                {
                                    fv.components[0] = *b ? 1.0f : 0.0f;
                                }
                                else if (auto* i = std::get_if<int32_t>(&pv))
                                {
                                    fv.components[0] = static_cast<float>(*i);
                                }
                                else if (auto* u = std::get_if<uint32_t>(&pv))
                                {
                                    fv.components[0] = static_cast<float>(*u);
                                }
                                else if (auto* v2 = std::get_if<winrt::Windows::Foundation::Numerics::float2>(&pv))
                                {
                                    fv.components[0] = v2->x;
                                    fv.components[1] = v2->y;
                                }
                                else if (auto* v3 = std::get_if<winrt::Windows::Foundation::Numerics::float3>(&pv))
                                {
                                    fv.components[0] = v3->x;
                                    fv.components[1] = v3->y;
                                    fv.components[2] = v3->z;
                                }
                                else if (auto* v4 = std::get_if<winrt::Windows::Foundation::Numerics::float4>(&pv))
                                {
                                    fv.components[0] = v4->x;
                                    fv.components[1] = v4->y;
                                    fv.components[2] = v4->z;
                                    fv.components[3] = v4->w;
                                }
                            }
                            node->analysisOutput.fields.push_back(std::move(fv));
                        }
                    }
                    node->cachedOutput = nullptr;
                    node->dirty = false;
                    break;
                }

                // Auto-compile ShaderLab effects that have HLSL but no bytecode.
                if (node->customEffect.has_value() &&
                    !node->customEffect->isCompiled() &&
                    !node->customEffect->hlslSource.empty())
                {
                    auto& def = node->customEffect.value();
                    std::string target = (def.shaderType == CustomShaderType::PixelShader)
                        ? "ps_5_0" : "cs_5_0";
                    auto gpuNames = ExtractGpuBindableNames(def);
                    auto cached = CompileViaCache(
                        node->name, /*effectVersion*/ 1u,
                        def.hlslSource, target, /*macroBitset*/ 0u, gpuNames);
                    if (cached.status == Effects::BytecodeStatus::Ready)
                    {
                        def.compiledBytecode = std::move(cached.bytecode);
                        CoCreateGuid(&def.shaderGuid);
                        node->dirty = true;
                        // Phase 8 eager precompile: kick off background
                        // compiles for the +N gpu-binding variants so
                        // they're warm if the user later wires a binding
                        // to a gpuBindable param. Idempotent (the cache
                        // dedupes by key); fires once per node per session
                        // since this branch runs only on !isCompiled().
                        if (!gpuNames.empty())
                        {
                            Effects::BytecodeCacheMetadata meta;
                            meta.effectId = node->name;
                            meta.version  = 1u;
                            Effects::BytecodeCache::Instance().PrecompileCommonShapes(
                                meta,
                                Effects::CanonicalizeHlslSource(def.hlslSource),
                                "main", target, gpuNames);
                        }
                    }
                    else
                    {
                        node->runtimeError = L"Auto-compile failed: " + cached.errorMessage;
                        node->cachedOutput = nullptr;
                        m_outputCache.erase(nodeId);
                        break;
                    }
                }

                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                if (!effect)
                {
                    node->runtimeError = L"Failed to create D2D effect. Check effect registration.";
                    node->cachedOutput = nullptr;
                    m_outputCache.erase(nodeId);
                    break;
                }
                node->runtimeError.clear();

                if (node->customEffect.has_value() && node->customEffect->isCompiled())
                {
                    // Resolve property bindings every frame.
                    std::map<std::wstring, PropertyValue> effectiveProps;
                    bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                    bool wasDirty = node->dirty || bindingsChanged;

                    // Inject host-driven output dimensions for shaders that
                    // declare OutputW / OutputH cbuffer fields. D2D pads input
                    // textures to atlas allocation sizes (typically 4096x4096),
                    // so HLSL `Texture2D::GetDimensions()` is unreliable for
                    // anything that needs the true output rect (e.g. Split
                    // Comparisons center-of-image pivot). The shader uses
                    // these values instead.
                    bool declaresOutputDims = false;
                    if (node->customEffect->shaderType == Graph::CustomShaderType::PixelShader)
                    {
                        for (const auto& p : node->customEffect->parameters)
                        {
                            if (p.name == L"OutputW" || p.name == L"OutputH")
                            { declaresOutputDims = true; break; }
                        }
                    }
                    if (declaresOutputDims)
                    {
                        auto inputs = graph.GetInputEdges(nodeId);
                        if (!inputs.empty() && dc)
                        {
                            // Compute the *union* of all input bounds, mirroring
                            // CustomPixelShaderEffect::MapInputRectsToOutputRect
                            // (which produces the actual D2D output rect this
                            // shader runs over). Using only inputs[0] would
                            // misreport OutputW/H whenever inputs have different
                            // bounds (e.g. Split Comparison fed a 4K and a 2K
                            // input), causing the shader's coordinate math --
                            // including UV normalization -- to use a smaller
                            // virtual canvas than the actual output rect.
                            //
                            // Per-input bounds (ImageAW/H, ImageBW/H ...) are
                            // also captured so the shader can compensate for
                            // D2D's atlas padding -- a D2D pixel-shader output
                            // bitmap may live inside e.g. a 4096x4096 atlas
                            // even when its content rect is only 1920x1080.
                            // Sampling [0,1] would otherwise read the padding.
                            float oldDpiX = 0, oldDpiY = 0;
                            dc->GetDpi(&oldDpiX, &oldDpiY);
                            dc->SetDpi(96.0f, 96.0f);
                            float unionLeft = (std::numeric_limits<float>::max)();
                            float unionTop = (std::numeric_limits<float>::max)();
                            float unionRight = -(std::numeric_limits<float>::max)();
                            float unionBottom = -(std::numeric_limits<float>::max)();
                            bool anyValid = false;
                            // Per-input content widths/heights, indexed by
                            // destination pin (so input #0 = ImageA, #1 = ImageB,
                            // ...). graph.GetInputEdges() returns edges in
                            // arbitrary order, so we have to scatter into a
                            // pin-indexed vector explicitly.
                            std::vector<std::pair<float, float>> perInputWH(4, { 0.0f, 0.0f });
                            for (const auto& edge : inputs)
                            {
                                auto* srcNode = graph.FindNode(edge->sourceNodeId);
                                if (!srcNode || !srcNode->cachedOutput) continue;
                                D2D1_RECT_F b{};
                                if (FAILED(dc->GetImageLocalBounds(srcNode->cachedOutput, &b)))
                                    continue;
                                float bw = b.right - b.left;
                                float bh = b.bottom - b.top;
                                if (bw <= 0 || bh <= 0) continue;
                                unionLeft   = (std::min)(unionLeft,   b.left);
                                unionTop    = (std::min)(unionTop,    b.top);
                                unionRight  = (std::max)(unionRight,  b.right);
                                unionBottom = (std::max)(unionBottom, b.bottom);
                                anyValid = true;
                                if (edge->destPin < perInputWH.size())
                                    perInputWH[edge->destPin] = { bw, bh };
                            }
                            dc->SetDpi(oldDpiX, oldDpiY);
                            if (anyValid)
                            {
                                float w = unionRight - unionLeft;
                                float h = unionBottom - unionTop;
                                if (w > 0 && h > 0)
                                {
                                    auto wIt = effectiveProps.find(L"OutputW");
                                    auto hIt = effectiveProps.find(L"OutputH");
                                    bool changed =
                                        (wIt == effectiveProps.end() ||
                                         !std::holds_alternative<float>(wIt->second) ||
                                         std::get<float>(wIt->second) != w) ||
                                        (hIt == effectiveProps.end() ||
                                         !std::holds_alternative<float>(hIt->second) ||
                                         std::get<float>(hIt->second) != h);
                                    effectiveProps[L"OutputW"] = w;
                                    effectiveProps[L"OutputH"] = h;
                                    node->properties[L"OutputW"] = w;
                                    node->properties[L"OutputH"] = h;

                                    // Write per-input ImageAW/H / ImageBW/H /
                                    // ImageCW/H / ImageDW/H -- but only if the
                                    // shader actually declares those params.
                                    static const wchar_t* kSlots[][2] = {
                                        { L"ImageAW", L"ImageAH" },
                                        { L"ImageBW", L"ImageBH" },
                                        { L"ImageCW", L"ImageCH" },
                                        { L"ImageDW", L"ImageDH" },
                                    };
                                    auto declaresParam = [&](const std::wstring& name) {
                                        for (const auto& p : node->customEffect->parameters)
                                            if (p.name == name) return true;
                                        return false;
                                    };
                                    for (size_t i = 0; i < perInputWH.size() && i < std::size(kSlots); ++i)
                                    {
                                        const wchar_t* nW = kSlots[i][0];
                                        const wchar_t* nH = kSlots[i][1];
                                        if (!declaresParam(nW) || !declaresParam(nH)) continue;
                                        // Fall back to the union dimensions when the input's
                                        // cachedOutput is briefly nullptr (e.g. first frame
                                        // after a compute branch is wired in -- deferred
                                        // compute hasn't populated the wrapper bitmap yet).
                                        // The shader's content/atlas math then degenerates
                                        // to "atlas == content" which is correct for compute
                                        // outputs (the typical case for null cachedOutput).
                                        float iw = perInputWH[i].first;
                                        float ih = perInputWH[i].second;
                                        if (iw <= 0 || ih <= 0) { iw = w; ih = h; }
                                        auto iwIt = effectiveProps.find(nW);
                                        auto ihIt = effectiveProps.find(nH);
                                        bool slotChanged =
                                            (iwIt == effectiveProps.end() ||
                                             !std::holds_alternative<float>(iwIt->second) ||
                                             std::get<float>(iwIt->second) != iw) ||
                                            (ihIt == effectiveProps.end() ||
                                             !std::holds_alternative<float>(ihIt->second) ||
                                             std::get<float>(ihIt->second) != ih);
                                        effectiveProps[nW] = iw;
                                        effectiveProps[nH] = ih;
                                        node->properties[nW] = iw;
                                        node->properties[nH] = ih;
                                        if (slotChanged) changed = true;
                                    }

                                    if (changed) wasDirty = true;
                                }
                            }
                        }
                    }

                    // Write resolved binding values back to node properties
                    // so the node graph UI shows live bound values.
                    if (bindingsChanged)
                    {
                        for (const auto& [propName, binding] : node->propertyBindings)
                        {
                            auto eit = effectiveProps.find(propName);
                            if (eit != effectiveProps.end())
                                node->properties[propName] = eit->second;
                        }
                    }

                    if (wasDirty)
                    {
                        ApplyCustomEffect(effect, *node, effectiveProps);

                        // Force-upload the cbuffer directly to the GPU.
                        auto implIt = m_customImplCache.find(node->id);
                        if (implIt != m_customImplCache.end())
                        {
                            if (node->type == NodeType::PixelShader && implIt->second.pixelImpl)
                                implIt->second.pixelImpl->ForceUploadConstantBuffer();
                            else if (node->type == NodeType::ComputeShader && implIt->second.computeImpl)
                                implIt->second.computeImpl->ForceUploadConstantBuffer();
                        }

                        node->dirty = false;
                    }

                    WireInputs(effect, *node, graph);

                    // For source effects (no declared inputs), feed a dummy bitmap
                    // so D2D has valid input for MapInputRectsToOutputRect sizing.
                    if (node->customEffect->inputNames.empty())
                    {
                        EnsureDummySourceBitmap(dc);
                        if (m_dummySourceBitmap)
                            effect->SetInput(0, m_dummySourceBitmap.get());
                    }

                    // Force D2D to re-render by toggling input 0.
                    {
                        winrt::com_ptr<ID2D1Image> savedInput;
                        effect->GetInput(0, savedInput.put());
                        if (savedInput)
                        {
                            effect->SetInput(0, nullptr);
                            effect->SetInput(0, savedInput.get());
                        }
                    }

                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    m_outputCache[nodeId] = output;
                    node->cachedOutput = output.get();

                    // Read back analysis data only when the node was dirty
                    // (avoids expensive BeginDraw/DrawImage/EndDraw + CPU readback every frame).
                    // Also defer for effects created this frame.
                    if (wasDirty &&
                        node->customEffect->analysisOutputType == AnalysisOutputType::Typed &&
                        !node->customEffect->analysisFields.empty() &&
                        node->cachedOutput &&
                        m_justCreated.find(node->id) == m_justCreated.end())
                    {
                        ReadCustomAnalysisOutput(dc, *node);
                    }
                }
                else
                {
                    if (node->customEffect.has_value() && !node->customEffect->isCompiled())
                    {
                        node->runtimeError = L"Shader not compiled. Open in Effect Designer and compile.";
                        node->cachedOutput = nullptr;
                        m_outputCache.erase(nodeId);
                    }
                    else
                    {
                        WireInputs(effect, *node, graph);
                        if (node->dirty)
                        {
                            ApplyProperties(effect, *node, node->properties);
                            node->dirty = false;
                        }
                        winrt::com_ptr<ID2D1Image> output;
                        effect->GetOutput(output.put());
                        m_outputCache[nodeId] = output;
                        node->cachedOutput = output.get();
                    }
                }
                break;
            }

            case NodeType::Output:
            {
                // The output node simply passes through the first input.
                node->cachedOutput = nullptr;
                auto inputs = graph.GetInputEdges(nodeId);
                if (!inputs.empty())
                {
                    const EffectNode* srcNode = graph.FindNode(inputs[0]->sourceNodeId);
                    if (srcNode && srcNode->cachedOutput)
                    {
                        node->cachedOutput = srcNode->cachedOutput;
                        finalOutput = node->cachedOutput;
                    }
                }
                node->dirty = false;
                break;
            }
            }
        }

        // Newly created effects need a second evaluation pass —
        // D2D requires one full render cycle to initialize the transform
        // pipeline before the output is valid. Mark them dirty for next frame.
        if (!m_justCreated.empty())
        {
            for (uint32_t id : m_justCreated)
            {
                auto* n = graph.FindNode(id);
                if (n) n->dirty = true;
            }
        }

        return finalOutput;
    }

    bool GraphEvaluator::ProcessDeferredCompute(
        EffectGraph& graph, ID2D1DeviceContext5* dc)
    {
        if (m_deferredCompute.empty() || !dc) return false;

        // P7 / first-frame safety: if any Source node still has a null
        // cachedOutput, the chain isn't ready (e.g., video provider was
        // just created but hasn't decoded its first frame; image source
        // hasn't loaded; capture provider awaiting first frame). Walking
        // a downstream effect's GetImageLocalBounds chain through a null
        // upstream input AVs deep inside d2d1.dll. Skip this frame --
        // the next worker iteration will retry once the source is
        // populated. We DO clear m_deferredCompute so the entries (which
        // hold raw ID2D1Image* pointers to potentially stale objects)
        // don't pile up across frames.
        for (const auto& n : graph.Nodes())
        {
            if (n.type == NodeType::Source && !n.cachedOutput)
            {
                m_deferredCompute.clear();
                return false;
            }
        }

        bool imageComputeProduced = false;

        // Phase 8c: build the per-frame "needs CPU readback" set. When
        // the skip-readback feature flag is OFF, the set is treated as
        // covering every node (preserves pre-Phase-8c behavior). When
        // ON, a node lands in the set if either:
        //   (1) it is hinted by the host via SetCpuAnalysisInterest
        //       (UI selected node, MCP target, etc.), or
        //   (2) at least one downstream property binding that consumes
        //       this node's analysis output is NOT served via the GPU
        //       SRV path (CanServeBindingViaGpu returned false).
        // Nodes not in the set skip the CopyResource + Map round-trip;
        // their `analysisOutput.fields` retains last-frame values.
        const bool skipFlag = Performance::IsSkipUnneededCpuReadbackEnabled();
        std::unordered_set<uint32_t> needsReadback;
        if (skipFlag)
        {
            // Throttle host-hint readbacks (selected node, canvas value
            // labels) to Performance::CpuAnalysisHintThrottleMs. CPU-
            // routed bindings (added below) are unaffected because the
            // consumer reads `analysisOutput.fields` every frame.
            const auto throttle = std::chrono::milliseconds(
                Performance::CpuAnalysisHintThrottleMs());
            const auto nowTs = std::chrono::steady_clock::now();
            for (uint32_t id : m_cpuAnalysisInterest)
            {
                auto it = m_lastHintReadbackTime.find(id);
                const bool freshHint = (it == m_lastHintReadbackTime.end());
                const bool dueAgain = !freshHint && (nowTs - it->second) >= throttle;
                if (freshHint || dueAgain || throttle.count() == 0)
                    needsReadback.insert(id);
            }
            // Add every source whose binding cannot be GPU-served. These
            // are the consumers actual data dependencies and run every
            // frame regardless of throttle.
            for (const auto& consumerNode : graph.Nodes())
            {
                for (const auto& [propName, binding] : consumerNode.propertyBindings)
                {
                    bool gpuServed =
                        CanServeBindingViaGpu(consumerNode, propName, binding, graph);
                    if (gpuServed) continue;
                    for (const auto& srcOpt : binding.sources)
                    {
                        if (srcOpt.has_value())
                            needsReadback.insert(srcOpt->sourceNodeId);
                    }
                }
            }
        }
        auto isReadbackNeeded = [&](uint32_t nodeId) -> bool
        {
            return !skipFlag || needsReadback.count(nodeId) > 0;
        };

        // Phase 8 perf: pre-render each unique upstream input ONCE
        // per frame and share the FP32 bitmap across all deferred-
        // compute consumers that share it. Without this, a graph
        // like {Source -> 4 stats + ICtCp Tone Map} pays 5x the
        // upstream-render cost (each bridge re-rendered the source
        // into its own private FP32 bitmap).
        //
        // We do the SetTarget+Clear+DrawImage inline (NOT via
        // PreRenderInputBitmap, which nests BeginDraw and breaks
        // submission inside an outer draw session). dc->Flush() forces
        // the commands out before the bridge's D3D11 compute reads.
        // Cache key is the raw ID2D1Image pointer of the upstream's
        // cachedOutput; cleared at end of ProcessDeferredCompute.
        struct SharedFp32 {
            winrt::com_ptr<ID2D1Bitmap1> bitmap;
            UINT32 width{ 0 };
            UINT32 height{ 0 };
        };
        std::unordered_map<ID2D1Image*, SharedFp32> sharedPreRender;

        auto preRenderShared = [&](ID2D1Image* inputImage) -> ID2D1Bitmap1*
        {
            auto it = sharedPreRender.find(inputImage);
            if (it != sharedPreRender.end()) return it->second.bitmap.get();

            float oldDpiX = 0, oldDpiY = 0;
            dc->GetDpi(&oldDpiX, &oldDpiY);
            dc->SetDpi(96.0f, 96.0f);

            D2D1_RECT_F bounds{};
            // GetImageLocalBounds walks the input image's effect chain back
            // to its source bitmap. If any upstream effect has a null/stale
            // input -- which can happen on the FIRST frame after a graph
            // load or GPU switch when a video source provider hasn't
            // decoded its first frame yet -- d2d1.dll AVs deep inside.
            // Wrap defensively and treat any failure as "not yet ready"
            // so the next frame can retry once upstream has its bitmap.
            HRESULT bhr = E_FAIL;
            try
            {
                bhr = dc->GetImageLocalBounds(inputImage, &bounds);
            }
            catch (...)
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                return nullptr;
            }
            if (FAILED(bhr))
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                return nullptr;
            }
            UINT32 w = static_cast<UINT32>((std::min)(bounds.right - bounds.left, 8192.0f));
            UINT32 h = static_cast<UINT32>((std::min)(bounds.bottom - bounds.top, 8192.0f));
            if (w == 0 || h == 0) { dc->SetDpi(oldDpiX, oldDpiY); return nullptr; }

            // Reuse the previous frame's bitmap if dimensions match;
            // otherwise allocate a new one.
            auto& cacheBitmap = m_sharedPreRenderCache[inputImage];
            if (!cacheBitmap.bitmap || cacheBitmap.width != w || cacheBitmap.height != h)
            {
                D2D1_BITMAP_PROPERTIES1 bp{};
                bp.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
                bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
                bp.dpiX = 96.0f;
                bp.dpiY = 96.0f;
                winrt::com_ptr<ID2D1Bitmap1> newBmp;
                HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bp, newBmp.put());
                if (FAILED(hr)) { dc->SetDpi(oldDpiX, oldDpiY); return nullptr; }
                cacheBitmap.bitmap = std::move(newBmp);
                cacheBitmap.width  = w;
                cacheBitmap.height = h;
            }

            // Render upstream into the cached bitmap. Inline pattern
            // (no nested BeginDraw); Flush forces submit before the
            // bridge's D3D11 compute reads.
            winrt::com_ptr<ID2D1Image> prevTarget;
            dc->GetTarget(prevTarget.put());
            dc->SetTarget(cacheBitmap.bitmap.get());
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            dc->DrawImage(inputImage, D2D1::Point2F(-bounds.left, -bounds.top));
            dc->SetTarget(prevTarget.get());
            dc->Flush();
            dc->SetDpi(oldDpiX, oldDpiY);

            SharedFp32 s;
            s.bitmap = cacheBitmap.bitmap;
            s.width  = w;
            s.height = h;
            auto* ptr = s.bitmap.get();
            sharedPreRender[inputImage] = std::move(s);
            return ptr;
        };

        for (auto& deferred : m_deferredCompute)
        {
            auto* node = graph.FindNode(deferred.nodeId);
            if (!node || deferred.inputImages.empty() || !deferred.inputImages[0]) continue;
            if (!node->customEffect.has_value()) continue;

            auto bit = m_bridgeImplCache.find(node->id);
            if (bit == m_bridgeImplCache.end() || !bit->second)
                continue;
            auto* bridge = bit->second;

            // Use the deferred entry's pre-rendered bitmap per-input slot
            // if it exists (image-producing computes pre-render in
            // EvaluateNode while D2D effect properties are fresh).
            // Otherwise share via the per-frame map.
            std::vector<ID2D1Bitmap1*> preRendered(deferred.inputImages.size(), nullptr);
            std::vector<ID2D1Image*>   inputRaw(deferred.inputImages.size(), nullptr);
            for (size_t i = 0; i < deferred.inputImages.size(); ++i)
            {
                if (!deferred.inputImages[i]) continue;
                inputRaw[i] = deferred.inputImages[i].get();
                if (i < deferred.preRenderedInputs.size() && deferred.preRenderedInputs[i])
                    preRendered[i] = deferred.preRenderedInputs[i].get();
                else
                    preRendered[i] = preRenderShared(inputRaw[i]);
            }

            const bool readback = isReadbackNeeded(node->id);
            DispatchViaBridge(dc, graph, *node, inputRaw,
                preRendered, bridge, readback);

            // Phase 8c: record the timestamp for hinted nodes whose
            // readback actually ran this frame so the throttle window
            // starts now. Bindings-driven readbacks (consumer needs CPU
            // value) intentionally don't update this map -- they fire
            // every frame regardless of throttle.
            if (readback && skipFlag && m_cpuAnalysisInterest.count(node->id))
                m_lastHintReadbackTime[node->id] = std::chrono::steady_clock::now();

            bool hasImageOutput = !node->outputPins.empty();
            if (hasImageOutput && node->cachedOutput)
            {
                imageComputeProduced = true;
                std::vector<uint32_t> queue = { node->id };
                for (size_t i = 0; i < queue.size(); ++i)
                {
                    for (const auto* edge : graph.GetOutputEdges(queue[i]))
                    {
                        auto* dn = graph.FindNode(edge->destNodeId);
                        if (dn && !dn->dirty)
                        {
                            dn->dirty = true;
                            queue.push_back(edge->destNodeId);
                        }
                    }
                }
            }
        }

        m_deferredCompute.clear();
        return true;
    }

    // -----------------------------------------------------------------------
    // Bridge-based D3D11 compute dispatch (Phase 8)
    // -----------------------------------------------------------------------
    //
    // Replaces the pre-Phase-8 DispatchUserD3D11Compute (analysis-only)
    // and DispatchImageCompute (image-producing) paths. Both flows now
    // go through CustomComputeBridgeEffect::Dispatch, which:
    //   * pre-renders the upstream D2D chain to an FP32 bitmap,
    //   * lazily compiles the shader on first call (or uses installed
    //     bytecode),
    //   * dispatches with optional u1 image-output binding,
    //   * reads back analysis floats and exposes the analysis SRV via
    //     IEngineComputeOutput for downstream Phase 8 GPU-binding
    //     consumers,
    //   * wraps the image-output texture as an ID2D1Bitmap1.

    void GraphEvaluator::DispatchViaBridge(
        ID2D1DeviceContext5* dc,
        const EffectGraph& graph,
        EffectNode& node,
        const std::vector<ID2D1Image*>& inputImages,
        const std::vector<ID2D1Bitmap1*>& preRenderedInputs,
        Effects::CustomComputeBridgeEffect* bridge,
        bool readbackToCpu)
    {
        if (!bridge) return;
        auto& def = node.customEffect.value();

        // Lazy compile: ProcessDeferredCompute runs inside BeginDraw so
        // we have a valid D3D11 device on the DC. If the host hasn't
        // already populated def.compiledBytecode (e.g. the editor or
        // an MCP /effect/compile call), compile via ShaderCompiler and
        // hand the bytecode to the bridge. Subsequent dispatches reuse
        // the installed shader.
        if (def.compiledBytecode.empty())
        {
            auto gpuNames = ExtractGpuBindableNames(def);
            auto cached = CompileViaCache(
                node.name, /*effectVersion*/ 1u,
                def.hlslSource, "cs_5_0", /*macroBitset*/ 0u, gpuNames);
            if (cached.status != Effects::BytecodeStatus::Ready)
            {
                node.runtimeError = cached.errorMessage.empty()
                    ? L"D3D11 compute shader compile failed"
                    : std::move(cached.errorMessage);
                return;
            }
            def.compiledBytecode = std::move(cached.bytecode);
            bridge->SetCompiledBytecode(def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            node.runtimeError.clear();
            // Phase 8 eager precompile (mirror of the ShaderLab built-in
            // case in EvaluateNode).
            if (!gpuNames.empty())
            {
                Effects::BytecodeCacheMetadata meta;
                meta.effectId = node.name;
                meta.version  = 1u;
                Effects::BytecodeCache::Instance().PrecompileCommonShapes(
                    meta,
                    Effects::CanonicalizeHlslSource(def.hlslSource),
                    "main", "cs_5_0", gpuNames);
            }
        }

        // -----------------------------------------------------------------
        // Phase 8 GPU-binding plan
        // -----------------------------------------------------------------
        // Walk gpuBindable params in declaration order. For each one
        // that has a binding from an upstream effect implementing
        // IEngineComputeOutput, capture (paramIndex, slot, srv) and
        // set the corresponding bit in macroBitset. Slot follows the
        // convention: gpuBindable param at index i binds to t<i+1>
        // (t0 is reserved for the input texture). HLSL authors using
        // SHADERLAB_GPU_BUFFER must pick consistent slot numbers.
        struct GpuBindingEntry {
            uint32_t                                 gpuBindableIndex;
            uint32_t                                 slot;
            uint32_t                                 fieldIndex;   // float4 index in upstream's analysis SRV
            std::wstring                             paramName;    // for _SLIdx_<name> cbuffer slot lookup
            winrt::com_ptr<ID3D11ShaderResourceView> srv;
        };
        std::vector<GpuBindingEntry> bindingPlan;
        uint32_t macroBitset = 0;
        const std::vector<uint8_t>* reflectBytecode = &def.compiledBytecode;
        std::vector<uint8_t> variantBytecode;

        if (Performance::IsGpuBindingsEnabled())
        {
            uint32_t gpuBindableIdx = 0;
            for (const auto& p : def.parameters)
            {
                if (!p.gpuBindable)
                    continue;
                const uint32_t thisGpuIdx = gpuBindableIdx++;

                auto bIt = node.propertyBindings.find(p.name);
                if (bIt == node.propertyBindings.end())
                    continue;
                const auto& binding = bIt->second;
                if (binding.wholeArray ||
                    binding.sources.empty() ||
                    !binding.sources[0].has_value())
                    continue;

                uint32_t srcId = binding.sources[0]->sourceNodeId;
                // Discover via m_bridgeImplCache (the bridge impl
                // pointer captured at CreateEffect time). D2D's outer
                // ID2D1Effect's QueryInterface doesn't delegate
                // arbitrary IIDs to the impl, so we use the impl
                // pointer directly. The bridge implements
                // IEngineComputeOutput by delegation to its internal
                // D3D11ComputeRunner.
                auto bridgeIt = m_bridgeImplCache.find(srcId);
                if (bridgeIt == m_bridgeImplCache.end() || !bridgeIt->second)
                    continue;
                winrt::com_ptr<Effects::IEngineComputeOutput> ieco;
                if (FAILED(bridgeIt->second->QueryInterface(
                    __uuidof(Effects::IEngineComputeOutput), ieco.put_void())))
                    continue;

                winrt::com_ptr<ID3D11ShaderResourceView> srv;
                if (FAILED(ieco->GetAnalysisSrv(srv.put())) || !srv)
                    continue;

                // Compute the upstream field's float4 index in the
                // analysis SRV. The upstream's CustomEffectDefinition
                // declares analysis fields in order; each field
                // occupies pixelCount() float4 slots (floats / float2 /
                // float3 / float4 = 1 slot each; arrays = arrayLength
                // * 1 slot per element). Walk the prefix-sum until we
                // hit the bound source field name.
                const EffectNode* srcNode = graph.FindNode(srcId);
                if (!srcNode || !srcNode->customEffect.has_value())
                    continue;
                uint32_t fieldIndex = 0;
                bool fieldFound = false;
                for (const auto& fd : srcNode->customEffect->analysisFields)
                {
                    if (fd.name == binding.sources[0]->sourceFieldName)
                    {
                        fieldIndex += binding.sources[0]->sourceIndex;
                        fieldFound = true;
                        break;
                    }
                    fieldIndex += fd.pixelCount();
                }
                if (!fieldFound)
                    continue;

                bindingPlan.push_back({
                    thisGpuIdx,
                    thisGpuIdx + 1u,
                    fieldIndex,
                    p.name,
                    std::move(srv) });
                macroBitset |= (1u << thisGpuIdx);
                // (telemetry already bumped by ResolveBindings; no
                // double-count here.)
            }

            // If we're routing any binding GPU-side, swap to the
            // variant bytecode (compiled with _SLPARAM_<name>_GPU=1
            // for the bits set). Eagerly precompiled at first encounter
            // (commit 74eb9a5), so this is typically a cache hit.
            if (macroBitset != 0)
            {
                auto gpuNames = ExtractGpuBindableNames(def);
                auto variant = CompileViaCache(
                    node.name, /*effectVersion*/ 1u,
                    def.hlslSource, "cs_5_0", macroBitset, gpuNames);
                if (variant.status == Effects::BytecodeStatus::Ready)
                {
                    variantBytecode  = std::move(variant.bytecode);
                    reflectBytecode  = &variantBytecode;
                    bridge->SetCompiledBytecode(
                        variantBytecode.data(),
                        static_cast<UINT32>(variantBytecode.size()));
                }
                else
                {
                    // Variant unavailable -- gracefully fall back to
                    // baseline (cbuffer mode for all params; CPU
                    // readback path stays as the source of truth).
                    bindingPlan.clear();
                    macroBitset = 0;
                }
            }
        }

        // Analysis float4 count: sum over typed-field pixel counts.
        UINT32 analysisFloat4Count = 0;
        for (const auto& f : def.analysisFields)
            analysisFloat4Count += f.pixelCount();

        // Image-output dimensions: DiagramSize / OutputSize for square
        // viewers, or OutputWidth + OutputHeight for non-square viewers,
        // fallback to upstream input bounds. 0/0 = analysis-only (no
        // u1 binding).
        UINT32 imageOutW = 0, imageOutH = 0;
        bool hasImageOutput = !node.outputPins.empty();
        if (hasImageOutput)
        {
            // Pass 1: explicit OutputWidth + OutputHeight (non-square).
            UINT32 explicitW = 0, explicitH = 0;
            for (const auto& [key, val] : node.properties)
            {
                if (key == L"OutputWidth")
                {
                    if (auto* f = std::get_if<float>(&val))
                        explicitW = (*f >= 1.0f) ? static_cast<UINT32>(*f) : 0;
                    else if (auto* u = std::get_if<uint32_t>(&val))
                        explicitW = *u;
                }
                else if (key == L"OutputHeight")
                {
                    if (auto* f = std::get_if<float>(&val))
                        explicitH = (*f >= 1.0f) ? static_cast<UINT32>(*f) : 0;
                    else if (auto* u = std::get_if<uint32_t>(&val))
                        explicitH = *u;
                }
            }
            if (explicitW > 0 && explicitH > 0)
            {
                imageOutW = explicitW;
                imageOutH = explicitH;
            }
            // Pass 2: square sizing via DiagramSize / OutputSize.
            if (imageOutW == 0)
            {
                for (const auto& p : def.parameters)
                {
                    if (p.name == L"DiagramSize" || p.name == L"OutputSize")
                    {
                        auto it = node.properties.find(p.name);
                        if (it != node.properties.end())
                        {
                            if (auto* f = std::get_if<float>(&it->second))
                                imageOutW = imageOutH = static_cast<UINT32>((std::max)(*f, 64.0f));
                            else if (auto* u = std::get_if<uint32_t>(&it->second))
                                imageOutW = imageOutH = (std::max)(*u, 64u);
                        }
                        break;
                    }
                }
            }
            if (imageOutW == 0)
            {
                // Fall back to upstream input #0 dimensions (96 DPI bounds
                // from the pre-rendered bitmap, if any, else the input
                // image's bounds). For multi-input shaders we still drive
                // dispatch sizing from t0 -- the contract is that all
                // inputs match dimensions.
                ID2D1Bitmap1* primaryPreRendered =
                    preRenderedInputs.empty() ? nullptr : preRenderedInputs[0];
                ID2D1Image* primaryInput =
                    inputImages.empty() ? nullptr : inputImages[0];
                if (primaryPreRendered)
                {
                    auto sz = primaryPreRendered->GetPixelSize();
                    imageOutW = sz.width; imageOutH = sz.height;
                }
                else if (primaryInput)
                {
                    float oldDpiX, oldDpiY;
                    dc->GetDpi(&oldDpiX, &oldDpiY);
                    dc->SetDpi(96.0f, 96.0f);
                    D2D1_RECT_F bounds{};
                    dc->GetImageLocalBounds(primaryInput, &bounds);
                    imageOutW = static_cast<UINT32>((std::min)(bounds.right - bounds.left, 8192.0f));
                    imageOutH = static_cast<UINT32>((std::min)(bounds.bottom - bounds.top, 8192.0f));
                    dc->SetDpi(oldDpiX, oldDpiY);
                }
                if (imageOutW < 64) imageOutW = 64;
                if (imageOutH < 64) imageOutH = 64;
            }
        }

        // Pack cbuffer (user portion only -- the runner prepends
        // Width/Height). Use the typed PackPropertyToCBuffer helper
        // (Phase 3) so HLSL `uint`/`int`/`bool` slots receive the
        // correctly converted scalar instead of a float bit pattern.
        // Reflection runs against the variant bytecode if we swapped;
        // GPU-bound params simply have no cbuffer slot in the variant
        // and PackPropertyToCBuffer skips them naturally.
        std::vector<BYTE> cbBytes;
        if (!def.parameters.empty() && !reflectBytecode->empty())
        {
            winrt::com_ptr<ID3D11ShaderReflection> reflect;
            HRESULT hr = D3DReflect(
                reflectBytecode->data(), reflectBytecode->size(),
                IID_ID3D11ShaderReflection, reinterpret_cast<void**>(reflect.put()));
            if (SUCCEEDED(hr) && reflect)
            {
                auto* cbReflect = reflect->GetConstantBufferByIndex(0);
                D3D11_SHADER_BUFFER_DESC cbDesc{};
                if (cbReflect && SUCCEEDED(cbReflect->GetDesc(&cbDesc)) && cbDesc.Size > 8)
                {
                    UINT32 userSize = cbDesc.Size - 8;
                    cbBytes.assign(userSize, BYTE{ 0 });
                    for (UINT v = 0; v < cbDesc.Variables; ++v)
                    {
                        auto* var = cbReflect->GetVariableByIndex(v);
                        D3D11_SHADER_VARIABLE_DESC varDesc{};
                        if (!var || FAILED(var->GetDesc(&varDesc))) continue;
                        if (varDesc.StartOffset < 8) continue; // skip Width/Height

                        std::wstring varName(varDesc.Name, varDesc.Name + strlen(varDesc.Name));
                        UINT32 destOff = varDesc.StartOffset - 8;
                        if (destOff >= cbBytes.size()) continue;
                        UINT32 remaining = static_cast<UINT32>(cbBytes.size() - destOff);

                        // Phase 8 GPU-binding index slot. Variant
                        // bytecode declares a `uint _SLIdx_<paramName>;`
                        // for each gpuBindable param routed via SRV.
                        // The host packs the upstream's float4 index
                        // here so the consumer's shader reads the right
                        // slot via _SLBuf_<name>[_SLIdx_<name>].
                        if (varName.starts_with(L"_SLIdx_"))
                        {
                            std::wstring paramName = varName.substr(7);
                            for (const auto& e : bindingPlan)
                            {
                                if (e.paramName == paramName)
                                {
                                    uint32_t idx = e.fieldIndex;
                                    if (remaining >= sizeof(uint32_t))
                                        memcpy(cbBytes.data() + destOff, &idx, sizeof(uint32_t));
                                    break;
                                }
                            }
                            continue;
                        }

                        auto propIt = node.properties.find(varName);
                        if (propIt == node.properties.end()) continue;

                        D3D11_SHADER_TYPE_DESC typeDesc{};
                        D3D_SHADER_VARIABLE_TYPE hlslType = D3D_SVT_FLOAT;
                        UINT cols = 1;
                        if (auto* typeInfo = var->GetType();
                            typeInfo && SUCCEEDED(typeInfo->GetDesc(&typeDesc)))
                        {
                            hlslType = typeDesc.Type;
                            cols = typeDesc.Columns;
                        }

                        Effects::PackPropertyToCBuffer(
                            cbBytes.data() + destOff, remaining,
                            hlslType, cols, propIt->second);
                    }
                }
            }
        }

        // Drive the dispatch through the bridge. The bridge handles
        // pre-rendering internally; if we have pre-rendered bitmaps
        // it's a cheap blit (same FP32 format). Use them directly when
        // available so D2D doesn't re-evaluate upstream effects with
        // potentially-different cached state.
        std::vector<ID2D1Image*> dispatchInputs(inputImages.size(), nullptr);
        for (size_t i = 0; i < inputImages.size(); ++i)
        {
            ID2D1Bitmap1* preRendered = (i < preRenderedInputs.size())
                ? preRenderedInputs[i] : nullptr;
            dispatchInputs[i] = preRendered
                ? static_cast<ID2D1Image*>(preRendered)
                : inputImages[i];
        }

        // Phase 8 GPU bindings: register each upstream SRV with its
        // declared t-slot. Bridge clears these at the end of Dispatch.
        for (const auto& e : bindingPlan)
            bridge->SetGpuBinding(e.slot, e.srv.get());

        // Image-producing per-pixel computes need dispatch dims that
        // cover the full output. analysis-only and "fixed-size loop"
        // image producers (e.g. CIE Histogram, Vectorscope) keep (1,1,1)
        // because their shader does its own internal iteration over the
        // source pixels into groupshared accumulators.
        //
        // Heuristic for "single group" vs "per-pixel tile":
        //   * Effects whose descriptor declares a `DiagramSize` /
        //     `OutputSize` parameter are visualization viewers that
        //     compute their output independently of the source resolution.
        //     Their shader assumes ONE thread group covers the whole
        //     output. Tiling them across the output (W/tx, H/ty)
        //     dispatches the same redundant histogram-of-the-whole-image
        //     N times -- on a 4K source feeding a 512x512 CIE Histogram
        //     that's a 256x amplification of an already heavy inner
        //     loop. Keep dispatch at (1,1,1) for these.
        //   * Otherwise tile per-pixel (the ICtCp Tone Map style: each
        //     thread group covers a [numthreads] tile of the output).
        if (hasImageOutput && imageOutW > 0 && imageOutH > 0 &&
            def.threadGroupX > 0 && def.threadGroupY > 0)
        {
            // Discriminator: single-group scatter shaders use a large
            // thread group (commonly 32x32 = 1024) and dispatch (1,1,1);
            // per-pixel-tile shaders use a small group (commonly 8x8 = 64)
            // and dispatch (W/tx, H/ty, 1). Threshold at 256 cleanly
            // separates the two conventions in the current catalog.
            //
            // The DiagramSize / OutputSize parameter names ALSO mark fixed-
            // output viewers (CIE Histogram, etc.) that pre-date the
            // generic threshold; keep that check for descriptors that use
            // an 8x8 group but still want single-group dispatch (none
            // exist today, but the check is cheap and safe).
            const bool largeGroup = (def.threadGroupX * def.threadGroupY) >= 256;
            bool isFixedSizeViewer = largeGroup;
            for (const auto& p : def.parameters)
            {
                if (p.name == L"DiagramSize" || p.name == L"OutputSize")
                {
                    isFixedSizeViewer = true;
                    break;
                }
            }
            const bool perPixelTiling =
                !isFixedSizeViewer &&
                def.threadGroupX * def.threadGroupY >= 4 &&
                def.threadGroupY >= 2;
            if (perPixelTiling)
            {
                UINT32 dx = (imageOutW + def.threadGroupX - 1) / def.threadGroupX;
                UINT32 dy = (imageOutH + def.threadGroupY - 1) / def.threadGroupY;
                bridge->SetDispatchDims(dx, dy, 1);
            }
        }

        std::vector<float> analysisFloats;
        // Phase 8c: pass nullptr for outAnalysisFloats when readback is
        // not needed; the bridge interprets that as "skip the Map" and
        // returns an empty `floats` vector. The structured-buffer SRV
        // is still populated on the GPU side for downstream consumers.
        HRESULT hr = bridge->Dispatch(
            dc,
            dispatchInputs.data(),
            static_cast<UINT32>(dispatchInputs.size()),
            cbBytes.empty() ? nullptr : cbBytes.data(),
            static_cast<UINT32>(cbBytes.size()),
            analysisFloat4Count,
            imageOutW, imageOutH,
            readbackToCpu ? &analysisFloats : nullptr);

        if (!readbackToCpu)
            Performance::IncrementSkippedCpuReadbacks();

        // After dispatch, restore the baseline bytecode on the bridge
        // so a subsequent dispatch with a different binding plan starts
        // from a known state. Cheap since bridge keeps both blobs as
        // bytecode pointers; CreateComputeShader is the only D3D cost
        // and the cache returns the same bytes for the same key.
        if (macroBitset != 0 && !def.compiledBytecode.empty())
        {
            bridge->SetCompiledBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
        }

        if (FAILED(hr))
        {
            node.runtimeError = std::format(L"Bridge dispatch failed 0x{:08X}",
                static_cast<uint32_t>(hr));
            return;
        }
        node.runtimeError.clear();

        // Unpack analysis floats into typed fields. Phase 8c: only
        // overwrite when readback actually ran -- skip-readback frames
        // leave `node.analysisOutput.fields` at the previous-frame
        // values (or empty if never populated). Hosts that need fresh
        // values must include the node in
        // `GraphEvaluator::SetCpuAnalysisInterest` (see Performance.h).
        if (readbackToCpu && analysisFloat4Count > 0)
        {
            node.analysisOutput.type = AnalysisOutputType::Typed;
            node.analysisOutput.fields.clear();
            UINT32 pixelOffset = 0;
            for (const auto& fd : def.analysisFields)
            {
                AnalysisFieldValue fv;
                fv.name = fd.name;
                fv.type = fd.type;

                UINT32 pc = fd.pixelCount();
                bool isArray = AnalysisFieldIsArray(fd.type);
                UINT32 cc = AnalysisFieldComponentCount(fd.type);

                if (!isArray)
                {
                    if (pixelOffset * 4 < analysisFloats.size())
                    {
                        for (UINT32 c = 0; c < cc && (pixelOffset * 4 + c) < analysisFloats.size(); ++c)
                            fv.components[c] = analysisFloats[pixelOffset * 4 + c];
                    }
                }
                else
                {
                    fv.arrayData.resize(fd.arrayLength * cc, 0.0f);
                    for (UINT32 i = 0; i < fd.arrayLength; ++i)
                    {
                        UINT32 base = (pixelOffset + i) * 4;
                        for (UINT32 c = 0; c < cc && (base + c) < analysisFloats.size(); ++c)
                            fv.arrayData[i * cc + c] = analysisFloats[base + c];
                    }
                }
                pixelOffset += pc;
                node.analysisOutput.fields.push_back(std::move(fv));
            }
        }

        // Image-producing nodes wire their output to the bridge's
        // wrapped bitmap. Downstream nodes consume node->cachedOutput
        // directly, same as for D2D-tiled compute / pixel-shader nodes.
        if (hasImageOutput)
            node.cachedOutput = bridge->GetImageOutput();
        else
            node.cachedOutput = nullptr;
    }

    // Phase 8c: predicate matching the GPU-routability checks used inside
    // DispatchViaBridge (see bindingPlan construction). Conservative:
    // any condition that would cause DispatchViaBridge to skip GPU
    // routing for the binding returns false here, so the pre-pass
    // counts the source as needing CPU readback. Keep this in sync
    // with the bindingPlan construction in DispatchViaBridge -- if a
    // new GPU-routability requirement is added there, mirror it here.
    bool GraphEvaluator::CanServeBindingViaGpu(
        const EffectNode&         consumer,
        const std::wstring&       paramName,
        const Graph::PropertyBinding& binding,
        const EffectGraph&        graph) const
    {
        if (!Performance::IsGpuBindingsEnabled())
            return false;
        // Consumer must be a D3D11 compute custom effect (only the
        // CustomComputeBridgeEffect can wire upstream SRVs at t-slots).
        // Pixel-shader / D2D-tiled / built-in D2D effects always go CPU.
        if (!consumer.customEffect.has_value())
            return false;
        if (consumer.customEffect->shaderType != Graph::CustomShaderType::D3D11ComputeShader)
            return false;
        // Single-component bindings only -- multi-source per-component
        // packing always goes CPU.
        if (binding.wholeArray ||
            binding.sources.empty() ||
            binding.sources.size() > 1 ||
            !binding.sources[0].has_value())
            return false;
        // Target parameter must be flagged gpuBindable.
        const auto& def = consumer.customEffect.value();
        const Graph::ParameterDefinition* paramDef = nullptr;
        for (const auto& p : def.parameters)
        {
            if (p.name == paramName) { paramDef = &p; break; }
        }
        if (!paramDef || !paramDef->gpuBindable)
            return false;
        // Source bridge must exist and expose IEngineComputeOutput. We
        // don't actually call GetAnalysisSrv here (it can fail before
        // first dispatch); presence of the bridge in m_bridgeImplCache
        // and a customEffect on the source is the predicate. The
        // pre-pass runs before DispatchViaBridge during the same
        // ProcessDeferredCompute, so by the time the consumer
        // dispatches, the upstream's runner has produced its SRV.
        const uint32_t srcId = binding.sources[0]->sourceNodeId;
        auto bridgeIt = m_bridgeImplCache.find(srcId);
        if (bridgeIt == m_bridgeImplCache.end() || !bridgeIt->second)
            return false;
        const EffectNode* srcNode = graph.FindNode(srcId);
        if (!srcNode || !srcNode->customEffect.has_value())
            return false;
        // Source field must exist on the upstream's analysisFields.
        const auto& srcFieldName = binding.sources[0]->sourceFieldName;
        bool fieldFound = false;
        for (const auto& fd : srcNode->customEffect->analysisFields)
        {
            if (fd.name == srcFieldName) { fieldFound = true; break; }
        }
        return fieldFound;
    }

    // -----------------------------------------------------------------------
    // Effect cache management
    // -----------------------------------------------------------------------

    void GraphEvaluator::ReleaseCache()
    {
        m_effectCache.clear();
        m_outputCache.clear();
        m_customImplCache.clear();
        m_bridgeImplCache.clear();
        m_sharedPreRenderCache.clear();
        m_lastHintReadbackTime.clear();
        m_dummySourceBitmap = nullptr;

        // P7: also drop any deferred-compute entries that the previous
        // Evaluate left pending for ProcessDeferredCompute. They hold raw
        // ID2D1Image* pointers to the cachedOutputs we just released; if
        // we don't clear here, the next ProcessDeferredCompute call (from
        // the post-SwitchAdapter worker, or any path that resets the
        // device) deref's freed bitmaps and AVs in d2d1.dll.
        m_deferredCompute.clear();
    }

    void GraphEvaluator::ReleaseCache(EffectGraph& graph)
    {
        ReleaseCache();
        // EffectNode::cachedOutput is a non-owning raw pointer whose lifetime is
        // owned by the caches we just cleared. Null every node's pointer so the
        // render path can't dereference a freed ID2D1Image.
        for (auto& node : const_cast<std::vector<EffectNode>&>(graph.Nodes()))
        {
            node.cachedOutput = nullptr;
            node.dirty = true;
        }
    }

    void GraphEvaluator::InvalidateNode(uint32_t nodeId)
    {
        m_effectCache.erase(nodeId);
        m_outputCache.erase(nodeId);
        m_customImplCache.erase(nodeId);
        m_bridgeImplCache.erase(nodeId);
        // Note: caller must also clear EffectNode::cachedOutput on the node
        // (the raw pointer it holds is now dangling). Prefer the graph-aware
        // overload below.
    }

    void GraphEvaluator::InvalidateNode(EffectGraph& graph, uint32_t nodeId)
    {
        InvalidateNode(nodeId);
        if (auto* node = graph.FindNode(nodeId))
        {
            node->cachedOutput = nullptr;
            node->dirty = true;
        }
    }

    void GraphEvaluator::ResolveSourceBindings(EffectGraph& graph)
    {
        for (auto& node : const_cast<std::vector<EffectNode>&>(graph.Nodes()))
        {
            if (node.type != NodeType::Source) continue;
            if (node.propertyBindings.empty()) continue;

            std::map<std::wstring, PropertyValue> effectiveProps;
            bool changed = ResolveBindings(node, graph, effectiveProps);
            if (changed)
            {
                for (const auto& [propName, binding] : node.propertyBindings)
                {
                    auto eit = effectiveProps.find(propName);
                    if (eit != effectiveProps.end())
                        node.properties[propName] = eit->second;
                }
                node.dirty = true;
            }
        }
    }

    void GraphEvaluator::UpdateNodeShader(uint32_t nodeId, const EffectNode& node)
    {
        if (!node.customEffect.has_value() || !node.customEffect->isCompiled())
            return;

        auto& def = node.customEffect.value();

        // D3D11 compute shaders: clear the bridge cache so the next
        // CreateOrGetEffect creates a fresh bridge with the new bytecode.
        if (def.shaderType == CustomShaderType::D3D11ComputeShader)
        {
            m_effectCache.erase(nodeId);
            m_bridgeImplCache.erase(nodeId);
            return;
        }
        auto implIt = m_customImplCache.find(nodeId);
        if (implIt == m_customImplCache.end())
            return; // First compile — next Evaluate() will create the effect.

        // Check if input count changed (structural change requires recreate).
        UINT32 newIC = static_cast<UINT32>(
            (std::max)(size_t(1), def.inputNames.size()));
        auto effectIt = m_effectCache.find(nodeId);
        if (effectIt != m_effectCache.end())
        {
            UINT32 curIC = effectIt->second->GetInputCount();
            if (curIC != newIC)
            {
                InvalidateNode(nodeId);
                return;
            }
        }

        // Non-structural recompile: update bytecode in place.
        if (node.type == NodeType::PixelShader && implIt->second.pixelImpl)
        {
            implIt->second.pixelImpl->SetShaderGuid(def.shaderGuid);
            implIt->second.pixelImpl->LoadShaderBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
        }
        else if (node.type == NodeType::ComputeShader && implIt->second.computeImpl)
        {
            implIt->second.computeImpl->SetShaderGuid(def.shaderGuid);
            implIt->second.computeImpl->LoadShaderBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            implIt->second.computeImpl->SetThreadGroupSize(
                def.threadGroupX, def.threadGroupY, def.threadGroupZ);
        }
    }

    // -----------------------------------------------------------------------
    // D2D effect creation / retrieval
    // -----------------------------------------------------------------------

    ID2D1Effect* GraphEvaluator::GetOrCreateEffect(
        ID2D1DeviceContext5* dc,
        const EffectNode& node)
    {
        // Check the cache first.
        auto it = m_effectCache.find(node.id);
        if (it != m_effectCache.end())
            return it->second.get();

        // For custom effects, use the per-definition shaderGuid as the CLSID
        // and register with the exact number of inputs.
        GUID clsid{};
        bool isD3D11Compute = node.customEffect.has_value() &&
            node.customEffect->shaderType == CustomShaderType::D3D11ComputeShader;

        if (isD3D11Compute)
        {
            // Phase 8: D3D11 compute custom effects route through the
            // shared CustomComputeBridgeEffect. The bridge satisfies
            // D2D's "one input -> one output" contract via a passthrough
            // pixel shader; the actual D3D11 compute dispatch happens
            // out-of-band in ProcessDeferredCompute via QI for
            // ICustomComputeBridge. Single CLSID for all instances --
            // bytecode is set per-instance after CreateEffect via
            // SetCompiledBytecode. The bridge is registered once at
            // engine startup in RegisterEngineD2DEffects.
            clsid = Effects::CustomComputeBridgeEffect::CLSID_CustomComputeBridge;
        }
        else if ((node.type == NodeType::PixelShader || node.type == NodeType::ComputeShader) &&
            node.customEffect.has_value() && node.customEffect->isCompiled())
        {
            clsid = node.customEffect->shaderGuid;
            // D2D custom effects require at least 1 input. Source effects
            // (empty inputNames) get a hidden input fed by a dummy bitmap.
            UINT32 inputCount = static_cast<UINT32>(
                (std::max)(size_t(1), node.customEffect->inputNames.size()));

            // Register this specific CLSID if not already registered.
            winrt::com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            auto factory1 = factory.as<ID2D1Factory1>();
            if (factory1)
            {
                HRESULT regHr;
                if (node.type == NodeType::PixelShader)
                    regHr = Effects::CustomPixelShaderEffect::RegisterWithInputCount(
                        factory1.get(), clsid, inputCount);
                else
                    regHr = Effects::CustomComputeShaderEffect::RegisterWithInputCount(
                        factory1.get(), clsid, inputCount);

                // S_OK = registered, D2DERR_ALREADY_REGISTERED-style = already done (also fine).
                if (FAILED(regHr) && regHr != static_cast<HRESULT>(0x88990004L))
                {
                    OutputDebugStringW(std::format(
                        L"[CustomFX] RegisterWithInputCount node {} inputs={} hr=0x{:08X}\n",
                        node.id, inputCount, static_cast<uint32_t>(regHr)).c_str());
                }
            }
        }
        else if (node.effectClsid.has_value())
        {
            clsid = node.effectClsid.value();
        }
        else
        {
            return nullptr;
        }

        // Clear thread-local impl pointers before CreateEffect.
        Effects::CustomPixelShaderEffect::s_lastCreated = nullptr;
        Effects::CustomComputeShaderEffect::s_lastCreated = nullptr;
        Effects::CustomComputeBridgeEffect::s_lastCreated = nullptr;

        winrt::com_ptr<ID2D1Effect> effect;
        // Set pending input count BEFORE CreateEffect so the constructor
        // initializes m_inputCount correctly for SetSingleTransformNode.
        if (node.customEffect.has_value())
        {
            // Always at least 1 for D2D (source effects get a dummy input).
            UINT32 ic = static_cast<UINT32>(
                (std::max)(size_t(1), node.customEffect->inputNames.size()));
            if (node.type == NodeType::PixelShader)
                Effects::CustomPixelShaderEffect::s_pendingInputCount = ic;
            else if (node.type == NodeType::ComputeShader)
                Effects::CustomComputeShaderEffect::s_pendingInputCount = ic;
        }

        HRESULT hr = dc->CreateEffect(clsid, effect.put());

        // Clear pending counts.
        Effects::CustomPixelShaderEffect::s_pendingInputCount = 0;
        Effects::CustomComputeShaderEffect::s_pendingInputCount = 0;

        if (FAILED(hr))
        {
            wchar_t guidStr[64]{};
            StringFromGUID2(clsid, guidStr, 64);
            OutputDebugStringW(std::format(
                L"[CustomFX] CreateEffect({}) FAILED hr=0x{:08X}\n",
                guidStr, static_cast<uint32_t>(hr)).c_str());
            return nullptr;
        }

        // Capture custom effect impl for host-side API.
        if (node.type == NodeType::PixelShader && Effects::CustomPixelShaderEffect::s_lastCreated)
        {
            auto* impl = Effects::CustomPixelShaderEffect::s_lastCreated;
            m_customImplCache[node.id] = { impl, nullptr };

            // For zero-input source effects, set fixed output size from properties.
            if (node.customEffect.has_value() && node.customEffect->inputNames.empty())
            {
                UINT32 outW = 512, outH = 512;
                // Look for OutputSize/DiagramSize/PlateSize/GradSize/PatternSize/PatchSize property.
                for (const auto& [key, val] : node.properties)
                {
                    if (key.find(L"Size") != std::wstring::npos ||
                        key.find(L"size") != std::wstring::npos)
                    {
                        if (auto* f = std::get_if<float>(&val))
                        {
                            outW = outH = static_cast<UINT32>(*f);
                            break;
                        }
                    }
                }
                // Special case: PatchSize for Color Checker (6 cols × 4 rows).
                auto patchIt = node.properties.find(L"PatchSize");
                if (patchIt != node.properties.end())
                {
                    if (auto* f = std::get_if<float>(&patchIt->second))
                    {
                        outW = static_cast<UINT32>(*f * 6);
                        outH = static_cast<UINT32>(*f * 4);
                    }
                }
                impl->SetFixedOutputSize(outW, outH);
            }
        }
        else if (node.type == NodeType::ComputeShader && Effects::CustomComputeShaderEffect::s_lastCreated)
        {
            m_customImplCache[node.id] = { nullptr, Effects::CustomComputeShaderEffect::s_lastCreated };
        }
        else if (isD3D11Compute && Effects::CustomComputeBridgeEffect::s_lastCreated)
        {
            // Phase 8: bridge effect captured. Install pre-compiled
            // bytecode now if the host has it (compiled via
            // ShaderCompiler at MCP /effect/compile time or by the
            // editor on Ctrl+Enter). If not yet compiled, the
            // ProcessDeferredCompute pass will compile lazily on
            // first dispatch and call SetCompiledBytecode then.
            auto* bridge = Effects::CustomComputeBridgeEffect::s_lastCreated;
            m_bridgeImplCache[node.id] = bridge;
            if (node.customEffect.has_value() &&
                !node.customEffect->compiledBytecode.empty())
            {
                bridge->SetCompiledBytecode(
                    node.customEffect->compiledBytecode.data(),
                    static_cast<UINT32>(node.customEffect->compiledBytecode.size()));
            }
        }

        auto* raw = effect.get();
        m_effectCache[node.id] = std::move(effect);
        m_justCreated.insert(node.id);
        return raw;
    }

    // -----------------------------------------------------------------------
    // Property application
    // -----------------------------------------------------------------------

    void GraphEvaluator::ApplyProperties(
        ID2D1Effect* effect,
        const EffectNode& node,
        const std::map<std::wstring, PropertyValue>& effectiveProps)
    {
        if (!effect)
            return;

        // D2D built-in effects use indexed properties (0, 1, 2, ...).
        // The property map in EffectNode uses string keys. We map numeric
        // string keys ("0", "1", ...) directly to D2D property indices.
        // Named keys are resolved through the effect's property name table.
        for (const auto& [key, value] : effectiveProps)
        {
            // Try to parse the key as a numeric index first.
            uint32_t index = UINT32_MAX;
            if (!key.empty() && key[0] >= L'0' && key[0] <= L'9')
            {
                size_t pos = 0;
                unsigned long parsed = std::stoul(key, &pos);
                if (pos == key.size())
                    index = static_cast<uint32_t>(parsed);
            }

            // If not numeric, look up the property by name.
            if (index == UINT32_MAX)
            {
                index = effect->GetPropertyIndex(key.c_str());
                if (index == UINT32_MAX)
                    continue;
            }

            // Set the property value based on its variant type.
            std::visit([effect, index](auto&& v)
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    effect->SetValue(index, static_cast<BOOL>(v));
                }
                else if constexpr (std::is_same_v<T, std::wstring>)
                {
                    // String properties are rare in D2D effects; skip.
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                {
                    D2D1_VECTOR_2F vec{ v.x, v.y };
                    effect->SetValue(index, vec);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                {
                    D2D1_VECTOR_3F vec{ v.x, v.y, v.z };
                    effect->SetValue(index, vec);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                {
                    D2D1_VECTOR_4F vec{ v.x, v.y, v.z, v.w };
                    effect->SetValue(index, vec);
                }
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                {
                    effect->SetValue(index, D2D1_PROPERTY_TYPE_MATRIX_5X4,
                        reinterpret_cast<const BYTE*>(&v), sizeof(v));
                }
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                {
                    effect->SetValue(index,
                        reinterpret_cast<const BYTE*>(v.data()),
                        static_cast<UINT32>(v.size() * sizeof(float)));
                }
            }, value);
        }
    }

    // -----------------------------------------------------------------------
    // Input wiring
    // -----------------------------------------------------------------------

    void GraphEvaluator::WireInputs(
        ID2D1Effect* effect,
        const EffectNode& destNode,
        const EffectGraph& graph)
    {
        if (!effect)
            return;

        auto inputEdges = graph.GetInputEdges(destNode.id);

        // Track which pins are connected so we can clear stale ones.
        UINT32 totalInputs = effect->GetInputCount();
        std::vector<bool> connected(totalInputs, false);

        for (const auto* edge : inputEdges)
        {
            if (edge->destPin >= totalInputs)
                continue;

            const EffectNode* srcNode = graph.FindNode(edge->sourceNodeId);
            if (srcNode && srcNode->cachedOutput)
            {
                effect->SetInput(edge->destPin, srcNode->cachedOutput);
                connected[edge->destPin] = true;
            }
        }

        // Clear any inputs that are no longer connected.
        for (UINT32 i = 0; i < totalInputs; ++i)
        {
            if (!connected[i])
                effect->SetInput(i, nullptr);
        }
    }

    // -----------------------------------------------------------------------
    // Property binding resolution
    // -----------------------------------------------------------------------

    // Helper: resolve a single ComponentSource to a float value.
    static bool ResolveComponentSource(
        const ComponentSource& src,
        const EffectGraph& graph,
        float& outValue)
    {
        const EffectNode* srcNode = graph.FindNode(src.sourceNodeId);
        if (!srcNode || srcNode->analysisOutput.type != AnalysisOutputType::Typed)
            return false;

        for (const auto& fv : srcNode->analysisOutput.fields)
        {
            if (fv.name != src.sourceFieldName) continue;

            if (AnalysisFieldIsArray(fv.type))
            {
                uint32_t stride = AnalysisFieldComponentCount(fv.type);
                uint32_t flatIdx = src.sourceIndex * stride + (std::min)(src.sourceComponent, stride - 1);
                if (flatIdx < fv.arrayData.size())
                {
                    outValue = fv.arrayData[flatIdx];
                    return true;
                }
            }
            else
            {
                uint32_t comp = (std::min)(src.sourceComponent, 3u);
                outValue = fv.components[comp];
                return true;
            }
            return false;
        }
        return false;
    }

    bool GraphEvaluator::ResolveBindings(
        EffectNode& node,
        const EffectGraph& graph,
        std::map<std::wstring, PropertyValue>& effectiveProps)
    {
        // Start with authored properties.
        effectiveProps = node.properties;

        if (node.propertyBindings.empty())
            return false;

        bool anyChanged = false;

        for (const auto& [propName, binding] : node.propertyBindings)
        {
            auto propIt = effectiveProps.find(propName);
            if (propIt == effectiveProps.end()) continue;

            // Phase 8: detect GPU-routable bindings before doing CPU
            // readback. A binding is GPU-routable when:
            //   * the feature flag is on,
            //   * the consumer's parameter is flagged gpuBindable in
            //     its CustomEffectDefinition,
            //   * the upstream effect publishes IEngineComputeOutput.
            //
            // Discovery uses m_bridgeImplCache directly: D2D's outer
            // ID2D1Effect's QI does not delegate arbitrary IIDs to the
            // inner impl, so a raw QI on the cached effect would always
            // fail for IEngineComputeOutput. The bridge impl pointer
            // we captured during CreateOrGetEffect (commit 2f69acd)
            // is the canonical "is this upstream a GPU-output producer"
            // signal -- the bridge always implements IEngineComputeOutput
            // by delegation to its internal D3D11ComputeRunner.
            //
            // The actual SRV-to-t-slot routing happens in DispatchViaBridge
            // for D3D11 compute consumers; this branch only bumps a
            // detection telemetry counter so we can see how many
            // bindings *could* be routed regardless of consumer kind.
            if (Performance::IsGpuBindingsEnabled() &&
                node.customEffect.has_value() &&
                !binding.wholeArray)
            {
                bool propGpuBindable = false;
                for (const auto& p : node.customEffect->parameters)
                {
                    if (p.name == propName) { propGpuBindable = p.gpuBindable; break; }
                }
                if (propGpuBindable && !binding.sources.empty() &&
                    binding.sources[0].has_value())
                {
                    uint32_t srcId = binding.sources[0]->sourceNodeId;
                    auto bridgeIt = m_bridgeImplCache.find(srcId);
                    if (bridgeIt != m_bridgeImplCache.end() && bridgeIt->second)
                    {
                        Performance::IncrementGpuBindingDetection();
                    }
                }
            }

            // Whole-array mode.
            if (binding.wholeArray)
            {
                const EffectNode* srcNode = graph.FindNode(binding.wholeArraySourceNodeId);
                if (!srcNode || srcNode->analysisOutput.type != AnalysisOutputType::Typed)
                    continue;
                for (const auto& fv : srcNode->analysisOutput.fields)
                {
                    if (fv.name == binding.wholeArraySourceFieldName && AnalysisFieldIsArray(fv.type))
                    {
                        effectiveProps[propName] = fv.arrayData;
                        anyChanged = true;
                        break;
                    }
                }
                continue;
            }

            // Per-component mode.
            if (binding.sources.empty()) continue;

            PropertyValue newVal;
            bool resolved = false;

            std::visit([&](auto&& existing)
            {
                using T = std::decay_t<decltype(existing)>;

                if constexpr (std::is_same_v<T, float>)
                {
                    if (!binding.sources.empty() && binding.sources[0].has_value())
                    {
                        float v = 0;
                        if (ResolveComponentSource(*binding.sources[0], graph, v))
                        { newVal = v; resolved = true; }
                    }
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    if (!binding.sources.empty() && binding.sources[0].has_value())
                    {
                        float v = 0;
                        if (ResolveComponentSource(*binding.sources[0], graph, v))
                        { newVal = static_cast<int32_t>(v); resolved = true; }
                    }
                }
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    if (!binding.sources.empty() && binding.sources[0].has_value())
                    {
                        float v = 0;
                        if (ResolveComponentSource(*binding.sources[0], graph, v))
                        { newVal = static_cast<uint32_t>(v); resolved = true; }
                    }
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                {
                    auto result = existing; // start from authored
                    bool any = false;
                    for (uint32_t c = 0; c < 2 && c < binding.sources.size(); ++c)
                    {
                        if (binding.sources[c].has_value())
                        {
                            float v = 0;
                            if (ResolveComponentSource(*binding.sources[c], graph, v))
                            { (&result.x)[c] = v; any = true; }
                        }
                    }
                    if (any) { newVal = result; resolved = true; }
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                {
                    auto result = existing;
                    bool any = false;
                    for (uint32_t c = 0; c < 3 && c < binding.sources.size(); ++c)
                    {
                        if (binding.sources[c].has_value())
                        {
                            float v = 0;
                            if (ResolveComponentSource(*binding.sources[c], graph, v))
                            { (&result.x)[c] = v; any = true; }
                        }
                    }
                    if (any) { newVal = result; resolved = true; }
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                {
                    auto result = existing;
                    bool any = false;
                    for (uint32_t c = 0; c < 4 && c < binding.sources.size(); ++c)
                    {
                        if (binding.sources[c].has_value())
                        {
                            float v = 0;
                            if (ResolveComponentSource(*binding.sources[c], graph, v))
                            { (&result.x)[c] = v; any = true; }
                        }
                    }
                    if (any) { newVal = result; resolved = true; }
                }
            }, propIt->second);

            if (resolved)
            {
                effectiveProps[propName] = newVal;
                anyChanged = true;
            }
        }

        return anyChanged;
    }

    // -----------------------------------------------------------------------
    // Custom effect application
    // -----------------------------------------------------------------------

    void GraphEvaluator::ApplyCustomEffect(
        ID2D1Effect* effect,
        EffectNode& node,
        const std::map<std::wstring, PropertyValue>& effectiveProps)
    {
        if (!node.customEffect.has_value()) return;
        auto& def = node.customEffect.value();

        auto implIt = m_customImplCache.find(node.id);
        if (implIt == m_customImplCache.end())
            return;

        // Load bytecode only if not already loaded (bytecode doesn't change
        // on property updates, only on recompile/update-in-graph).
        bool needsShaderLoad = false;
        if (node.type == NodeType::PixelShader && implIt->second.pixelImpl)
        {
            if (implIt->second.pixelImpl->NeedsShaderLoad())
            {
                implIt->second.pixelImpl->SetShaderGuid(def.shaderGuid);
                implIt->second.pixelImpl->LoadShaderBytecode(
                    def.compiledBytecode.data(),
                    static_cast<UINT32>(def.compiledBytecode.size()));
                needsShaderLoad = true;
            }
        }
        else if (node.type == NodeType::ComputeShader && implIt->second.computeImpl)
        {
            implIt->second.computeImpl->SetShaderGuid(def.shaderGuid);
            implIt->second.computeImpl->LoadShaderBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            implIt->second.computeImpl->SetThreadGroupSize(
                def.threadGroupX, def.threadGroupY, def.threadGroupZ);
        }

        // Reflect the bytecode to discover cbuffer layout.
        auto reflection = Effects::ShaderCompiler::Reflect(def.compiledBytecode);
        if (!reflection.constantBuffers.empty())
        {
            auto& cb = reflection.constantBuffers[0];
            std::vector<BYTE> cbData(cb.sizeBytes, 0);

            // Pack each property into the cbuffer at the reflected offset.
            for (const auto& var : cb.variables)
            {
                // Skip system-injected variables for compute shaders.
                // _TileOffset (int2, offset 0) is populated per-tile in
                // CalculateThreadgroups. Shaders use Source.GetDimensions()
                // for image size instead of a cbuffer variable.
                if (node.type == NodeType::ComputeShader &&
                    var.name == L"_TileOffset")
                {
                    continue;
                }

                auto propIt = effectiveProps.find(
                    std::wstring(var.name.begin(), var.name.end()));
                if (propIt == effectiveProps.end()) continue;

                std::visit([&](const auto& v)
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, float> || std::is_same_v<T, int32_t> ||
                                  std::is_same_v<T, uint32_t> || std::is_same_v<T, bool>)
                    {
                        if (var.offset + sizeof(T) <= cbData.size())
                            memcpy(cbData.data() + var.offset, &v, sizeof(T));
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                    {
                        if (var.offset + sizeof(float) * 2 <= cbData.size())
                            memcpy(cbData.data() + var.offset, &v, sizeof(float) * 2);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                    {
                        if (var.offset + sizeof(float) * 3 <= cbData.size())
                            memcpy(cbData.data() + var.offset, &v, sizeof(float) * 3);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                    {
                        if (var.offset + sizeof(float) * 4 <= cbData.size())
                            memcpy(cbData.data() + var.offset, &v, sizeof(float) * 4);
                    }
                }, propIt->second);
            }

            // Set the packed cbuffer on the concrete impl.
            if (node.type == NodeType::PixelShader && implIt->second.pixelImpl)
            {
                implIt->second.pixelImpl->SetConstantBufferData(cbData.data(), static_cast<UINT32>(cbData.size()));

                // Fixed-output sizing.
                //   - Source effects (no inputs): always size to a Size/PatchSize
                //     param so D2D doesn't render a 0x0 or full-source-bounds
                //     output.
                //   - Effects with inputs: only fix the output size if the
                //     descriptor *explicitly* declares a `DiagramSize`/`OutputSize`/
                //     `PatchSize` parameter. Without this, an analysis-style
                //     viewer like Gamut Coverage / Waveform Monitor (whose pixel
                //     shader does an O(N) inner loop per output pixel) would
                //     render at the full source resolution -- billions of
                //     samples per frame at 4K, hanging the GPU. The shader
                //     already uses `DiagramSize` to map output coords to its
                //     visualization domain, so honoring it as the actual
                //     output size keeps D2D in sync with shader intent.
                bool hasFixedSizeParam = false;
                UINT32 outW = 512, outH = 512;
                {
                    auto pickFloat = [&](const std::wstring& key, UINT32* w, UINT32* h) -> bool {
                        auto it = effectiveProps.find(key);
                        if (it == effectiveProps.end()) return false;
                        if (auto* f = std::get_if<float>(&it->second))
                        {
                            *w = static_cast<UINT32>(*f);
                            *h = static_cast<UINT32>(*f);
                            return true;
                        }
                        return false;
                    };
                    if (pickFloat(L"DiagramSize", &outW, &outH))      hasFixedSizeParam = true;
                    else if (pickFloat(L"OutputSize", &outW, &outH))  hasFixedSizeParam = true;
                    else
                    {
                        auto patchIt = effectiveProps.find(L"PatchSize");
                        if (patchIt != effectiveProps.end())
                        {
                            if (auto* f = std::get_if<float>(&patchIt->second))
                            {
                                outW = static_cast<UINT32>(*f * 6);
                                outH = static_cast<UINT32>(*f * 4);
                                hasFixedSizeParam = true;
                            }
                        }
                    }
                    if (!hasFixedSizeParam)
                    {
                        // Generic fallback for source effects: any param whose
                        // name contains "Size" / "size".
                        if (node.customEffect->inputNames.empty())
                        {
                            for (const auto& [key, val] : effectiveProps)
                            {
                                if (key.find(L"Size") != std::wstring::npos ||
                                    key.find(L"size") != std::wstring::npos)
                                {
                                    if (auto* f = std::get_if<float>(&val))
                                    {
                                        outW = outH = static_cast<UINT32>(*f);
                                        hasFixedSizeParam = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                const bool applyFixedSize =
                    hasFixedSizeParam ||
                    node.customEffect->inputNames.empty();
                if (applyFixedSize)
                    implIt->second.pixelImpl->SetFixedOutputSize(outW, outH);
            }
            else if (node.type == NodeType::ComputeShader && implIt->second.computeImpl)
                implIt->second.computeImpl->SetConstantBufferData(cbData.data(), static_cast<UINT32>(cbData.size()));
        }
    }

    // -----------------------------------------------------------------------
    // Dummy source bitmap for zero-input source effects
    // -----------------------------------------------------------------------

    void GraphEvaluator::EnsureDummySourceBitmap(ID2D1DeviceContext5* dc)
    {
        if (m_dummySourceBitmap) return;

        // Create a bitmap sized to the max source effect output (512x512 default).
        // The actual output rect comes from MapInputRectsToOutputRect which uses
        // SetFixedOutputSize, but D2D needs the input bitmap to be at least as large.
        D2D1_BITMAP_PROPERTIES1 props = {};
        props.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        dc->CreateBitmap(D2D1::SizeU(2048, 2048), nullptr, 0, props, m_dummySourceBitmap.put());
    }

    // -----------------------------------------------------------------------
    // Analysis effect readback
    // -----------------------------------------------------------------------

    void GraphEvaluator::ReadHistogramOutput(
        ID2D1DeviceContext5* dc,
        ID2D1Effect* effect,
        EffectNode& node)
    {
        if (!node.cachedOutput) return;

        // Force D2D to compute the histogram by drawing the effect output.
        // The histogram processes the entire input, so we need a target large
        // enough and must actually draw the image (not just a 1x1 region).
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());

        // Get the input image bounds to size the temp target appropriately.
        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(node.cachedOutput, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, 4096.0f));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, 4096.0f));
        if (w == 0) w = 1;
        if (h == 0) h = 1;

        // Recreate temp target if size changed.
        if (!m_analysisTarget || m_analysisTargetW != w || m_analysisTargetH != h ||
            m_analysisTargetFormat != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            m_analysisTarget = nullptr;
            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, m_analysisTarget.put());
            if (FAILED(hr)) { dc->SetTarget(prevTarget.get()); return; }
            m_analysisTargetW = w;
            m_analysisTargetH = h;
            m_analysisTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        }

        dc->SetTarget(m_analysisTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(node.cachedOutput, D2D1::Point2F(-bounds.left, -bounds.top));
        dc->EndDraw();
        dc->SetTarget(prevTarget.get());

        // Read the channel selector for labeling.
        uint32_t channelIdx = 0;
        auto chanIt = node.properties.find(L"ChannelSelect");
        if (chanIt != node.properties.end())
        {
            if (auto* pv = std::get_if<uint32_t>(&chanIt->second))
                channelIdx = *pv;
        }
        static const std::wstring channelNames[] = { L"Red", L"Green", L"Blue", L"Alpha" };

        // Query the actual output data size.
        UINT32 dataSize = effect->GetValueSize(D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT);
        if (dataSize == 0) return;

        uint32_t numBins = dataSize / sizeof(float);
        std::vector<float> histData(numBins, 0.0f);

        HRESULT hr = effect->GetValue(
            D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT,
            reinterpret_cast<BYTE*>(histData.data()),
            dataSize);

        if (SUCCEEDED(hr))
        {
            node.analysisOutput.type = AnalysisOutputType::Histogram;
            node.analysisOutput.data = std::move(histData);
            node.analysisOutput.channelIndex = channelIdx;
            node.analysisOutput.label = (channelIdx < 4)
                ? channelNames[channelIdx] + L" Histogram"
                : L"Histogram";
        }
        else
        {
            node.analysisOutput.type = AnalysisOutputType::None;
            node.analysisOutput.data.clear();
        }
    }

    // -----------------------------------------------------------------------
    // Custom analysis readback
    // -----------------------------------------------------------------------

    void GraphEvaluator::ReadCustomAnalysisOutput(
        ID2D1DeviceContext5* dc,
        EffectNode& node)
    {
        if (!node.cachedOutput || !node.customEffect.has_value()) return;
        auto& def = node.customEffect.value();
        if (def.analysisFields.empty()) return;

        uint32_t totalPixels = def.totalAnalysisPixels();
        if (totalPixels == 0) return;

        // Force D2D to evaluate the compute effect by drawing its output.
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(node.cachedOutput, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, 4096.0f));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, 4096.0f));
        if (w == 0) w = 1;
        if (h == 0) h = 1;

        if (!m_analysisTarget || m_analysisTargetW != w || m_analysisTargetH != h ||
            m_analysisTargetFormat != DXGI_FORMAT_R32G32B32A32_FLOAT)
        {
            m_analysisTarget = nullptr;
            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, m_analysisTarget.put());
            if (FAILED(hr)) { dc->SetTarget(prevTarget.get()); return; }
            m_analysisTargetW = w;
            m_analysisTargetH = h;
            m_analysisTargetFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
        }

        dc->SetTarget(m_analysisTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(node.cachedOutput, D2D1::Point2F(-bounds.left, -bounds.top));
        HRESULT drawHr = dc->EndDraw();
        dc->SetTarget(prevTarget.get());
        if (FAILED(drawHr)) return;

        // Create a CPU-readable bitmap to read back analysis pixels.
        winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
        D2D1_BITMAP_PROPERTIES1 cpuProps = {};
        cpuProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        cpuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        uint32_t readW = (std::max)(totalPixels, 1u);
        HRESULT hr = dc->CreateBitmap(D2D1::SizeU(readW, 1), nullptr, 0, cpuProps, cpuBitmap.put());
        if (FAILED(hr)) return;

        D2D1_POINT_2U dest = { 0, 0 };
        D2D1_RECT_U srcRect = { 0, 0, readW, 1 };
        hr = cpuBitmap->CopyFromBitmap(&dest, m_analysisTarget.get(), &srcRect);
        if (FAILED(hr)) return;

        D2D1_MAPPED_RECT mapped{};
        hr = cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) return;

        // Unpack typed fields from the pixel row.
        node.analysisOutput.type = AnalysisOutputType::Typed;
        node.analysisOutput.fields.clear();

        const float* pixels = reinterpret_cast<const float*>(mapped.bits);
        uint32_t pixelOffset = 0;

        for (const auto& fieldDesc : def.analysisFields)
        {
            AnalysisFieldValue fv;
            fv.name = fieldDesc.name;
            fv.type = fieldDesc.type;
            uint32_t pc = fieldDesc.pixelCount();

            if (!AnalysisFieldIsArray(fieldDesc.type))
            {
                // Scalar: read one pixel's worth of components.
                uint32_t cc = AnalysisFieldComponentCount(fieldDesc.type);
                for (uint32_t c = 0; c < cc && c < 4; ++c)
                    fv.components[c] = pixels[(pixelOffset) * 4 + c];
            }
            else if (fieldDesc.type == AnalysisFieldType::FloatArray)
            {
                // FloatArray: 4 floats packed per pixel.
                fv.arrayData.resize(fieldDesc.arrayLength, 0.0f);
                for (uint32_t i = 0; i < fieldDesc.arrayLength; ++i)
                {
                    uint32_t pix = pixelOffset + i / 4;
                    uint32_t comp = i % 4;
                    fv.arrayData[i] = pixels[pix * 4 + comp];
                }
            }
            else
            {
                // Float2Array, Float3Array, Float4Array: 1 pixel per element.
                uint32_t cc = AnalysisFieldComponentCount(fieldDesc.type);
                fv.arrayData.resize(fieldDesc.arrayLength * cc, 0.0f);
                for (uint32_t i = 0; i < fieldDesc.arrayLength; ++i)
                {
                    for (uint32_t c = 0; c < cc; ++c)
                        fv.arrayData[i * cc + c] = pixels[(pixelOffset + i) * 4 + c];
                }
            }

            pixelOffset += pc;
            node.analysisOutput.fields.push_back(std::move(fv));
        }

        cpuBitmap->Unmap();
    }


    // -----------------------------------------------------------------------
    // Pre-render upstream D2D chain to FP32 bitmap at 96 DPI
    // -----------------------------------------------------------------------

    winrt::com_ptr<ID2D1Bitmap1> GraphEvaluator::PreRenderInputBitmap(
        ID2D1DeviceContext5* dc, ID2D1Image* inputImage)
    {
        if (!dc || !inputImage) return nullptr;

        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(inputImage, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, 8192.0f));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, 8192.0f));
        if (w == 0 || h == 0) { dc->SetDpi(oldDpiX, oldDpiY); return nullptr; }

        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, bitmap.put());
        if (FAILED(hr)) { dc->SetDpi(oldDpiX, oldDpiY); return nullptr; }

        // Render inside a standalone BeginDraw/EndDraw pair so the D2D
        // effect chain is fully evaluated with current property values.
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());
        dc->SetTarget(bitmap.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(inputImage, D2D1::Point2F(-bounds.left, -bounds.top));
        dc->EndDraw();
        dc->SetTarget(prevTarget.get());

        dc->SetDpi(oldDpiX, oldDpiY);
        return bitmap;
    }

}

