// ive_poc: a web-map style viewer. A NASA Blue Marble base map on the Mercator plane, with any file
// vsgXchange can read (including VPB .ive paged terrain) placed on top of it. Drag to pan, scroll to
// zoom about the cursor, arrows pan, + / - zoom, Home or Space returns to the start view, Esc quits.
//
// Usage:
//   ive_poc [scene files ...] [--layer FILE ...] [--base-tiles <tiles/root.ive>] [--base <image>] [--no-base]
//           [--projection mercator|globe] [--source-crs EPSG:xxxx] [--trackball] [--debug|-d] [--frames N]
//           [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z] [--log-loads] [--no-ui]
//
// --projection picks the frame everything is displayed in: the flat Web Mercator map (default) or the
// WGS84 globe (EPSG:4978). On the globe, dragging spins it about its axis the way a desk globe turns
// (longitude from sideways motion, latitude from vertical, north always up), the wheel zooms about the
// cursor, middle-drag tilts and turns the view, N restores north-up. --trackball swaps either
// projection's navigation for VSG's free orbit. Every OSG file is reprojected on load from its own CRS
// (a VPB CoordinateSystemNode, else --source-crs, default EPSG:3857) into that frame, paged tiles
// included, so the same .ive databases work in either projection.
//
// Every scene file becomes a layer above the base map (the first one frames the start view); --layer
// files are added after start-up through the runtime path the panel's "Add layer" button also uses;
// loads run on worker threads (read, convert, GPU compile) so rendering is never interrupted.
// The panel lists the layers with show/hide, Frame and Remove, plus a field to add a file as a layer.
//
// A Dear ImGui panel (vsgImGui) shows the view, go-to / home / max-zoom controls, layer toggles and
// paging statistics; --no-ui turns it off.
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
#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/Hud.h>
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
#include <string>
#include <vector>

#ifndef IVEVSG_DEFAULT_TILES
#    define IVEVSG_DEFAULT_TILES ""
#endif

int main(int argc, char** argv)
{
    vsg::CommandLine arguments(&argc, argv);

    ivevsg::ViewerSettings settings;
    settings.debugLayer = arguments.read({"--debug", "-d"});
    settings.maxFrames = arguments.value(-1, "--frames");
    const std::string projectionName = arguments.value(std::string("mercator"), "--projection");
    auto projection = ivevsg::createProjection(projectionName);
    if (!projection)
    {
        std::cerr << "unknown --projection '" << projectionName << "' (use mercator or globe)\n";
        return 1;
    }
    // the globe starts straight down (north up) like the map; --elevation tilts the start view
    settings.framingElevationDegrees = arguments.value(projection->geocentric() ? 90.0 : 45.0, "--elevation");
    const std::string sourceCrs = arguments.value(std::string("EPSG:3857"), "--source-crs");
    // flat map navigation on a flat projection, desk-globe navigation on the globe; --trackball
    // swaps either for VSG's free orbit
    if (arguments.read("--trackball"))
        settings.navigation = ivevsg::NavigationMode::Trackball;
    else
        settings.navigation = projection->geocentric() ? ivevsg::NavigationMode::Globe : ivevsg::NavigationMode::Map;
    settings.ui = !arguments.read("--no-ui");

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
    std::vector<std::string> runtimeLayers;
    for (std::string path; arguments.read("--layer", path);) runtimeLayers.push_back(path);
    const bool logLoads = arguments.read("--log-loads"); // all options must be consumed before the positional scene files

    // default base: the tile pyramid baked in at configure time, when present and nothing else was asked for
    bool defaultTiles = false;
    if (baseTiles.empty() && !haveBaseImage && !noBaseMap && std::filesystem::exists(IVEVSG_DEFAULT_TILES))
    {
        baseTiles = IVEVSG_DEFAULT_TILES;
        defaultTiles = true;
    }

    std::vector<std::string> filenames;
    for (int i = 1; i < argc; ++i) filenames.emplace_back(argv[i]);
    if (filenames.empty() && runtimeLayers.empty() && baseTiles.empty() && noBaseMap)
    {
        std::cerr << "usage: " << argv[0] << " [scene files ...] [--layer FILE ...] [--base-tiles <root.ive>] [--base <image>] [--no-base] [--trackball] [--debug] [--frames N] [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z] [--log-loads] [--no-ui]\n";
        return 1;
    }

    ivevsg::LoaderSettings loaderSettings = ivevsg::SceneLoader::defaultSettings();
    loaderSettings.logReads = logLoads;
    loaderSettings.targetCrs = projection->crs();
    loaderSettings.defaultSourceCrs = sourceCrs;
    loaderSettings.quadTessellation = projection->geocentric() ? 8 : 1; // bend single-quad tiles over the globe
    auto loader = std::make_shared<ivevsg::SceneLoader>(loaderSettings);
    std::cout << "vsgXchange OSG support: " << (ivevsg::SceneLoader::supportsOsg() ? "ON" : "OFF")
              << "  (OSG plugins: " << loader->settings().osgPluginDirectory << ")\n";

    // every scene file is a layer above the base; the first one frames the start view
    auto layers = std::make_shared<ivevsg::LayerStack>();
    vsg::ref_ptr<vsg::Node> data;
    for (const std::string& filename : filenames)
    {
        const ivevsg::LoadResult loaded = loader->load(filename);
        if (!loaded)
        {
            std::cerr << "Failed to load " << filename << ": " << loaded.error << "\n";
            return 1;
        }
        layers->addLayer(vsg::simpleFilename(vsg::Path(filename)).string(), loaded.node);
        if (!data) data = loaded.node;
        std::cout << "Layer: " << filename << "\n";
    }

    std::cout << "Projection: " << projection->name() << " (data reprojected from " << sourceCrs << " where no CRS is recorded)\n";

    // Base layer: a paged tile pyramid when given, else the single Blue Marble image. It hangs under an
    // "underlay" transform that the viewer keeps a small, altitude-scaled distance below the data plane
    // so the data always draws on top without z-fighting and without disturbing the tiles' LOD choice.
    vsg::ref_ptr<vsg::MatrixTransform> underlay;
    if (!baseTiles.empty())
    {
        const ivevsg::LoadResult tiles = loader->load(baseTiles);
        if (!tiles)
        {
            std::cerr << "Failed to load tile set " << baseTiles << ": " << tiles.error << "\n";
            return 1;
        }
        underlay = vsg::MatrixTransform::create();
        underlay->addChild(tiles.node);
        std::cout << "Base map: tile pyramid " << baseTiles << (defaultTiles ? " (default)" : "") << "\n";
    }
    else if (!noBaseMap && projection->geocentric())
    {
        std::cout << "Base map: none (the single-image base is a flat quad; use a tile pyramid on the globe)\n";
    }
    else if (!noBaseMap)
    {
        const ivevsg::BaseMapResult baseMap = ivevsg::createBaseMap(baseMapSettings, loader->options());
        std::cout << "Base map: " << baseMap.message << "\n";
        underlay = vsg::MatrixTransform::create();
        underlay->addChild(baseMap.node);
    }
    vsg::ref_ptr<vsg::Node> base = underlay;

    // Scene = base underneath, data on top, each switchable from the panel. The initial view frames
    // the data when there is some.
    if (base) layers->setBase(base);
    vsg::ref_ptr<vsg::Node> world = layers->node();
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
    app.setLayers(layers);
    app.setLoader(loader);
    app.onLayerLoaded = [](const std::string& name, bool ok, const std::string& error, double seconds) {
        if (ok)
            std::cout << "Layer loaded: " << name << " (" << seconds << " s on a worker thread)\n";
        else
            std::cerr << "Layer failed: " << name << ": " << error << "\n";
    };
    if (settings.ui)
    {
        ivevsg::MapHudContext hud;
        hud.app = &app;
        hud.projection = projection;
        hud.layers = layers;
        hud.loader = loader;
        app.addUi(ivevsg::createMapHud(hud));
    }
    for (const std::string& path : runtimeLayers) app.queueLayerLoad(path); // goes through the runtime path at the first frame
    if (!app.initialize(scene, focus))
    {
        std::cerr << app.error() << "\n";
        return 1;
    }

    const ivevsg::GeoExtent extent = projection->extentOf(app.sceneBounds());
    std::cout << "Data extent: lon [" << extent.minLongitude << ", " << extent.maxLongitude << "]"
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
    else if (settings.navigation == ivevsg::NavigationMode::Globe && app.globeNavigation())
    {
        auto navigation = app.globeNavigation();
        navigation->setZoomLevelRange(minZoom, maxZoom);
        if (haveCenter || startZoom >= 0.0)
        {
            ivevsg::GlobeCamera camera = navigation->globeCamera();
            if (haveCenter)
            {
                camera.longitude = centerLongitude;
                camera.latitude = centerLatitude;
            }
            if (startZoom >= 0.0)
            {
                camera.altitude = ivevsg::altitudeForZoomLevel(startZoom, navigation->settings().fieldOfViewDegrees,
                                                               navigation->viewportHeight(), navigation->settings().worldWidthMetres);
            }
            navigation->setGlobeCamera(camera);
            navigation->setHome(navigation->globeCamera());
        }
        std::cout << "Zoom range: " << navigation->minZoomLevel() << " .. " << navigation->maxZoomLevel() << "\n"
                  << "Start zoom level: " << app.zoomLevel() << "\n"
                  << "Controls: drag = spin the globe about its axis, wheel = zoom at cursor, middle drag = tilt / turn,\n"
                  << "          right drag = zoom, arrows = spin, +/- = zoom, PgUp/PgDn = tilt, N = north up, Home/Space = reset, Esc = quit\n";
    }

    return app.run();
}
