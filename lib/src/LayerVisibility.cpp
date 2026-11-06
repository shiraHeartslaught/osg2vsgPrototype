#include <ivevsg/LayerOptions.h>
#include <ivevsg/LayerVisibility.h>

#include <limits>
#include <map>

namespace ivevsg
{
    double resolvedUntilZoom(double scaleDenominator, double resolutionTolerance)
    {
        if (!(scaleDenominator > 0.0)) return std::numeric_limits<double>::infinity();
        return zoomLevelForScaleDenominator(scaleDenominator) + resolutionTolerance;
    }

    std::vector<LayerVisibilityResult> evaluateLayerVisibility(const std::vector<LayerVisibilityInput>& layers, double zoom, const VisibilitySettings& settings)
    {
        std::vector<LayerVisibilityResult> results(layers.size());
        std::map<std::string, std::vector<std::size_t>> groups;
        for (std::size_t i = 0; i < layers.size(); ++i)
        {
            const LayerVisibilityInput& in = layers[i];
            LayerVisibilityResult& out = results[i];
            const double floor = std::max(in.minZoom, settings.globalMinZoom);
            out.inZoomWindow = in.userVisible && zoom >= floor && zoom <= in.maxZoom;
            out.resolved = zoom <= resolvedUntilZoom(in.scaleDenominator, settings.resolutionTolerance);
            out.shown = out.inZoomWindow;
            if (!in.lodGroup.empty() && in.scaleDenominator > 0.0 && out.inZoomWindow) groups[in.lodGroup].push_back(i);
        }
        for (auto& [name, members] : groups)
        {
            // the coarsest member that resolves; failing that the finest member there is
            std::size_t chosen = members.front();
            bool anyResolves = false;
            for (std::size_t i : members)
            {
                const bool resolves = results[i].resolved;
                if (resolves && (!anyResolves || layers[i].scaleDenominator > layers[chosen].scaleDenominator))
                {
                    chosen = i;
                    anyResolves = true;
                }
                else if (!anyResolves && layers[i].scaleDenominator < layers[chosen].scaleDenominator)
                {
                    chosen = i;
                }
            }
            for (std::size_t i : members)
            {
                if (i == chosen)
                {
                    results[i].standingIn = !anyResolves;
                    continue;
                }
                results[i].shown = false;
                results[i].replacedInGroup = results[i].inZoomWindow;
            }
        }
        return results;
    }
} // namespace ivevsg
