#pragma once

#include <ivevsg/ElevationSource.h>
#include <ivevsg/MapScene.h>

#include <vsg/core/ref_ptr.h>
#include <vsg/io/Options.h>
#include <vsg/nodes/Node.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ivevsg
{
    struct LoaderSettings
    {
        /// Directory holding OSG's osgdb_*.so format plugins, required to read .ive/.osgb/.osgt files.
        /// Defaults to the vcpkg plugin directory matching this build configuration (see lib/CMakeLists.txt).
        /// Only used when OSG_LIBRARY_PATH is not already set in the environment.
        std::string osgPluginDirectory;

        /// Extra directories searched for relative filenames; VSG_FILE_PATH is always honoured too.
        std::vector<std::string> searchPaths;

        /// Print every file read through these options (including tiles paged in at runtime) to stdout.
        bool logReads = false;

        /// CRS of the frame scenes are displayed in (the current Projection's crs()). OSG files whose
        /// own CRS differs are reprojected on load, paged tiles included. Empty disables reprojection.
        std::string targetCrs = "EPSG:3857";
        /// Assumed CRS of OSG files that carry no CoordinateSystemNode and were loaded without an
        /// explicit source CRS (this project's VPB and ivetiler databases are Web Mercator).
        std::string defaultSourceCrs = "EPSG:3857";
        /// Grid size single-quad tiles are split into before reprojection (1 = leave as quads).
        int quadTessellation = 1;
        /// For tile layers cut at load time (rasters, tile directories): > 1 hands a tile over to
        /// its children while it is still smaller on screen (sharper, more tiles resident).
        double detailBias = 1.0;

        /// The terrain currently shown, shared with the viewer so vector layers drape over it and the
        /// camera stays above it. Created by the loader when left null.
        vsg::ref_ptr<ElevationRegistry> elevation;
        /// Vector features follow the terrain from this tile level on (when drape is set).
        bool drape = true;
        double drapeMinZoom = 8.0;
        /// Deepest tile level vector layers page to.
        int vectorMaxLevel = 16;
        /// The Vulkan device draws lines wider than a pixel (the wideLines feature); otherwise widths clamp to 1.
        bool wideLines = false;
    };

    /// What the loader (and the database pager, through the shared options) has read so far.
    struct LoadStats
    {
        std::size_t requests = 0; // files handed to the reader chain, including paged tiles
        std::string lastFile;
    };

    struct LoadResult
    {
        vsg::ref_ptr<vsg::Node> node;
        std::string error;
        std::string sourceCrs; // the CRS the file was taken to be in ("" for formats that carry none, e.g. VSG native)
        std::string targetCrs; // the frame it was reprojected into ("" when left as read)
        std::string kind;      // what the reader made of the file: "terrain", "vector", "raster", "scene" ("" = not reported)
        std::string info;      // one line from the reader for panels
        double declaredScale = 0.0;     // 1:S found in the file name (vector layers)
        int nativeLevel = -1;           // tile level of the data's own resolution (terrain)
        int maxLevel = -1;              // deepest tile level the layer pages to
        unsigned terrainRegistryId = 0; // the layer's entry in the ElevationRegistry (terrain)

        explicit operator bool() const { return node.valid(); }
    };

    /// Owns the vsgXchange reader set and loads scene files through it. The same vsg::Options are
    /// inherited by PagedLOD children, so paged tiles are read with the same loaders at runtime.
    class SceneLoader
    {
    public:
        explicit SceneLoader(LoaderSettings settings = defaultSettings());
        ~SceneLoader();

        static LoaderSettings defaultSettings();

        /// True when the linked vsgXchange was built with osg2vsg, i.e. it can read .ive files.
        static bool supportsOsg();

        /// Plugin directory baked in at build time ("" if none was found).
        static std::string defaultOsgPluginDirectory();

        /// Adds a plugin directory to OSG's registry so osgdb_*.so loaders/writers are found. No-op when
        /// the directory is empty, already registered, or OSG_LIBRARY_PATH is set by the user.
        static void registerOsgPluginDirectory(const std::string& directory);

        /// sourceCrs overrides the CRS the file is assumed to be in (see LoaderSettings::defaultSourceCrs).
        LoadResult load(const std::string& filename, const std::string& sourceCrs = {}) const;
        /// Loads with the source's CRS override and layer options (kind, styles, detail cap) applied.
        LoadResult load(const LayerSource& source) const;

        vsg::ref_ptr<vsg::Options> options() const { return _options; }
        const LoaderSettings& settings() const { return _settings; }
        vsg::ref_ptr<ElevationRegistry> elevation() const { return _settings.elevation; }

        /// Thread-safe snapshot of the read activity (the pager reads on its own threads).
        LoadStats loadStats() const;

    private:
        class LoadMonitor;
        LoaderSettings _settings;
        vsg::ref_ptr<vsg::Options> _options;
        vsg::ref_ptr<LoadMonitor> _monitor;
    };
} // namespace ivevsg
