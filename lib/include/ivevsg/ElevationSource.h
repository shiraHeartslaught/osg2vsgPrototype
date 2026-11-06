#pragma once

#include <ivevsg/LayerOptions.h>
#include <ivevsg/Projection.h>

#include <vsg/core/Inherit.h>
#include <vsg/core/Object.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ivevsg
{
    /// A rectangular window of elevation posts in geographic coordinates: rows run from the north,
    /// columns from the west, each post sitting at the centre of its cell. NaN marks a void.
    struct HeightGrid
    {
        int width = 0, height = 0;
        double lon0 = 0.0, lat0 = 0.0; // position of post (0, 0): the north-west one
        double dLon = 0.0, dLat = 0.0; // post spacing in degrees (both positive)
        std::vector<float> heights;    // row-major from the north

        bool valid() const { return width > 0 && height > 0 && heights.size() == static_cast<std::size_t>(width) * height; }
        float at(int col, int row) const; // clamped to the grid, voids as 0
        bool isVoid(int col, int row) const;
        /// Height at a position (clamped to the grid's edges) with the given interpolation; voids count as 0.
        double sample(double longitude, double latitude, HeightInterpolation mode) const;
        /// The grid's geographic footprint (outer edges of the edge posts' cells).
        GeoExtent extent() const;
    };

    /// Heights from a GDAL-readable elevation raster: DTED (.dt0 / .dt1 / .dt2), SRTM .hgt, a GeoTIFF
    /// DEM, a VRT, or a directory of such files (mosaicked through a VRT at the finest resolution).
    /// Data in a projected CRS is warped to geographic WGS84 on the fly, so everything here is in
    /// longitude / latitude. Windows are read on demand (the pager's threads call concurrently; one
    /// read at a time per source) and at any resolution: coarser than the posts GDAL averages them,
    /// finer than the posts the HeightGrid interpolates between them.
    class ElevationSource
    {
    public:
        ~ElevationSource();

        /// Opens a file or a directory; sourceCrs overrides the CRS the file declares ("" = use the file's).
        static std::shared_ptr<ElevationSource> open(const std::string& path, const std::string& sourceCrs, std::string& error);
        /// Extensions that are elevation by convention (.dt0 .dt1 .dt2 .hgt), or a directory holding such files.
        static bool looksLikeElevation(const std::string& path);
        /// Elevation files found directly in a directory (what open() mosaics).
        static std::vector<std::string> elevationFilesIn(const std::string& directory);

        const std::string& path() const { return _path; }
        const std::string& sourceCrs() const { return _sourceCrs; }
        std::string description() const;
        const GeoExtent& extent() const { return _extent; }
        bool covers(double longitude, double latitude) const;
        int width() const { return _width; }   // posts
        int height() const { return _height; } // posts
        double postSpacingLon() const { return _dLon; } // degrees
        double postSpacingLat() const { return _dLat; } // degrees
        /// Ground distance between posts in metres at the data's centre latitude (the finer axis).
        double postSpacingMetres() const;
        /// Approximate height range of the data (from GDAL statistics; estimated when the file carries none).
        double minHeight() const { return _minHeight; }
        double maxHeight() const { return _maxHeight; }
        int fileCount() const { return _fileCount; }

        /// Reads the posts covering `extent` plus `marginPosts` around it. When that window holds more
        /// than `maxPosts` along either axis it is resampled (averaged) down to at most maxPosts, so a
        /// tile that covers a whole DTED cell costs the same as one that covers a few posts.
        bool readWindow(const GeoExtent& extent, int marginPosts, int maxPosts, HeightGrid& out) const;

        /// One bilinear height, or nothing outside the data. Thread safe; a few microseconds.
        std::optional<double> heightAt(double longitude, double latitude) const;

    private:
        ElevationSource() = default;
        struct Impl;
        std::unique_ptr<Impl> _impl;
        std::string _path;
        std::string _sourceCrs;
        GeoExtent _extent;
        int _width = 0, _height = 0;
        double _dLon = 0.0, _dLat = 0.0;
        double _minHeight = 0.0, _maxHeight = 0.0;
        int _fileCount = 1;
    };

    /// The elevation sources currently shown as terrain, so that other readers (vector draping) and the
    /// viewer (keeping the camera above the ground) can ask for the height under a point. Stored in the
    /// loader's options (loaderKeys::elevationRegistry). Sources are held weakly: a terrain layer that
    /// leaves the scene drops out by itself.
    class ElevationRegistry : public vsg::Inherit<vsg::Object, ElevationRegistry>
    {
    public:
        unsigned add(const std::shared_ptr<const ElevationSource>& source, double exaggeration);
        void remove(unsigned id);
        void setEnabled(unsigned id, bool enabled);

        /// Height (exaggeration applied) from the finest enabled source covering the point.
        std::optional<double> heightAt(double longitude, double latitude) const;
        /// Whether any enabled source is alive.
        bool hasTerrain() const;
        std::size_t size() const;

    private:
        struct Entry
        {
            unsigned id = 0;
            std::weak_ptr<const ElevationSource> source;
            double exaggeration = 1.0;
            bool enabled = true;
        };

        mutable std::mutex _mutex;
        std::vector<Entry> _entries;
        unsigned _nextId = 1;
    };
} // namespace ivevsg
