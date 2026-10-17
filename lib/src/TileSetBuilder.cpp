#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TileSetBuilder.h>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <osg/CoordinateSystemNode>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osgDB/Options>
#include <osgDB/WriteFile>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace ivevsg
{
    void TileSetSettings::setDetailExtentDegrees(double minLongitude, double minLatitude, double maxLongitude, double maxLatitude)
    {
        MercatorProjection projection;
        const vsg::dvec3 a = projection.geoToWorld(GeoPoint{minLongitude, minLatitude, 0.0});
        const vsg::dvec3 b = projection.geoToWorld(GeoPoint{maxLongitude, maxLatitude, 0.0});
        detailExtent = vsg::dbox(vsg::dvec3(std::min(a.x, b.x), std::min(a.y, b.y), 0.0), vsg::dvec3(std::max(a.x, b.x), std::max(a.y, b.y), 0.0));
    }

    double subdivisionPixelThreshold(int tileSizePixels, double detailBias)
    {
        // a square tile's bounding sphere spans sqrt(2) tile sides; divide by the bias so that a larger
        // bias makes the hand-over happen while the tile is still smaller on screen
        return tileSizePixels * std::sqrt(2.0) / std::max(detailBias, 1e-3);
    }

    namespace
    {
        bool overlaps(const vsg::dbox& a, const vsg::dbox& b)
        {
            return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y;
        }
    } // namespace

    int subdivisionLimit(const TileIndex& tile, const TileSetSettings& settings, const TileScheme& scheme, int resolvedMaxLevel)
    {
        int limit = resolvedMaxLevel;
        if (settings.detailExtent.valid() && settings.detailMaxLevel > limit && overlaps(scheme.tileExtent(tile), settings.detailExtent))
        {
            limit = settings.detailMaxLevel;
        }
        return limit;
    }

    namespace
    {
        /// GDAL-backed source raster in EPSG:3857 metres. One per worker thread.
        class SourceRaster
        {
        public:
            explicit SourceRaster(const std::string& path)
            {
                _dataset = GDALDataset::FromHandle(GDALOpen(path.c_str(), GA_ReadOnly));
                if (!_dataset) return;
                if (_dataset->GetGeoTransform(_geo) != CE_None)
                {
                    GDALClose(_dataset);
                    _dataset = nullptr;
                    return;
                }
                _width = _dataset->GetRasterXSize();
                _height = _dataset->GetRasterYSize();
                _bands = _dataset->GetRasterCount();
            }
            ~SourceRaster()
            {
                if (_dataset) GDALClose(_dataset);
            }
            SourceRaster(const SourceRaster&) = delete;
            SourceRaster& operator=(const SourceRaster&) = delete;

            bool valid() const { return _dataset != nullptr && _bands >= 1 && _geo[1] > 0.0 && _geo[5] < 0.0; }
            double metersPerPixel() const { return _geo[1]; }

            /// Resamples the raster covering `extent` into a size x size RGB buffer (rows north to south).
            /// Returns false when the extent lies entirely outside the raster.
            bool read(const vsg::dbox& extent, int size, std::vector<unsigned char>& rgb) const
            {
                rgb.assign(static_cast<std::size_t>(size) * size * 3, 0);

                // the tile's footprint in source pixels, rows counted from the top
                const double col0 = (extent.min.x - _geo[0]) / _geo[1];
                const double col1 = (extent.max.x - _geo[0]) / _geo[1];
                const double row0 = (extent.max.y - _geo[3]) / _geo[5];
                const double row1 = (extent.min.y - _geo[3]) / _geo[5];

                const double c0 = std::max(col0, 0.0), c1 = std::min(col1, static_cast<double>(_width));
                const double r0 = std::max(row0, 0.0), r1 = std::min(row1, static_cast<double>(_height));
                if (c1 <= c0 || r1 <= r0) return false;

                // the part of the tile the intersection covers, in tile pixels
                const double sx = size / (col1 - col0), sy = size / (row1 - row0);
                const int dx0 = std::clamp(static_cast<int>(std::floor((c0 - col0) * sx + 1e-6)), 0, size);
                const int dx1 = std::clamp(static_cast<int>(std::ceil((c1 - col0) * sx - 1e-6)), 0, size);
                const int dy0 = std::clamp(static_cast<int>(std::floor((r0 - row0) * sy + 1e-6)), 0, size);
                const int dy1 = std::clamp(static_cast<int>(std::ceil((r1 - row0) * sy - 1e-6)), 0, size);
                const int dw = dx1 - dx0, dh = dy1 - dy0;
                if (dw <= 0 || dh <= 0) return false;

                GDALRasterIOExtraArg extra;
                INIT_RASTERIO_EXTRA_ARG(extra);
                extra.bFloatingPointWindowValidity = TRUE;
                extra.dfXOff = c0;
                extra.dfYOff = r0;
                extra.dfXSize = c1 - c0;
                extra.dfYSize = r1 - r0;
                extra.eResampleAlg = ((c1 - c0) > dw) ? GRIORA_Average : GRIORA_Cubic; // shrink: average, enlarge: cubic

                const int bandMap[3] = {1, std::min(2, _bands), std::min(3, _bands)};
                unsigned char* destination = rgb.data() + (static_cast<std::size_t>(dy0) * size + dx0) * 3;
                const int x0 = static_cast<int>(std::floor(c0)), y0 = static_cast<int>(std::floor(r0));
                const int xs = std::max(1, static_cast<int>(std::ceil(c1)) - x0), ys = std::max(1, static_cast<int>(std::ceil(r1)) - y0);
                const CPLErr err = _dataset->RasterIO(GF_Read, x0, y0, xs, ys, destination, dw, dh, GDT_Byte, 3, bandMap,
                                                      3, static_cast<GSpacing>(size) * 3, 1, &extra);
                return err == CE_None;
            }

        private:
            GDALDataset* _dataset = nullptr;
            double _geo[6] = {0, 1, 0, 0, 0, -1};
            int _width = 0, _height = 0, _bands = 0;
        };

        osg::ref_ptr<osg::Image> makeImage(const std::vector<unsigned char>& rgbTopDown, int size)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(size, size, 1, GL_RGB, GL_UNSIGNED_BYTE);
            image->setInternalTextureFormat(GL_RGB8);
            // OSG images start at the bottom-left; the buffer starts at the north (top) row
            for (int row = 0; row < size; ++row)
            {
                std::memcpy(image->data(0, size - 1 - row), rgbTopDown.data() + static_cast<std::size_t>(row) * size * 3, static_cast<std::size_t>(size) * 3);
            }
            return image;
        }

        osg::ref_ptr<osg::Texture2D> makeTexture(osg::ref_ptr<osg::Image> image)
        {
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            texture->setResizeNonPowerOfTwoHint(false);
            return texture;
        }

        /// Textured quad for one tile, in local coordinates around the tile centre for float precision.
        osg::ref_ptr<osg::Node> makeTileQuad(const TileScheme& scheme, const TileIndex& tile, osg::ref_ptr<osg::Image> image)
        {
            const float h = static_cast<float>(scheme.tileSideMetres(tile.level) * 0.5);

            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            vertices->push_back(osg::Vec3(-h, -h, 0.0f)); // south-west
            vertices->push_back(osg::Vec3(h, -h, 0.0f));  // south-east
            vertices->push_back(osg::Vec3(h, h, 0.0f));   // north-east
            vertices->push_back(osg::Vec3(-h, h, 0.0f));  // north-west
            osg::ref_ptr<osg::Vec2Array> texcoords = new osg::Vec2Array; // image row 0 is the south edge
            texcoords->push_back(osg::Vec2(0.0f, 0.0f));
            texcoords->push_back(osg::Vec2(1.0f, 0.0f));
            texcoords->push_back(osg::Vec2(1.0f, 1.0f));
            texcoords->push_back(osg::Vec2(0.0f, 1.0f));
            osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
            normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->push_back(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(vertices);
            geometry->setTexCoordArray(0, texcoords);
            geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
            geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
            geometry->addPrimitiveSet(new osg::DrawElementsUShort(GL_TRIANGLES, 6, std::array<GLushort, 6>{0, 1, 2, 0, 2, 3}.data()));

            geometry->getOrCreateStateSet()->setTextureAttributeAndModes(0, makeTexture(image), osg::StateAttribute::ON);

            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->addDrawable(geometry);

            const vsg::dvec3 centre = scheme.tileCentre(tile);
            osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform(osg::Matrixd::translate(centre.x, centre.y, centre.z));
            transform->addChild(geode);
            return transform;
        }

        /// Where a tile sits on the globe: its centre and bounding radius in EPSG:4978 metres.
        struct GlobeTileBound
        {
            osg::Vec3d centre;
            double radius = 0.0;
        };

        osg::Vec3d toEcef(const MercatorProjection& mercator, const GeocentricProjection& globe, double x, double y)
        {
            const vsg::dvec3 w = globe.geoToWorld(mercator.worldToGeo(vsg::dvec3(x, y, 0.0)));
            return {w.x, w.y, w.z};
        }

        GlobeTileBound globeTileBound(const TileScheme& scheme, const TileIndex& tile, int grid)
        {
            const MercatorProjection mercator;
            const GeocentricProjection globe;
            const vsg::dbox extent = scheme.tileExtent(tile);
            const vsg::dvec3 c = scheme.tileCentre(tile);
            GlobeTileBound bound;
            bound.centre = toEcef(mercator, globe, c.x, c.y);
            const int n = std::max(1, grid);
            for (int j = 0; j <= n; ++j)
                for (int i = 0; i <= n; ++i)
                {
                    const double x = extent.min.x + (extent.max.x - extent.min.x) * i / n;
                    const double y = extent.min.y + (extent.max.y - extent.min.y) * j / n;
                    bound.radius = std::max(bound.radius, (toEcef(mercator, globe, x, y) - bound.centre).length());
                }
            return bound;
        }

        /// The same tile as an N x N grid on the WGS84 ellipsoid, in geocentric metres around the tile's
        /// centre. Grid lines are evenly spaced in Mercator x / y so the texture maps linearly, exactly as
        /// on the flat quad; vertices are where those Mercator positions sit on the globe.
        osg::ref_ptr<osg::Node> makeTileGlobeGrid(const TileScheme& scheme, const TileIndex& tile, osg::ref_ptr<osg::Image> image, int grid)
        {
            const MercatorProjection mercator;
            const GeocentricProjection globe;
            const vsg::dbox extent = scheme.tileExtent(tile);
            const vsg::dvec3 c = scheme.tileCentre(tile);
            const osg::Vec3d centre = toEcef(mercator, globe, c.x, c.y);
            const int n = std::max(1, grid);

            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec2Array> texcoords = new osg::Vec2Array;
            for (int j = 0; j <= n; ++j)
                for (int i = 0; i <= n; ++i)
                {
                    const double x = extent.min.x + (extent.max.x - extent.min.x) * i / n;
                    const double y = extent.min.y + (extent.max.y - extent.min.y) * j / n; // south to north, like the image rows
                    const osg::Vec3d world = toEcef(mercator, globe, x, y);
                    const osg::Vec3d local = world - centre;
                    vertices->push_back(osg::Vec3(static_cast<float>(local.x()), static_cast<float>(local.y()), static_cast<float>(local.z())));
                    const vsg::dvec3 up = globe.upVector(vsg::dvec3(world.x(), world.y(), world.z()));
                    normals->push_back(osg::Vec3(static_cast<float>(up.x), static_cast<float>(up.y), static_cast<float>(up.z)));
                    texcoords->push_back(osg::Vec2(static_cast<float>(i) / n, static_cast<float>(j) / n));
                }
            osg::ref_ptr<osg::DrawElementsUShort> indices = new osg::DrawElementsUShort(GL_TRIANGLES);
            const auto index = [n](int i, int j) { return static_cast<GLushort>(j * (n + 1) + i); };
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i)
                {
                    indices->push_back(index(i, j));
                    indices->push_back(index(i + 1, j));
                    indices->push_back(index(i + 1, j + 1));
                    indices->push_back(index(i, j));
                    indices->push_back(index(i + 1, j + 1));
                    indices->push_back(index(i, j + 1));
                }
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->push_back(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(vertices);
            geometry->setTexCoordArray(0, texcoords);
            geometry->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
            geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
            geometry->addPrimitiveSet(indices);
            geometry->getOrCreateStateSet()->setTextureAttributeAndModes(0, makeTexture(image), osg::StateAttribute::ON);

            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->addDrawable(geometry);
            osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform(osg::Matrixd::translate(centre));
            transform->addChild(geode);
            return transform;
        }

        /// A tile that hands over to its children's file once it grows past the pixel threshold.
        osg::ref_ptr<osg::Node> makePagedTile(const TileScheme& scheme, const TileIndex& tile, osg::ref_ptr<osg::Node> quad, double pixelThreshold, const GlobeTileBound* globe)
        {
            const vsg::dvec3 centre = scheme.tileCentre(tile);
            osg::ref_ptr<osg::PagedLOD> plod = new osg::PagedLOD;
            plod->setCenterMode(osg::LOD::USER_DEFINED_CENTER);
            plod->setCenter(globe ? globe->centre : osg::Vec3d(centre.x, centre.y, centre.z));
            plod->setRadius(globe ? globe->radius : scheme.tileRadius(tile));
            plod->setRangeMode(osg::LOD::PIXEL_SIZE_ON_SCREEN);
            plod->addChild(quad, 0.0f, static_cast<float>(pixelThreshold)); // this tile while it is small
            plod->setFileName(1, TileScheme::relativePath(tile));           // its children once it is large
            plod->setRange(1, static_cast<float>(pixelThreshold), FLT_MAX);
            return plod;
        }

        std::string wellKnownText(const char* userInput)
        {
            OGRSpatialReference srs;
            if (srs.SetFromUserInput(userInput) != OGRERR_NONE) return userInput;
            char* wkt = nullptr;
            if (srs.exportToWkt(&wkt) != OGRERR_NONE || !wkt) return userInput;
            std::string text(wkt);
            CPLFree(wkt);
            return text;
        }

        /// Everything the workers share.
        struct Job
        {
            const TileSetSettings& settings;
            TileScheme scheme;
            int maxLevel = 0;
            double threshold = 0.0;
            std::filesystem::path outDir;
            osg::ref_ptr<osgDB::Options> writeOptions;
            std::vector<TileIndex> parents; // tiles that get a children file, any order
            std::atomic<std::size_t> next{0};
            std::atomic<std::size_t> filesWritten{0};
            std::atomic<std::size_t> tilesWritten{0};
            std::atomic<bool> stop{false};
            std::mutex mutex; // progress callback + error
            std::string error;

            Job(const TileSetSettings& s) : settings(s) {}

            bool needsFile(const TileIndex& tile) const { return tile.level < subdivisionLimit(tile, settings, scheme, maxLevel); }

            void plan(const TileIndex& tile)
            {
                if (!needsFile(tile)) return;
                parents.push_back(tile);
                for (const TileIndex& child : TileScheme::children(tile)) plan(child);
            }

            osg::ref_ptr<osg::Node> makeTile(const SourceRaster& source, const TileIndex& tile, std::vector<unsigned char>& rgb)
            {
                if (!source.read(scheme.tileExtent(tile), settings.tileSizePixels, rgb)) return {};
                ++tilesWritten;
                osg::ref_ptr<osg::Image> image = makeImage(rgb, settings.tileSizePixels);
                if (settings.geocentric)
                {
                    osg::ref_ptr<osg::Node> grid = makeTileGlobeGrid(scheme, tile, image, settings.geocentricGrid);
                    if (!needsFile(tile)) return grid;
                    const GlobeTileBound bound = globeTileBound(scheme, tile, settings.geocentricGrid);
                    return makePagedTile(scheme, tile, grid, threshold, &bound);
                }
                osg::ref_ptr<osg::Node> quad = makeTileQuad(scheme, tile, image);
                return needsFile(tile) ? makePagedTile(scheme, tile, quad, threshold, nullptr) : quad;
            }

            bool write(osg::Node& node, const std::filesystem::path& file)
            {
                std::error_code ec;
                std::filesystem::create_directories(file.parent_path(), ec);
                if (!osgDB::writeNodeFile(node, file.string(), writeOptions.get()))
                {
                    fail("failed to write " + file.string() + " (is the OSG ive plugin available?)");
                    return false;
                }
                const std::size_t done = ++filesWritten;
                if (settings.progress)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!settings.progress(done, parents.size() + 1))
                    {
                        stop = true;
                        return false;
                    }
                }
                return true;
            }

            void fail(const std::string& message)
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (error.empty()) error = message;
                stop = true;
            }

            /// Writes one parent's children file; false when nothing of the source lies under it.
            bool writeChildrenFile(const SourceRaster& source, const TileIndex& parent, std::vector<unsigned char>& rgb)
            {
                osg::ref_ptr<osg::Group> group = new osg::Group;
                for (const TileIndex& child : TileScheme::children(parent))
                {
                    if (auto node = makeTile(source, child, rgb)) group->addChild(node);
                }
                if (group->getNumChildren() == 0) return false;
                return write(*group, outDir / TileScheme::relativePath(parent));
            }

            void work()
            {
                SourceRaster source(settings.sourceRaster);
                if (!source.valid())
                {
                    fail("worker could not open " + settings.sourceRaster);
                    return;
                }
                std::vector<unsigned char> rgb;
                while (!stop)
                {
                    const std::size_t i = next++;
                    if (i >= parents.size()) break;
                    writeChildrenFile(source, parents[i], rgb);
                }
            }
        };
    } // namespace

    TileSetReport buildTileSet(const TileSetSettings& settings)
    {
        TileSetReport report;
        GDALAllRegister(); // once, on this thread, before any worker opens the source

        {
            SourceRaster probe(settings.sourceRaster);
            if (!probe.valid())
            {
                report.error = "cannot open " + settings.sourceRaster + " as a georeferenced raster";
                return report;
            }
            report.sourceMetersPerPixel = probe.metersPerPixel();
        }
        if (settings.tileSizePixels < 2)
        {
            report.error = "tile size must be at least 2 pixels";
            return report;
        }

        Job job(settings);
        job.scheme.tileSizePixels = settings.tileSizePixels;
        job.maxLevel = settings.maxLevel >= 0 ? settings.maxLevel : job.scheme.nativeLevelFor(report.sourceMetersPerPixel);
        job.threshold = subdivisionPixelThreshold(settings.tileSizePixels, settings.detailBias);
        job.outDir = settings.outputDirectory;
        if (settings.compressed) job.writeOptions = new osgDB::Options("compressed");
        job.plan({0, 0, 0});

        report.maxLevel = job.maxLevel;
        report.detailMaxLevel = (settings.detailExtent.valid() && settings.detailMaxLevel > job.maxLevel) ? settings.detailMaxLevel : job.maxLevel;
        report.filesPlanned = job.parents.size() + 1;
        report.compressed = settings.compressed;
        report.threads = settings.threads > 0 ? settings.threads : std::max(1u, std::thread::hardware_concurrency());

        SceneLoader::registerOsgPluginDirectory(SceneLoader::defaultOsgPluginDirectory()); // for the .ive writer

        std::error_code ec;
        std::filesystem::create_directories(job.outDir, ec);

        // root.ive on this thread first: it also loads the ive plugin before the workers need it
        {
            SourceRaster source(settings.sourceRaster);
            std::vector<unsigned char> rgb;
            osg::ref_ptr<osg::Node> root = job.makeTile(source, {0, 0, 0}, rgb);
            if (!root)
            {
                report.error = "the source raster does not cover any part of the world square";
                return report;
            }
            // name the frame at the root the way VirtualPlanetBuilder does: WKT, plus an ellipsoid
            // model when the coordinates are geocentric
            osg::ref_ptr<osg::CoordinateSystemNode> csn = new osg::CoordinateSystemNode;
            csn->setFormat("WKT");
            csn->setCoordinateSystem(wellKnownText(settings.geocentric ? "EPSG:4326" : "EPSG:3857"));
            if (settings.geocentric) csn->setEllipsoidModel(new osg::EllipsoidModel);
            csn->addChild(root);
            report.crs = settings.geocentric ? "EPSG:4978" : "EPSG:3857";
            report.rootFile = (job.outDir / "root.ive").string();
            if (!job.write(*csn, report.rootFile))
            {
                report.error = job.error;
                return report;
            }
        }

        std::vector<std::thread> workers;
        for (unsigned i = 1; i < report.threads; ++i) workers.emplace_back([&job] { job.work(); });
        job.work();
        for (auto& worker : workers) worker.join();

        report.filesWritten = job.filesWritten;
        report.tilesWritten = job.tilesWritten;
        report.error = job.error;
        return report;
    }
} // namespace ivevsg
