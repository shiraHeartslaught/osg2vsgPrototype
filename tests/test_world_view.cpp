#include <ivevsg/GlobeView.h>
#include <ivevsg/MercatorView.h>
#include <ivevsg/TrackballView.h>
#include <ivevsg/WorldView.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

#include <cmath>

namespace
{
    constexpr double kViewW = 800.0;
    constexpr double kViewH = 600.0;

    vsg::ref_ptr<vsg::Camera> makeCamera()
    {
        auto lookAt = vsg::LookAt::create(vsg::dvec3(0.0, 0.0, 1.0), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 1.0, 0.0));
        auto perspective = vsg::Perspective::create(30.0, kViewW / kViewH, 1.0, 1.0e8);
        return vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(VkExtent2D{static_cast<uint32_t>(kViewW), static_cast<uint32_t>(kViewH)}));
    }

    /// bounds of a 10 x 10 degree patch around lon 20 / lat 40 in the projection's frame
    vsg::dbox patchBounds(const ivevsg::Projection& projection)
    {
        vsg::dbox box;
        for (double lon : {15.0, 25.0})
            for (double lat : {35.0, 45.0}) box.add(projection.geoToWorld(ivevsg::GeoPoint{lon, lat, 0.0}));
        return box;
    }

    int childCount(vsg::ref_ptr<vsg::Node> node)
    {
        auto group = node.cast<vsg::Group>();
        return group ? static_cast<int>(group->children.size()) : -1;
    }
} // namespace

TEST(WorldViewFactory, KnowsMercatorAndGlobeUnderSeveralNames)
{
    for (const char* key : {"mercator", "EPSG:3857", "map"})
    {
        std::unique_ptr<ivevsg::WorldView> view = ivevsg::createWorldView(key);
        ASSERT_TRUE(view) << key;
        EXPECT_EQ(view->key(), "mercator");
        EXPECT_EQ(view->projection()->crs(), "EPSG:3857");
        EXPECT_FALSE(view->projection()->geocentric());
    }
    for (const char* key : {"globe", "EPSG:4978", "ecef", "geocentric"})
    {
        std::unique_ptr<ivevsg::WorldView> view = ivevsg::createWorldView(key);
        ASSERT_TRUE(view) << key;
        EXPECT_EQ(view->key(), "globe");
        EXPECT_EQ(view->projection()->crs(), "EPSG:4978");
        EXPECT_TRUE(view->projection()->geocentric());
    }
    EXPECT_FALSE(ivevsg::createWorldView("polar"));
    EXPECT_EQ(ivevsg::worldViewKeys().size(), 2u);

    // the free-orbit variant keeps the projection's key so per-view base maps still resolve
    std::unique_ptr<ivevsg::WorldView> orbit = ivevsg::createWorldView("globe", true);
    ASSERT_TRUE(orbit);
    EXPECT_EQ(orbit->key(), "globe");
    EXPECT_TRUE(dynamic_cast<ivevsg::TrackballView*>(orbit.get()));
}

TEST(WorldViewScene, MercatorRepeatsTheWorldSidewaysAndTheGlobeDoesNot)
{
    auto world = vsg::Group::create();
    ivevsg::MercatorView mercator;
    EXPECT_EQ(childCount(mercator.assembleScene(world, 1.5)), 3);  // one copy each side
    EXPECT_EQ(childCount(mercator.assembleScene(world, 3.0)), 5);  // ultra-wide: two each side
    EXPECT_EQ(mercator.quadTessellation(), 1);
    EXPECT_TRUE(mercator.supportsFlatBaseImage());

    ivevsg::GlobeView globe;
    EXPECT_EQ(globe.assembleScene(world, 1.5), world); // the very same node
    EXPECT_GT(globe.quadTessellation(), 1);
    EXPECT_FALSE(globe.supportsFlatBaseImage());
}

TEST(WorldViewCamera, MercatorAttachFramesTheBoundsStraightDown)
{
    ivevsg::MercatorView view;
    auto camera = makeCamera();
    view.attach(camera, patchBounds(*view.projection()), 90.0);
    ASSERT_TRUE(view.eventHandler());

    const ivevsg::ViewState state = view.state();
    EXPECT_NEAR(state.centre.longitude, 20.0, 1e-6);
    EXPECT_NEAR(state.centre.latitude, 40.0, 0.5); // the Mercator midpoint of 35..45 is a little north of 40
    EXPECT_GT(state.altitude, 0.0);
    EXPECT_DOUBLE_EQ(state.heading, 0.0);
    EXPECT_DOUBLE_EQ(state.tilt, 0.0);
    EXPECT_FALSE(view.hasOrientation());
    auto lookAt = camera->viewMatrix.cast<vsg::LookAt>();
    EXPECT_NEAR(lookAt->eye.x, lookAt->center.x, 1e-6); // straight down
    EXPECT_NEAR(lookAt->up.y, 1.0, 1e-12);              // north up

    std::optional<ivevsg::GeoPoint> under = view.groundAtPixel(kViewW * 0.5, kViewH * 0.5);
    ASSERT_TRUE(under);
    EXPECT_NEAR(under->longitude, state.centre.longitude, 1e-6);

    view.detach();
    EXPECT_FALSE(view.eventHandler());
}

TEST(WorldViewCamera, GlobeAttachUsesTheElevationAndCanBeTurnedNorthUp)
{
    ivevsg::GlobeView view;
    auto camera = makeCamera();
    view.attach(camera, patchBounds(*view.projection()), 60.0);
    ASSERT_TRUE(view.eventHandler());
    EXPECT_TRUE(view.hasOrientation());
    ivevsg::ViewState state = view.state();
    EXPECT_NEAR(state.tilt, 30.0, 1e-6); // 60 degrees above the ground = 30 from straight down
    EXPECT_NEAR(state.centre.longitude, 20.0, 0.5); // the ECEF box centre of the patch is not exactly on its meridian

    view.setOrientation(45.0, 0.0);
    state = view.state();
    EXPECT_NEAR(state.heading, 45.0, 1e-9);
    EXPECT_NEAR(state.tilt, 0.0, 1e-9);

    std::optional<ivevsg::GeoPoint> under = view.groundAtPixel(kViewW * 0.5, kViewH * 0.5);
    ASSERT_TRUE(under);
    EXPECT_NEAR(under->longitude, state.centre.longitude, 1e-6);
    EXPECT_NEAR(under->latitude, state.centre.latitude, 1e-6);
}

TEST(WorldViewCamera, LookAtAndStateHandTheCameraOverBetweenViews)
{
    // the viewer switches views by reading one view's state and looking at it with the other
    ivevsg::MercatorView mercator;
    auto camera = makeCamera();
    mercator.attach(camera, patchBounds(*mercator.projection()), 90.0);
    mercator.lookAt(ivevsg::GeoPoint{33.2, 35.0, 0.0}, 250000.0);
    const ivevsg::ViewState onMap = mercator.state();
    EXPECT_NEAR(onMap.centre.longitude, 33.2, 1e-9);
    EXPECT_NEAR(onMap.centre.latitude, 35.0, 1e-9);
    EXPECT_NEAR(onMap.altitude, 250000.0, 1e-6);
    mercator.detach();

    ivevsg::GlobeView globe;
    globe.attach(camera, patchBounds(*globe.projection()), 90.0);
    globe.lookAt(onMap.centre, onMap.altitude);
    const ivevsg::ViewState onGlobe = globe.state();
    EXPECT_NEAR(onGlobe.centre.longitude, 33.2, 1e-9);
    EXPECT_NEAR(onGlobe.centre.latitude, 35.0, 1e-9);
    EXPECT_NEAR(onGlobe.altitude, 250000.0, 1e-6);
    // same altitude and viewport: the same zoom level and ground resolution on both
    EXPECT_NEAR(onGlobe.zoomLevel, onMap.zoomLevel, 1e-9);
    EXPECT_NEAR(onGlobe.metersPerPixel, onMap.metersPerPixel, 1e-9);
    // and the camera really is over Cyprus in ECEF
    auto lookAt = camera->viewMatrix.cast<vsg::LookAt>();
    const ivevsg::GeoPoint eye = globe.projection()->worldToGeo(lookAt->eye);
    EXPECT_NEAR(eye.longitude, 33.2, 1e-6);
    EXPECT_NEAR(eye.latitude, 35.0, 1e-6);
    EXPECT_NEAR(eye.altitude, 250000.0, 1e-3);
}

TEST(WorldViewCamera, ZoomRangeAndHomeWorkThroughTheInterface)
{
    std::unique_ptr<ivevsg::WorldView> view = ivevsg::createWorldView("globe");
    auto camera = makeCamera();
    view->attach(camera, patchBounds(*view->projection()), 90.0);
    const ivevsg::ViewState start = view->state();
    view->setZoomLevelRange(3.0, 12.0);
    EXPECT_NEAR(view->maxZoomLevel(), 12.0, 1e-9);
    EXPECT_GE(view->minZoomLevel(), 3.0 - 1e-9);
    view->lookAt(ivevsg::GeoPoint{0.0, 0.0, 0.0}, 10.0); // far below the zoom-12 floor: clamped
    EXPECT_LE(view->state().zoomLevel, 12.0 + 1e-9);
    view->home();
    EXPECT_NEAR(view->state().centre.longitude, start.centre.longitude, 1e-9);
    EXPECT_FALSE(view->controls().empty());
}

TEST(WorldViewCamera, TrackballViewReportsItsLookAt)
{
    ivevsg::TrackballView view(std::make_shared<ivevsg::MercatorProjection>(), "mercator");
    auto camera = makeCamera();
    view.attach(camera, patchBounds(*view.projection()), 45.0);
    ASSERT_TRUE(view.eventHandler());
    view.lookAt(ivevsg::GeoPoint{10.0, 50.0, 0.0}, 5000.0);
    const ivevsg::ViewState state = view.state();
    EXPECT_NEAR(state.centre.longitude, 10.0, 1e-9);
    EXPECT_NEAR(state.centre.latitude, 50.0, 1e-9);
    EXPECT_NEAR(state.altitude, 5000.0, 1e-6);
    EXPECT_FALSE(view.groundAtPixel(1.0, 1.0));
    auto world = vsg::Group::create();
    EXPECT_EQ(view.assembleScene(world, 2.0), world); // no wrapping: the node itself
}
