#include <ivevsg/ElevationSource.h>
#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/LoaderKeys.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/SceneBounds.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TerrainLayerReader.h>
#include <ivevsg/TileScheme.h>

#include <gdal_priv.h>
#include <gtest/gtest.h>
#include <ogr_spatialref.h>
#include <vsg/all.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace
{
    /// A synthetic DTED level 1 cell (1201 x 1201 posts, one degree) whose heights follow `height(lon, lat)`.
    /// GDAL's DTED driver writes it from an in-memory raster with the cell's geotransform.
    std::filesystem::path writeDted(const std::filesystem::path& file, int lon0, int lat0, const std::function<double(double, double)>& height, bool withVoid = false)
    {
        GDALAllRegister();
        GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
        GDALDriver* dted = GetGDALDriverManager()->GetDriverByName("DTED");
        if (!mem || !dted) return {};
        const int n = 1201;
        const double step = 1.0 / 1200.0;
        GDALDataset* image = mem->Create("", n, n, 1, GDT_Int16, nullptr);
        double geo[6] = {lon0 - 0.5 * step, step, 0.0, lat0 + 1.0 + 0.5 * step, 0.0, -step};
        image->SetGeoTransform(geo);
        OGRSpatialReference srs;
        srs.importFromEPSG(4326);
        char* wkt = nullptr;
        srs.exportToWkt(&wkt);
        image->SetProjection(wkt);
        CPLFree(wkt);
        image->GetRasterBand(1)->SetNoDataValue(-32767.0);
        std::vector<int16_t> posts(static_cast<std::size_t>(n) * n);
        for (int row = 0; row < n; ++row)
            for (int col = 0; col < n; ++col)
            {
                const double lon = lon0 + col * step, lat = lat0 + 1.0 - row * step;
                double h = height(lon, lat);
                if (withVoid && row < 12 && col < 12) h = -32767.0; // a void in the north-west corner
                posts[static_cast<std::size_t>(row) * n + col] = static_cast<int16_t>(std::lround(h));
            }
        if (image->RasterIO(GF_Write, 0, 0, n, n, posts.data(), n, n, GDT_Int16, 1, nullptr, 0, 0, 0, nullptr) != CE_None)
        {
            GDALClose(image);
            return {};
        }
        std::filesystem::create_directories(file.parent_path());
        GDALDataset* out = dted->CreateCopy(file.string().c_str(), image, FALSE, nullptr, nullptr, nullptr);
        const bool ok = out != nullptr;
        if (out) GDALClose(out);
        GDALClose(image);
        std::error_code ec;
        std::filesystem::remove(file.string() + ".aux.xml", ec);
        return ok ? file : std::filesystem::path();
    }

    /// Float32 DEM as a GeoTIFF in a projected CRS (EPSG:3857), bounds in metres.
    std::filesystem::path writeMercatorDem(const std::filesystem::path& file, const vsg::dbox& bounds, int size, float constantHeight)
    {
        GDALAllRegister();
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        if (!driver) return {};
        GDALDataset* dataset = driver->Create(file.string().c_str(), size, size, 1, GDT_Float32, nullptr);
        if (!dataset) return {};
        double geo[6] = {bounds.min.x, (bounds.max.x - bounds.min.x) / size, 0.0, bounds.max.y, 0.0, -(bounds.max.y - bounds.min.y) / size};
        dataset->SetGeoTransform(geo);
        OGRSpatialReference srs;
        srs.importFromEPSG(3857);
        char* wkt = nullptr;
        srs.exportToWkt(&wkt);
        dataset->SetProjection(wkt);
        CPLFree(wkt);
        std::vector<float> heights(static_cast<std::size_t>(size) * size, constantHeight);
        const CPLErr written = dataset->RasterIO(GF_Write, 0, 0, size, size, heights.data(), size, size, GDT_Float32, 1, nullptr, 0, 0, 0, nullptr);
        GDALClose(dataset);
        return written == CE_None ? file : std::filesystem::path();
    }

    // the planar ramp every test uses: bilinear and Catmull-Rom interpolation reproduce it exactly between posts
    double ramp(double lon, double lat) { return 100.0 + 1000.0 * (lon - 20.0) + 500.0 * (lat - 10.0); }

    struct Census : public vsg::ConstVisitor
    {
        int pagedLODs = 0, cullNodes = 0, draws = 0;
        std::vector<const vsg::PagedLOD*> plods;
        void apply(const vsg::Node& node) override { node.traverse(*this); }
        void apply(const vsg::PagedLOD& plod) override
        {
            ++pagedLODs;
            plods.push_back(&plod);
            plod.traverse(*this);
        }
        void apply(const vsg::CullNode& cull) override
        {
            ++cullNodes;
            cull.traverse(*this);
        }
        void apply(const vsg::VertexIndexDraw&) override { ++draws; }
    };

    Census census(const vsg::Node& node)
    {
        Census c;
        node.accept(c);
        return c;
    }

    struct WorldVertices : public vsg::ConstVisitor
    {
        std::vector<vsg::dmat4> stack{vsg::dmat4()};
        std::vector<vsg::dvec3> positions;
        void apply(const vsg::Node& node) override { node.traverse(*this); }
        void apply(const vsg::MatrixTransform& transform) override
        {
            stack.push_back(stack.back() * transform.matrix);
            transform.traverse(*this);
            stack.pop_back();
        }
        void apply(const vsg::VertexIndexDraw& draw) override
        {
            if (draw.arrays.empty() || !draw.arrays[0]->data) return;
            if (auto vertices = draw.arrays[0]->data.cast<vsg::vec3Array>())
                for (const vsg::vec3& v : *vertices) positions.push_back(stack.back() * vsg::dvec3(v));
        }
    };

    std::vector<vsg::dvec3> worldVertices(const vsg::Node& node)
    {
        WorldVertices visitor;
        node.accept(visitor);
        return visitor.positions;
    }

    struct FindTextures : public vsg::ConstVisitor
    {
        std::vector<vsg::ref_ptr<vsg::ubvec4Array2D>> images;
        void apply(const vsg::Node& node) override { node.traverse(*this); }
        void apply(const vsg::StateGroup& group) override
        {
            for (const auto& command : group.stateCommands)
            {
                auto bind = command.cast<vsg::BindDescriptorSet>();
                if (!bind || !bind->descriptorSet) continue;
                for (const auto& descriptor : bind->descriptorSet->descriptors)
                {
                    auto image = descriptor.cast<vsg::DescriptorImage>();
                    if (!image) continue;
                    for (const auto& info : image->imageInfoList)
                        if (info && info->imageView && info->imageView->image)
                            if (auto data = info->imageView->image->data.cast<vsg::ubvec4Array2D>(); data && data->width() > 1) images.push_back(data);
                }
            }
            group.traverse(*this);
        }
    };

    /// The id a terrain layer's virtual child names carry ("ivevsg-terrain://<id>/z/x/y").
    std::string layerIdOf(const vsg::PagedLOD& plod)
    {
        const std::string name = plod.filename.string();
        const std::size_t start = name.find("://") + 3;
        return name.substr(start, name.find('/', start) - start);
    }

    class ElevationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            dir = std::filesystem::temp_directory_path() / ("ivevsg_elevation_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
        }
        void TearDown() override { std::filesystem::remove_all(dir); }

        ivevsg::SceneLoader loaderFor(const std::string& targetCrs)
        {
            ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
            settings.targetCrs = targetCrs;
            settings.quadTessellation = targetCrs == "EPSG:4978" ? 8 : 1;
            return ivevsg::SceneLoader(settings);
        }

        std::filesystem::path dir;
    };
} // namespace

TEST_F(ElevationTest, OpensADtedCellAndDescribesIt)
{
    const std::filesystem::path cell = writeDted(dir / "n10e020.dt1", 20, 10, ramp);
    ASSERT_FALSE(cell.empty()) << "GDAL could not write a DTED cell";
    EXPECT_TRUE(ivevsg::ElevationSource::looksLikeElevation(cell.string()));
    EXPECT_FALSE(ivevsg::ElevationSource::looksLikeElevation((dir / "x.tif").string()));

    std::string error;
    std::shared_ptr<ivevsg::ElevationSource> source = ivevsg::ElevationSource::open(cell.string(), "", error);
    ASSERT_TRUE(source) << error;
    EXPECT_EQ(source->width(), 1201);
    EXPECT_EQ(source->height(), 1201);
    EXPECT_NEAR(source->postSpacingLat(), 1.0 / 1200.0, 1e-9);
    EXPECT_NEAR(source->postSpacingLon(), 1.0 / 1200.0, 1e-9);
    // the finer axis at latitude 10.5: 92.77 m along the meridian, 91.2 m along the parallel
    EXPECT_NEAR(source->postSpacingMetres(), 111319.49 / 1200.0 * std::cos(10.5 * 3.14159265358979 / 180.0), 0.5);
    const ivevsg::GeoExtent& e = source->extent();
    EXPECT_NEAR(e.minLongitude, 20.0 - 0.5 / 1200.0, 1e-6);
    EXPECT_NEAR(e.maxLongitude, 21.0 + 0.5 / 1200.0, 1e-6);
    EXPECT_NEAR(e.minLatitude, 10.0 - 0.5 / 1200.0, 1e-6);
    EXPECT_NEAR(e.maxLatitude, 11.0 + 0.5 / 1200.0, 1e-6);
    EXPECT_TRUE(source->covers(20.5, 10.5));
    EXPECT_FALSE(source->covers(22.0, 10.5));
    EXPECT_FALSE(source->sourceCrs().empty());
    EXPECT_LT(source->minHeight(), 150.0);
    EXPECT_GT(source->maxHeight(), 1500.0);
    EXPECT_NE(source->description().find("1201x1201"), std::string::npos);
}

TEST_F(ElevationTest, SamplesExactlyAtPostsAndLinearlyBetweenThem)
{
    const std::filesystem::path cell = writeDted(dir / "n10e020.dt1", 20, 10, ramp);
    ASSERT_FALSE(cell.empty());
    std::string error;
    std::shared_ptr<ivevsg::ElevationSource> source = ivevsg::ElevationSource::open(cell.string(), "", error);
    ASSERT_TRUE(source) << error;

    // a post: column 100, row 200 from the north
    const double postLon = 20.0 + 100.0 / 1200.0, postLat = 11.0 - 200.0 / 1200.0;
    std::optional<double> atPost = source->heightAt(postLon, postLat);
    ASSERT_TRUE(atPost);
    EXPECT_NEAR(*atPost, ramp(postLon, postLat), 0.51) << "int16 posts round to a metre";
    // between posts the ramp is reproduced by bilinear interpolation
    std::optional<double> between = source->heightAt(20.5, 10.5);
    ASSERT_TRUE(between);
    EXPECT_NEAR(*between, 850.0, 0.75);
    std::optional<double> nearEdge = source->heightAt(20.9999, 10.0001);
    ASSERT_TRUE(nearEdge);
    EXPECT_NEAR(*nearEdge, ramp(20.9999, 10.0001), 1.0);
    EXPECT_FALSE(source->heightAt(19.0, 10.5)) << "outside the cell";
}

TEST_F(ElevationTest, ReadsWindowsAtNativeAndReducedResolution)
{
    const std::filesystem::path cell = writeDted(dir / "n10e020.dt1", 20, 10, ramp, true);
    ASSERT_FALSE(cell.empty());
    std::string error;
    std::shared_ptr<ivevsg::ElevationSource> source = ivevsg::ElevationSource::open(cell.string(), "", error);
    ASSERT_TRUE(source) << error;

    // a tenth of a degree with a margin of two posts: native posts come back
    ivevsg::HeightGrid native;
    ASSERT_TRUE(source->readWindow(ivevsg::GeoExtent{20.4, 10.4, 20.5, 10.5}, 2, 10000, native));
    EXPECT_GE(native.width, 121 + 4);
    EXPECT_LE(native.width, 121 + 6);
    EXPECT_NEAR(native.dLon, 1.0 / 1200.0, 1e-9);
    EXPECT_NEAR(native.dLat, 1.0 / 1200.0, 1e-9);
    EXPECT_LE(native.lon0, 20.4);
    EXPECT_GE(native.lat0, 10.5);
    EXPECT_NEAR(native.sample(20.45, 10.45, ivevsg::HeightInterpolation::Bilinear), ramp(20.45, 10.45), 0.75);
    EXPECT_NEAR(native.sample(20.45, 10.45, ivevsg::HeightInterpolation::Bicubic), ramp(20.45, 10.45), 0.75) << "Catmull-Rom reproduces a plane";
    const ivevsg::GeoExtent footprint = native.extent();
    EXPECT_LE(footprint.minLongitude, 20.4);
    EXPECT_GE(footprint.maxLatitude, 10.5);

    // the whole cell capped at 64 posts: averaged down, spacing widened accordingly, still the ramp
    ivevsg::HeightGrid coarse;
    ASSERT_TRUE(source->readWindow(source->extent(), 0, 64, coarse));
    EXPECT_LE(coarse.width, 64);
    EXPECT_LE(coarse.height, 64);
    EXPECT_GE(coarse.width, 60);
    EXPECT_NEAR(coarse.dLon, 1201.0 / 1200.0 / coarse.width, 1e-6);
    EXPECT_NEAR(coarse.sample(20.5, 10.5, ivevsg::HeightInterpolation::Bilinear), 850.0, 6.0);

    // the void in the north-west corner reads as NaN, samples as 0
    ivevsg::HeightGrid corner;
    ASSERT_TRUE(source->readWindow(ivevsg::GeoExtent{20.0, 10.995, 20.003, 11.0}, 0, 10000, corner));
    EXPECT_TRUE(corner.isVoid(0, 0));
    EXPECT_EQ(corner.at(0, 0), 0.0f);
    EXPECT_EQ(corner.sample(20.0, 11.0, ivevsg::HeightInterpolation::Nearest), 0.0);
}

TEST(HeightGrid, InterpolationModesDifferOnACurveAndAgreeAtPosts)
{
    // h = col^2 along x, constant along y: 8 x 4 posts, one degree apart
    ivevsg::HeightGrid grid;
    grid.width = 8;
    grid.height = 4;
    grid.lon0 = 0.0;
    grid.lat0 = 3.0;
    grid.dLon = grid.dLat = 1.0;
    grid.heights.resize(32);
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 8; ++col) grid.heights[static_cast<std::size_t>(row) * 8 + col] = static_cast<float>(col * col);
    ASSERT_TRUE(grid.valid());
    for (auto mode : {ivevsg::HeightInterpolation::Nearest, ivevsg::HeightInterpolation::Bilinear, ivevsg::HeightInterpolation::Bicubic})
        EXPECT_NEAR(grid.sample(3.0, 1.0, mode), 9.0, 1e-6) << "at a post every mode returns the post";
    EXPECT_EQ(grid.sample(3.4, 1.0, ivevsg::HeightInterpolation::Nearest), 9.0);
    EXPECT_EQ(grid.sample(3.6, 1.0, ivevsg::HeightInterpolation::Nearest), 16.0);
    const double trueValue = 3.5 * 3.5; // 12.25
    const double bilinear = grid.sample(3.5, 1.0, ivevsg::HeightInterpolation::Bilinear);
    const double bicubic = grid.sample(3.5, 1.0, ivevsg::HeightInterpolation::Bicubic);
    EXPECT_NEAR(bilinear, 12.5, 1e-6);
    EXPECT_NEAR(bicubic, trueValue, 1e-6) << "Catmull-Rom reproduces quadratics";
    EXPECT_LT(std::abs(bicubic - trueValue), std::abs(bilinear - trueValue));
    // clamped beyond the edges, never out of range
    EXPECT_NEAR(grid.sample(-5.0, 1.0, ivevsg::HeightInterpolation::Bicubic), 0.0, 1e-6);
    EXPECT_NEAR(grid.sample(50.0, 1.0, ivevsg::HeightInterpolation::Bilinear), 49.0, 1e-6);
}

TEST_F(ElevationTest, MosaicsADirectoryOfCells)
{
    ASSERT_FALSE(writeDted(dir / "cells" / "n10e020.dt1", 20, 10, ramp).empty());
    ASSERT_FALSE(writeDted(dir / "cells" / "n10e021.dt1", 21, 10, [](double lon, double lat) { return 3000.0 + 100.0 * (lon - 21.0) + 100.0 * (lat - 10.0); }).empty());
    EXPECT_EQ(ivevsg::ElevationSource::elevationFilesIn((dir / "cells").string()).size(), 2u);
    EXPECT_TRUE(ivevsg::ElevationSource::looksLikeElevation((dir / "cells").string()));

    std::string error;
    std::shared_ptr<ivevsg::ElevationSource> source = ivevsg::ElevationSource::open((dir / "cells").string(), "", error);
    ASSERT_TRUE(source) << error;
    EXPECT_EQ(source->fileCount(), 2);
    EXPECT_NEAR(source->extent().minLongitude, 20.0 - 0.5 / 1200.0, 1e-6);
    EXPECT_NEAR(source->extent().maxLongitude, 22.0 + 0.5 / 1200.0, 1e-6);
    EXPECT_GE(source->width(), 2401);
    std::optional<double> west = source->heightAt(20.5, 10.5);
    std::optional<double> east = source->heightAt(21.5, 10.5);
    ASSERT_TRUE(west && east);
    EXPECT_NEAR(*west, 850.0, 1.0);
    EXPECT_NEAR(*east, 3100.0, 1.0);
}

TEST_F(ElevationTest, TerrainLayerBuildsPagedTilesOnTheGlobeAndTheMap)
{
    const std::filesystem::path cell = writeDted(dir / "n10e020.dt1", 20, 10, ramp);
    ASSERT_FALSE(cell.empty());

    ivevsg::SceneLoader globe = loaderFor("EPSG:4978");
    const ivevsg::LoadResult onGlobe = globe.load(cell.string());
    ASSERT_TRUE(onGlobe) << onGlobe.error;
    EXPECT_EQ(onGlobe.kind, "terrain");
    EXPECT_EQ(onGlobe.targetCrs, "EPSG:4978");
    EXPECT_FALSE(onGlobe.info.empty());
    const int expectedNative = ivevsg::TerrainLayerReader::nativeLevelFor(111319.49 / 1200.0 * std::cos(10.5 * 3.14159265358979 / 180.0) /*lon spacing is the finer one in Mercator metres? no: lat spacing / cos*/, 32);
    EXPECT_GE(onGlobe.nativeLevel, 13);
    EXPECT_LE(onGlobe.nativeLevel, 15);
    (void)expectedNative;
    EXPECT_EQ(onGlobe.maxLevel, onGlobe.nativeLevel + 4) << "four levels of interpolation past the data by default";
    EXPECT_NE(onGlobe.terrainRegistryId, 0u);

    const Census c = census(*onGlobe.node);
    EXPECT_GE(c.pagedLODs, 1);
    EXPECT_GE(c.draws, 1);
    FindTextures textures;
    onGlobe.node->accept(textures);
    ASSERT_FALSE(textures.images.empty());
    EXPECT_EQ(textures.images.front()->width(), 128u);
    EXPECT_EQ(textures.images.front()->at(64, 64).a, 255);

    // every vertex sits on the ramp above the ellipsoid, within the cell
    ivevsg::GeocentricProjection ecef;
    const std::vector<vsg::dvec3> vertices = worldVertices(*onGlobe.node);
    ASSERT_FALSE(vertices.empty());
    for (const vsg::dvec3& v : vertices)
    {
        const ivevsg::GeoPoint g = ecef.worldToGeo(v);
        EXPECT_GE(g.longitude, 19.99);
        EXPECT_LE(g.longitude, 21.01);
        EXPECT_GE(g.latitude, 9.99);
        EXPECT_LE(g.latitude, 11.01);
        EXPECT_NEAR(g.altitude, ramp(g.longitude, g.latitude), 40.0) << "root tiles average the posts: within a few tens of metres on a steep ramp";
    }

    // the registry knows the terrain while the node lives, and forgets it afterwards
    ASSERT_TRUE(globe.elevation());
    EXPECT_TRUE(globe.elevation()->hasTerrain());
    std::optional<double> h = globe.elevation()->heightAt(20.5, 10.5);
    ASSERT_TRUE(h);
    EXPECT_NEAR(*h, 850.0, 1.0);
    EXPECT_FALSE(globe.elevation()->heightAt(25.0, 10.5));

    // the same cell on the flat map: heights become z
    ivevsg::SceneLoader map = loaderFor("EPSG:3857");
    const ivevsg::LoadResult onMap = map.load(cell.string());
    ASSERT_TRUE(onMap) << onMap.error;
    EXPECT_EQ(onMap.kind, "terrain");
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*onMap.node);
    ASSERT_TRUE(bounds.valid());
    ivevsg::MercatorProjection mercator;
    EXPECT_NEAR(bounds.min.x, mercator.geoToWorld(ivevsg::GeoPoint{20.0, 10.0, 0.0}).x, 100.0);
    EXPECT_NEAR(bounds.max.x, mercator.geoToWorld(ivevsg::GeoPoint{21.0, 10.0, 0.0}).x, 100.0);
    EXPECT_GT(bounds.min.z, 50.0);
    EXPECT_LT(bounds.max.z, 1700.0);
    EXPECT_GT(bounds.max.z - bounds.min.z, 1000.0);
}

TEST_F(ElevationTest, RegistryForgetsATerrainLayerOnceItsNodeIsGone)
{
    const std::filesystem::path cell = writeDted(dir / "n10e020.dt1", 20, 10, ramp);
    ASSERT_FALSE(cell.empty());
    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");
    {
        const ivevsg::LoadResult loaded = loader.load(cell.string());
        ASSERT_TRUE(loaded) << loaded.error;
        EXPECT_EQ(loader.elevation()->size(), 1u);
        EXPECT_TRUE(loader.elevation()->hasTerrain());
    }
    EXPECT_EQ(loader.elevation()->size(), 0u);
    EXPECT_FALSE(loader.elevation()->hasTerrain());
    EXPECT_FALSE(loader.elevation()->heightAt(20.5, 10.5));
}

TEST_F(ElevationTest, TilesKeepSubdividingPastTheNativeLevelWithInterpolatedHeights)
{
    const std::filesystem::path cell = writeDted(dir / "n10e020.dt1", 20, 10, ramp);
    ASSERT_FALSE(cell.empty());
    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");
    ivevsg::LayerSource source;
    source.path = cell.string();
    source.options.terrain.oversampleLevels = 3;
    const ivevsg::LoadResult loaded = loader.load(source);
    ASSERT_TRUE(loaded) << loaded.error;
    EXPECT_EQ(loaded.maxLevel, loaded.nativeLevel + 3);
    const Census root = census(*loaded.node);
    ASSERT_FALSE(root.plods.empty());
    const std::string id = layerIdOf(*root.plods.front());
    const vsg::ref_ptr<vsg::Options> options = root.plods.front()->options;
    ASSERT_TRUE(options);

    ivevsg::MercatorProjection mercator;
    ivevsg::TileScheme scheme;
    const vsg::dvec3 centre = mercator.geoToWorld(ivevsg::GeoPoint{20.5, 10.5, 0.0});
    auto childrenOf = [&](int level) {
        const ivevsg::TileIndex tile = scheme.tileAt(centre.x, centre.y, level);
        const std::string name = "ivevsg-terrain://" + id + "/" + std::to_string(tile.level) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y);
        return vsg::read_cast<vsg::Node>(vsg::Path(name), options);
    };

    // two levels past the native resolution: still PagedLODs, heights interpolated along the ramp
    auto deep = childrenOf(loaded.nativeLevel + 1);
    ASSERT_TRUE(deep);
    const Census deepCensus = census(*deep);
    EXPECT_EQ(deepCensus.draws, 4);
    EXPECT_EQ(deepCensus.pagedLODs, 4) << "one level short of the cap: the tiles still page";
    for (const vsg::dvec3& v : worldVertices(*deep))
    {
        const ivevsg::GeoPoint g = mercator.worldToGeo(v);
        EXPECT_NEAR(v.z, ramp(g.longitude, g.latitude), 1.0) << "between posts the bicubic estimate follows the ramp, no steps";
    }
    // the last level: leaves
    auto last = childrenOf(loaded.maxLevel - 1);
    ASSERT_TRUE(last);
    const Census lastCensus = census(*last);
    EXPECT_EQ(lastCensus.draws, 4);
    EXPECT_EQ(lastCensus.pagedLODs, 0);
    EXPECT_EQ(lastCensus.cullNodes, 4);
    // and the children of a leaf are an empty group (nothing to page)
    EXPECT_TRUE(childrenOf(loaded.maxLevel));

    // nearest-neighbour sampling, by contrast, shows the posts as steps past the native level
    ivevsg::LayerSource stepped = source;
    stepped.options.terrain.interpolation = ivevsg::HeightInterpolation::Nearest;
    const ivevsg::LoadResult steppedLoad = loader.load(stepped);
    ASSERT_TRUE(steppedLoad) << steppedLoad.error;
    const Census steppedRoot = census(*steppedLoad.node);
    const std::string steppedId = layerIdOf(*steppedRoot.plods.front());
    const ivevsg::TileIndex tile = scheme.tileAt(centre.x, centre.y, loaded.nativeLevel + 2);
    auto steppedChildren = vsg::read_cast<vsg::Node>(vsg::Path("ivevsg-terrain://" + steppedId + "/" + std::to_string(tile.level) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y)), steppedRoot.plods.front()->options);
    ASSERT_TRUE(steppedChildren);
    double worst = 0.0;
    for (const vsg::dvec3& v : worldVertices(*steppedChildren))
    {
        const ivevsg::GeoPoint g = mercator.worldToGeo(v);
        worst = std::max(worst, std::abs(v.z - ramp(g.longitude, g.latitude)));
    }
    EXPECT_GT(worst, 0.3) << "nearest keeps a post's value across its cell";
}

TEST_F(ElevationTest, ExaggerationCapAndForcedKindApply)
{
    const std::filesystem::path cell = writeDted(dir / "n10e020.dt1", 20, 10, ramp);
    ASSERT_FALSE(cell.empty());
    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");

    ivevsg::LayerSource tall;
    tall.path = cell.string();
    tall.options.terrain.exaggeration = 2.0;
    tall.options.detailLevelCap = 12;
    const ivevsg::LoadResult loaded = loader.load(tall);
    ASSERT_TRUE(loaded) << loaded.error;
    EXPECT_EQ(loaded.maxLevel, 12) << "the detail cap wins over native + oversample";
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*loaded.node);
    EXPECT_GT(bounds.max.z, 2800.0) << "the 1600 m top of the ramp, doubled";
    std::optional<double> h = loader.elevation()->heightAt(20.5, 10.5);
    ASSERT_TRUE(h);
    EXPECT_NEAR(*h, 1700.0, 2.0) << "the registry reports exaggerated heights, so draping and the camera match the mesh";

    // a projected GeoTIFF DEM is imagery by default and terrain when asked
    const vsg::dbox metres(vsg::dvec3(2.2e6, 1.1e6, 0.0), vsg::dvec3(2.4e6, 1.3e6, 0.0));
    const std::filesystem::path dem = writeMercatorDem(dir / "dem.tif", metres, 64, 500.0f);
    ASSERT_FALSE(dem.empty());
    const ivevsg::LoadResult asImage = loader.load(dem.string());
    ASSERT_TRUE(asImage) << asImage.error;
    EXPECT_EQ(asImage.kind, "raster");
    ivevsg::LayerSource asTerrain;
    asTerrain.path = dem.string();
    asTerrain.options.kind = ivevsg::LayerKind::Terrain;
    const ivevsg::LoadResult terrain = loader.load(asTerrain);
    ASSERT_TRUE(terrain) << terrain.error;
    EXPECT_EQ(terrain.kind, "terrain");
    const vsg::dbox demBounds = ivevsg::computeGeometryBounds(*terrain.node);
    EXPECT_NEAR(demBounds.min.z, 500.0, 2.0);
    EXPECT_NEAR(demBounds.max.z, 500.0, 2.0);
    EXPECT_NEAR(demBounds.min.x, metres.min.x, 2000.0) << "warped back from geographic to the Mercator footprint";
}

TEST(TerrainLayerReader, NativeLevelFollowsPostSpacingAndMeshDensity)
{
    // level 0 tile = 40 075 km across; 32 cells -> 1 252 km per cell; halve per level until a cell is a post
    EXPECT_EQ(ivevsg::TerrainLayerReader::nativeLevelFor(2.0e6, 32), 0);    // posts coarser than a level-0 cell
    EXPECT_EQ(ivevsg::TerrainLayerReader::nativeLevelFor(700000.0, 32), 1); // between a level-0 and a level-1 cell
    EXPECT_EQ(ivevsg::TerrainLayerReader::nativeLevelFor(90.0, 32), 14);   // DTED level 1 (3 arcsec)
    EXPECT_EQ(ivevsg::TerrainLayerReader::nativeLevelFor(30.0, 32), 16);   // DTED level 2 (1 arcsec)
    EXPECT_EQ(ivevsg::TerrainLayerReader::nativeLevelFor(900.0, 32), 11);  // DTED level 0 (30 arcsec)
    EXPECT_EQ(ivevsg::TerrainLayerReader::nativeLevelFor(90.0, 64), 13) << "a denser mesh reaches the posts a level sooner";
    EXPECT_EQ(ivevsg::TerrainLayerReader::nativeLevelFor(0.0, 32), 0);
}
