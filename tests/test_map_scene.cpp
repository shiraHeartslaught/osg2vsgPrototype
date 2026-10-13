#include <ivevsg/MapScene.h>
#include <ivevsg/SceneBounds.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

namespace
{
    vsg::ref_ptr<vsg::Node> makeUnitQuad()
    {
        auto draw = vsg::VertexIndexDraw::create();
        draw->assignArrays(vsg::DataList{vsg::vec3Array::create({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}})});
        draw->assignIndices(vsg::ushortArray::create({0, 1, 2, 0, 2, 3}));
        draw->indexCount = 6;
        draw->instanceCount = 1;
        return draw;
    }
} // namespace

TEST(MapScene, LayeredSceneDrawsTheBaseMapFirst)
{
    auto base = vsg::Group::create();
    auto overlay = vsg::Group::create();
    auto scene = ivevsg::createLayeredScene(base, overlay);
    ASSERT_EQ(scene->children.size(), 2u);
    EXPECT_EQ(scene->children[0], base);
    EXPECT_EQ(scene->children[1], overlay);

    auto overlayOnly = ivevsg::createLayeredScene({}, overlay);
    ASSERT_EQ(overlayOnly->children.size(), 1u);
    EXPECT_EQ(overlayOnly->children[0], overlay);
}

TEST(MapScene, WrappedSceneRepeatsTheWorldOnBothSides)
{
    auto world = makeUnitQuad();
    auto scene = ivevsg::createWrappedScene(world, 100.0, 1);
    ASSERT_EQ(scene->children.size(), 3u);

    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*scene);
    ASSERT_TRUE(bounds.valid());
    EXPECT_DOUBLE_EQ(bounds.min.x, -100.0);
    EXPECT_DOUBLE_EQ(bounds.max.x, 101.0);
    EXPECT_DOUBLE_EQ(bounds.min.y, 0.0);
    EXPECT_DOUBLE_EQ(bounds.max.y, 1.0);

    // the copies share the same node rather than duplicating it
    for (auto& child : scene->children)
    {
        auto transform = child.cast<vsg::MatrixTransform>();
        ASSERT_TRUE(transform);
        ASSERT_EQ(transform->children.size(), 1u);
        EXPECT_EQ(transform->children[0], world);
    }
}

TEST(MapScene, MoreCopiesForWiderViewports)
{
    auto scene = ivevsg::createWrappedScene(makeUnitQuad(), 100.0, 2);
    ASSERT_EQ(scene->children.size(), 5u);
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*scene);
    EXPECT_DOUBLE_EQ(bounds.min.x, -200.0);
    EXPECT_DOUBLE_EQ(bounds.max.x, 201.0);
}

TEST(MapScene, NoWrapWidthMeansASingleCopy)
{
    auto world = makeUnitQuad();
    auto scene = ivevsg::createWrappedScene(world, 0.0, 1);
    ASSERT_EQ(scene->children.size(), 1u);
    EXPECT_EQ(scene->children[0], world);
    EXPECT_EQ(ivevsg::createWrappedScene({}, 100.0, 1)->children.size(), 0u);
}
