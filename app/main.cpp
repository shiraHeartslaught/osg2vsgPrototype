// ive_poc: a web-map style viewer. A NASA Blue Marble base map with any file vsgXchange can read
// (including VPB .ive paged databases) placed on top of it as layers, shown on the flat Web Mercator
// map or on the WGS84 globe, switchable at runtime from the panel.
//
// Usage:
//   ive_poc [scene files ...] [--layer FILE ...] [--layer-crs EPSG:xxxx] [--base-tiles <tiles/root.ive>]
//           [--globe-tiles <tiles/root.ive>] [--base <image>] [--no-base]
//           [--projection mercator|globe] [--source-crs EPSG:xxxx] [--trackball] [--debug|-d] [--frames N]
//           [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z] [--elevation DEG]
//           [--switch-at FRAME mercator|globe] [--log-loads] [--no-ui]
//
// Display: --projection picks the start view. Each view is a WorldView (lib/): the coordinate maths,
// the navigation that suits it and its scene assembly rules live together, so the viewer only ever
// deals with one of them and the panel's "Display projection" chooser swaps them at runtime, keeping
// the camera over the same ground. --trackball uses VSG's free orbit on the chosen projection instead.
//
// Base maps: one per view. Mercator uses the Web Mercator tile pyramid (--base-tiles, default baked in
// at configure time) or a single image (--base); the globe uses a geocentric pyramid (--globe-tiles,
// default baked in) built with `ivetiler --geocentric`. --no-base shows none.
//
// Layers: every scene file and --layer file becomes a layer above the base (the first scene file
// frames the start view). A layer is reprojected on load from its native CRS into the display frame,
// paged tiles included: the native CRS is what the file declares (a CoordinateSystemNode: with an
// ellipsoid model = ECEF), else --layer-crs for --layer files, else --source-crs (default EPSG:3857).
// The panel shows native and display CRS per layer and lets the native CRS be chosen when adding one.
//
// --switch-at FRAME VIEW switches the display projection after FRAME frames (demos, screenshots).
//
// The heavy lifting lives in the ivevsg library (lib/), which is what the Google Test suite exercises.

#include <ivevsg/CrsTransform.h>
#include <ivevsg/Hud.h>
#include <ivevsg/MapNavigation.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TileScheme.h>
#include <ivevsg/ViewerApp.h>
#include <ivevsg/WorldView.h>

#include <vsg/all.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef IVEVSG_DEFAULT_TILES
#    define IVEVSG_DEFAULT_TILES ""
#endif
#ifndef IVEVSG_DEFAULT_GLOBE_TILES
#    define IVEVSG_DEFAULT_GLOBE_TILES ""
#endif

int main(int argc, char** argv)
{
    vsg::CommandLine arguments(&argc, argv);

    ivevsg::ViewerSettings settings;
    settings.debugLayer = arguments.read({"--debug", "-d"});
    settings.maxFrames = arguments.value(-1, "--frames");
    settings.ui = !arguments.read("--no-ui");
    settings.logReads = arguments.read("--log-loads");
    settings.defaultSourceCrs = arguments.value(std::string("EPSG:3857"), "--source-crs");
    const std::string projectionName = arguments.value(std::string("mercator"), "--projection");
    ivevsg::WorldViewSettings& views = settings.views;
    views.freeOrbit = arguments.read("--trackball");
    std::unique_ptr<ivevsg::WorldView> view = ivevsg::createWorldView(projectionName, views);
    if (!view)
    {
        std::cerr << "unknown --projection '" << projectionName << "' (use mercator or globe)\n";
        return 1;
    }
    settings.framingElevationDegrees = arguments.value(views.freeOrbit ? 45.0 : 90.0, "--elevation");

    const bool noBaseMap = arguments.read("--no-base");
    std::string baseImage = arguments.value(std::string(), "--base");
    std::string baseTiles = arguments.value(std::string(), "--base-tiles");
    std::string globeTiles = arguments.value(std::string(), "--globe-tiles");
    const double startZoom = arguments.value(-1.0, "--zoom");
    const double minZoom = arguments.value(0.0, "--min-zoom");
    const double maxZoom = arguments.value(22.0, "--max-zoom");
    double centerLongitude = 0.0, centerLatitude = 0.0;
    const bool haveCenter = arguments.read("--center", centerLongitude, centerLatitude);
    const std::string layerCrs = arguments.value(std::string(), "--layer-crs");
    std::vector<std::string> runtimeLayers;
    for (std::string path; arguments.read("--layer", path);) runtimeLayers.push_back(path);
    int switchFrame = -1;
    std::string switchTo;
    arguments.read("--switch-at", switchFrame, switchTo); // all options must be consumed before the positional scene files

    std::vector<std::string> filenames;
    for (int i = 1; i < argc; ++i) filenames.emplace_back(argv[i]);

    // base maps per view: the pyramids baked in at configure time unless told otherwise
    bool defaultTiles = false, defaultGlobeTiles = false;
    if (!noBaseMap && baseTiles.empty() && baseImage.empty() && std::filesystem::exists(IVEVSG_DEFAULT_TILES))
    {
        baseTiles = IVEVSG_DEFAULT_TILES;
        defaultTiles = true;
    }
    if (!noBaseMap && globeTiles.empty() && std::filesystem::exists(IVEVSG_DEFAULT_GLOBE_TILES))
    {
        globeTiles = IVEVSG_DEFAULT_GLOBE_TILES;
        defaultGlobeTiles = true;
    }
    if (filenames.empty() && runtimeLayers.empty() && noBaseMap)
    {
        std::cerr << "usage: " << argv[0] << " [scene files ...] [--layer FILE ...] [--layer-crs EPSG:xxxx] [--base-tiles <root.ive>] [--globe-tiles <root.ive>] [--base <image>] [--no-base]\n"
                  << "       [--projection mercator|globe] [--source-crs EPSG:xxxx] [--trackball] [--debug] [--frames N] [--zoom Z] [--center LON LAT]\n"
                  << "       [--max-zoom Z] [--min-zoom Z] [--elevation DEG] [--switch-at FRAME VIEW] [--log-loads] [--no-ui]\n";
        return 1;
    }

    ivevsg::ViewerApp app(settings, std::move(view));
    if (!noBaseMap)
    {
        ivevsg::BaseSource mercatorBase;
        mercatorBase.tiles = baseTiles;
        mercatorBase.image = baseTiles.empty() ? baseImage : std::string();
        app.setBaseSource("mercator", mercatorBase);
        ivevsg::BaseSource globeBase;
        globeBase.tiles = globeTiles;
        app.setBaseSource("globe", globeBase);
    }
    for (const std::string& filename : filenames) app.addLayer(ivevsg::LayerSource{filename, "", ""});
    for (const std::string& path : runtimeLayers) app.queueLayerLoad(ivevsg::LayerSource{path, "", layerCrs}); // worker-thread path, first frame

    std::cout << "vsgXchange OSG support: " << (ivevsg::SceneLoader::supportsOsg() ? "ON" : "OFF")
              << "  (OSG plugins: " << ivevsg::SceneLoader::defaultOsgPluginDirectory() << ")\n"
              << "Display: " << app.view().name() << "\n"
              << "Base map (mercator): " << (baseTiles.empty() ? (baseImage.empty() ? "none" : "image " + baseImage) : "tile pyramid " + baseTiles) << (defaultTiles ? " (default)" : "") << "\n"
              << "Base map (globe):    " << (globeTiles.empty() ? "none" : "tile pyramid " + globeTiles) << (defaultGlobeTiles ? " (default)" : "") << "\n"
              << "Files naming no CRS are taken as " << settings.defaultSourceCrs << "\n";

    // Report the view as it changes: zoom, metres per pixel and the OGC tile level that resolution calls for.
    ivevsg::TileScheme scheme;
    int lastReportedQuarterLevel = -1000;
    app.onViewChanged = [&](const ivevsg::ViewState& state) {
        const double tileLevel = scheme.levelForMetersPerPixel(std::max(state.metersPerPixel, 1e-9));
        const int quarterLevel = static_cast<int>(std::floor(tileLevel * 4.0));
        if (quarterLevel == lastReportedQuarterLevel) return;
        lastReportedQuarterLevel = quarterLevel;
        std::cout << "view: zoom " << state.zoomLevel << "  " << state.metersPerPixel << " m/px  -> tile level " << tileLevel
                  << " (" << scheme.metersPerPixel(std::max(0, static_cast<int>(std::round(tileLevel)))) << " m/px)"
                  << "  centre lon " << state.centre.longitude << " lat " << state.centre.latitude << "\n";
    };
    app.onLayerLoaded = [&](const std::string& name, bool ok, const std::string& error, double seconds) {
        if (!ok)
        {
            std::cerr << "Layer failed: " << name << ": " << error << "\n";
            return;
        }
        std::cout << "Layer loaded: " << name << " (" << seconds << " s on a worker thread)";
        if (auto layers = app.layers(); layers && layers->layerCount() > 0)
        {
            const ivevsg::Layer& layer = layers->layer(layers->layerCount() - 1);
            std::cout << "  native " << (layer.sourceCrs.empty() ? std::string("n/a") : ivevsg::CrsTransform::displayName(layer.sourceCrs))
                      << (layer.targetCrs.empty() ? " shown as read" : " -> " + ivevsg::CrsTransform::displayName(layer.targetCrs));
        }
        std::cout << "\n";
    };
    app.onViewSwitched = [&](const ivevsg::WorldView& current) {
        std::cout << "Display: " << current.name() << "  base: " << app.baseInfo().kind
                  << (app.baseInfo().present ? " native " + app.baseInfo().sourceCrs + (app.baseInfo().targetCrs.empty() ? " shown as read" : " -> " + app.baseInfo().targetCrs) : "")
                  << "; reloading " << app.loadingLayers().size() << " layer(s); zoom range "
                  << current.minZoomLevel() << " .. " << current.maxZoomLevel() << "\n";
        lastReportedQuarterLevel = -1000;
    };
    if (settings.ui)
    {
        ivevsg::MapHudContext hud;
        hud.app = &app;
        app.addUi(ivevsg::createMapHud(hud));
    }
    if (!app.initialize())
    {
        std::cerr << app.error() << "\n";
        return 1;
    }

    const ivevsg::GeoExtent extent = app.projection().extentOf(app.sceneBounds());
    std::cout << "Start extent: lon [" << extent.minLongitude << ", " << extent.maxLongitude << "]"
              << " lat [" << extent.minLatitude << ", " << extent.maxLatitude << "]\n";
    app.setZoomLevelRange(minZoom, maxZoom); // kept for every view switched to
    if (startZoom > maxZoom) std::cout << "Note: --zoom " << startZoom << " is above --max-zoom " << maxZoom << ", clamping\n";
    if (haveCenter || startZoom >= 0.0)
    {
        ivevsg::ViewState start = app.viewState();
        if (haveCenter) start.centre = ivevsg::GeoPoint{centerLongitude, centerLatitude, 0.0};
        if (startZoom >= 0.0)
        {
            const double viewportHeight = static_cast<double>(app.camera()->getViewport().height);
            start.altitude = ivevsg::altitudeForZoomLevel(startZoom, settings.fieldOfViewDegrees, viewportHeight, 40075016.685578488);
        }
        app.lookAt(start.centre, start.altitude);
    }
    std::cout << "Zoom range: " << app.view().minZoomLevel() << " .. " << app.view().maxZoomLevel()
              << " (past the deepest tiles the deepest tiles are simply magnified)\n"
              << "Start zoom level: " << app.zoomLevel() << "\n";
    for (const std::string& line : app.view().controls()) std::cout << "Controls: " << line << "\n";
    std::cout << "Esc = quit\n";

    if (switchFrame < 0) return app.run();

    // scripted projection switch, for demos and screenshots
    int code = 0;
    try
    {
        while (app.frame())
        {
            if (app.frameCount() == switchFrame) app.requestViewSwitch(switchTo); // applied at the start of the next frame
        }
    }
    catch (const vsg::Exception& e)
    {
        std::cerr << "Vulkan error while rendering: " << e.message << "\n";
        code = 2;
    }
    return code;
}
