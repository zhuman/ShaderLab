#include "pch_engine.h"
#include "WorkingSpaceSync.h"

#include "../Graph/EffectGraph.h"
#include "DisplayMonitor.h"

namespace ShaderLab::Rendering
{
    bool UpdateWorkingSpaceNodes(Graph::EffectGraph& graph, DisplayMonitor& monitor)
    {
        using namespace Graph;
        using winrt::Windows::Foundation::Numerics::float2;

        auto profile = monitor.ActiveProfile();
        const auto& caps = profile.caps;
        const bool isSim = monitor.IsSimulated();

        struct ScalarField { const wchar_t* name; float value; };
        const ScalarField scalars[] = {
            { L"ActiveColorMode",  static_cast<float>(caps.activeColorMode) },
            { L"HdrSupported",     caps.hdrSupported    ? 1.0f : 0.0f },
            { L"HdrUserEnabled",   caps.hdrUserEnabled  ? 1.0f : 0.0f },
            { L"WcgSupported",     caps.wcgSupported    ? 1.0f : 0.0f },
            { L"WcgUserEnabled",   caps.wcgUserEnabled  ? 1.0f : 0.0f },
            { L"IsSimulated",      isSim ? 1.0f : 0.0f },
            { L"SdrWhiteNits",     caps.sdrWhiteLevelNits },
            { L"PeakNits",         caps.maxLuminanceNits },
            { L"MinNits",          caps.minLuminanceNits },
            { L"MaxFullFrameNits", caps.maxFullFrameLuminanceNits },
        };

        struct VectorField { const wchar_t* name; float2 value; };
        const VectorField vectors[] = {
            { L"RedPrimary",   float2{ profile.primaryRed.x,   profile.primaryRed.y   } },
            { L"GreenPrimary", float2{ profile.primaryGreen.x, profile.primaryGreen.y } },
            { L"BluePrimary",  float2{ profile.primaryBlue.x,  profile.primaryBlue.y  } },
            { L"WhitePoint",   float2{ profile.whitePoint.x,   profile.whitePoint.y   } },
        };

        bool anyChanged = false;
        // const_cast: EffectGraph::Nodes() returns const&, but the
        // working-space sync must mutate node.properties + node.dirty.
        // Same const_cast the original MainWindow helper used.
        for (auto& node : const_cast<std::vector<EffectNode>&>(graph.Nodes()))
        {
            if (!node.customEffect.has_value()) continue;
            if (node.customEffect->shaderLabEffectId != L"Working Space") continue;

            bool nodeChanged = false;

            for (const auto& f : scalars)
            {
                auto it = node.properties.find(f.name);
                if (it == node.properties.end())
                {
                    node.properties[f.name] = PropertyValue{ f.value };
                    nodeChanged = true;
                    continue;
                }
                if (auto* cur = std::get_if<float>(&it->second))
                {
                    if (*cur != f.value)
                    {
                        *cur = f.value;
                        nodeChanged = true;
                    }
                }
                else
                {
                    it->second = PropertyValue{ f.value };
                    nodeChanged = true;
                }
            }

            for (const auto& f : vectors)
            {
                auto it = node.properties.find(f.name);
                if (it == node.properties.end())
                {
                    node.properties[f.name] = PropertyValue{ f.value };
                    nodeChanged = true;
                    continue;
                }
                if (auto* cur = std::get_if<float2>(&it->second))
                {
                    if (cur->x != f.value.x || cur->y != f.value.y)
                    {
                        *cur = f.value;
                        nodeChanged = true;
                    }
                }
                else
                {
                    it->second = PropertyValue{ f.value };
                    nodeChanged = true;
                }
            }

            if (nodeChanged)
            {
                node.dirty = true;
                anyChanged = true;
            }
        }

        return anyChanged;
    }
}
