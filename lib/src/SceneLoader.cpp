#include <ivevsg/CrsTransform.h>
#include <ivevsg/LoaderKeys.h>
#include <ivevsg/OsgReader.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TerrainLayerReader.h>
#include <ivevsg/TileLayerReader.h>
#include <ivevsg/VectorLayerReader.h>
#include <ivevsg/VsgReproject.h>

#include <vsg/all.h>
#include <vsgXchange/Version.h>
#include <vsgXchange/all.h>

#include <osgDB/Registry>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>

#ifndef IVEVSG_OSG_PLUGIN_DIR
#    define IVEVSG_OSG_PLUGIN_DIR ""
#endif

namespace ivevsg
{
    /// Sits first in the reader chain, notes each request and declines it so the real readers run.
    class SceneLoader::LoadMonitor : public vsg::Inherit<vsg::ReaderWriter, LoadMonitor>
    {
    public:
        explicit LoadMonitor(bool print) : _print(print) {}

        vsg::ref_ptr<vsg::Object> read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options>) const override
        {
            ++_requests;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _lastFile = filename.string();
            }
            if (_print)
            {
                static const auto start = std::chrono::steady_clock::now();
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                std::cout << "load: " << filename << "  (t=" << seconds << " s)" << std::endl;
            }
            return {};
        }

        LoadStats stats() const
        {
            LoadStats stats;
            stats.requests = _requests.load();
            std::lock_guard<std::mutex> lock(_mutex);
            stats.lastFile = _lastFile;
            return stats;
        }

    private:
        bool _print = false;
        mutable std::atomic<std::size_t> _requests{0};
        mutable std::mutex _mutex;
        mutable std::string _lastFile;
    };

    LoaderSettings SceneLoader::defaultSettings()
    {
        LoaderSettings settings;
        settings.osgPluginDirectory = defaultOsgPluginDirectory();
        return settings;
    }

    std::string SceneLoader::defaultOsgPluginDirectory()
    {
        return IVEVSG_OSG_PLUGIN_DIR;
    }

    bool SceneLoader::supportsOsg()
    {
#ifdef vsgXchange_OSG
        return true;
#else
        return false;
#endif
    }

    void SceneLoader::registerOsgPluginDirectory(const std::string& directory)
    {
        // OSG reads OSG_LIBRARY_PATH while its registry is constructed, which happens during static
        // initialisation before main(), so setting the environment variable at runtime would be too late.
        // Add the plugin directory to the registry's search list instead, unless the user has already
        // pointed OSG_LIBRARY_PATH at a plugin set of their own.
        if (directory.empty() || std::getenv("OSG_LIBRARY_PATH") != nullptr) return;
        auto& searchPaths = osgDB::Registry::instance()->getLibraryFilePathList();
        if (std::find(searchPaths.begin(), searchPaths.end(), directory) == searchPaths.end())
        {
            searchPaths.push_front(directory);
        }
    }

    SceneLoader::SceneLoader(LoaderSettings settings) :
        _settings(std::move(settings))
    {
        registerOsgPluginDirectory(_settings.osgPluginDirectory);
        ensureGdalRegistered(); // on this thread, before any worker opens a raster or a vector file
        if (!_settings.elevation) _settings.elevation = ElevationRegistry::create();

        _options = vsg::Options::create();
        _monitor = LoadMonitor::create(_settings.logReads);
        _options->add(_monitor);
        _options->add(TerrainLayerReader::create()); // DTED / DEMs become paged terrain surfaces in the display frame
        _options->add(VectorLayerReader::create());  // shapefiles and other OGR files become paged line layers
        _options->add(TileLayerReader::create());    // rasters and tile directories become paged tile layers in the display frame
        _options->add(VsgSceneReader::create());     // VSG native files, placed (reprojected) in the display frame
        _options->add(OsgReader::create());          // our OSG reader: fix-ups + reprojection before osg2vsg conversion
        _options->add(vsgXchange::all::create());    // everything else (images, glTF, ...)
        _options->setValue(loaderKeys::targetCrs, _settings.targetCrs);
        _options->setValue(loaderKeys::defaultSourceCrs, _settings.defaultSourceCrs);
        _options->setValue(loaderKeys::quadTessellation, _settings.quadTessellation);
        _options->setValue(loaderKeys::detailBias, _settings.detailBias);
        _options->setValue(loaderKeys::drape, _settings.drape);
        _options->setValue(loaderKeys::drapeMinZoom, _settings.drapeMinZoom);
        _options->setValue(loaderKeys::vectorMaxLevel, _settings.vectorMaxLevel);
        _options->setValue(loaderKeys::wideLines, _settings.wideLines);
        _options->setObject(loaderKeys::elevationRegistry, _settings.elevation);
        _options->paths = vsg::getEnvPaths("VSG_FILE_PATH");
        for (const auto& path : _settings.searchPaths) _options->paths.push_back(vsg::Path(path));
    }

    SceneLoader::~SceneLoader() = default;

    LoadStats SceneLoader::loadStats() const
    {
        return _monitor ? _monitor->stats() : LoadStats{};
    }

    LoadResult SceneLoader::load(const std::string& filename, const std::string& sourceCrs) const
    {
        LayerSource source;
        source.path = filename;
        source.sourceCrs = sourceCrs;
        return load(source);
    }

    LoadResult SceneLoader::load(const LayerSource& source) const
    {
        LoadResult result;
        const std::string& filename = source.path;
        const vsg::Path path(filename);
        // a {z}/{x}/{y} tile template names no single file; everything else must exist
        const bool templated = filename.find("{z}") != std::string::npos;
        const vsg::Path found = templated ? path : vsg::findFile(path, _options);
        if (!found)
        {
            result.error = "File not found: " + filename;
            return result;
        }

        // Read with a copy of the options that also searches the file's own directory, so relative
        // PagedLOD filenames inside it (e.g. tile pyramids: "0/0/0.ive") resolve. PagedLODs created
        // while loading inherit these options, so the paths carry through every level of paging.
        auto options = vsg::Options::create(*_options);
        const vsg::Path directory = vsg::filePath(found);
        if (!directory.empty()) options->paths.push_back(directory);
        if (!source.sourceCrs.empty()) options->setValue(loaderKeys::sourceCrs, source.sourceCrs);
        const LayerOptions& o = source.options;
        options->setValue(loaderKeys::layerKind, std::string(layerKindName(o.kind)));
        options->setValue(loaderKeys::detailLevelCap, o.detailLevelCap);
        options->setValue(loaderKeys::terrainExaggeration, o.terrain.exaggeration);
        options->setValue(loaderKeys::terrainInterpolation, static_cast<int>(o.terrain.interpolation));
        options->setValue(loaderKeys::terrainOversample, o.terrain.oversampleLevels);
        options->setValue(loaderKeys::terrainCells, o.terrain.cellsPerTile);
        options->setValue(loaderKeys::terrainTexture, o.terrain.texturePixels);
        options->setValue(loaderKeys::terrainHillshade, o.terrain.hillshade);
        options->setValue(loaderKeys::vectorColour, o.vector.colour);
        options->setValue(loaderKeys::vectorLineWidth, o.vector.lineWidth);
        options->setValue(loaderKeys::vectorPointSize, o.vector.pointSize);

        vsg::ref_ptr<vsg::Object> object = vsg::read(found, options);
        result.node = object.cast<vsg::Node>();
        if (result.node)
        {
            result.node->getValue(loaderKeys::nodeSourceCrs, result.sourceCrs);
            result.node->getValue(loaderKeys::nodeTargetCrs, result.targetCrs);
            result.node->getValue(loaderKeys::nodeLayerKind, result.kind);
            result.node->getValue(loaderKeys::nodeInfo, result.info);
            result.node->getValue(loaderKeys::nodeDeclaredScale, result.declaredScale);
            result.node->getValue(loaderKeys::nodeNativeLevel, result.nativeLevel);
            result.node->getValue(loaderKeys::nodeMaxLevel, result.maxLevel);
            result.node->getValue(loaderKeys::nodeTerrainRegistryId, result.terrainRegistryId);
            if (result.kind.empty())
            {
                if (TileLayerReader::handlesRaster(found) || TileLayerReader::handlesTileSet(found))
                    result.kind = "raster";
                else
                    result.kind = "scene";
            }
        }
        else if (auto readError = object.cast<vsg::ReadError>())
        {
            result.error = readError->message; // a reader recognised the file and says why it could not use it
        }
        else if (object)
        {
            result.error = filename + " is not a scene (it read as " + object->className() + ")";
        }
        if (!result.node && result.error.empty())
        {
            result.error = "No loader could read " + filename;
            const vsg::Path ext = vsg::lowerCaseFileExtension(path);
            if (ext == ".ive" || ext == ".osgb" || ext == ".osgt" || ext == ".osg")
            {
                if (!supportsOsg())
                {
                    result.error += " (vsgXchange was built without OSG support)";
                }
                else
                {
                    const char* pluginPath = std::getenv("OSG_LIBRARY_PATH");
                    result.error += " (check that OSG's format plugins are found, OSG_LIBRARY_PATH=";
                    result.error += pluginPath ? pluginPath : "<unset>";
                    result.error += ")";
                }
            }
        }
        return result;
    }
} // namespace ivevsg
