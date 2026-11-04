#pragma once

#include <string>
#include <vector>

namespace ivevsg
{
    /// Viewer-wide rules for when layers appear.
    struct VisibilitySettings
    {
        double globalMinZoom = 0.0;       // no layer appears while the view is zoomed out past this level
        bool drape = true;                // vector features follow the terrain ...
        double drapeMinZoom = 8.0;        // ... from this zoom (tile level) on
        double resolutionTolerance = 2.0; // zoom levels past its declared scale a layer still counts as resolving
    };

    /// What decides one layer's visibility (taken from its LayerOptions and the user's toggle).
    struct LayerVisibilityInput
    {
        bool userVisible = true;
        double minZoom = 0.0;
        double maxZoom = 30.0;
        double scaleDenominator = 0.0; // 0 = no declared resolution
        std::string lodGroup;          // "" = stands alone
    };

    struct LayerVisibilityResult
    {
        bool shown = false;          // what the scene should draw
        bool inZoomWindow = false;   // the zoom is within [max(minZoom, globalMinZoom), maxZoom] and the user has it on
        bool resolved = true;        // the zoom is at or below the level the declared scale resolves to (plus tolerance)
        bool replacedInGroup = false; // hidden because another member of its group is shown instead
        bool standingIn = false;     // shown past its resolution because no finer member of its group is available
    };

    /// The zoom level up to which data declared at 1:S counts as resolving (tolerance levels beyond
    /// the exact figure); effectively infinite without a declared scale.
    double resolvedUntilZoom(double scaleDenominator, double resolutionTolerance);

    /// Applies the zoom windows and the LOD groups: within a group, among the members that are on and
    /// in their window and declare a scale, the coarsest one that still resolves at `zoom` is shown; when
    /// none resolves, the finest stands in. Members without a declared scale follow their own window.
    std::vector<LayerVisibilityResult> evaluateLayerVisibility(const std::vector<LayerVisibilityInput>& layers, double zoom, const VisibilitySettings& settings);
} // namespace ivevsg
