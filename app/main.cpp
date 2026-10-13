// ive_poc: a web-map style viewer. A NASA Blue Marble base map on the Mercator plane, with any file
// vsgXchange can read (including VPB .ive paged terrain) placed on top of it. Drag to pan, scroll to
// zoom about the cursor, arrows pan, + / - zoom, Home or Space returns to the start view, Esc quits.
//
// Usage:
//   ive_poc [scene file] [--base-tiles <tiles/root.ive>] [--base <image>] [--no-base] [--trackball]
//           [--debug|-d] [--frames N] [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z]
//           [--log-loads]
//
// The base map is the OGC WebMercatorQuad .ive pyramid built by ivetiler (the default set baked in at
// configure time, or --base-tiles); VSG pages the tile level matching the view's metres per pixel in
// and out as you pan and zoom. If the default set is missing, or --base <image> is given, the single
// Blue Marble image is used instead. Zooming past the deepest tile level just magnifies the deepest
// tiles; --max-zoom (default 22) is the ceiling. The scene file is optional.
//
// On the Mercator projection the map repeats sideways (pan east forever, like a web map) and the camera
// stops at the map's top and bottom edges (+-85.05 degrees, the Mercator limit) instead of showing
// empty space beyond them.
//
// The heavy lifting lives in the ivevsg library (lib/), which is what the Google Test suite exercises.

#include <ivevsg/BaseMap.h>
#include <ivevsg/MapScene.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TileScheme.h>
#include <ivevsg/ViewerApp.h>

#include <vsg/all.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>

#ifndef IVEVSG_DEFAULT_TILES
#    define IVEVSG_DEFAULT_TILES ""
#endif

int main(int argc, char** argv)
{
    vsg::CommandLine arguments(&argc, argv);

    ivevsg::ViewerSettings settings;
    settings.debugLayer = arguments.read({"--debug", "-d"});
    settings.maxFrames = arguments.value(-1, "--frames");
    settings.framingElevationDegrees = arguments.value(45.0, "--elevation");
    settings.navigation = arguments.read("--trackball") ? ivevsg::NavigationMode::Trackball : ivevsg::NavigationMode::Map;

    ivevsg::BaseMapSettings baseMapSettings;
    const bool noBaseMap = arguments.read("--no-base");
    std::string baseImage;
    const bool haveBaseImage = arguments.read("--base", baseImage);
    if (haveBaseImage) baseMapSettings.imagePath = baseImage;

    const double startZoom = arguments.value(-1.0, "--zoom");
    const double minZoom = arguments.value(0.0, "--min-zoom");
    const double maxZoom = arguments.value(22.0, "--max-zoom");
    double centerLongitude = 0.0, centerLatitude = 0.0;
    const bool haveCenter = arguments.read("--center", centerLongitude, centerLatitude);
    std::string baseTiles = arguments.value(std::string(), "--base-tiles");
    const bool logLoads = arguments.read("--log-loads"); // all options must be consumed before the positional scene file

    // default base: the tile pyramid baked in at configure time, when present and nothing else was asked for
    bool defaultTiles = false;
    if (baseTiles.empty() && !haveBaseImage && !noBaseMap && std::filesystem::exists(IVEVSG_DEFAULT_TILES))
    {
        baseTiles = IVEVSG_DEFAULT_TILES;
        defaultTiles = true;
    }

    const std::string filename = (argc >= 2) ? argv[1] : std::string();
    if (filename.empty() && baseTiles.empty() && noBaseMap)
    {
        std::cerr << "usage: " << argv[0] << " [scene file (.vsgb/.vsgt/.ive/...)] [--base-tiles <root.ive>] [--base <image>] [--no-base] [--trackball] [--debug] [--frames N] [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z] [--log-loads]\n";
        return 1;
    }

    ivevsg::LoaderSettings loaderSettings = ivevsg::SceneLoader::defaultSettings();
    loaderSettings.logReads = logLoads;
    ivevsg::SceneLoader loader(loaderSettings);
    std::cout << "vsgXchange OSG support: " << (ivevsg::SceneLoader::supportsOsg() ? "ON" : "OFF")
              << "  (OSG plugins: " << loader.settings().osgPluginDirectory << ")\n";

    vsg::ref_ptr<vsg::Node> data;
    if (!filename.empty())
    {
        const ivevsg::LoadResult loaded = loader.load(filename);
        if (!loaded)
        {
            std::cerr << "Failed to load " << filename << ": " << loaded.error << "\n";
            return 1;
        }
        data = loaded.node;
    }

    auto projection = std::make_shared<ivevsg::MercatorProjection>();

    // Base layer: a paged tile pyramid when given, else the single Blue Marble image. It hangs under an
    // "underlay" transform that the viewer keeps a small, altitude-scaled distance below the data plane
    // so the data always draws on top without z-fighting and without disturbing the tiles' LOD choice.
    vsg::ref_ptr<vsg::MatrixTransform> underlay;
    if (!baseTiles.empty())
    {
        const ivevsg::LoadResult tiles = loader.load(baseTiles);
        if (!tiles)
        {
            std::cerr << "Failed to load tile set " << baseTiles << ": " << tiles.error << "\n";
            return 1;
        }
        underlay = vsg::MatrixTransform::create();
        underlay->addChild(tiles.node);
        std::cout << "Base map: tile pyramid " << baseTiles << (defaultTiles ? " (default)" : "") << "\n";
    }
    else if (!noBaseMap)
    {
        const ivevsg::BaseMapResult baseMap = ivevsg::createBaseMap(baseMapSettings, loader.options());
        std::cout << "Base map: " << baseMap.message << "\n";
        underlay = vsg::MatrixTransform::create();
        underlay->addChild(baseMap.node);
    }
    vsg::ref_ptr<vsg::Node> base = underlay;

    // Scene = base underneath, data on top. The initial view frames the data when there is some.
    vsg::ref_ptr<vsg::Node> world = data;
    if (base) world = ivevsg::createLayeredScene(base, data);
    vsg::ref_ptr<vsg::Node> focus = data ? data : base;

    // Projections that repeat sideways get the world drawn again on each side so the screen is always
    // covered while the camera's x wraps. Enough copies for the window's aspect ratio.
    vsg::ref_ptr<vsg::Node> scene = world;
    if (settings.navigation == ivevsg::NavigationMode::Map && projection->wrapWidth() > 0.0)
    {
        const double aspect = static_cast<double>(settings.width) / static_cast<double>(settings.height);
        const int copiesEachSide = std::max(1, static_cast<int>(std::ceil(aspect / 2.0)));
        scene = ivevsg::createWrappedScene(world, projection->wrapWidth(), copiesEachSide);
        std::cout << "Map repeats horizontally (" << copiesEachSide << " extra copies each side)\n";
    }

    ivevsg::ViewerApp app(settings, projection);
    // Report the view as it changes: zoom, metres per pixel and the OGC tile level that resolution calls for.
    ivevsg::TileScheme scheme;
    int lastReportedQuarterLevel = -1000;
    app.onMapViewChanged = [&](const ivevsg::MapCamera& camera, double zoomLevel) {
        auto navigation = app.mapNavigation();
        const double mpp = ivevsg::metersPerPixel(camera.altitude, navigation->settings().fieldOfViewDegrees, navigation->viewportHeight());
        const double tileLevel = scheme.levelForMetersPerPixel(mpp);
        const int quarterLevel = static_cast<int>(std::floor(tileLevel * 4.0));
        if (quarterLevel == lastReportedQuarterLevel) return;
        lastReportedQuarterLevel = quarterLevel;
        const ivevsg::GeoPoint centre = projection->worldToGeo(vsg::dvec3(camera.centre.x, camera.centre.y, 0.0));
        std::cout << "view: zoom " << zoomLevel << "  " << mpp << " m/px  -> tile level " << tileLevel
                  << " (" << scheme.metersPerPixel(std::max(0, static_cast<int>(std::round(tileLevel)))) << " m/px)"
                  << "  centre lon " << centre.longitude << " lat " << centre.latitude << "\n";
    };
    if (underlay) app.setUnderlay(underlay);
    if (!app.initialize(scene, focus))
    {
        std::cerr << app.error() << "\n";
        return 1;
    }

    const ivevsg::GeoExtent extent = projection->extentOf(app.sceneBounds());
    std::cout << "Projection: " << projection->name() << "\n"
              << "Data extent: lon [" << extent.minLongitude << ", " << extent.maxLongitude << "]"
              << " lat [" << extent.minLatitude << ", " << extent.maxLatitude << "]\n";
    if (settings.navigation == ivevsg::NavigationMode::Map)
    {
        auto navigation = app.mapNavigation();
        navigation->setZoomLevelRange(minZoom, maxZoom);
        if (startZoom > maxZoom) std::cout << "Note: --zoom " << startZoom << " is above --max-zoom " << maxZoom << ", clamping\n";
        if (haveCenter || startZoom >= 0.0)
        {
            ivevsg::MapCamera camera = navigation->mapCamera();
            if (haveCenter)
            {
                const vsg::dvec3 world = projection->geoToWorld(ivevsg::GeoPoint{centerLongitude, centerLatitude, 0.0});
                camera.centre = {world.x, world.y};
            }
            if (startZoom >= 0.0)
            {
                camera.altitude = ivevsg::altitudeForZoomLevel(startZoom, navigation->settings().fieldOfViewDegrees,
                                                               navigation->viewportHeight(), navigation->settings().worldWidthMetres);
            }
            navigation->setMapCamera(camera);
            navigation->setHome(navigation->mapCamera());
        }
        std::cout << "Zoom range: " << navigation->minZoomLevel() << " .. " << navigation->maxZoomLevel()
                  << " (past the deepest tiles the deepest tiles are simply magnified)\n"
                  << "Start zoom level: " << app.zoomLevel() << "\n"
                  << "Controls: drag = pan, wheel = zoom at cursor, arrows = pan, +/- = zoom, Home/Space = reset, Esc = quit\n";
    }

    return app.run();
}
