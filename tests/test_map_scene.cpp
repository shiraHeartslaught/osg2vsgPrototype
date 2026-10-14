#include <ivevsg/FileDialog.h>
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

TEST(MapScene, LayerStackTogglesBaseAndLayersIndependently)
{
    auto base = vsg::Group::create();
    auto overlay = vsg::Group::create();
    ivevsg::LayerStack stack = ivevsg::createLayerStack(base, overlay);
    ASSERT_TRUE(stack.node());
    ASSERT_EQ(stack.node()->children.size(), 2u);
    EXPECT_EQ(stack.node()->children[0].node, base);
    EXPECT_EQ(stack.node()->children[1].node, stack.layer(0).placement); // layers sit under a placement transform
    EXPECT_EQ(stack.layer(0).node, overlay);
    EXPECT_TRUE(stack.hasBase());
    EXPECT_EQ(stack.layerCount(), 1u);
    EXPECT_EQ(stack.layer(0).name, "data");
    EXPECT_TRUE(stack.baseVisible());
    EXPECT_TRUE(stack.layerVisible(0));

    stack.setBaseVisible(false);
    EXPECT_FALSE(stack.baseVisible());
    EXPECT_TRUE(stack.layerVisible(0));
    EXPECT_EQ(stack.node()->children[0].mask, vsg::MASK_OFF);

    stack.setLayerVisible(0, false);
    stack.setBaseVisible(true);
    EXPECT_TRUE(stack.baseVisible());
    EXPECT_FALSE(stack.layerVisible(0));

    // a hidden layer is skipped by traversals, so it also drops out of the geometry bounds
    auto quad = makeUnitQuad();
    ivevsg::LayerStack withGeometry = ivevsg::createLayerStack(quad);
    EXPECT_TRUE(ivevsg::computeGeometryBounds(*withGeometry.node()).valid());
    withGeometry.setBaseVisible(false);
    EXPECT_FALSE(ivevsg::computeGeometryBounds(*withGeometry.node()).valid());
}

TEST(MapScene, LayerStackAddsAndRemovesNamedLayersAboveTheBase)
{
    auto base = vsg::Group::create();
    auto a = vsg::Group::create();
    auto b = vsg::Group::create();
    auto c = vsg::Group::create();
    ivevsg::LayerStack stack;
    stack.setBase(base);
    EXPECT_EQ(stack.addLayer("a", a), 0u);
    EXPECT_EQ(stack.addLayer("b", b), 1u);
    EXPECT_EQ(stack.addLayer("c", c), 2u);
    ASSERT_EQ(stack.node()->children.size(), 4u);
    EXPECT_EQ(stack.node()->children[0].node, base);
    EXPECT_EQ(stack.node()->children[2].node, stack.layer(1).placement);
    EXPECT_EQ(stack.layer(1).node, b);

    stack.setLayerVisible(1, false);
    EXPECT_TRUE(stack.layerVisible(0));
    EXPECT_FALSE(stack.layerVisible(1));
    EXPECT_TRUE(stack.layerVisible(2));

    // removing the middle layer shifts the ones above it down and keeps their visibility
    EXPECT_TRUE(stack.removeLayer(1));
    ASSERT_EQ(stack.layerCount(), 2u);
    EXPECT_EQ(stack.layer(1).name, "c");
    EXPECT_EQ(stack.node()->children[2].node, stack.layer(1).placement);
    EXPECT_EQ(stack.layer(1).node, c);
    EXPECT_TRUE(stack.layerVisible(1));
    EXPECT_FALSE(stack.removeLayer(5));

    // replacing the base keeps the layers in place
    auto newBase = vsg::Group::create();
    stack.setBase(newBase);
    EXPECT_EQ(stack.node()->children[0].node, newBase);
    EXPECT_EQ(stack.layer(0).node, a);
    EXPECT_EQ(stack.node()->children[1].node, stack.layer(0).placement);
    EXPECT_FALSE(stack.layerVisible(5));
}

TEST(MapScene, LayerStackWithoutABase)
{
    auto overlay = vsg::Group::create();
    ivevsg::LayerStack stack = ivevsg::createLayerStack({}, overlay);
    ASSERT_EQ(stack.node()->children.size(), 1u);
    EXPECT_FALSE(stack.hasBase());
    EXPECT_FALSE(stack.baseVisible());
    EXPECT_TRUE(stack.layerVisible(0));
    stack.setBaseVisible(false); // no base: harmless
    stack.setLayerVisible(0, false);
    EXPECT_FALSE(stack.layerVisible(0));
    stack.setBase(vsg::Group::create()); // base inserted in front of the existing layer
    EXPECT_EQ(stack.node()->children.size(), 2u);
    EXPECT_EQ(stack.node()->children[1].node, stack.layer(0).placement);
    EXPECT_FALSE(stack.layerVisible(0));
}

TEST(MapScene, LayerHeightLiftsALayerWithoutTouchingItsContent)
{
    auto quad = makeUnitQuad();
    ivevsg::LayerStack stack;
    const std::size_t index = stack.addLayer("quad", quad);
    EXPECT_EQ(stack.layer(index).node, quad);
    ASSERT_TRUE(stack.layer(index).placement);
    EXPECT_EQ(stack.node()->children[0].node, stack.layer(index).placement);

    EXPECT_NEAR(ivevsg::computeGeometryBounds(*stack.node()).max.z, 0.0, 1e-9);
    stack.setLayerHeight(index, 250.0);
    const vsg::dbox lifted = ivevsg::computeGeometryBounds(*stack.node());
    EXPECT_NEAR(lifted.min.z, 250.0, 1e-9);
    EXPECT_NEAR(lifted.max.z, 250.0, 1e-9);
    EXPECT_DOUBLE_EQ(lifted.max.x, 1.0); // x/y untouched
    stack.setLayerHeight(7, 1.0);        // out of range: ignored
}

TEST(FileDialog, SceneFiltersOfferPagedDatabasesFirst)
{
    const std::vector<ivevsg::FileFilter> filters = ivevsg::sceneFileFilters();
    ASSERT_FALSE(filters.empty());
    EXPECT_NE(filters[0].extensions.find("ive"), std::string::npos);
    EXPECT_NE(filters[0].extensions.find("vsgb"), std::string::npos);
    for (const auto& filter : filters)
    {
        EXPECT_FALSE(filter.name.empty());
        EXPECT_EQ(filter.extensions.find('.'), std::string::npos) << "NFD wants extensions without dots: " << filter.extensions;
        EXPECT_EQ(filter.extensions.find(' '), std::string::npos);
    }
}
