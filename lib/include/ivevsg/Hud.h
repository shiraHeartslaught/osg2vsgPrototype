#pragma once

#include <ivevsg/MapScene.h>
#include <ivevsg/Projection.h>
#include <ivevsg/TileScheme.h>

#include <functional>
#include <memory>
#include <string>

namespace ivevsg
{
    class SceneLoader;
    class ViewerApp;

    /// Everything the map panel reads and controls. All pointers must outlive the viewer.
    struct MapHudContext
    {
        ViewerApp* app = nullptr;
        std::shared_ptr<const Projection> projection;
        std::shared_ptr<LayerStack> layers;
        std::shared_ptr<const SceneLoader> loader;
        TileScheme scheme;
        std::string title = "Map";
    };

    /// An ImGui panel showing the view (zoom, metres per pixel, OGC tile level, centre and cursor
    /// coordinates), go-to / home / max-zoom controls, the layer list (toggle, frame, remove, add a
    /// file as a new layer), paging statistics and frame rate.
    /// Register the result with ViewerApp::addUi(); it is drawn every frame by vsgImGui.
    std::function<bool()> createMapHud(MapHudContext context);
} // namespace ivevsg
