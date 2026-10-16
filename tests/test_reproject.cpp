#include <ivevsg/CrsTransform.h>
#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/OsgReader.h>
#include <ivevsg/OsgReproject.h>
#include <ivevsg/SceneBounds.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TileSetBuilder.h>

#include <gdal_priv.h>
#include <gtest/gtest.h>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osgDB/WriteFile>
#include <vsg/all.h>

#include <cmath>
#include <filesystem>

namespace
{
    constexpr double kE = 20037508.342789244;

    /// A 10 km Mercator tile centred on lon 10 / lat 50 under a VPB-style MatrixTransform, with a
    /// PagedLOD around it, exactly like the databases the viewer loads.
    struct SyntheticTile
    {
        osg::ref_ptr<osg::Group> root;
        osg::ref_ptr<osg::MatrixTransform> transform;
        osg::ref_ptr<osg::Geometry> geometry;
        osg::ref_ptr<osg::PagedLOD> plod;
        osg::Vec3d centre;
        double half = 5000.0;
    };

    SyntheticTile makeTile(bool withNormals = true)
    {
        SyntheticTile tile;
        ivevsg::MercatorProjection mercator;
        const vsg::dvec3 c = mercator.geoToWorld(ivevsg::GeoPoint{10.0, 50.0, 0.0});
        tile.centre = osg::Vec3d(c.x, c.y, 0.0);
        tile.geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        const float h = static_cast<float>(tile.half);
        vertices->push_back(osg::Vec3(-h, -h, 0.0f));
        vertices->push_back(osg::Vec3(h, -h, 0.0f));
        vertices->push_back(osg::Vec3(h, h, 0.0f));
        vertices->push_back(osg::Vec3(-h, h, 0.0f));
        tile.geometry->setVertexArray(vertices);
        osg::ref_ptr<osg::Vec2Array> texcoords = new osg::Vec2Array;
        texcoords->push_back(osg::Vec2(0.0f, 0.0f));
        texcoords->push_back(osg::Vec2(1.0f, 0.0f));
        texcoords->push_back(osg::Vec2(1.0f, 1.0f));
        texcoords->push_back(osg::Vec2(0.0f, 1.0f));
        tile.geometry->setTexCoordArray(0, texcoords);
        if (withNormals)
        {
            osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
            normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));
            tile.geometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
        }
        osg::ref_ptr<osg::DrawElementsUShort> indices = new osg::DrawElementsUShort(GL_TRIANGLES);
        for (unsigned short i : {0, 1, 2, 0, 2, 3}) indices->push_back(i);
        tile.geometry->addPrimitiveSet(indices);
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(tile.geometry);
        tile.transform = new osg::MatrixTransform(osg::Matrixd::translate(tile.centre));
        tile.transform->addChild(geode);

        tile.plod = new osg::PagedLOD;
        tile.plod->setCenterMode(osg::LOD::USER_DEFINED_CENTER);
        tile.plod->setCenter(tile.centre);
        tile.plod->setRadius(tile.half * std::sqrt(2.0));
        tile.plod->setRangeMode(osg::LOD::DISTANCE_FROM_EYE_POINT);
        tile.plod->addChild(tile.transform, 1000.0f, 1.0e9f);
        tile.root = new osg::Group;
        tile.root->addChild(tile.plod);
        return tile;
    }

    osg::Vec3d worldVertex(const SyntheticTile& tile, std::size_t index)
    {
        auto* vertices = dynamic_cast<osg::Vec3Array*>(tile.geometry->getVertexArray());
        return osg::Vec3d((*vertices)[index]) * tile.transform->getMatrix();
    }
} // namespace

TEST(CrsTransform, MercatorAndEcefToGeographic)
{
    std::string error;
    auto mercator = ivevsg::CrsTransform::create("EPSG:3857", &error);
    ASSERT_TRUE(mercator) << error;
    EXPECT_FALSE(mercator->isGeographic());
    ivevsg::GeoPoint geo;
    ASSERT_TRUE(mercator->toGeographic(1113194.90793274, 6446275.84101716, 123.0, geo));
    EXPECT_NEAR(geo.longitude, 10.0, 1e-7);
    EXPECT_NEAR(geo.latitude, 50.0, 1e-7);
    EXPECT_NEAR(geo.altitude, 123.0, 1e-6); // height passes through

    auto ecef = ivevsg::CrsTransform::create("EPSG:4978", &error);
    ASSERT_TRUE(ecef) << error;
    ASSERT_TRUE(ecef->toGeographic(4517590.879, 0.0, 4487348.409, geo));
    EXPECT_NEAR(geo.longitude, 0.0, 1e-7);
    EXPECT_NEAR(geo.latitude, 45.0, 1e-6);
    EXPECT_NEAR(geo.altitude, 0.0, 0.01);

    auto geographic = ivevsg::CrsTransform::create("EPSG:4326", &error);
    ASSERT_TRUE(geographic) << error;
    EXPECT_TRUE(geographic->isGeographic());
    ASSERT_TRUE(geographic->toGeographic(-122.4, 37.8, 5.0, geo));
    EXPECT_DOUBLE_EQ(geo.longitude, -122.4);
    EXPECT_DOUBLE_EQ(geo.latitude, 37.8);

    EXPECT_FALSE(ivevsg::CrsTransform::create("not a crs", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(ivevsg::CrsTransform::sameCrs("EPSG:3857", "epsg:3857"));
    EXPECT_FALSE(ivevsg::CrsTransform::sameCrs("EPSG:3857", "EPSG:4978"));
}

TEST(Reproject, SameCrsIsANoOp)
{
    SyntheticTile tile = makeTile();
    const osg::Vec3d before = worldVertex(tile, 0);
    ivevsg::ReprojectSettings settings;
    settings.sourceCrs = "EPSG:3857";
    settings.target = std::make_shared<ivevsg::MercatorProjection>();
    const ivevsg::ReprojectReport report = ivevsg::reprojectScene(*tile.root, settings);
    EXPECT_TRUE(report) << report.error;
    EXPECT_TRUE(report.skipped);
    EXPECT_EQ(report.geometries, 0u);
    EXPECT_EQ(worldVertex(tile, 0), before);
    EXPECT_EQ(dynamic_cast<osg::Vec3Array*>(tile.geometry->getVertexArray())->size(), 4u);
}

TEST(Reproject, MercatorTileLandsOnTheGlobeWithTessellationAndMovedLod)
{
    SyntheticTile tile = makeTile();
    ivevsg::MercatorProjection mercator;
    ivevsg::GeocentricProjection globe;
    const double oldRadius = tile.plod->getRadius();
    const float oldMinRange = tile.plod->getMinRange(0);

    ivevsg::ReprojectSettings settings;
    settings.sourceCrs = "EPSG:3857";
    settings.target = std::make_shared<ivevsg::GeocentricProjection>();
    settings.quadTessellation = 4;
    const ivevsg::ReprojectReport report = ivevsg::reprojectScene(*tile.root, settings);
    ASSERT_TRUE(report) << report.error;
    EXPECT_FALSE(report.skipped);
    EXPECT_EQ(report.transforms, 1u);
    EXPECT_EQ(report.geometries, 1u);
    EXPECT_EQ(report.vertices, 25u);
    EXPECT_EQ(report.lods, 1u);

    // the transform now sits at the ECEF position of the tile centre
    const vsg::dvec3 expectedCentre = globe.geoToWorld(mercator.worldToGeo(vsg::dvec3(tile.centre.x(), tile.centre.y(), 0.0)));
    const osg::Vec3d newCentre = tile.transform->getMatrix().getTrans();
    EXPECT_NEAR(newCentre.x(), expectedCentre.x, 1e-3);
    EXPECT_NEAR(newCentre.y(), expectedCentre.y, 1e-3);
    EXPECT_NEAR(newCentre.z(), expectedCentre.z, 1e-3);

    // the first vertex is the old south-west corner, now in ECEF (float local + double origin)
    const vsg::dvec3 swMercator(tile.centre.x() - tile.half, tile.centre.y() - tile.half, 0.0);
    const vsg::dvec3 swExpected = globe.geoToWorld(mercator.worldToGeo(swMercator));
    const osg::Vec3d sw = worldVertex(tile, 0);
    EXPECT_NEAR(sw.x(), swExpected.x, 0.5);
    EXPECT_NEAR(sw.y(), swExpected.y, 0.5);
    EXPECT_NEAR(sw.z(), swExpected.z, 0.5);
    // the grid's last vertex is the old north-east corner
    const vsg::dvec3 neExpected = globe.geoToWorld(mercator.worldToGeo(vsg::dvec3(tile.centre.x() + tile.half, tile.centre.y() + tile.half, 0.0)));
    const osg::Vec3d ne = worldVertex(tile, 24);
    EXPECT_NEAR(ne.x(), neExpected.x, 0.5);
    EXPECT_NEAR(ne.z(), neExpected.z, 0.5);
    // every vertex lies on the ellipsoid
    for (std::size_t i = 0; i < 25; ++i)
    {
        const osg::Vec3d w = worldVertex(tile, i);
        EXPECT_NEAR(globe.worldToGeo(vsg::dvec3(w.x(), w.y(), w.z())).altitude, 0.0, 0.5) << "vertex " << i;
    }
    // texcoords were interpolated along with the grid
    ASSERT_EQ(dynamic_cast<osg::Vec2Array*>(tile.geometry->getTexCoordArray(0))->size(), 25u);
    // the overall normal became the ellipsoid normal at the tile
    auto* normals = dynamic_cast<osg::Vec3Array*>(tile.geometry->getNormalArray());
    ASSERT_TRUE(normals);
    const vsg::dvec3 up = globe.upVector(expectedCentre);
    EXPECT_NEAR((*normals)[0].x(), up.x, 1e-5);
    EXPECT_NEAR((*normals)[0].z(), up.z, 1e-5);

    // the PagedLOD moved with the tile and shrank by the Mercator stretch at lat 50 (1 / cos 50 = 1.556)
    const osg::Vec3d lodCentre = tile.plod->getCenter();
    EXPECT_NEAR(lodCentre.x(), expectedCentre.x, 1.0);
    EXPECT_NEAR(lodCentre.z(), expectedCentre.z, 1.0);
    EXPECT_NEAR(tile.plod->getRadius() / oldRadius, std::cos(50.0 * 3.14159265358979323846 / 180.0), 0.02);
    EXPECT_NEAR(tile.plod->getMinRange(0) / oldMinRange, std::cos(50.0 * 3.14159265358979323846 / 180.0), 0.02);
}

TEST(Reproject, GeographicDegreesBecomeMercatorMetres)
{
    SyntheticTile tile = makeTile(false);
    // reinterpret the tile as 1 x 1 degree around lon 10 / lat 50
    tile.transform->setMatrix(osg::Matrixd::translate(10.0, 50.0, 0.0));
    auto* vertices = dynamic_cast<osg::Vec3Array*>(tile.geometry->getVertexArray());
    (*vertices)[0] = osg::Vec3(-0.5f, -0.5f, 0.0f);
    (*vertices)[1] = osg::Vec3(0.5f, -0.5f, 0.0f);
    (*vertices)[2] = osg::Vec3(0.5f, 0.5f, 0.0f);
    (*vertices)[3] = osg::Vec3(-0.5f, 0.5f, 0.0f);

    ivevsg::ReprojectSettings settings;
    settings.sourceCrs = "EPSG:4326";
    settings.target = std::make_shared<ivevsg::MercatorProjection>();
    ASSERT_TRUE(ivevsg::reprojectScene(*tile.root, settings));
    ivevsg::MercatorProjection mercator;
    const vsg::dvec3 expected = mercator.geoToWorld(ivevsg::GeoPoint{9.5, 49.5, 0.0});
    const osg::Vec3d sw = worldVertex(tile, 0);
    EXPECT_NEAR(sw.x(), expected.x, 0.01);
    EXPECT_NEAR(sw.y(), expected.y, 0.01);
    EXPECT_EQ(vertices->size(), 4u); // tessellation 1: still a quad
}

TEST(Reproject, OptionsValuesSurviveCopyingSoPagedChildrenInheritThem)
{
    auto options = vsg::Options::create();
    options->setValue(ivevsg::OsgReader::kTargetCrs, std::string("EPSG:4978"));
    options->setValue(ivevsg::OsgReader::kQuadTessellation, 8);
    auto copy = vsg::Options::create(*options); // what osg2vsg does for each PagedLOD
    std::string target;
    int tessellation = 0;
    EXPECT_TRUE(copy->getValue(ivevsg::OsgReader::kTargetCrs, target));
    EXPECT_EQ(target, "EPSG:4978");
    EXPECT_TRUE(copy->getValue(ivevsg::OsgReader::kQuadTessellation, tessellation));
    EXPECT_EQ(tessellation, 8);
}

TEST(Reproject, LoaderReprojectsAnOsgFileAndItsPagedChildrenOntoTheGlobe)
{
    // a tiny ivetiler pyramid (Mercator) loaded with the globe as the target frame
    GDALAllRegister();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ivevsg_reproject_tiles";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path source = dir / "world.tif";
    {
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        ASSERT_TRUE(driver);
        GDALDataset* dataset = driver->Create(source.string().c_str(), 64, 64, 3, GDT_Byte, nullptr);
        ASSERT_TRUE(dataset);
        double geo[6] = {-kE, 2.0 * kE / 64.0, 0.0, kE, 0.0, -2.0 * kE / 64.0};
        dataset->SetGeoTransform(geo);
        GDALClose(dataset);
    }
    ivevsg::TileSetSettings tiles;
    tiles.sourceRaster = source.string();
    tiles.outputDirectory = (dir / "tiles").string();
    tiles.maxLevel = 1;
    tiles.tileSizePixels = 16;
    tiles.threads = 1;
    ASSERT_TRUE(ivevsg::buildTileSet(tiles));

    ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
    settings.targetCrs = "EPSG:4978";
    settings.quadTessellation = 4;
    ivevsg::SceneLoader loader(settings);
    const ivevsg::LoadResult root = loader.load((dir / "tiles" / "root.ive").string());
    ASSERT_TRUE(root) << root.error;

    ivevsg::GeocentricProjection globe;
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*root.node);
    ASSERT_TRUE(bounds.valid());
    // the world tile now wraps around the earth: its extent is the earth's size, not Mercator's 40 000 km square
    EXPECT_LT(bounds.max.x, 1.01 * ivevsg::GeocentricProjection::kSemiMajor);
    EXPECT_GT(bounds.max.x, 0.5 * ivevsg::GeocentricProjection::kSemiMajor);
    EXPECT_LT(bounds.max.z, 1.01 * ivevsg::GeocentricProjection::kSemiMinor);

    // the paged child resolves with the inherited options and is reprojected the same way
    struct FirstPagedLOD : public vsg::Visitor
    {
        vsg::ref_ptr<vsg::PagedLOD> found;
        void apply(vsg::Node& node) override { if (!found) node.traverse(*this); }
        void apply(vsg::PagedLOD& plod) override { if (!found) found = vsg::ref_ptr<vsg::PagedLOD>(&plod); }
    } finder;
    root.node->accept(finder);
    ASSERT_TRUE(finder.found);
    std::string target;
    ASSERT_TRUE(finder.found->options);
    EXPECT_TRUE(finder.found->options->getValue(ivevsg::OsgReader::kTargetCrs, target));
    EXPECT_EQ(target, "EPSG:4978");
    auto child = vsg::read_cast<vsg::Node>(finder.found->filename, finder.found->options);
    ASSERT_TRUE(child);
    const vsg::dbox childBounds = ivevsg::computeGeometryBounds(*child);
    ASSERT_TRUE(childBounds.valid());
    EXPECT_LT(vsg::length(childBounds.max), 1.01 * ivevsg::GeocentricProjection::kSemiMajor * std::sqrt(3.0));
    EXPECT_GT(globe.worldToGeo(childBounds.max).altitude, -300000.0); // on the globe, not out in Mercator space

    std::filesystem::remove_all(dir);
}
