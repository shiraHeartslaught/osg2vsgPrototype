// ive_poc: a web-map style viewer. A NASA Blue Marble base map with any file vsgXchange can read
// (including VPB .ive paged databases) placed on top of it as layers, shown on the flat Web Mercator
// map or on the WGS84 globe, switchable at runtime from the panel. DTED / DEM files become terrain
// surfaces, shapefiles and other OGR files become vector layers that can drape over the terrain.
//
// Usage:
//   ive_poc [scene files ...] [--layer FILE[?opts] ...] [--terrain FILE|DIR ...] [--vector FILE ...] [--layer-crs EPSG:xxxx]
//           [--base-tiles <tiles/root.ive>] [--globe-tiles <tiles/root.ive>] [--base <image>] [--no-base]
//           [--projection mercator|globe] [--source-crs EPSG:xxxx] [--trackball] [--debug|-d] [--frames N]
//           [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z] [--globe-max-zoom Z] [--map-max-zoom Z]
//           [--global-min-zoom Z] [--no-drape] [--drape-min-zoom Z] [--resolution-tolerance N]
//           [--exaggeration F] [--terrain-interp nearest|bilinear|bicubic] [--terrain-oversample N] [--vector-max-level N]
//           [--elevation DEG] [--switch-at FRAME mercator|globe] [--script FILE] [--log-loads] [--no-ui]
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
// frames the start view). Besides OSG databases this covers GeoTIFFs and other GDAL rasters (cut into
// tiles on the fly), TMS / XYZ tile directories (or a {z}/{x}/{y}.png template), VSG native files,
// DTED / SRTM / DEM elevation (--terrain, or by extension: a paged terrain surface that keeps
// subdividing with interpolated heights past the data's resolution) and OGR vector files (--vector,
// or by extension: shapefiles, GeoJSON, GeoPackage, KML ... drawn as lines that gain detail per zoom
// level and drape over the terrain). A path may carry options: "file.shp?minzoom=3&maxzoom=12&scale=50m
// &group=admin0&colour=ff8800&width=2" (see LayerOptions.h); Natural Earth names (ne_10m_...) declare
// their scale by themselves, and the scales of one set form an LOD group: the coarsest that still
// resolves at the current zoom is the one shown. A layer is reprojected on load from its native CRS
// into the display frame, paged tiles included: the native CRS is what the file declares, else
// --layer-crs for --layer files, else --source-crs (default EPSG:3857) for OSG files.
//
// --switch-at FRAME VIEW switches the display projection after FRAME frames (demos, screenshots).
// --script FILE runs a scenario unattended: one action per line, "FRAME ACTION ARGS", with the
// actions add PATH[?opts] [CRS] | terrain PATH | vector PATH | remove INDEX|all | reload INDEX |
// switch VIEW | goto LON LAT ZOOM | orient HEADING TILT | home | set KEY VALUE (drape on|off, drapeminzoom, globalminzoom,
// globemaxzoom, mapmaxzoom, tolerance) | width INDEX PX | colour INDEX rrggbb | quit. Blank lines and
// lines starting with # are ignored.
//
// The heavy lifting lives in the ivevsg library (lib/), which is what the Google Test suite exercises.

#include <ivevsg/CrsTransform.h>
#include <ivevsg/Hud.h>
#include <ivevsg/LayerOptions.h>
#include <ivevsg/MapNavigation.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TileScheme.h>
#include <ivevsg/ViewerApp.h>
#include <ivevsg/WorldView.h>

#include <vsg/all.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct ScriptStep
    {
        int frame = 0;
        std::string action;
        std::vector<std::string> args;
        bool done = false;
    };

    std::vector<ScriptStep> readScript(const std::string& file, std::string& error)
    {
        std::vector<ScriptStep> steps;
        std::ifstream in(file);
        if (!in)
        {
            error = "cannot read script " + file;
            return steps;
        }
        std::string line;
        int lineNumber = 0;
        while (std::getline(in, line))
        {
            ++lineNumber;
            std::istringstream words(line);
            ScriptStep step;
            if (!(words >> step.frame)) continue; // blank, or a comment
            if (line.find_first_not_of(" \t") != std::string::npos && line[line.find_first_not_of(" \t")] == '#') continue;
            words >> step.action;
            for (std::string word; words >> word;) step.args.push_back(word);
            if (step.action.empty())
            {
                error = file + ":" + std::to_string(lineNumber) + ": no action";
                return steps;
            }
            steps.push_back(step);
        }
        return steps;
    }

    bool onOff(const std::string& text)
    {
        return text == "on" || text == "1" || text == "true" || text == "yes";
    }

    /// Runs the steps due at this frame. Returns false when the script asks to quit.
    bool runScript(std::vector<ScriptStep>& steps, ivevsg::ViewerApp& app, double fieldOfViewDegrees, const ivevsg::LayerOptions& layerDefaults)
    {
        for (ScriptStep& step : steps)
        {
            if (step.done || step.frame > app.frameCount()) continue;
            step.done = true;
            std::cout << "script: frame " << app.frameCount() << " " << step.action;
            for (const std::string& a : step.args) std::cout << " " << a;
            std::cout << std::endl;
            auto source = [&](const std::string& path, ivevsg::LayerKind kind) {
                ivevsg::LayerSource s;
                s.path = path;
                s.options = layerDefaults;
                s.options.kind = kind;
                return s;
            };
            if (step.action == "add" && !step.args.empty())
            {
                ivevsg::LayerSource s = source(step.args[0], ivevsg::LayerKind::Auto);
                if (step.args.size() > 1) s.sourceCrs = step.args[1];
                app.queueLayerLoad(s);
            }
            else if (step.action == "terrain" && !step.args.empty())
            {
                app.queueLayerLoad(source(step.args[0], ivevsg::LayerKind::Terrain));
            }
            else if (step.action == "vector" && !step.args.empty())
            {
                app.queueLayerLoad(source(step.args[0], ivevsg::LayerKind::Vector));
            }
            else if (step.action == "remove" && !step.args.empty())
            {
                if (step.args[0] == "all")
                {
                    if (auto layers = app.layers())
                        while (layers->layerCount() > 0) app.removeLayer(layers->layerCount() - 1);
                }
                else
                {
                    app.removeLayer(static_cast<std::size_t>(std::atoi(step.args[0].c_str())));
                }
            }
            else if (step.action == "reload" && !step.args.empty())
            {
                app.reloadLayer(static_cast<std::size_t>(std::atoi(step.args[0].c_str())));
            }
            else if (step.action == "switch" && !step.args.empty())
            {
                app.requestViewSwitch(step.args[0]);
            }
            else if (step.action == "goto" && step.args.size() >= 3)
            {
                const double viewportHeight = app.camera() ? static_cast<double>(app.camera()->getViewport().height) : 1024.0;
                const double altitude = ivevsg::altitudeForZoomLevel(std::atof(step.args[2].c_str()), fieldOfViewDegrees, viewportHeight, 40075016.685578488);
                app.lookAt(ivevsg::GeoPoint{std::atof(step.args[0].c_str()), std::atof(step.args[1].c_str()), 0.0}, altitude);
            }
            else if (step.action == "home")
            {
                app.frameScene();
            }
            else if (step.action == "orient" && step.args.size() >= 2)
            {
                app.view().setOrientation(std::atof(step.args[0].c_str()), std::atof(step.args[1].c_str())); // heading, tilt (globe)
            }
            else if (step.action == "set" && step.args.size() >= 2)
            {
                ivevsg::VisibilitySettings rules = app.visibility();
                const std::string& key = step.args[0];
                const std::string& value = step.args[1];
                if (key == "drape") rules.drape = onOff(value);
                else if (key == "drapeminzoom") rules.drapeMinZoom = std::atof(value.c_str());
                else if (key == "globalminzoom") rules.globalMinZoom = std::atof(value.c_str());
                else if (key == "tolerance") rules.resolutionTolerance = std::atof(value.c_str());
                else if (key == "globemaxzoom") app.setViewMaxZoomLevel("globe", std::atof(value.c_str()));
                else if (key == "mapmaxzoom") app.setViewMaxZoomLevel("mercator", std::atof(value.c_str()));
                else std::cerr << "script: unknown setting '" << key << "'\n";
                app.setVisibility(rules);
            }
            else if (step.action == "width" && step.args.size() >= 2)
            {
                app.setLayerLineWidth(static_cast<std::size_t>(std::atoi(step.args[0].c_str())), static_cast<float>(std::atof(step.args[1].c_str())));
            }
            else if (step.action == "colour" && step.args.size() >= 2)
            {
                std::string path;
                ivevsg::LayerOptions options;
                std::string error;
                if (ivevsg::parseLayerSpec("x?colour=" + step.args[1], path, options, error))
                    app.setLayerColour(static_cast<std::size_t>(std::atoi(step.args[0].c_str())), options.vector.colour);
                else
                    std::cerr << "script: " << error << "\n";
            }
            else if (step.action == "quit")
            {
                return false;
            }
            else
            {
                std::cerr << "script: unknown action '" << step.action << "'\n";
            }
        }
        return true;
    }
} // namespace

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

    // layer rules and terrain defaults
    settings.visibility.globalMinZoom = arguments.value(0.0, "--global-min-zoom");
    settings.visibility.drape = !arguments.read("--no-drape");
    settings.visibility.drapeMinZoom = arguments.value(8.0, "--drape-min-zoom");
    settings.visibility.resolutionTolerance = arguments.value(2.0, "--resolution-tolerance");
    settings.vectorMaxLevel = arguments.value(16, "--vector-max-level");
    ivevsg::LayerOptions layerDefaults;
    layerDefaults.terrain.exaggeration = arguments.value(1.0, "--exaggeration");
    layerDefaults.terrain.interpolation = ivevsg::parseHeightInterpolation(arguments.value(std::string("bicubic"), "--terrain-interp"));
    layerDefaults.terrain.oversampleLevels = arguments.value(4, "--terrain-oversample");

    const bool noBaseMap = arguments.read("--no-base");
    std::string baseImage = arguments.value(std::string(), "--base");
    std::string baseTiles = arguments.value(std::string(), "--base-tiles");
    std::string globeTiles = arguments.value(std::string(), "--globe-tiles");
    const double startZoom = arguments.value(-1.0, "--zoom");
    const double minZoom = arguments.value(0.0, "--min-zoom");
    const double maxZoom = arguments.value(22.0, "--max-zoom");
    const double globeMaxZoom = arguments.value(-1.0, "--globe-max-zoom");
    const double mapMaxZoom = arguments.value(-1.0, "--map-max-zoom");
    double centerLongitude = 0.0, centerLatitude = 0.0;
    const bool haveCenter = arguments.read("--center", centerLongitude, centerLatitude);
    const std::string layerCrs = arguments.value(std::string(), "--layer-crs");
    std::vector<ivevsg::LayerSource> runtimeLayers;
    auto collect = [&](const char* flag, ivevsg::LayerKind kind) {
        for (std::string path; arguments.read(flag, path);)
        {
            ivevsg::LayerSource source;
            source.path = path;
            source.sourceCrs = layerCrs;
            source.options = layerDefaults;
            source.options.kind = kind;
            runtimeLayers.push_back(source);
        }
    };
    collect("--layer", ivevsg::LayerKind::Auto);
    collect("--terrain", ivevsg::LayerKind::Terrain);
    collect("--vector", ivevsg::LayerKind::Vector);
    int switchFrame = -1;
    std::string switchTo;
    arguments.read("--switch-at", switchFrame, switchTo);
    const std::string scriptFile = arguments.value(std::string(), "--script"); // all options must be consumed before the positional scene files
    std::vector<ScriptStep> script;
    if (!scriptFile.empty())
    {
        std::string error;
        script = readScript(scriptFile, error);
        if (!error.empty())
        {
            std::cerr << error << "\n";
            return 1;
        }
    }

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
        std::cerr << "usage: " << argv[0] << " [scene files ...] [--layer FILE[?opts] ...] [--terrain FILE|DIR ...] [--vector FILE ...] [--layer-crs EPSG:xxxx]\n"
                  << "       [--base-tiles <root.ive>] [--globe-tiles <root.ive>] [--base <image>] [--no-base] [--projection mercator|globe] [--source-crs EPSG:xxxx]\n"
                  << "       [--trackball] [--debug] [--frames N] [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z] [--globe-max-zoom Z] [--map-max-zoom Z]\n"
                  << "       [--global-min-zoom Z] [--no-drape] [--drape-min-zoom Z] [--resolution-tolerance N] [--exaggeration F] [--terrain-interp MODE]\n"
                  << "       [--terrain-oversample N] [--vector-max-level N] [--elevation DEG] [--switch-at FRAME VIEW] [--script FILE] [--log-loads] [--no-ui]\n";
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
    for (const std::string& filename : filenames)
    {
        ivevsg::LayerSource source;
        source.path = filename;
        source.options = layerDefaults;
        app.addLayer(source);
    }
    for (const ivevsg::LayerSource& source : runtimeLayers) app.queueLayerLoad(source); // worker-thread path, first frame

    std::cout << "vsgXchange OSG support: " << (ivevsg::SceneLoader::supportsOsg() ? "ON" : "OFF")
              << "  (OSG plugins: " << ivevsg::SceneLoader::defaultOsgPluginDirectory() << ")\n"
              << "Display: " << app.view().name() << "\n"
              << "Base map (mercator): " << (baseTiles.empty() ? (baseImage.empty() ? "none" : "image " + baseImage) : "tile pyramid " + baseTiles) << (defaultTiles ? " (default)" : "") << "\n"
              << "Base map (globe):    " << (globeTiles.empty() ? "none" : "tile pyramid " + globeTiles) << (defaultGlobeTiles ? " (default)" : "") << "\n"
              << "Files naming no CRS are taken as " << settings.defaultSourceCrs << "\n"
              << "Layer rules: load from zoom " << settings.visibility.globalMinZoom << ", drape " << (settings.visibility.drape ? "on" : "off") << " from zoom " << settings.visibility.drapeMinZoom
              << ", resolution tolerance " << settings.visibility.resolutionTolerance << " levels\n";

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
                  << "  centre lon " << state.centre.longitude << " lat " << state.centre.latitude;
        if (app.view().surfaceHeight() != 0.0) std::cout << "  terrain " << app.view().surfaceHeight() << " m";
        std::cout << "\n";
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
            std::cout << "  [" << (layer.kind.empty() ? "scene" : layer.kind) << "] native " << (layer.sourceCrs.empty() ? std::string("n/a") : ivevsg::CrsTransform::displayName(layer.sourceCrs))
                      << (layer.targetCrs.empty() ? " shown as read" : " -> " + ivevsg::CrsTransform::displayName(layer.targetCrs));
            if (!layer.info.empty()) std::cout << "\n  " << layer.info;
            if (layer.effectiveScaleDenominator() > 0.0)
                std::cout << "\n  declared " << ivevsg::formatScaleDenominator(layer.effectiveScaleDenominator()) << " (resolves to zoom " << ivevsg::zoomLevelForScaleDenominator(layer.effectiveScaleDenominator())
                          << "), LOD group '" << layer.effectiveLodGroup() << "'";
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
    if (globeMaxZoom >= 0.0) app.setViewMaxZoomLevel("globe", globeMaxZoom);
    if (mapMaxZoom >= 0.0) app.setViewMaxZoomLevel("mercator", mapMaxZoom);
    if (startZoom > app.viewMaxZoomLevel(app.view().key())) std::cout << "Note: --zoom " << startZoom << " is above this view's max zoom " << app.viewMaxZoomLevel(app.view().key()) << ", clamping\n";
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
              << " (globe max " << app.viewMaxZoomLevel("globe") << ", map max " << app.viewMaxZoomLevel("mercator") << "; past the deepest tiles the deepest tiles are simply magnified)\n"
              << "Wide lines: " << (app.wideLinesSupported() ? "supported" : "not supported by this device (vector widths clamp to 1 px)") << "\n"
              << "Start zoom level: " << app.zoomLevel() << "\n";
    for (const std::string& line : app.view().controls()) std::cout << "Controls: " << line << "\n";
    std::cout << "Esc = quit\n";

    if (switchFrame < 0 && script.empty()) return app.run();

    // scripted projection switch / scenario, for demos, screenshots and unattended checks
    int code = 0;
    try
    {
        while (app.frame())
        {
            if (app.frameCount() == switchFrame) app.requestViewSwitch(switchTo); // applied at the start of the next frame
            if (!runScript(script, app, settings.fieldOfViewDegrees, layerDefaults)) break;
        }
    }
    catch (const vsg::Exception& e)
    {
        std::cerr << "Vulkan error while rendering: " << e.message << "\n";
        code = 2;
    }
    return code;
}
