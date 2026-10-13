#include <ivevsg/SceneBounds.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

namespace
{
    vsg::ref_ptr<vsg::Node> makeTriangle()
    {
        auto draw = vsg::VertexIndexDraw::create();
        auto vertices = vsg::vec3Array::create({{-1.0f, -2.0f, 0.0f}, {3.0f, 4.0f, 0.0f}, {1.0f, 1.0f, 5.0f}});
        draw->assignArrays(vsg::DataList{vertices});
        draw->assignIndices(vsg::ushortArray::create({0, 1, 2}));
        draw->indexCount = 3;
        draw->instanceCount = 1;
        return draw;
    }
} // namespace

TEST(SceneBounds, TightBoxAroundGeometry)
{
    auto triangle = makeTriangle();
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*triangle);
    ASSERT_TRUE(bounds.valid());
    EXPECT_DOUBLE_EQ(bounds.min.x, -1.0);
    EXPECT_DOUBLE_EQ(bounds.min.y, -2.0);
    EXPECT_DOUBLE_EQ(bounds.min.z, 0.0);
    EXPECT_DOUBLE_EQ(bounds.max.x, 3.0);
    EXPECT_DOUBLE_EQ(bounds.max.y, 4.0);
    EXPECT_DOUBLE_EQ(bounds.max.z, 5.0);
}

TEST(SceneBounds, IgnoresCullNodeBoundingSphere)
{
    // A paged terrain's root is wrapped in CullNodes whose spheres are far larger than the flat geometry.
    auto cull = vsg::CullNode::create(vsg::dsphere(vsg::dvec3(0.0, 0.0, 0.0), 1.0e7), makeTriangle());
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*cull);
    ASSERT_TRUE(bounds.valid());
    EXPECT_DOUBLE_EQ(bounds.min.x, -1.0);
    EXPECT_DOUBLE_EQ(bounds.max.z, 5.0);
    EXPECT_LT(bounds.max.x - bounds.min.x, 10.0);
}

TEST(SceneBounds, AppliesTransforms)
{
    auto transform = vsg::MatrixTransform::create(vsg::translate(100.0, 200.0, 300.0));
    transform->addChild(makeTriangle());
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*transform);
    ASSERT_TRUE(bounds.valid());
    EXPECT_DOUBLE_EQ(bounds.min.x, 99.0);
    EXPECT_DOUBLE_EQ(bounds.min.y, 198.0);
    EXPECT_DOUBLE_EQ(bounds.max.z, 305.0);
}

TEST(SceneBounds, EmptySceneGivesInvalidBox)
{
    auto group = vsg::Group::create();
    EXPECT_FALSE(ivevsg::computeGeometryBounds(*group).valid());
}
