#pragma once

#include <ivevsg/TileScheme.h>

#include <functional>
#include <string>

namespace ivevsg
{
    class ViewerApp;

    /// Everything the panel reads and controls. The app must outlive the viewer; the view, layer
    /// stack and loader are taken from it every frame, since they change when the view is switched.
    struct MapHudContext
    {
        ViewerApp* app = nullptr;
        TileScheme scheme;
        std::string title = "Map";
    };

    /// An ImGui panel: the display projection (switchable), the view (zoom, metres per pixel, OGC tile
    /// level, centre, orientation and cursor coordinates), go-to / home / max-zoom controls, the layer
    /// list with each layer's native and display CRS (toggle, frame, remove), adding a file as a layer
    /// with its native CRS chosen explicitly, paging statistics and frame rate.
    /// Register the result with ViewerApp::addUi(); it is drawn every frame by vsgImGui.
    std::function<bool()> createMapHud(MapHudContext context);
} // namespace ivevsg
