#pragma once

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

        vsg::ref_ptr<vsg::Options> options() const { return _options; }
        const LoaderSettings& settings() const { return _settings; }

        /// Thread-safe snapshot of the read activity (the pager reads on its own threads).
        LoadStats loadStats() const;

    private:
        class LoadMonitor;
        LoaderSettings _settings;
        vsg::ref_ptr<vsg::Options> _options;
        vsg::ref_ptr<LoadMonitor> _monitor;
    };
} // namespace ivevsg
