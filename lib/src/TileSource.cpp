#include <ivevsg/CrsTransform.h>
#include <ivevsg/TileSource.h>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal_alg.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>

#include <vsg/core/Array2D.h>
#include <vsg/io/read.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>

namespace ivevsg
{
    namespace
    {
        void ensureGdal()
        {
            ensureGdalRegistered(); // one registration for the whole process, whichever module gets there first
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

        bool overlaps(const vsg::dbox& a, const vsg::dbox& b)
        {
            return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y;
        }

        vsg::dbox intersect(const vsg::dbox& a, const vsg::dbox& b)
        {
            vsg::dbox box;
            if (!overlaps(a, b)) return box;
            box.min = vsg::dvec3(std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), 0.0);
            box.max = vsg::dvec3(std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), 0.0);
            return box;
        }
    } // namespace

    // ---- rasters -------------------------------------------------------------------------------------

    struct RasterTileSource::Impl
    {
        GDALDatasetH source = nullptr;   // the file as opened
        GDALDatasetH expanded = nullptr; // palette expanded to RGBA, when the file has a colour table
        GDALDatasetH warped = nullptr;   // the source warped into the grid's CRS (null when read directly)
        GDALDatasetH reader = nullptr;   // what tiles are read from: the warped VRT, or the source itself
        bool direct = false;             // the source already is in the grid's CRS: no warping, its mask band gives the alpha
        double geo[6] = {0, 1, 0, 0, 0, -1};
        int width = 0, height = 0;
        int bandMap[4] = {1, 2, 3, 4};
        std::mutex mutex; // GDAL datasets are not thread safe; the pager reads on several threads

        ~Impl()
        {
            if (warped) GDALClose(warped);
            if (expanded) GDALClose(expanded);
            if (source) GDALClose(source);
        }
    };

    RasterTileSource::~RasterTileSource() = default;

    std::shared_ptr<RasterTileSource> RasterTileSource::open(const std::string& path, const std::string& sourceCrs, int tileSizePixels, std::string& error)
    {
        ensureGdal();
        auto impl = std::make_unique<Impl>();
        CPLErrorReset();
        impl->source = GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!impl->source)
        {
            const char* message = CPLGetLastErrorMsg();
            error = "GDAL cannot open " + path + (message && *message ? std::string(": ") + message : "");
            return nullptr;
        }
        GDALDatasetH dataset = impl->source;
        if (GDALGetRasterCount(dataset) < 1)
        {
            error = path + " has no raster bands";
            return nullptr;
        }

        // the CRS: the override, else what the file declares (its projection, or that of its GCPs)
        std::string sourceWkt;
        std::string sourceLabel = sourceCrs;
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
            if (declared && *declared)
                sourceWkt = declared;
            else if (GDALGetGCPCount(dataset) > 0 && GDALGetGCPProjection(dataset) && *GDALGetGCPProjection(dataset))
                sourceWkt = GDALGetGCPProjection(dataset);
            if (sourceWkt.empty())
            {
                error = path + " names no coordinate reference system; choose the native CRS when adding it";
                return nullptr;
            }
            sourceLabel = sourceWkt;
        }
        double geo[6];
        if (GDALGetGeoTransform(dataset, geo) != CE_None && GDALGetGCPCount(dataset) == 0)
        {
            error = path + " is not georeferenced (no geotransform or ground control points)";
            return nullptr;
        }

        // a colour-table image is expanded to RGBA first; the warper only resamples real bands
        if (GDALGetRasterColorTable(GDALGetRasterBand(dataset, 1)))
        {
            const char* argv[] = {"-of", "VRT", "-expand", "rgba", nullptr};
            GDALTranslateOptions* translate = GDALTranslateOptionsNew(const_cast<char**>(argv), nullptr);
            impl->expanded = GDALTranslate("", dataset, translate, nullptr);
            GDALTranslateOptionsFree(translate);
            if (impl->expanded) dataset = impl->expanded;
        }

        const int bands = GDALGetRasterCount(dataset);
        int alphaBand = 0;
        for (int b = 1; b <= bands; ++b)
        {
            if (GDALGetRasterColorInterpretation(GDALGetRasterBand(dataset, b)) == GCI_AlphaBand)
            {
                alphaBand = b;
                break;
            }
        }
        const int colourBands = bands - (alphaBand ? 1 : 0);

        TileGrid grid = TileGrid::webMercatorQuad(tileSizePixels);
        const std::string targetWkt = wellKnownText(grid.crs);

        // a north-up raster already in the grid's CRS is read as it is (its mask band, which GDAL
        // derives from an alpha band or a nodata value, gives the alpha); anything else is warped
        // into the grid's CRS on demand, with an alpha band that is 0 outside the raster's footprint,
        // outside its own alpha and on its nodata value
        {
            OGRSpatialReference source, target;
            source.SetFromUserInput(sourceWkt.c_str());
            target.SetFromUserInput(targetWkt.c_str());
            double sourceGeo[6];
            if (source.IsSame(&target) && GDALGetGeoTransform(dataset, sourceGeo) == CE_None && sourceGeo[2] == 0.0 && sourceGeo[4] == 0.0 && sourceGeo[1] > 0.0 && sourceGeo[5] < 0.0)
            {
                impl->direct = true;
                impl->reader = dataset;
                std::copy(sourceGeo, sourceGeo + 6, impl->geo);
            }
        }
        if (!impl->direct)
        {
            GDALWarpOptions* warpOptions = GDALCreateWarpOptions();
            warpOptions->nSrcAlphaBand = alphaBand;
            warpOptions->nDstAlphaBand = bands + 1;
            warpOptions->papszWarpOptions = CSLSetNameValue(nullptr, "INIT_DEST", "0");
            CPLErrorReset();
            impl->warped = GDALAutoCreateWarpedVRTEx(dataset, sourceWkt.c_str(), targetWkt.c_str(), GRA_Bilinear, 0.125, warpOptions, nullptr);
            GDALDestroyWarpOptions(warpOptions);
            if (!impl->warped)
            {
                const char* message = CPLGetLastErrorMsg();
                error = "cannot warp " + path + " into " + grid.crs + (message && *message ? std::string(": ") + message : "");
                return nullptr;
            }
            if (GDALGetGeoTransform(impl->warped, impl->geo) != CE_None || impl->geo[1] <= 0.0 || impl->geo[5] >= 0.0)
            {
                error = "warped raster has no usable geotransform: " + path;
                return nullptr;
            }
            impl->reader = impl->warped;
        }
        impl->width = GDALGetRasterXSize(impl->reader);
        impl->height = GDALGetRasterYSize(impl->reader);
        if (colourBands >= 3)
        {
            impl->bandMap[0] = 1;
            impl->bandMap[1] = 2;
            impl->bandMap[2] = 3;
        }
        else
        {
            impl->bandMap[0] = impl->bandMap[1] = impl->bandMap[2] = 1;
        }
        impl->bandMap[3] = impl->direct ? 0 : bands + 1; // direct reads take the alpha from the mask band

        std::shared_ptr<RasterTileSource> source(new RasterTileSource());
        source->_path = path;
        source->_sourceCrs = sourceLabel;
        source->_grid = grid;
        source->_sourceWidth = GDALGetRasterXSize(impl->source);
        source->_sourceHeight = GDALGetRasterYSize(impl->source);
        source->_hasOverviews = GDALGetOverviewCount(GDALGetRasterBand(impl->source, 1)) > 0;
        source->_metersPerPixel = impl->geo[1];
        const vsg::dbox footprint(vsg::dvec3(impl->geo[0], impl->geo[3] + impl->geo[5] * impl->height, 0.0),
                                  vsg::dvec3(impl->geo[0] + impl->geo[1] * impl->width, impl->geo[3], 0.0));
        source->_bounds = intersect(footprint, grid.extent);
        if (!source->_bounds.valid())
        {
            error = path + " lies entirely outside the " + grid.crs + " world";
            return nullptr;
        }
        source->_maxLevel = grid.levelForUnitsPerPixel(impl->geo[1]);
        source->_impl = std::move(impl);
        return source;
    }

    std::string RasterTileSource::description() const
    {
        return std::filesystem::path(_path).filename().string() + " " + std::to_string(_sourceWidth) + "x" + std::to_string(_sourceHeight) +
               (_hasOverviews ? "" : " (no overviews)") + ", " + std::to_string(_metersPerPixel) + " m/px" + (_impl && _impl->direct ? "" : " warped") +
               ", levels 0.." + std::to_string(_maxLevel);
    }

    bool RasterTileSource::mayHave(const TileIndex& tile) const
    {
        return _grid.contains(tile) && tile.level <= _maxLevel && overlaps(_grid.tileExtent(tile), _bounds);
    }

    vsg::ref_ptr<vsg::Data> RasterTileSource::readTile(const TileIndex& tile, vsg::ref_ptr<const vsg::Options>)
    {
        if (!mayHave(tile)) return {};
        const vsg::dbox extent = _grid.tileExtent(tile);
        const int size = _grid.tileSizePixels;
        Impl& impl = *_impl;

        // the tile's footprint in warped pixels, rows counted from the top
        const double col0 = (extent.min.x - impl.geo[0]) / impl.geo[1];
        const double col1 = (extent.max.x - impl.geo[0]) / impl.geo[1];
        const double row0 = (extent.max.y - impl.geo[3]) / impl.geo[5];
        const double row1 = (extent.min.y - impl.geo[3]) / impl.geo[5];
        const double c0 = std::max(col0, 0.0), c1 = std::min(col1, static_cast<double>(impl.width));
        const double r0 = std::max(row0, 0.0), r1 = std::min(row1, static_cast<double>(impl.height));
        if (c1 <= c0 || r1 <= r0) return {};

        // the part of the tile the intersection covers, in tile pixels
        const double sx = size / (col1 - col0), sy = size / (row1 - row0);
        const int dx0 = std::clamp(static_cast<int>(std::floor((c0 - col0) * sx + 1e-6)), 0, size);
        const int dx1 = std::clamp(static_cast<int>(std::ceil((c1 - col0) * sx - 1e-6)), 0, size);
        const int dy0 = std::clamp(static_cast<int>(std::floor((r0 - row0) * sy + 1e-6)), 0, size);
        const int dy1 = std::clamp(static_cast<int>(std::ceil((r1 - row0) * sy - 1e-6)), 0, size);
        const int dw = dx1 - dx0, dh = dy1 - dy0;
        if (dw <= 0 || dh <= 0) return {};

        auto image = vsg::ubvec4Array2D::create(static_cast<uint32_t>(size), static_cast<uint32_t>(size), vsg::ubvec4(0, 0, 0, 0), vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});

        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.bFloatingPointWindowValidity = TRUE;
        extra.dfXOff = c0;
        extra.dfYOff = r0;
        extra.dfXSize = c1 - c0;
        extra.dfYSize = r1 - r0;
        extra.eResampleAlg = ((c1 - c0) > dw) ? GRIORA_Average : GRIORA_Bilinear; // shrink: average, enlarge: bilinear

        unsigned char* destination = reinterpret_cast<unsigned char*>(image->data()) + (static_cast<std::size_t>(dy0) * size + dx0) * 4;
        const int x0 = static_cast<int>(std::floor(c0)), y0 = static_cast<int>(std::floor(r0));
        const int xs = std::max(1, static_cast<int>(std::ceil(c1)) - x0), ys = std::max(1, static_cast<int>(std::ceil(r1)) - y0);
        CPLErr err;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            if (impl.direct)
            {
                err = GDALDatasetRasterIOEx(impl.reader, GF_Read, x0, y0, xs, ys, destination, dw, dh, GDT_Byte, 3, impl.bandMap,
                                            4, static_cast<GSpacing>(size) * 4, 1, &extra);
                if (err == CE_None)
                {
                    GDALRasterBandH mask = GDALGetMaskBand(GDALGetRasterBand(impl.reader, 1));
                    err = GDALRasterIOEx(mask, GF_Read, x0, y0, xs, ys, destination + 3, dw, dh, GDT_Byte, 4, static_cast<GSpacing>(size) * 4, &extra);
                }
            }
            else
            {
                err = GDALDatasetRasterIOEx(impl.reader, GF_Read, x0, y0, xs, ys, destination, dw, dh, GDT_Byte, 4, impl.bandMap,
                                            4, static_cast<GSpacing>(size) * 4, 1, &extra);
            }
        }
        if (err != CE_None) return {};
        return image;
    }

    // ---- tile directories ----------------------------------------------------------------------------

    DirectoryTileSource::DirectoryTileSource(TileSetLayout layout) :
        _layout(std::move(layout))
    {
    }

    bool DirectoryTileSource::mayHave(const TileIndex& tile) const
    {
        if (!_layout.grid.contains(tile) || tile.level < _layout.minLevel || tile.level > _layout.maxLevel) return false;
        if (_layout.bounds.valid() && !overlaps(_layout.grid.tileExtent(tile), _layout.bounds)) return false;
        std::error_code ec;
        return std::filesystem::is_regular_file(_layout.tilePath(tile), ec);
    }

    vsg::ref_ptr<vsg::Data> DirectoryTileSource::readTile(const TileIndex& tile, vsg::ref_ptr<const vsg::Options> options)
    {
        if (!mayHave(tile)) return {};
        auto image = vsg::read_cast<vsg::Data>(vsg::Path(_layout.tilePath(tile)), options);
        return toRgba8(image);
    }

    vsg::ref_ptr<vsg::Data> toRgba8(vsg::ref_ptr<vsg::Data> image)
    {
        if (!image || image->height() == 0 || image->depth() != 1) return {};
        const uint32_t w = image->width(), h = image->height();
        vsg::ref_ptr<vsg::ubvec4Array2D> rgba;
        if (auto in = image.cast<vsg::ubvec4Array2D>())
        {
            rgba = in;
        }
        else
        {
            rgba = vsg::ubvec4Array2D::create(w, h, vsg::ubvec4(0, 0, 0, 255));
            if (auto in3 = image.cast<vsg::ubvec3Array2D>())
            {
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        const vsg::ubvec3& p = in3->at(x, y);
                        rgba->at(x, y) = vsg::ubvec4(p.r, p.g, p.b, 255);
                    }
            }
            else if (auto in2 = image.cast<vsg::ubvec2Array2D>())
            {
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        const vsg::ubvec2& p = in2->at(x, y);
                        rgba->at(x, y) = vsg::ubvec4(p.r, p.r, p.r, p.g);
                    }
            }
            else if (auto in1 = image.cast<vsg::ubyteArray2D>())
            {
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        const uint8_t v = in1->at(x, y);
                        rgba->at(x, y) = vsg::ubvec4(v, v, v, 255);
                    }
            }
            else
            {
                return {};
            }
        }
        // the mesh expects the first row at the north edge
        if ((rgba->properties.origin & vsg::BOTTOM_LEFT) == vsg::BOTTOM_LEFT && h > 1)
        {
            auto flipped = vsg::ubvec4Array2D::create(w, h, vsg::ubvec4(0, 0, 0, 0));
            for (uint32_t y = 0; y < h; ++y) std::copy_n(&rgba->at(0, h - 1 - y), w, &flipped->at(0, y));
            rgba = flipped;
        }
        rgba->properties.format = VK_FORMAT_R8G8B8A8_UNORM;
        rgba->properties.origin = vsg::TOP_LEFT;
        return rgba;
    }
} // namespace ivevsg
