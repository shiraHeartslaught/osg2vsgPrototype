#include <ivevsg/CrsTransform.h>
#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/SceneBounds.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TileLayerReader.h>
#include <ivevsg/TileSource.h>

#include <gdal_priv.h>
#include <gtest/gtest.h>
#include <ogr_spatialref.h>
#include <vsg/all.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr double kE = 20037508.342789244;

    std::string wkt(int epsg)
    {
        OGRSpatialReference srs;
        srs.importFromEPSG(epsg);
        char* text = nullptr;
        srs.exportToWkt(&text);
        std::string result = text ? text : "";
        CPLFree(text);
        return result;
    }

    /// RGB GeoTIFF with a solid colour, optionally georeferenced: bounds in the CRS's units.
    std::filesystem::path writeRaster(const std::filesystem::path& file, int epsg, const vsg::dbox& bounds, int width, int height,
                                      unsigned char r, unsigned char g, unsigned char b, bool withNoDataCorner = false)
    {
        GDALAllRegister();
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        if (!driver) return {};
        GDALDataset* dataset = driver->Create(file.string().c_str(), width, height, 3, GDT_Byte, nullptr);
        if (!dataset) return {};
        if (epsg > 0)
        {
            double geo[6] = {bounds.min.x, (bounds.max.x - bounds.min.x) / width, 0.0, bounds.max.y, 0.0, -(bounds.max.y - bounds.min.y) / height};
            dataset->SetGeoTransform(geo);
            dataset->SetProjection(wkt(epsg).c_str());
        }
        std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                unsigned char* px = rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
                const bool hole = withNoDataCorner && x < width / 4 && y < height / 4; // the north-west corner is nodata
                px[0] = hole ? 0 : r;
                px[1] = hole ? 0 : g;
                px[2] = hole ? 0 : b;
            }
        if (withNoDataCorner)
            for (int band = 1; band <= 3; ++band) dataset->GetRasterBand(band)->SetNoDataValue(0.0);
        const CPLErr err = dataset->RasterIO(GF_Write, 0, 0, width, height, rgb.data(), width, height, GDT_Byte, 3, nullptr, 3, static_cast<GSpacing>(width) * 3, 1, nullptr);
        GDALClose(dataset);
        return err == CE_None ? file : std::filesystem::path();
    }

    bool writePng(const std::filesystem::path& file, unsigned char r, unsigned char g, unsigned char b)
    {
        GDALAllRegister();
        GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
        GDALDriver* png = GetGDALDriverManager()->GetDriverByName("PNG");
        if (!mem || !png) return false;
        GDALDataset* image = mem->Create("", 64, 64, 3, GDT_Byte, nullptr);
        std::vector<unsigned char> rgb(64 * 64 * 3);
        for (std::size_t i = 0; i < 64 * 64; ++i)
        {
            rgb[i * 3] = r;
            rgb[i * 3 + 1] = g;
            rgb[i * 3 + 2] = b;
        }
        if (image->RasterIO(GF_Write, 0, 0, 64, 64, rgb.data(), 64, 64, GDT_Byte, 3, nullptr, 3, 64 * 3, 1, nullptr) != CE_None)
        {
            GDALClose(image);
            return false;
        }
        std::filesystem::create_directories(file.parent_path());
        GDALDataset* out = png->CreateCopy(file.string().c_str(), image, FALSE, nullptr, nullptr, nullptr);
        const bool ok = out != nullptr;
        if (out) GDALClose(out);
        GDALClose(image);
        return ok;
    }

    struct Census : public vsg::ConstVisitor
    {
        int pagedLODs = 0, cullNodes = 0, draws = 0, stateGroups = 0;
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
        void apply(const vsg::StateGroup& group) override
        {
            ++stateGroups;
            group.traverse(*this);
        }
        void apply(const vsg::VertexIndexDraw&) override { ++draws; }
    };

    Census census(const vsg::Node& node)
    {
        Census c;
        node.accept(c);
        return c;
    }

    /// The textures bound by the StateGroups below a node, in traversal order.
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
                    {
                        if (info && info->imageView && info->imageView->image)
                        {
                            auto data = info->imageView->image->data.cast<vsg::ubvec4Array2D>();
                            if (data && data->width() > 1) images.push_back(data); // skip the pipeline's 1 x 1 fallback texture
                        }
                    }
                }
            }
            group.traverse(*this);
        }
    };

    std::vector<vsg::ref_ptr<vsg::ubvec4Array2D>> texturesOf(const vsg::Node& node)
    {
        FindTextures finder;
        node.accept(finder);
        return finder.images;
    }

    /// World positions of every vertex below a node (transforms applied).
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

    class TileLayerTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            dir = std::filesystem::temp_directory_path() / ("ivevsg_tile_layer_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
        }
        void TearDown() override { std::filesystem::remove_all(dir); }

        ivevsg::SceneLoader loaderFor(const std::string& targetCrs, int tessellation = 1)
        {
            ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
            settings.targetCrs = targetCrs;
            settings.quadTessellation = tessellation;
            return ivevsg::SceneLoader(settings);
        }

        std::filesystem::path dir;
    };
} // namespace

TEST_F(TileLayerTest, AWorldRasterBecomesPagedTilesOnTheMercatorPlane)
{
    const std::filesystem::path tif = writeRaster(dir / "world.tif", 3857, vsg::dbox(vsg::dvec3(-kE, -kE, 0.0), vsg::dvec3(kE, kE, 0.0)), 512, 512, 200, 100, 50);
    ASSERT_FALSE(tif.empty());
    ASSERT_TRUE(ivevsg::TileLayerReader::handlesRaster(vsg::Path(tif.string())));

    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");
    const ivevsg::LoadResult result = loader.load(tif.string());
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(ivevsg::CrsTransform::displayName(result.sourceCrs), "EPSG:3857");
    EXPECT_TRUE(result.targetCrs.empty()) << "already in the display frame: shown as read";

    const Census c = census(*result.node);
    EXPECT_EQ(c.pagedLODs, 1) << "the 512 px world is level 0 with one level of children";
    EXPECT_EQ(c.draws, 1);
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*result.node);
    ASSERT_TRUE(bounds.valid());
    EXPECT_NEAR(bounds.min.x, -kE, 1.0);
    EXPECT_NEAR(bounds.max.x, kE, 1.0);
    EXPECT_NEAR(bounds.min.y, -kE, 1.0);
    EXPECT_NEAR(bounds.max.y, kE, 1.0);
    EXPECT_NEAR(bounds.max.z, 0.0, 1e-6);

    // the texture: 256 x 256 RGBA, the raster's colour, fully opaque
    const auto textures = texturesOf(*result.node);
    ASSERT_EQ(textures.size(), 1u);
    EXPECT_EQ(textures[0]->width(), 256u);
    EXPECT_EQ(textures[0]->height(), 256u);
    const vsg::ubvec4 texel = textures[0]->at(128, 128);
    EXPECT_NEAR(texel.r, 200, 2);
    EXPECT_NEAR(texel.g, 100, 2);
    EXPECT_NEAR(texel.b, 50, 2);
    EXPECT_EQ(texel.a, 255);

    // the children come back through the same reader with the PagedLOD's own options
    ASSERT_EQ(c.plods.size(), 1u);
    const vsg::PagedLOD& plod = *c.plods.front();
    ASSERT_TRUE(ivevsg::TileLayerReader::isVirtualTile(plod.filename));
    ASSERT_TRUE(plod.options);
    auto children = vsg::read_cast<vsg::Node>(plod.filename, plod.options);
    ASSERT_TRUE(children);
    const Census cc = census(*children);
    EXPECT_EQ(cc.draws, 4);
    EXPECT_EQ(cc.pagedLODs, 0) << "level 1 is the raster's own resolution: leaves";
    EXPECT_EQ(cc.cullNodes, 4);
    const vsg::dbox childBounds = ivevsg::computeGeometryBounds(*children);
    EXPECT_NEAR(childBounds.min.x, -kE, 1.0);
    EXPECT_NEAR(childBounds.max.y, kE, 1.0);
    EXPECT_EQ(loader.loadStats().requests, 2u);
}

TEST_F(TileLayerTest, PartialCoverageIsTransparentOutsideTheRaster)
{
    // the western half of the north-east quadrant
    const vsg::dbox extent(vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(kE * 0.5, kE, 0.0));
    const std::filesystem::path tif = writeRaster(dir / "quarter.tif", 3857, extent, 128, 256, 10, 20, 30);
    ASSERT_FALSE(tif.empty());

    std::string error;
    auto source = ivevsg::RasterTileSource::open(tif.string(), "", 256, error);
    ASSERT_TRUE(source) << error;
    EXPECT_EQ(source->maxLevel(), 1);
    EXPECT_NEAR(source->bounds().min.x, 0.0, 1.0);
    EXPECT_NEAR(source->bounds().max.x, kE * 0.5, 1.0);
    EXPECT_TRUE(source->mayHave({1, 1, 0}));
    EXPECT_FALSE(source->mayHave({1, 0, 1}));

    auto image = source->readTile({1, 1, 0}, {});
    ASSERT_TRUE(image);
    auto rgba = image.cast<vsg::ubvec4Array2D>();
    ASSERT_TRUE(rgba);
    EXPECT_EQ(rgba->at(64, 128).a, 255) << "inside the raster";
    EXPECT_EQ(rgba->at(64, 128).b, 30);
    EXPECT_EQ(rgba->at(192, 128).a, 0) << "outside the raster, in the same tile";
    EXPECT_FALSE(source->readTile({1, 0, 1}, {})) << "a tile the raster does not touch";

    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");
    const ivevsg::LoadResult result = loader.load(tif.string());
    ASSERT_TRUE(result) << result.error;
    const Census c = census(*result.node);
    EXPECT_EQ(c.draws, 1) << "the root is the one level-1 tile holding the raster";
    EXPECT_EQ(c.cullNodes, 1);
    EXPECT_EQ(c.pagedLODs, 0);
}

TEST_F(TileLayerTest, NoDataBecomesTransparent)
{
    const vsg::dbox extent(vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(kE, kE, 0.0));
    const std::filesystem::path tif = writeRaster(dir / "nodata.tif", 3857, extent, 256, 256, 90, 90, 90, true);
    ASSERT_FALSE(tif.empty());
    std::string error;
    auto source = ivevsg::RasterTileSource::open(tif.string(), "", 256, error);
    ASSERT_TRUE(source) << error;
    auto rgba = source->readTile({1, 1, 0}, {}).cast<vsg::ubvec4Array2D>();
    ASSERT_TRUE(rgba);
    EXPECT_EQ(rgba->at(20, 20).a, 0) << "the nodata corner";
    EXPECT_EQ(rgba->at(200, 200).a, 255);
    EXPECT_EQ(rgba->at(200, 200).r, 90);
}

TEST_F(TileLayerTest, AGeographicRasterIsWarpedIntoPlace)
{
    const vsg::dbox degrees(vsg::dvec3(30.0, 33.0, 0.0), vsg::dvec3(36.0, 37.0, 0.0));
    const std::filesystem::path tif = writeRaster(dir / "cyprus_4326.tif", 4326, degrees, 300, 200, 250, 250, 0);
    ASSERT_FALSE(tif.empty());

    std::string error;
    auto source = ivevsg::RasterTileSource::open(tif.string(), "", 256, error);
    ASSERT_TRUE(source) << error;
    EXPECT_EQ(ivevsg::CrsTransform::displayName(source->sourceCrs()), "EPSG:4326");
    const ivevsg::MercatorProjection mercator;
    const vsg::dvec3 sw = mercator.geoToWorld(ivevsg::GeoPoint{30.0, 33.0, 0.0});
    const vsg::dvec3 ne = mercator.geoToWorld(ivevsg::GeoPoint{36.0, 37.0, 0.0});
    EXPECT_NEAR(source->bounds().min.x, sw.x, 2000.0);
    EXPECT_NEAR(source->bounds().min.y, sw.y, 2000.0);
    EXPECT_NEAR(source->bounds().max.x, ne.x, 2000.0);
    EXPECT_NEAR(source->bounds().max.y, ne.y, 2000.0);
    EXPECT_GE(source->maxLevel(), 5);

    // the tile under the raster's centre is opaque there and transparent in a far corner
    const vsg::dvec3 centre = mercator.geoToWorld(ivevsg::GeoPoint{33.0, 35.0, 0.0});
    const ivevsg::TileIndex tile = source->grid().tileAt(centre.x, centre.y, 5);
    auto rgba = source->readTile(tile, {}).cast<vsg::ubvec4Array2D>();
    ASSERT_TRUE(rgba);
    const vsg::dbox footprint = source->grid().tileExtent(tile);
    const int px = static_cast<int>((centre.x - footprint.min.x) / (footprint.max.x - footprint.min.x) * 256.0);
    const int py = static_cast<int>((footprint.max.y - centre.y) / (footprint.max.y - footprint.min.y) * 256.0);
    EXPECT_EQ(rgba->at(px, py).a, 255);
    EXPECT_EQ(rgba->at(px, py).r, 250);

    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");
    const ivevsg::LoadResult result = loader.load(tif.string());
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(ivevsg::CrsTransform::displayName(result.sourceCrs), "EPSG:4326");
    EXPECT_EQ(result.targetCrs, "EPSG:3857");
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*result.node);
    ASSERT_TRUE(bounds.valid());
    EXPECT_LE(bounds.min.x, sw.x);
    EXPECT_GE(bounds.max.x, ne.x);
    EXPECT_LE(bounds.min.y, sw.y);
    EXPECT_GE(bounds.max.y, ne.y);
}

TEST_F(TileLayerTest, TilesBendOverTheEllipsoidOnTheGlobe)
{
    const vsg::dbox degrees(vsg::dvec3(30.0, 33.0, 0.0), vsg::dvec3(36.0, 37.0, 0.0));
    const std::filesystem::path tif = writeRaster(dir / "cyprus_globe.tif", 4326, degrees, 300, 200, 1, 2, 3);
    ASSERT_FALSE(tif.empty());

    ivevsg::SceneLoader loader = loaderFor("EPSG:4978", 8);
    const ivevsg::LoadResult result = loader.load(tif.string());
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.targetCrs, "EPSG:4978");

    WorldVertices vertices;
    result.node->accept(vertices);
    ASSERT_EQ(vertices.positions.size(), 81u) << "an 8 x 8 grid";
    const ivevsg::GeocentricProjection globe;
    for (const vsg::dvec3& p : vertices.positions)
    {
        const ivevsg::GeoPoint geo = globe.worldToGeo(p);
        EXPECT_NEAR(geo.altitude, 0.0, 1.0);
        EXPECT_GE(geo.latitude, 20.0);
        EXPECT_LE(geo.latitude, 50.0);
    }
    const Census c = census(*result.node);
    EXPECT_EQ(c.pagedLODs, 1);
    const vsg::dsphere& bound = c.plods.front()->bound;
    for (const vsg::dvec3& p : vertices.positions) EXPECT_LE(vsg::length(p - bound.center), bound.radius);
}

TEST_F(TileLayerTest, TmsAndXyzDirectoriesLoadWithTheRightRowOrder)
{
    // four level-1 tiles, each a different colour, named the TMS way (row 0 at the south)
    const std::filesystem::path tms = dir / "tms";
    ASSERT_TRUE(writePng(tms / "1/0/1.png", 255, 0, 0)); // north-west in TMS numbering
    ASSERT_TRUE(writePng(tms / "1/1/1.png", 0, 255, 0)); // north-east
    ASSERT_TRUE(writePng(tms / "1/0/0.png", 0, 0, 255)); // south-west
    ASSERT_TRUE(writePng(tms / "1/1/0.png", 255, 255, 0));
    std::ofstream(tms / "tilemapresource.xml") << R"(<TileMap version="1.0.0"><SRS>EPSG:3857</SRS>
<BoundingBox minx="-20037508.342789244" miny="-20037508.342789244" maxx="20037508.342789244" maxy="20037508.342789244"/>
<Origin x="-20037508.342789244" y="-20037508.342789244"/>
<TileFormat width="64" height="64" mime-type="image/png" extension="png"/>
<TileSets profile="mercator"><TileSet href="1" units-per-pixel="312999.0" order="1"/></TileSets></TileMap>)";

    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");
    ivevsg::LoadResult result = loader.load(tms.string());
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.sourceCrs, "EPSG:3857");
    Census c = census(*result.node);
    EXPECT_EQ(c.draws, 4);
    EXPECT_EQ(c.cullNodes, 4);
    EXPECT_EQ(c.pagedLODs, 0);
    auto textures = texturesOf(*result.node);
    ASSERT_EQ(textures.size(), 4u);
    // tiles are made in children() order: north-west first
    EXPECT_EQ(textures[0]->at(10, 10).r, 255);
    EXPECT_EQ(textures[0]->at(10, 10).g, 0);
    EXPECT_EQ(textures[2]->at(10, 10).b, 255) << "south-west";
    vsg::dbox bounds = ivevsg::computeGeometryBounds(*result.node);
    EXPECT_NEAR(bounds.min.x, -kE, 1.0);
    EXPECT_NEAR(bounds.max.y, kE, 1.0);

    // the same files read as XYZ (no tilemapresource.xml): rows count from the north, so 1/0/0 is now north-west
    const std::filesystem::path xyz = dir / "xyz";
    std::filesystem::create_directories(xyz);
    std::filesystem::copy(tms / "1", xyz / "1", std::filesystem::copy_options::recursive);
    result = loader.load(xyz.string());
    ASSERT_TRUE(result) << result.error;
    textures = texturesOf(*result.node);
    ASSERT_EQ(textures.size(), 4u);
    EXPECT_EQ(textures[0]->at(10, 10).b, 255) << "north-west is 1/0/0.png in XYZ";
    EXPECT_EQ(textures[2]->at(10, 10).r, 255) << "south-west is 1/0/1.png";

    // a {z}/{x}/{y} template names the same set
    result = loader.load((xyz / "{z}/{x}/{y}.png").string());
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(census(*result.node).draws, 4);
}

TEST_F(TileLayerTest, TmsTilesDrapeTheGlobeToo)
{
    const std::filesystem::path tms = dir / "tms_globe";
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y) ASSERT_TRUE(writePng(tms / "1" / std::to_string(x) / (std::to_string(y) + ".png"), 50, 60, 70));
    ivevsg::SceneLoader loader = loaderFor("EPSG:4978", 8);
    const ivevsg::LoadResult result = loader.load(tms.string());
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.targetCrs, "EPSG:4978");
    WorldVertices vertices;
    result.node->accept(vertices);
    // level-1 tiles span 180 degrees, so they get a finer grid than the 8 x 8 minimum (about one cell per 4 degrees)
    EXPECT_EQ(vertices.positions.size(), 4u * 46u * 46u);
    const ivevsg::GeocentricProjection globe;
    for (const vsg::dvec3& p : vertices.positions) EXPECT_NEAR(globe.worldToGeo(p).altitude, 0.0, 1.0);
}

TEST_F(TileLayerTest, UnusableFilesGiveClearErrors)
{
    const std::filesystem::path plain = writeRaster(dir / "plain.tif", 0, vsg::dbox(), 16, 16, 1, 1, 1);
    ASSERT_FALSE(plain.empty());
    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");
    ivevsg::LoadResult result = loader.load(plain.string());
    EXPECT_FALSE(result);
    EXPECT_NE(result.error.find("coordinate reference system"), std::string::npos) << result.error;

    // a CRS given by hand makes it usable: a 16 x 16 raster at the origin in Mercator metres... no geotransform though
    std::filesystem::create_directories(dir / "empty");
    result = loader.load((dir / "empty").string());
    EXPECT_FALSE(result);
    EXPECT_NE(result.error.find("tile"), std::string::npos) << result.error;
}

TEST_F(TileLayerTest, AWholeWorldRasterOnTheGlobeHasFiniteBounds)
{
    const std::filesystem::path tif = writeRaster(dir / "globe_world.tif", 3857, vsg::dbox(vsg::dvec3(-kE, -kE, 0.0), vsg::dvec3(kE, kE, 0.0)), 1024, 1024, 20, 40, 60);
    ASSERT_FALSE(tif.empty());
    ivevsg::SceneLoader loader = loaderFor("EPSG:4978", 8);
    const ivevsg::LoadResult result = loader.load(tif.string());
    ASSERT_TRUE(result) << result.error;
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*result.node);
    ASSERT_TRUE(bounds.valid());
    for (double v : {bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x, bounds.max.y, bounds.max.z}) EXPECT_TRUE(std::isfinite(v)) << v;
    EXPECT_NEAR(bounds.max.x, 6378137.0, 1.0);
    EXPECT_NEAR(bounds.min.x, -6378137.0, 1.0);
    WorldVertices vertices;
    result.node->accept(vertices);
    for (const vsg::dvec3& p : vertices.positions)
    {
        ASSERT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        EXPECT_NEAR(ivevsg::GeocentricProjection().worldToGeo(p).altitude, 0.0, 1.0);
    }
    const Census c = census(*result.node);
    ASSERT_EQ(c.plods.size(), 1u);
    EXPECT_TRUE(std::isfinite(c.plods[0]->bound.radius));
    EXPECT_GT(c.plods[0]->bound.radius, 6.0e6);
}

TEST_F(TileLayerTest, ChildReadsOnManyThreadsSurviveTheLayerBeingDropped)
{
    const std::filesystem::path tif = writeRaster(dir / "stress.tif", 3857, vsg::dbox(vsg::dvec3(-kE, -kE, 0.0), vsg::dvec3(kE, kE, 0.0)), 2048, 2048, 30, 60, 90);
    ASSERT_FALSE(tif.empty());
    ivevsg::SceneLoader loader = loaderFor("EPSG:4978", 8);
    vsg::ref_ptr<ivevsg::TileLayerReader> reader;
    for (auto& candidate : loader.options()->readerWriters)
        if (auto found = candidate.cast<ivevsg::TileLayerReader>()) reader = found;
    ASSERT_TRUE(reader) << "the tile layer reader is part of the loader chain";
    ivevsg::LoadResult result = loader.load(tif.string());
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(reader->openLayerCount(), 1u);
    const Census c = census(*result.node);
    ASSERT_EQ(c.plods.size(), 1u);
    const vsg::Path filename = c.plods[0]->filename;
    const vsg::ref_ptr<const vsg::Options> options = c.plods[0]->options;

    // eight threads page children while the scene drops the layer's node half way through
    std::atomic<int> served{0}, refused{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 12; ++i)
            {
                vsg::ref_ptr<vsg::Object> object = vsg::read(filename, options);
                if (object.cast<vsg::Node>())
                    ++served;
                else if (object.cast<vsg::ReadError>())
                    ++refused;
            }
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    result.node = {}; // what removing the layer from the scene does
    for (auto& thread : threads) thread.join();
    EXPECT_GT(served, 0);
    EXPECT_EQ(served + refused, 8 * 12) << "every request got a node or a ReadError, nothing crashed or vanished";
    EXPECT_EQ(reader->openLayerCount(), 0u) << "the raster closed with its node";
    vsg::ref_ptr<vsg::Object> late = vsg::read(filename, options);
    EXPECT_TRUE(late.cast<vsg::ReadError>());

    // a fresh layer in the same reader gets a new id, so nothing can confuse it with the old one
    result = loader.load(tif.string());
    ASSERT_TRUE(result) << result.error;
    EXPECT_NE(census(*result.node).plods[0]->filename, filename);
}
