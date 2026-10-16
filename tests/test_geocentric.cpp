#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/MercatorProjection.h>

#include <gtest/gtest.h>

#include <cmath>

using ivevsg::GeocentricProjection;
using ivevsg::GeoPoint;

TEST(GeocentricProjection, KnownEcefPoints)
{
    GeocentricProjection globe;
    const vsg::dvec3 origin = globe.geoToWorld(GeoPoint{0.0, 0.0, 0.0});
    EXPECT_NEAR(origin.x, 6378137.0, 1e-6);
    EXPECT_NEAR(origin.y, 0.0, 1e-6);
    EXPECT_NEAR(origin.z, 0.0, 1e-6);

    const vsg::dvec3 east = globe.geoToWorld(GeoPoint{90.0, 0.0, 0.0});
    EXPECT_NEAR(east.x, 0.0, 1e-6);
    EXPECT_NEAR(east.y, 6378137.0, 1e-6);

    const vsg::dvec3 pole = globe.geoToWorld(GeoPoint{0.0, 90.0, 0.0});
    EXPECT_NEAR(pole.x, 0.0, 1e-6);
    EXPECT_NEAR(pole.z, 6356752.314245179, 1e-6);

    // lat 45, the textbook check: N = 6388838.290, x = N cos45, z = N (1 - e^2) sin45
    const vsg::dvec3 mid = globe.geoToWorld(GeoPoint{0.0, 45.0, 0.0});
    EXPECT_NEAR(mid.x, 4517590.879, 1e-3);
    EXPECT_NEAR(mid.z, 4487348.409, 1e-3);

    const vsg::dvec3 raised = globe.geoToWorld(GeoPoint{0.0, 0.0, 1000.0});
    EXPECT_NEAR(raised.x, 6379137.0, 1e-6);
}

TEST(GeocentricProjection, RoundTripsEverywhereIncludingAltitude)
{
    GeocentricProjection globe;
    for (double lon : {-179.5, -120.0, -45.0, 0.0, 33.3, 120.0, 179.5})
        for (double lat : {-89.9, -60.0, -30.0, 0.0, 30.0, 60.0, 89.9})
            for (double alt : {-264000.0, 0.0, 1234.5, 20000000.0})
            {
                const GeoPoint back = globe.worldToGeo(globe.geoToWorld(GeoPoint{lon, lat, alt}));
                EXPECT_NEAR(back.longitude, lon, 1e-8) << lon << "," << lat << "," << alt;
                EXPECT_NEAR(back.latitude, lat, 1e-8) << lon << "," << lat << "," << alt;
                EXPECT_NEAR(back.altitude, alt, 1e-3) << lon << "," << lat << "," << alt;
            }
}

TEST(GeocentricProjection, UpAndNorthAreUnitAndPerpendicular)
{
    GeocentricProjection globe;
    const vsg::dvec3 p = globe.geoToWorld(GeoPoint{-122.4, 37.8, 0.0});
    const vsg::dvec3 up = globe.upVector(p);
    const vsg::dvec3 north = globe.northVector(p);
    EXPECT_NEAR(vsg::length(up), 1.0, 1e-12);
    EXPECT_NEAR(vsg::length(north), 1.0, 1e-12);
    EXPECT_NEAR(vsg::dot(up, north), 0.0, 1e-9);
    EXPECT_GT(north.z, 0.0);
    // the ellipsoid normal at the equator points straight out along x
    const vsg::dvec3 equatorUp = globe.upVector(globe.geoToWorld(GeoPoint{0.0, 0.0, 0.0}));
    EXPECT_NEAR(equatorUp.x, 1.0, 1e-12);
}

TEST(GeocentricProjection, FramingLooksAtTheSurfaceFromOutside)
{
    GeocentricProjection globe;
    const vsg::dvec3 a = globe.geoToWorld(GeoPoint{9.0, 47.0, 0.0});
    const vsg::dvec3 b = globe.geoToWorld(GeoPoint{11.0, 49.0, 0.0});
    const vsg::dbox bounds(vsg::dvec3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)), vsg::dvec3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)));
    const ivevsg::CameraPose pose = globe.framingPose(bounds, 90.0);
    EXPECT_NEAR(globe.worldToGeo(pose.centre).altitude, 0.0, 1e-3);
    EXPECT_GT(vsg::length(pose.eye), vsg::length(pose.centre));           // eye is above the surface
    EXPECT_GT(globe.worldToGeo(pose.eye).altitude, 100000.0);
    EXPECT_GT(pose.farPlane, 2.0 * GeocentricProjection::kSemiMajor);   // sees past the earth
    EXPECT_LT(pose.nearPlane, pose.farPlane);
    EXPECT_NEAR(vsg::dot(vsg::normalize(pose.eye - pose.centre), pose.up), 0.0, 1e-9); // up is perpendicular to the view
}

TEST(GeocentricProjection, LayerOffsetIsRadial)
{
    GeocentricProjection globe;
    const vsg::dvec3 p = globe.geoToWorld(GeoPoint{40.0, -20.0, 0.0});
    const vsg::dvec3 lifted = globe.layerOffsetTransform(500.0) * p;
    EXPECT_NEAR(globe.worldToGeo(lifted).altitude, 500.0 * vsg::length(p) / GeocentricProjection::kSemiMajor, 2.0);
    EXPECT_GT(vsg::length(lifted), vsg::length(p));
    const vsg::dvec3 lowered = globe.layerOffsetTransform(-500.0) * p;
    EXPECT_LT(vsg::length(lowered), vsg::length(p));
    EXPECT_NEAR(vsg::length(lifted - p) + vsg::length(lowered - p), 1000.0 * vsg::length(p) / GeocentricProjection::kSemiMajor, 1e-6);

    ivevsg::MercatorProjection flat;
    const vsg::dvec3 q = flat.layerOffsetTransform(-400.0) * vsg::dvec3(1.0, 2.0, 0.0);
    EXPECT_DOUBLE_EQ(q.z, -400.0);
    EXPECT_DOUBLE_EQ(q.x, 1.0);
}

TEST(GeocentricProjection, FactoryAndCrsNames)
{
    EXPECT_TRUE(std::dynamic_pointer_cast<GeocentricProjection>(ivevsg::createProjection("globe")));
    EXPECT_TRUE(std::dynamic_pointer_cast<GeocentricProjection>(ivevsg::createProjection("EPSG:4978")));
    EXPECT_TRUE(std::dynamic_pointer_cast<ivevsg::MercatorProjection>(ivevsg::createProjection("mercator")));
    EXPECT_TRUE(std::dynamic_pointer_cast<ivevsg::MercatorProjection>(ivevsg::createProjection("epsg:3857")));
    EXPECT_FALSE(ivevsg::createProjection("sinusoidal"));
    EXPECT_EQ(GeocentricProjection().crs(), "EPSG:4978");
    EXPECT_TRUE(GeocentricProjection().geocentric());
    EXPECT_FALSE(ivevsg::MercatorProjection().geocentric());
    EXPECT_DOUBLE_EQ(GeocentricProjection().wrapWidth(), 0.0);
    EXPECT_FALSE(GeocentricProjection().viewLimits().valid());
}
