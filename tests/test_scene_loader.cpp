#include <ivevsg/SceneBounds.h>
#include <ivevsg/SceneLoader.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef IVEVSG_TEST_IVE_DEFAULT
#    define IVEVSG_TEST_IVE_DEFAULT ""
#endif

namespace
{
    struct CountPagedLODs : public vsg::ConstVisitor
    {
        int count = 0;
        void apply(const vsg::Node& node) override { node.traverse(*this); }
        void apply(const vsg::PagedLOD& plod) override
        {
            ++count;
            plod.traverse(*this);
        }
    };

    std::string testIvePath()
    {
        if (const char* fromEnv = std::getenv("IVEVSG_TEST_IVE")) return fromEnv;
        return IVEVSG_TEST_IVE_DEFAULT;
    }

    vsg::ref_ptr<vsg::Node> makeTriangleScene()
    {
        auto draw = vsg::VertexIndexDraw::create();
        draw->assignArrays(vsg::DataList{vsg::vec3Array::create({{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 3.0f, 0.0f}})});
        draw->assignIndices(vsg::ushortArray::create({0, 1, 2}));
        draw->indexCount = 3;
        draw->instanceCount = 1;
        auto group = vsg::Group::create();
        group->addChild(draw);
        return group;
    }
} // namespace

TEST(SceneLoader, BuildHasOsgSupport)
{
    EXPECT_TRUE(ivevsg::SceneLoader::supportsOsg()) << "vsgXchange must be built with the osg feature for .ive loading";
}

TEST(SceneLoader, DefaultPluginDirectoryExists)
{
    const std::string dir = ivevsg::SceneLoader::defaultOsgPluginDirectory();
    ASSERT_FALSE(dir.empty()) << "no osgPlugins-* directory was found at configure time";
    EXPECT_TRUE(std::filesystem::is_directory(dir)) << dir;
}

TEST(SceneLoader, MissingFileReportsAnError)
{
    ivevsg::SceneLoader loader;
    const ivevsg::LoadResult result = loader.load("/nonexistent/directory/scene.vsgt");
    EXPECT_FALSE(result);
    EXPECT_FALSE(result.error.empty());
}

TEST(SceneLoader, RoundTripsANativeVsgFile)
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ivevsg_loader_test.vsgt";
    ASSERT_TRUE(vsg::write(makeTriangleScene(), vsg::Path(file.string())));

    ivevsg::SceneLoader loader;
    const ivevsg::LoadResult result = loader.load(file.string());
    ASSERT_TRUE(result) << result.error;

    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*result.node);
    ASSERT_TRUE(bounds.valid());
    EXPECT_DOUBLE_EQ(bounds.max.x, 2.0);
    EXPECT_DOUBLE_EQ(bounds.max.y, 3.0);

    std::filesystem::remove(file);
}

TEST(SceneLoader, LoadsAPagedIveTerrainDatabase)
{
    const std::string path = testIvePath();
    if (path.empty() || !std::filesystem::exists(path))
    {
        GTEST_SKIP() << "no .ive database at '" << path << "' (set IVEVSG_TEST_IVE to point at one)";
    }
    ASSERT_TRUE(ivevsg::SceneLoader::supportsOsg());

    ivevsg::SceneLoader loader;
    const ivevsg::LoadResult result = loader.load(path);
    ASSERT_TRUE(result) << result.error;

    CountPagedLODs counter;
    result.node->accept(counter);
    EXPECT_GE(counter.count, 1) << "a VPB database root should contain a PagedLOD";

    // gadas-world.tif spans 23872812.67 m in EPSG:3857; any other database just has to be non-degenerate.
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*result.node);
    ASSERT_TRUE(bounds.valid());
    EXPECT_GT(bounds.max.x - bounds.min.x, 0.0);
    if (path.find("gadas-world") != std::string::npos)
    {
        EXPECT_NEAR(bounds.max.x - bounds.min.x, 23872812.67, 23872812.67 * 0.01);
        EXPECT_NEAR(bounds.max.y - bounds.min.y, 11349369.96, 11349369.96 * 0.01);
    }
}
