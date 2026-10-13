#include <ivevsg/MercatorProjection.h>

#include <gtest/gtest.h>

using ivevsg::GeoPoint;
using ivevsg::MercatorProjection;

TEST(MercatorProjection, OriginMapsToZero)
{
    MercatorProjection projection;
    const vsg::dvec3 world = projection.geoToWorld(GeoPoint{0.0, 0.0, 0.0});
    // log(tan(pi/4)) is not exactly zero in floating point; sub-micrometre is close enough.
    EXPECT_NEAR(world.x, 0.0, 1e-6);
    EXPECT_NEAR(world.y, 0.0, 1e-6);
    EXPECT_DOUBLE_EQ(world.z, 0.0);
}

TEST(MercatorProjection, AntimeridianAndMaxLatitudeHitTheSquareWorldEdge)
{
    MercatorProjection projection;
    const vsg::dvec3 corner = projection.geoToWorld(GeoPoint{180.0, MercatorProjection::kMaxLatitude, 0.0});
    EXPECT_NEAR(corner.x, MercatorProjection::kMaxExtent, 1e-3);
    EXPECT_NEAR(corner.y, MercatorProjection::kMaxExtent, 1e-3);
}

TEST(MercatorProjection, MatchesGdalForKnownPoints)
{
    // Reference values from: gdaltransform -s_srs EPSG:4326 -t_srs EPSG:3857
    MercatorProjection projection;

    const vsg::dvec3 europe = projection.geoToWorld(GeoPoint{10.0, 50.0, 0.0});
    EXPECT_NEAR(europe.x, 1113194.90793274, 1e-3);
    EXPECT_NEAR(europe.y, 6446275.84101716, 1e-3);

    const vsg::dvec3 newYork = projection.geoToWorld(GeoPoint{-73.98, 40.75, 0.0});
    EXPECT_NEAR(newYork.x, -8235415.92888638, 1e-3);
    EXPECT_NEAR(newYork.y, 4975536.36106925, 1e-3);
}

TEST(MercatorProjection, GadasWorldCornersBackToGeographic)
{
    // The corners of gadas-world.tif (EPSG:3857), reference degrees from gdaltransform.
    MercatorProjection projection;

    const GeoPoint upperLeft = projection.worldToGeo({-11789647.276630, 6955739.919510, 0.0});
    EXPECT_NEAR(upperLeft.longitude, -105.908203429748, 1e-6);
    EXPECT_NEAR(upperLeft.latitude, 52.8523862916866, 1e-6);

    const GeoPoint lowerRight = projection.worldToGeo({12083165.397, -4393630.040, 0.0});
    EXPECT_NEAR(lowerRight.longitude, 108.544921566692, 1e-6);
    EXPECT_NEAR(lowerRight.latitude, -36.6730385453232, 1e-6);
}

TEST(MercatorProjection, RoundTripsAcrossTheGlobe)
{
    MercatorProjection projection;
    for (double longitude : {-179.9, -90.0, -45.5, 0.0, 30.25, 120.0, 179.9})
    {
        for (double latitude : {-80.0, -45.0, -10.0, 0.0, 15.5, 60.0, 84.0})
        {
            const GeoPoint back = projection.worldToGeo(projection.geoToWorld(GeoPoint{longitude, latitude, 0.0}));
            EXPECT_NEAR(back.longitude, longitude, 1e-9) << "lon " << longitude << " lat " << latitude;
            EXPECT_NEAR(back.latitude, latitude, 1e-9) << "lon " << longitude << " lat " << latitude;
        }
    }
}

TEST(MercatorProjection, AltitudeIsCarriedOnZ)
{
    MercatorProjection projection;
    const vsg::dvec3 world = projection.geoToWorld(GeoPoint{12.5, -33.0, 1234.5});
    EXPECT_DOUBLE_EQ(world.z, 1234.5);
    EXPECT_DOUBLE_EQ(projection.worldToGeo(world).altitude, 1234.5);
}

TEST(MercatorProjection, LatitudeIsClampedToTheSquareWorld)
{
    MercatorProjection projection;
    const vsg::dvec3 beyond = projection.geoToWorld(GeoPoint{0.0, 89.0, 0.0});
    const vsg::dvec3 edge = projection.geoToWorld(GeoPoint{0.0, MercatorProjection::kMaxLatitude, 0.0});
    EXPECT_DOUBLE_EQ(beyond.y, edge.y);
}

TEST(MercatorProjection, UpIsPlusZEverywhere)
{
    MercatorProjection projection;
    const vsg::dvec3 up = projection.upVector({-5.0e6, 3.0e6, 100.0});
    EXPECT_DOUBLE_EQ(up.x, 0.0);
    EXPECT_DOUBLE_EQ(up.y, 0.0);
    EXPECT_DOUBLE_EQ(up.z, 1.0);
}

TEST(MercatorProjection, ExtentOfWorldBoundsIsGeographic)
{
    MercatorProjection projection;
    const vsg::dbox bounds(vsg::dvec3(-11789647.276630, -4393630.040, 0.0),
                           vsg::dvec3(12083165.397, 6955739.919510, 0.0));
    const ivevsg::GeoExtent extent = projection.extentOf(bounds);
    EXPECT_NEAR(extent.minLongitude, -105.908203429748, 1e-6);
    EXPECT_NEAR(extent.maxLongitude, 108.544921566692, 1e-6);
    EXPECT_NEAR(extent.minLatitude, -36.6730385453232, 1e-6);
    EXPECT_NEAR(extent.maxLatitude, 52.8523862916866, 1e-6);
}

TEST(MercatorProjection, FramingPoseLooksDownOntoTheMap)
{
    MercatorProjection projection;
    const vsg::dbox bounds(vsg::dvec3(-11789647.276630, -4393630.040, 0.0),
                           vsg::dvec3(12083165.397, 6955739.919510, 0.0));
    const ivevsg::CameraPose pose = projection.framingPose(bounds, 45.0);
    EXPECT_GT(pose.eye.z, 0.0);
    EXPECT_LT(pose.eye.y, pose.centre.y);
    EXPECT_DOUBLE_EQ(pose.eye.x, pose.centre.x);
    EXPECT_GT(pose.nearPlane, 0.0);
    EXPECT_LT(pose.nearPlane, pose.farPlane);
}

TEST(MercatorProjection, ViewLimitsAreTheWorldSquare)
{
    MercatorProjection projection;
    const vsg::dbox limits = projection.viewLimits();
    ASSERT_TRUE(limits.valid());
    EXPECT_DOUBLE_EQ(limits.min.x, -MercatorProjection::kMaxExtent);
    EXPECT_DOUBLE_EQ(limits.max.y, MercatorProjection::kMaxExtent);
    // the square's top edge is the Mercator latitude limit, the nearest thing to a pole the map has
    EXPECT_NEAR(projection.worldToGeo(limits.max).latitude, MercatorProjection::kMaxLatitude, 1e-6);
}

TEST(MercatorProjection, LongitudeWrapsEveryWorldWidth)
{
    MercatorProjection projection;
    EXPECT_DOUBLE_EQ(projection.wrapWidth(), 2.0 * MercatorProjection::kMaxExtent);
    const vsg::dvec3 east = projection.geoToWorld(GeoPoint{179.0, 10.0, 0.0});
    const vsg::dvec3 west = projection.geoToWorld(GeoPoint{-181.0, 10.0, 0.0});
    EXPECT_NEAR(east.x - west.x, projection.wrapWidth(), 1e-3);
}
