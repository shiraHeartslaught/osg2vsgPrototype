#include <ivevsg/CameraFraming.h>

#include <gtest/gtest.h>

#include <cmath>

namespace
{
    const vsg::dbox kFlatBox(vsg::dvec3(-10.0, -5.0, 0.0), vsg::dvec3(10.0, 5.0, 0.0));
    constexpr double kPi = 3.14159265358979323846;
} // namespace

TEST(CameraFraming, SummaryOfBox)
{
    const ivevsg::BoundsSummary summary = ivevsg::summarize(kFlatBox);
    EXPECT_DOUBLE_EQ(summary.centre.x, 0.0);
    EXPECT_DOUBLE_EQ(summary.centre.y, 0.0);
    EXPECT_DOUBLE_EQ(summary.centre.z, 0.0);
    EXPECT_NEAR(summary.radius, std::sqrt(20.0 * 20.0 + 10.0 * 10.0) * 0.6, 1e-12);
}

TEST(CameraFraming, InvalidBoundsFallBackToUnitRadiusAtOrigin)
{
    const vsg::dbox empty; // default box is invalid (min > max)
    const ivevsg::BoundsSummary summary = ivevsg::summarize(empty);
    EXPECT_DOUBLE_EQ(summary.radius, 1.0);
    EXPECT_DOUBLE_EQ(summary.centre.x, 0.0);
    EXPECT_DOUBLE_EQ(summary.centre.y, 0.0);
    EXPECT_DOUBLE_EQ(summary.centre.z, 0.0);
}

TEST(CameraFraming, DegeneratePointBoundsGetUnitRadius)
{
    const vsg::dbox point(vsg::dvec3(3.0, 4.0, 5.0), vsg::dvec3(3.0, 4.0, 5.0));
    EXPECT_DOUBLE_EQ(ivevsg::summarize(point).radius, 1.0);
}

TEST(CameraFraming, EyeIsSouthOfAndAboveTheCentreAtTheRequestedElevation)
{
    const ivevsg::CameraPose pose = ivevsg::computeFlatFramingPose(kFlatBox, 45.0);
    const vsg::dvec3 offset = pose.eye - pose.centre;
    EXPECT_DOUBLE_EQ(offset.x, 0.0);
    EXPECT_LT(offset.y, 0.0);
    EXPECT_GT(offset.z, 0.0);
    EXPECT_NEAR(std::atan2(offset.z, -offset.y) * 180.0 / kPi, 45.0, 1e-9);
    EXPECT_NEAR(vsg::length(offset), ivevsg::summarize(kFlatBox).radius * 2.55, 1e-9);
    EXPECT_DOUBLE_EQ(pose.up.z, 1.0);
}

TEST(CameraFraming, StraightDownUsesNorthAsScreenUp)
{
    const ivevsg::CameraPose pose = ivevsg::computeFlatFramingPose(kFlatBox, 90.0);
    const vsg::dvec3 offset = pose.eye - pose.centre;
    EXPECT_NEAR(offset.x, 0.0, 1e-9);
    EXPECT_NEAR(offset.y, 0.0, 1e-9);
    EXPECT_GT(offset.z, 0.0);
    EXPECT_DOUBLE_EQ(pose.up.y, 1.0);
    EXPECT_DOUBLE_EQ(pose.up.z, 0.0);
}

TEST(CameraFraming, DepthRangeCoversTheWholeScene)
{
    const ivevsg::CameraPose pose = ivevsg::computeFlatFramingPose(kFlatBox, 30.0);
    const double radius = ivevsg::summarize(kFlatBox).radius;
    EXPECT_GT(pose.nearPlane, 0.0);
    EXPECT_LT(pose.nearPlane, pose.farPlane);
    EXPECT_GE(pose.farPlane, vsg::length(pose.eye - pose.centre) + radius);
    EXPECT_DOUBLE_EQ(pose.nearPlane, 0.0005 * radius);
}

TEST(CameraFraming, ElevationIsClampedToAtLeastOneDegree)
{
    const ivevsg::CameraPose grazing = ivevsg::computeFlatFramingPose(kFlatBox, 0.0);
    const ivevsg::CameraPose oneDegree = ivevsg::computeFlatFramingPose(kFlatBox, 1.0);
    EXPECT_DOUBLE_EQ(grazing.eye.y, oneDegree.eye.y);
    EXPECT_DOUBLE_EQ(grazing.eye.z, oneDegree.eye.z);
    EXPECT_GT(grazing.eye.z, grazing.centre.z);
}
