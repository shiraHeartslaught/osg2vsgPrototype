#include <ivevsg/CrsTransform.h>
#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/LoaderKeys.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/SceneBounds.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/VsgReproject.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

#include <cmath>
#include <filesystem>

namespace
{
    /// A 10 km Mercator tile centred on lon 10 / lat 50 under a MatrixTransform, inside a PagedLOD,
    /// drawn through a flat-shaded pipeline so the reprojector can tell vertices from normals.
    struct SyntheticTile
    {
        vsg::ref_ptr<vsg::Group> root;
        vsg::ref_ptr<vsg::PagedLOD> plod;
        vsg::ref_ptr<vsg::MatrixTransform> transform;
        vsg::ref_ptr<vsg::vec3Array> vertices;
        vsg::ref_ptr<vsg::vec3Array> normals;
        vsg::dvec3 centre;
        double half = 5000.0;
    };

    SyntheticTile makeTile(bool withPipeline)
    {
        SyntheticTile tile;
        ivevsg::MercatorProjection mercator;
        tile.centre = mercator.geoToWorld(ivevsg::GeoPoint{10.0, 50.0, 0.0});
        const float h = static_cast<float>(tile.half);
        tile.vertices = vsg::vec3Array::create({{-h, -h, 0.0f}, {h, -h, 0.0f}, {h, h, 0.0f}, {-h, h, 0.0f}});
        tile.normals = vsg::vec3Array::create({{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}});
        auto texcoords = vsg::vec2Array::create({{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}});
        auto colours = vsg::vec4Array::create({{1.0f, 1.0f, 1.0f, 1.0f}});
        auto draw = vsg::VertexIndexDraw::create();
        draw->assignArrays(vsg::DataList{tile.vertices, tile.normals, texcoords, colours});
        draw->assignIndices(vsg::ushortArray::create({0, 1, 2, 0, 2, 3}));
        draw->indexCount = 6;
        draw->instanceCount = 1;

        vsg::ref_ptr<vsg::Node> leaf = draw;
        if (withPipeline)
        {
            auto config = vsg::GraphicsPipelineConfigurator::create(vsg::createFlatShadedShaderSet());
            config->enableArray("vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, 12, VK_FORMAT_R32G32B32_SFLOAT);
            config->enableArray("vsg_Normal", VK_VERTEX_INPUT_RATE_VERTEX, 12, VK_FORMAT_R32G32B32_SFLOAT);
            config->enableArray("vsg_TexCoord0", VK_VERTEX_INPUT_RATE_VERTEX, 8, VK_FORMAT_R32G32_SFLOAT);
            config->enableArray("vsg_Color", VK_VERTEX_INPUT_RATE_INSTANCE, 16, VK_FORMAT_R32G32B32A32_SFLOAT);
            config->init();
            auto state = vsg::StateGroup::create();
            config->copyTo(state);
            state->addChild(draw);
            leaf = state;
        }
        tile.transform = vsg::MatrixTransform::create(vsg::translate(tile.centre));
        tile.transform->addChild(leaf);

        tile.plod = vsg::PagedLOD::create();
        tile.plod->bound = vsg::dsphere(tile.centre, tile.half * std::sqrt(2.0));
        tile.plod->filename = "children.vsgb";
        tile.plod->children[0] = vsg::PagedLOD::Child{0.3, {}};
        tile.plod->children[1] = vsg::PagedLOD::Child{0.0, tile.transform};
        tile.root = vsg::Group::create();
        tile.root->addChild(tile.plod);
        return tile;
    }

    /// World position of a vertex after reprojection: the new root transform, the tile transform, the vertex.
    vsg::dvec3 worldVertex(const vsg::Node& newRoot, const SyntheticTile& tile, std::size_t index)
    {
        auto root = dynamic_cast<const vsg::MatrixTransform*>(&newRoot);
        const vsg::dmat4 outer = root ? root->matrix : vsg::dmat4();
        return outer * (tile.transform->matrix * vsg::dvec3(tile.vertices->at(index)));
    }
} // namespace

TEST(VsgReproject, MercatorTileLandsOnTheGlobeWithMovedBoundsAndNormals)
{
    for (bool withPipeline : {true, false})
    {
        SyntheticTile tile = makeTile(withPipeline);
        vsg::ref_ptr<vsg::Node> scene = tile.root;
        ivevsg::ReprojectSettings settings;
        settings.sourceCrs = "EPSG:3857";
        settings.target = std::make_shared<ivevsg::GeocentricProjection>();
        auto childOptions = vsg::Options::create();
        childOptions->setValue(ivevsg::loaderKeys::sourceCrs, std::string("EPSG:3857"));

        const ivevsg::ReprojectReport report = ivevsg::reprojectVsgScene(scene, settings, childOptions);
        ASSERT_TRUE(report) << report.error;
        EXPECT_FALSE(report.skipped);
        EXPECT_EQ(report.geometries, 1u);
        EXPECT_EQ(report.vertices, 4u);
        EXPECT_EQ(report.lods, 1u);
        EXPECT_EQ(scene.get(), tile.root.get()) << "the scene keeps its root: paged children must line up with it";

        const ivevsg::GeocentricProjection globe;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const ivevsg::GeoPoint geo = globe.worldToGeo(worldVertex(*scene, tile, i));
            EXPECT_NEAR(geo.altitude, 0.0, 1.0) << "vertex " << i << (withPipeline ? " (pipeline)" : "");
            EXPECT_NEAR(geo.latitude, 50.0, 0.1);
            EXPECT_NEAR(geo.longitude, 10.0, 0.1);
        }
        // the bound is in world coordinates: its centre near the tile, its radius still enclosing the corners
        const vsg::dvec3 boundWorld = tile.plod->bound.center;
        EXPECT_NEAR(vsg::length(boundWorld - globe.geoToWorld(ivevsg::GeoPoint{10.0, 50.0, 0.0})), 0.0, 50.0);
        for (std::size_t i = 0; i < 4; ++i) EXPECT_LE(vsg::length(worldVertex(*scene, tile, i) - boundWorld), tile.plod->bound.radius + 1.0);
        EXPECT_LT(tile.plod->bound.radius, tile.half * 2.0) << "a 10 km tile at 50 N shrinks on the globe";
        ASSERT_TRUE(tile.plod->options);
        std::string childCrs;
        EXPECT_TRUE(tile.plod->options->getValue(ivevsg::loaderKeys::sourceCrs, childCrs));
        EXPECT_EQ(childCrs, "EPSG:3857");

        // normals follow the ellipsoid when the pipeline says which array they are
        const vsg::dvec3 up = globe.upVector(globe.geoToWorld(ivevsg::GeoPoint{10.0, 50.0, 0.0}));
        const vsg::vec3 n = tile.normals->at(0);
        if (withPipeline)
            EXPECT_NEAR(vsg::dot(vsg::dvec3(n), up), 1.0, 1e-3);
        else
            EXPECT_FLOAT_EQ(n.z, 1.0f) << "without a pipeline only the first array is known to be positions";
    }
}

TEST(VsgReproject, SameCrsIsSkipped)
{
    SyntheticTile tile = makeTile(false);
    vsg::ref_ptr<vsg::Node> scene = tile.root;
    ivevsg::ReprojectSettings settings;
    settings.sourceCrs = "EPSG:3857";
    settings.target = std::make_shared<ivevsg::MercatorProjection>();
    const ivevsg::ReprojectReport report = ivevsg::reprojectVsgScene(scene, settings);
    EXPECT_TRUE(report);
    EXPECT_TRUE(report.skipped);
    EXPECT_EQ(scene.get(), tile.root.get());
}

TEST(VsgReproject, NativeFilesCarryTheirCrsAndAreReprojectedByTheLoader)
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ivevsg_tagged_tile.vsgt";
    {
        SyntheticTile tile = makeTile(true);
        ivevsg::tagCrs(*tile.root, "EPSG:3857");
        EXPECT_EQ(ivevsg::taggedCrs(*tile.root), "EPSG:3857");
        ASSERT_TRUE(vsg::write(tile.root, vsg::Path(file.string())));
    }

    // onto the globe: reprojected
    {
        ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
        settings.targetCrs = "EPSG:4978";
        ivevsg::SceneLoader loader(settings);
        const ivevsg::LoadResult result = loader.load(file.string());
        ASSERT_TRUE(result) << result.error;
        EXPECT_EQ(result.sourceCrs, "EPSG:3857");
        EXPECT_EQ(result.targetCrs, "EPSG:4978");
        const vsg::dbox bounds = ivevsg::computeGeometryBounds(*result.node);
        ASSERT_TRUE(bounds.valid());
        const ivevsg::GeocentricProjection globe;
        const vsg::dvec3 expected = globe.geoToWorld(ivevsg::GeoPoint{10.0, 50.0, 0.0});
        EXPECT_NEAR(vsg::length((bounds.min + bounds.max) * 0.5 - expected), 0.0, 100.0);
    }
    // on the Mercator map: already there, shown as read
    {
        ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
        settings.targetCrs = "EPSG:3857";
        ivevsg::SceneLoader loader(settings);
        const ivevsg::LoadResult result = loader.load(file.string());
        ASSERT_TRUE(result) << result.error;
        EXPECT_EQ(result.sourceCrs, "EPSG:3857");
        EXPECT_TRUE(result.targetCrs.empty());
        const vsg::dbox bounds = ivevsg::computeGeometryBounds(*result.node);
        const ivevsg::MercatorProjection mercator;
        const vsg::dvec3 expected = mercator.geoToWorld(ivevsg::GeoPoint{10.0, 50.0, 0.0});
        EXPECT_NEAR(bounds.min.x, expected.x - 5000.0, 1.0);
        EXPECT_NEAR(bounds.max.y, expected.y + 5000.0, 1.0);
    }
    // an explicit source CRS wins over the tag: the same numbers read as degrees land somewhere else entirely
    {
        ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
        settings.targetCrs = "EPSG:4978";
        ivevsg::SceneLoader loader(settings);
        const ivevsg::LoadResult result = loader.load(file.string(), "EPSG:4978");
        ASSERT_TRUE(result) << result.error;
        EXPECT_EQ(result.sourceCrs, "EPSG:4978");
        EXPECT_TRUE(result.targetCrs.empty()) << "told it is already geocentric: left alone";
    }
    std::filesystem::remove(file);
}

TEST(VsgReproject, UntaggedFilesFallBackToTheLoaderDefault)
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ivevsg_untagged_tile.vsgb";
    {
        SyntheticTile tile = makeTile(false);
        ASSERT_TRUE(vsg::write(tile.root, vsg::Path(file.string())));
    }
    ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
    settings.targetCrs = "EPSG:4978";
    settings.defaultSourceCrs = "EPSG:3857";
    ivevsg::SceneLoader loader(settings);
    const ivevsg::LoadResult result = loader.load(file.string());
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.sourceCrs, "EPSG:3857");
    EXPECT_EQ(result.targetCrs, "EPSG:4978");
    std::filesystem::remove(file);
}

// ---- the real geocentric Cyprus database, when it is on this machine -------------------------------

namespace
{
    std::string cyprusEcefIve()
    {
        if (const char* fromEnv = std::getenv("IVEVSG_TEST_ECEF_IVE")) return fromEnv;
        return "/run/media/heartslaught/games/ive_test/ecef/cyprus/gadas-cyprus-ecef.ive";
    }

    struct FindPagedLODs : public vsg::ConstVisitor
    {
        std::vector<const vsg::PagedLOD*> plods;
        std::vector<vsg::dmat4> stack{vsg::dmat4()};
        std::vector<vsg::dsphere> worldBounds; // each PagedLOD's sphere in world coordinates
        void apply(const vsg::Node& node) override { node.traverse(*this); }
        void apply(const vsg::MatrixTransform& transform) override
        {
            stack.push_back(stack.back() * transform.matrix);
            transform.traverse(*this);
            stack.pop_back();
        }
        void apply(const vsg::PagedLOD& plod) override
        {
            plods.push_back(&plod);
            worldBounds.emplace_back(stack.back() * plod.bound.center, plod.bound.radius);
            plod.traverse(*this);
        }
    };
} // namespace

TEST(VsgReproject, ConvertedVpbDatabaseMatchesTheIveOnTheMercatorMap)
{
    const std::string ive = cyprusEcefIve();
    if (!std::filesystem::exists(ive)) GTEST_SKIP() << "no geocentric database at " << ive;

    // convert the root as ivevsgconv does: read as is, tag the CRS, write native
    const std::filesystem::path vsgt = std::filesystem::path(ive).parent_path() / "ivevsg_test_cyprus_ecef.vsgt";
    {
        ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
        settings.targetCrs.clear();
        ivevsg::SceneLoader loader(settings);
        const ivevsg::LoadResult asRead = loader.load(ive);
        ASSERT_TRUE(asRead) << asRead.error;
        EXPECT_EQ(asRead.sourceCrs, "EPSG:4978");
        ivevsg::tagCrs(*asRead.node, asRead.sourceCrs);
        ASSERT_TRUE(vsg::write(asRead.node, vsg::Path(vsgt.string())));
    }

    ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
    settings.targetCrs = "EPSG:3857";
    ivevsg::SceneLoader loader(settings);
    const ivevsg::LoadResult fromIve = loader.load(ive);
    const ivevsg::LoadResult fromVsgt = loader.load(vsgt.string());
    std::filesystem::remove(vsgt);
    ASSERT_TRUE(fromIve) << fromIve.error;
    ASSERT_TRUE(fromVsgt) << fromVsgt.error;
    EXPECT_EQ(fromVsgt.sourceCrs, "EPSG:4978");
    EXPECT_EQ(fromVsgt.targetCrs, "EPSG:3857");

    const vsg::dbox a = ivevsg::computeGeometryBounds(*fromIve.node);
    const vsg::dbox b = ivevsg::computeGeometryBounds(*fromVsgt.node);
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    const double size = vsg::length(a.max - a.min);
    EXPECT_NEAR(vsg::length(a.min - b.min), 0.0, size * 0.01);
    EXPECT_NEAR(vsg::length(a.max - b.max), 0.0, size * 0.01);

    // the paged roots: same sphere (world coordinates), children read with the source CRS
    FindPagedLODs pa, pb;
    fromIve.node->accept(pa);
    fromVsgt.node->accept(pb);
    ASSERT_EQ(pa.plods.size(), 1u);
    ASSERT_EQ(pb.plods.size(), 1u);
    EXPECT_NEAR(vsg::length(pa.worldBounds[0].center - pb.worldBounds[0].center), 0.0, size * 0.05);
    // the VSG reprojector encloses the moved children's own spheres, so its radius is more generous
    EXPECT_GE(pb.worldBounds[0].radius, pa.worldBounds[0].radius * 0.9);
    EXPECT_LE(pb.worldBounds[0].radius, pa.worldBounds[0].radius * 2.5);
    for (const vsg::dvec3& corner : {b.min, b.max})
        EXPECT_LE(vsg::length(corner - pb.worldBounds[0].center), pb.worldBounds[0].radius * 1.05) << "the sphere must enclose the geometry";
    ASSERT_TRUE(pb.plods[0]->options);
    std::string childCrs;
    EXPECT_TRUE(pb.plods[0]->options->getValue(ivevsg::loaderKeys::sourceCrs, childCrs));
    EXPECT_EQ(childCrs, "EPSG:4978");
    EXPECT_NEAR(pb.plods[0]->children[0].minimumScreenHeightRatio, pa.plods[0]->children[0].minimumScreenHeightRatio, 0.05);
}
