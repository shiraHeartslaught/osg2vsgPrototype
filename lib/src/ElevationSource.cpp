#include <ivevsg/CrsTransform.h>
#include <ivevsg/ElevationSource.h>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

namespace ivevsg
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kMetresPerDegree = 111319.49079327358; // at the equator, WGS84 semi-major

        void ensureGdal()
        {
            ensureGdalRegistered();
        }

        std::string lowerExtension(const std::string& path)
        {
            std::string ext = std::filesystem::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        bool isElevationExtension(const std::string& ext)
        {
            return ext == ".dt0" || ext == ".dt1" || ext == ".dt2" || ext == ".dt3" || ext == ".hgt";
        }

        bool isRasterExtension(const std::string& ext)
        {
            return isElevationExtension(ext) || ext == ".tif" || ext == ".tiff" || ext == ".vrt" || ext == ".img" || ext == ".dem" || ext == ".asc" || ext == ".bil" || ext == ".flt";
        }

        std::string wellKnownText(const std::string& userInput)
        {
            OGRSpatialReference srs;
            if (srs.SetFromUserInput(userInput.c_str()) != OGRERR_NONE) return {};
            char* wkt = nullptr;
            if (srs.exportToWkt(&wkt) != OGRERR_NONE || !wkt) return {};
            std::string text(wkt);
            CPLFree(wkt);
            return text;
        }

        /// Catmull-Rom weights for the fractional position t in [0, 1].
        void cubicWeights(double t, double w[4])
        {
            const double t2 = t * t, t3 = t2 * t;
            w[0] = 0.5 * (-t3 + 2.0 * t2 - t);
            w[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
            w[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
            w[3] = 0.5 * (t3 - t2);
        }
    } // namespace

    // ---- HeightGrid ---------------------------------------------------------------------------------

    float HeightGrid::at(int col, int row) const
    {
        if (!valid()) return 0.0f;
        col = std::clamp(col, 0, width - 1);
        row = std::clamp(row, 0, height - 1);
        const float h = heights[static_cast<std::size_t>(row) * width + col];
        return std::isnan(h) ? 0.0f : h;
    }

    bool HeightGrid::isVoid(int col, int row) const
    {
        if (!valid()) return true;
        col = std::clamp(col, 0, width - 1);
        row = std::clamp(row, 0, height - 1);
        return std::isnan(heights[static_cast<std::size_t>(row) * width + col]);
    }

    double HeightGrid::sample(double longitude, double latitude, HeightInterpolation mode) const
    {
        if (!valid()) return 0.0;
        if (width == 1 && height == 1) return at(0, 0);
        const double u = std::clamp(dLon > 0.0 ? (longitude - lon0) / dLon : 0.0, 0.0, static_cast<double>(width - 1));
        const double v = std::clamp(dLat > 0.0 ? (lat0 - latitude) / dLat : 0.0, 0.0, static_cast<double>(height - 1));
        if (mode == HeightInterpolation::Nearest) return at(static_cast<int>(std::lround(u)), static_cast<int>(std::lround(v)));
        const int i = std::min(static_cast<int>(std::floor(u)), width - 1);
        const int j = std::min(static_cast<int>(std::floor(v)), height - 1);
        const double fu = u - i, fv = v - j;
        if (mode == HeightInterpolation::Bilinear || width < 4 || height < 4)
        {
            const double h00 = at(i, j), h10 = at(i + 1, j), h01 = at(i, j + 1), h11 = at(i + 1, j + 1);
            return (h00 * (1.0 - fu) + h10 * fu) * (1.0 - fv) + (h01 * (1.0 - fu) + h11 * fu) * fv;
        }
        double wu[4], wv[4];
        cubicWeights(fu, wu);
        cubicWeights(fv, wv);
        double sum = 0.0;
        for (int b = 0; b < 4; ++b)
        {
            double row = 0.0;
            for (int a = 0; a < 4; ++a) row += wu[a] * at(i - 1 + a, j - 1 + b);
            sum += wv[b] * row;
        }
        return sum;
    }

    GeoExtent HeightGrid::extent() const
    {
        GeoExtent e;
        e.minLongitude = lon0 - 0.5 * dLon;
        e.maxLongitude = lon0 + (width - 0.5) * dLon;
        e.maxLatitude = lat0 + 0.5 * dLat;
        e.minLatitude = lat0 - (height - 0.5) * dLat;
        return e;
    }

    // ---- ElevationSource ----------------------------------------------------------------------------

    struct ElevationSource::Impl
    {
        GDALDatasetH source = nullptr; // the file, or the mosaic VRT
        GDALDatasetH warped = nullptr; // the source warped to geographic WGS84 when it is not already
        GDALDatasetH reader = nullptr; // where the posts come from
        std::vector<GDALDatasetH> parts; // the files of a mosaic
        double geo[6] = {0, 1, 0, 0, 0, -1};
        double noData = std::numeric_limits<double>::quiet_NaN();
        bool hasNoData = false;
        mutable std::mutex mutex;

        ~Impl()
        {
            if (warped) GDALClose(warped);
            if (source) GDALClose(source);
            for (GDALDatasetH part : parts) GDALClose(part);
        }
    };

    ElevationSource::~ElevationSource() = default;

    bool ElevationSource::looksLikeElevation(const std::string& path)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) return !elevationFilesIn(path).empty();
        return isElevationExtension(lowerExtension(path));
    }

    std::vector<std::string> ElevationSource::elevationFilesIn(const std::string& directory)
    {
        std::vector<std::string> files;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        {
            if (!entry.is_regular_file(ec)) continue;
            if (isElevationExtension(lowerExtension(entry.path().string()))) files.push_back(entry.path().string());
        }
        if (files.empty())
        {
            // no DTED / HGT: any raster will do when the directory was given as terrain explicitly
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                if (!entry.is_regular_file(ec)) continue;
                if (isRasterExtension(lowerExtension(entry.path().string()))) files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    std::shared_ptr<ElevationSource> ElevationSource::open(const std::string& path, const std::string& sourceCrs, std::string& error)
    {
        ensureGdal();
        auto impl = std::make_unique<Impl>();
        std::error_code ec;
        int fileCount = 1;
        CPLErrorReset();
        if (std::filesystem::is_directory(path, ec))
        {
            const std::vector<std::string> files = elevationFilesIn(path);
            if (files.empty())
            {
                error = path + " holds no elevation files (.dt0/.dt1/.dt2/.hgt or rasters)";
                return nullptr;
            }
            for (const std::string& file : files)
            {
                GDALDatasetH part = GDALOpenEx(file.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
                if (part) impl->parts.push_back(part);
            }
            if (impl->parts.empty())
            {
                error = "GDAL cannot open any of the " + std::to_string(files.size()) + " elevation files in " + path;
                return nullptr;
            }
            const char* argv[] = {"-resolution", "highest", nullptr};
            GDALBuildVRTOptions* options = GDALBuildVRTOptionsNew(const_cast<char**>(argv), nullptr);
            int usageError = 0;
            impl->source = GDALBuildVRT("", static_cast<int>(impl->parts.size()), impl->parts.data(), nullptr, options, &usageError);
            GDALBuildVRTOptionsFree(options);
            if (!impl->source)
            {
                const char* message = CPLGetLastErrorMsg();
                error = "cannot mosaic the elevation files in " + path + (message && *message ? std::string(": ") + message : "");
                return nullptr;
            }
            fileCount = static_cast<int>(impl->parts.size());
        }
        else
        {
            impl->source = GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
            if (!impl->source)
            {
                const char* message = CPLGetLastErrorMsg();
                error = "GDAL cannot open " + path + (message && *message ? std::string(": ") + message : "");
                return nullptr;
            }
        }
        GDALDatasetH dataset = impl->source;
        if (GDALGetRasterCount(dataset) < 1)
        {
            error = path + " has no raster bands";
            return nullptr;
        }

        std::string sourceWkt, sourceLabel = sourceCrs;
        if (!sourceCrs.empty())
        {
            sourceWkt = wellKnownText(sourceCrs);
            if (sourceWkt.empty())
            {
                error = "unrecognised coordinate reference system '" + sourceCrs + "'";
                return nullptr;
            }
        }
        else
        {
            const char* declared = GDALGetProjectionRef(dataset);
            if (declared && *declared) sourceWkt = declared;
            if (sourceWkt.empty())
            {
                // DTED and HGT are geographic WGS84 by definition; anything else must say
                const std::string ext = lowerExtension(path);
                if (!isElevationExtension(ext) && !std::filesystem::is_directory(path, ec))
                {
                    error = path + " names no coordinate reference system; choose the native CRS when adding it";
                    return nullptr;
                }
                sourceWkt = wellKnownText("EPSG:4326");
            }
            sourceLabel = sourceWkt;
        }
        double geo[6];
        if (GDALGetGeoTransform(dataset, geo) != CE_None)
        {
            error = path + " is not georeferenced";
            return nullptr;
        }

        // posts are wanted in longitude / latitude: anything else is warped on the fly
        {
            OGRSpatialReference source, target;
            source.SetFromUserInput(sourceWkt.c_str());
            target.SetFromUserInput("EPSG:4326");
            source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            const bool geographic = source.IsGeographic() == TRUE && source.IsSame(&target) == TRUE;
            if (geographic && geo[2] == 0.0 && geo[4] == 0.0 && geo[1] > 0.0 && geo[5] < 0.0)
            {
                impl->reader = dataset;
                std::copy(geo, geo + 6, impl->geo);
            }
            else
            {
                const std::string targetWkt = wellKnownText("EPSG:4326");
                GDALWarpOptions* warpOptions = GDALCreateWarpOptions();
                CPLErrorReset();
                impl->warped = GDALAutoCreateWarpedVRTEx(dataset, sourceWkt.c_str(), targetWkt.c_str(), GRA_Bilinear, 0.125, warpOptions, nullptr);
                GDALDestroyWarpOptions(warpOptions);
                if (!impl->warped || GDALGetGeoTransform(impl->warped, impl->geo) != CE_None)
                {
                    const char* message = CPLGetLastErrorMsg();
                    error = "cannot warp " + path + " to geographic coordinates" + (message && *message ? std::string(": ") + message : "");
                    return nullptr;
                }
                impl->reader = impl->warped;
            }
        }

        GDALRasterBandH band = GDALGetRasterBand(impl->reader, 1);
        int hasNoData = 0;
        const double noData = GDALGetRasterNoDataValue(band, &hasNoData);
        impl->hasNoData = hasNoData != 0;
        impl->noData = noData;

        std::shared_ptr<ElevationSource> result(new ElevationSource());
        result->_path = path;
        result->_sourceCrs = sourceLabel;
        result->_width = GDALGetRasterXSize(impl->reader);
        result->_height = GDALGetRasterYSize(impl->reader);
        result->_dLon = impl->geo[1];
        result->_dLat = -impl->geo[5];
        result->_extent.minLongitude = impl->geo[0];
        result->_extent.maxLongitude = impl->geo[0] + impl->geo[1] * result->_width;
        result->_extent.maxLatitude = impl->geo[3];
        result->_extent.minLatitude = impl->geo[3] + impl->geo[5] * result->_height;
        result->_fileCount = fileCount;

        // the height range, for colouring: whatever statistics the file carries, else an estimate
        double minimum = 0.0, maximum = 0.0, mean = 0.0, stddev = 0.0;
        GDALRasterBandH sourceBand = GDALGetRasterBand(impl->source, 1);
        CPLPushErrorHandler(CPLQuietErrorHandler);
        CPLErr statsErr = GDALGetRasterStatistics(sourceBand, TRUE, FALSE, &minimum, &maximum, &mean, &stddev);
        if (statsErr != CE_None) statsErr = GDALComputeRasterStatistics(sourceBand, TRUE, &minimum, &maximum, &mean, &stddev, nullptr, nullptr);
        CPLPopErrorHandler();
        if (statsErr == CE_None)
        {
            result->_minHeight = minimum;
            result->_maxHeight = maximum;
        }
        else
        {
            result->_minHeight = 0.0;
            result->_maxHeight = 1000.0;
        }
        result->_impl = std::move(impl);
        return result;
    }

    std::string ElevationSource::description() const
    {
        std::string text = std::filesystem::path(_path).filename().string();
        if (_fileCount > 1) text += " (" + std::to_string(_fileCount) + " files)";
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), " %dx%d posts, %.1f m spacing, %.0f..%.0f m", _width, _height, postSpacingMetres(), _minHeight, _maxHeight);
        return text + buffer;
    }

    bool ElevationSource::covers(double longitude, double latitude) const
    {
        return longitude >= _extent.minLongitude && longitude <= _extent.maxLongitude && latitude >= _extent.minLatitude && latitude <= _extent.maxLatitude;
    }

    double ElevationSource::postSpacingMetres() const
    {
        const double centreLatitude = 0.5 * (_extent.minLatitude + _extent.maxLatitude);
        const double lonMetres = _dLon * kMetresPerDegree * std::max(0.01, std::cos(centreLatitude * kPi / 180.0));
        const double latMetres = _dLat * kMetresPerDegree;
        return std::min(lonMetres, latMetres);
    }

    bool ElevationSource::readWindow(const GeoExtent& extent, int marginPosts, int maxPosts, HeightGrid& out) const
    {
        out = HeightGrid{};
        if (!_impl || _width <= 0 || _height <= 0) return false;
        const Impl& impl = *_impl;
        // post indices (cells, rows from the north) covering the extent, widened by the margin
        const double c0f = (extent.minLongitude - impl.geo[0]) / impl.geo[1];
        const double c1f = (extent.maxLongitude - impl.geo[0]) / impl.geo[1];
        const double r0f = (extent.maxLatitude - impl.geo[3]) / impl.geo[5];
        const double r1f = (extent.minLatitude - impl.geo[3]) / impl.geo[5];
        int c0 = static_cast<int>(std::floor(std::min(c0f, c1f))) - marginPosts;
        int c1 = static_cast<int>(std::ceil(std::max(c0f, c1f))) + marginPosts;
        int r0 = static_cast<int>(std::floor(std::min(r0f, r1f))) - marginPosts;
        int r1 = static_cast<int>(std::ceil(std::max(r0f, r1f))) + marginPosts;
        c0 = std::clamp(c0, 0, _width);
        c1 = std::clamp(c1, 0, _width);
        r0 = std::clamp(r0, 0, _height);
        r1 = std::clamp(r1, 0, _height);
        if (c1 <= c0 || r1 <= r0) return false;
        const int cols = c1 - c0, rows = r1 - r0;
        maxPosts = std::max(2, maxPosts);
        int outW = cols, outH = rows;
        if (cols > maxPosts || rows > maxPosts)
        {
            const double scale = std::min(static_cast<double>(maxPosts) / cols, static_cast<double>(maxPosts) / rows);
            outW = std::max(2, static_cast<int>(std::lround(cols * scale)));
            outH = std::max(2, static_cast<int>(std::lround(rows * scale)));
        }
        std::vector<float> buffer(static_cast<std::size_t>(outW) * outH);
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.eResampleAlg = (outW < cols) ? GRIORA_Average : GRIORA_NearestNeighbour;
        CPLErr err;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            err = GDALRasterIOEx(GDALGetRasterBand(impl.reader, 1), GF_Read, c0, r0, cols, rows, buffer.data(), outW, outH, GDT_Float32, 0, 0, &extra);
        }
        if (err != CE_None) return false;
        if (impl.hasNoData)
        {
            const float nd = static_cast<float>(impl.noData);
            for (float& h : buffer)
                if (h == nd || (std::isnan(nd) && std::isnan(h))) h = std::numeric_limits<float>::quiet_NaN();
        }
        out.width = outW;
        out.height = outH;
        out.dLon = impl.geo[1] * cols / outW;
        out.dLat = -impl.geo[5] * rows / outH;
        out.lon0 = impl.geo[0] + impl.geo[1] * c0 + 0.5 * out.dLon;
        out.lat0 = impl.geo[3] + impl.geo[5] * r0 - 0.5 * out.dLat;
        out.heights = std::move(buffer);
        return true;
    }

    std::optional<double> ElevationSource::heightAt(double longitude, double latitude) const
    {
        if (!covers(longitude, latitude)) return std::nullopt;
        GeoExtent point;
        point.minLongitude = point.maxLongitude = longitude;
        point.minLatitude = point.maxLatitude = latitude;
        HeightGrid grid;
        if (!readWindow(point, 1, 8, grid)) return std::nullopt;
        return grid.sample(longitude, latitude, HeightInterpolation::Bilinear);
    }

    // ---- ElevationRegistry --------------------------------------------------------------------------

    unsigned ElevationRegistry::add(const std::shared_ptr<const ElevationSource>& source, double exaggeration)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(), [](const Entry& e) { return e.source.expired(); }), _entries.end());
        Entry entry;
        entry.id = _nextId++;
        entry.source = source;
        entry.exaggeration = exaggeration;
        _entries.push_back(entry);
        return entry.id;
    }

    void ElevationRegistry::remove(unsigned id)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(), [id](const Entry& e) { return e.id == id || e.source.expired(); }), _entries.end());
    }

    void ElevationRegistry::setEnabled(unsigned id, bool enabled)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (Entry& e : _entries)
            if (e.id == id) e.enabled = enabled;
    }

    std::optional<double> ElevationRegistry::heightAt(double longitude, double latitude) const
    {
        std::vector<Entry> entries;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            entries = _entries;
        }
        std::shared_ptr<const ElevationSource> best;
        double bestSpacing = 0.0, bestExaggeration = 1.0;
        for (const Entry& e : entries)
        {
            if (!e.enabled) continue;
            std::shared_ptr<const ElevationSource> source = e.source.lock();
            if (!source || !source->covers(longitude, latitude)) continue;
            const double spacing = source->postSpacingMetres();
            if (!best || spacing < bestSpacing)
            {
                best = source;
                bestSpacing = spacing;
                bestExaggeration = e.exaggeration;
            }
        }
        if (!best) return std::nullopt;
        std::optional<double> h = best->heightAt(longitude, latitude);
        if (!h) return std::nullopt;
        return *h * bestExaggeration;
    }

    bool ElevationRegistry::hasTerrain() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const Entry& e : _entries)
            if (e.enabled && !e.source.expired()) return true;
        return false;
    }

    std::size_t ElevationRegistry::size() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::size_t alive = 0;
        for (const Entry& e : _entries)
            if (!e.source.expired()) ++alive;
        return alive;
    }
} // namespace ivevsg
