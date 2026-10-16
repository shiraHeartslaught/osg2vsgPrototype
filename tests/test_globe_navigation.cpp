#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/GlobeNavigation.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

#include <cmath>
#include <memory>

namespace
{
    constexpr double kFov = 30.0;
    constexpr double kViewW = 800.0;
    constexpr double kViewH = 600.0;

    const ivevsg::GeocentricProjection& globe()
    {
        static const ivevsg::GeocentricProjection instance;
        return instance;
    }

    ivevsg::GlobeNavigationSettings settings()
    {
        ivevsg::GlobeNavigationSettings s;
        s.fieldOfViewDegrees = kFov;
        return s;
    }

    ivevsg::GlobeCamera over(double lon, double lat, double altitude, double heading = 0.0, double tilt = 0.0)
    {
        ivevsg::GlobeCamera c;
        c.longitude = lon;
        c.latitude = lat;
        c.altitude = altitude;
        c.heading = heading;
        c.tilt = tilt;
        return c;
    }

    vsg::ref_ptr<vsg::Camera> makeCamera(const ivevsg::GlobeCamera& globeCamera)
    {
        const ivevsg::CameraPose pose = ivevsg::globeCameraPose(globeCamera, globe());
        auto lookAt = vsg::LookAt::create(pose.eye, pose.centre, pose.up);
        auto perspective = vsg::Perspective::create(kFov, kViewW / kViewH, pose.nearPlane, pose.farPlane);
        return vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(VkExtent2D{static_cast<uint32_t>(kViewW), static_cast<uint32_t>(kViewH)}));
    }

    vsg::ref_ptr<vsg::LookAt> lookAtOf(vsg::ref_ptr<vsg::Camera> camera) { return camera->viewMatrix.cast<vsg::LookAt>(); }

    vsg::ref_ptr<vsg::MoveEvent> move(double x, double y, vsg::ButtonMask mask)
    {
        return vsg::MoveEvent::create(vsg::ref_ptr<vsg::Window>(), vsg::clock::now(), static_cast<int32_t>(x), static_cast<int32_t>(y), mask);
    }
} // namespace

// ---- pose --------------------------------------------------------------------------------------

TEST(GlobeCameraMath, StraightDownPoseLooksAlongTheEllipsoidNormalWithNorthUp)
{
    const ivevsg::GlobeCamera camera = over(10.0, 50.0, 1.0e6);
    const ivevsg::CameraPose pose = ivevsg::globeCameraPose(camera, globe());

    const vsg::dvec3 surface = globe().geoToWorld(ivevsg::GeoPoint{10.0, 50.0, 0.0});
    const vsg::dvec3 up = globe().upVector(surface);
    EXPECT_NEAR(vsg::length(pose.centre - surface), 0.0, 1e-6);
    EXPECT_NEAR(vsg::length(pose.eye - (surface + up * 1.0e6)), 0.0, 1e-6);
    EXPECT_NEAR(vsg::dot(pose.up, globe().northVector(surface)), 1.0, 1e-9); // north at the top of the screen
    EXPECT_NEAR(vsg::dot(pose.up, vsg::normalize(pose.centre - pose.eye)), 0.0, 1e-9);
    EXPECT_GT(pose.farPlane, vsg::length(pose.eye) + ivevsg::GeocentricProjection::kSemiMajor); // the far side of the globe is inside
    EXPECT_LT(pose.nearPlane, 1.0e6);
}

TEST(GlobeCameraMath, TiltedPoseKeepsTheDistanceAndRoundTripsHeadingAndTilt)
{
    const ivevsg::GlobeCamera camera = over(-120.0, 37.0, 50000.0, 35.0, 60.0);
    const ivevsg::CameraPose pose = ivevsg::globeCameraPose(camera, globe());
    EXPECT_NEAR(vsg::length(pose.eye - pose.centre), 50000.0, 1e-6);
    EXPECT_NEAR(vsg::dot(pose.up, vsg::normalize(pose.centre - pose.eye)), 0.0, 1e-9);
    // tilted 60 degrees from straight down: the eye sits 60 degrees away from the surface normal
    const vsg::dvec3 up = globe().upVector(pose.centre);
    EXPECT_NEAR(std::acos(vsg::dot(vsg::normalize(pose.eye - pose.centre), up)) * 180.0 / M_PI, 60.0, 1e-9);

    const ivevsg::GlobeCamera back = ivevsg::globeCameraFromLookAt(pose.eye, pose.centre, pose.up, globe());
    EXPECT_NEAR(back.longitude, camera.longitude, 1e-9);
    EXPECT_NEAR(back.latitude, camera.latitude, 1e-9);
    EXPECT_NEAR(back.altitude, camera.altitude, 1e-6);
    EXPECT_NEAR(back.heading, camera.heading, 1e-7);
    EXPECT_NEAR(back.tilt, camera.tilt, 1e-7);
}

TEST(GlobeCameraMath, StraightDownLookAtRecoversTheHeadingFromTheUpVector)
{
    const ivevsg::GlobeCamera camera = over(30.0, -20.0, 2.0e6, 90.0, 0.0); // east at the top of the screen
    const ivevsg::CameraPose pose = ivevsg::globeCameraPose(camera, globe());
    const ivevsg::GlobeCamera back = ivevsg::globeCameraFromLookAt(pose.eye, pose.centre, pose.up, globe());
    EXPECT_NEAR(back.heading, 90.0, 1e-7);
    EXPECT_NEAR(back.tilt, 0.0, 1e-7);
}

// ---- picking -----------------------------------------------------------------------------------

TEST(GlobeCameraMath, ScreenCentreHitsTheGroundUnderTheCamera)
{
    const ivevsg::GlobeCamera camera = over(-45.0, 30.0, 3.0e6, 20.0, 40.0);
    ivevsg::GeoPoint hit;
    ASSERT_TRUE(ivevsg::globePointAtPixel(camera, kViewW * 0.5, kViewH * 0.5, kViewW, kViewH, settings(), globe(), hit));
    EXPECT_NEAR(hit.longitude, -45.0, 1e-6);
    EXPECT_NEAR(hit.latitude, 30.0, 1e-6);
}

TEST(GlobeCameraMath, PixelsOffTheDiscMissTheGlobe)
{
    // 60 000 km up the globe is about 40% of the view height: the corners look at space
    const ivevsg::GlobeCamera camera = over(0.0, 0.0, 6.0e7);
    ivevsg::GeoPoint hit;
    EXPECT_FALSE(ivevsg::globePointAtPixel(camera, 0.0, 0.0, kViewW, kViewH, settings(), globe(), hit));
    EXPECT_TRUE(ivevsg::globePointAtPixel(camera, kViewW * 0.5, kViewH * 0.5, kViewW, kViewH, settings(), globe(), hit));
    EXPECT_FALSE(ivevsg::globePointAtPixel(camera, kViewW * 0.5, kViewH * 0.5, 0.0, 0.0, settings(), globe(), hit)); // no viewport
}

TEST(GlobeCameraMath, PixelsRightOfCentreLookEastWhenNorthIsUp)
{
    const ivevsg::GlobeCamera camera = over(0.0, 0.0, 1.0e6);
    ivevsg::GeoPoint right, below;
    ASSERT_TRUE(ivevsg::globePointAtPixel(camera, kViewW * 0.5 + 100.0, kViewH * 0.5, kViewW, kViewH, settings(), globe(), right));
    ASSERT_TRUE(ivevsg::globePointAtPixel(camera, kViewW * 0.5, kViewH * 0.5 + 100.0, kViewW, kViewH, settings(), globe(), below));
    EXPECT_GT(right.longitude, 0.0);
    EXPECT_NEAR(right.latitude, 0.0, 1e-9);
    EXPECT_LT(below.latitude, 0.0);
    EXPECT_NEAR(below.longitude, 0.0, 1e-9);
}

// ---- spinning ----------------------------------------------------------------------------------

TEST(GlobeCameraMath, HorizontalDragSpinsAboutThePolarAxisOnly)
{
    const ivevsg::GlobeCamera camera = over(10.0, 45.0, 5.0e6);
    const double cx = kViewW * 0.5, cy = kViewH * 0.5;
    const ivevsg::GlobeCamera moved = ivevsg::spinGlobeByPixels(camera, cx, cy, cx + 80.0, cy, kViewW, kViewH, settings(), globe());
    EXPECT_LT(moved.longitude, camera.longitude); // the ground went right (east), so the camera went west
    EXPECT_NEAR(moved.latitude, camera.latitude, 1e-9);
    EXPECT_DOUBLE_EQ(moved.altitude, camera.altitude);
    EXPECT_DOUBLE_EQ(moved.heading, camera.heading);
    EXPECT_DOUBLE_EQ(moved.tilt, camera.tilt);
}

TEST(GlobeCameraMath, VerticalDragChangesLatitudeOnly)
{
    const ivevsg::GlobeCamera camera = over(10.0, 45.0, 5.0e6);
    const double cx = kViewW * 0.5, cy = kViewH * 0.5;
    const ivevsg::GlobeCamera moved = ivevsg::spinGlobeByPixels(camera, cx, cy, cx, cy + 80.0, kViewW, kViewH, settings(), globe());
    EXPECT_GT(moved.latitude, camera.latitude); // the ground went down (south), so the camera went north
    EXPECT_NEAR(moved.longitude, camera.longitude, 1e-9);
}

TEST(GlobeCameraMath, TheGroundUnderTheCursorFollowsTheDrag)
{
    const ivevsg::GlobeCamera camera = over(-30.0, 20.0, 2.0e6, 15.0, 25.0);
    const double fromX = 300.0, fromY = 250.0, toX = 420.0, toY = 330.0;
    ivevsg::GeoPoint before;
    ASSERT_TRUE(ivevsg::globePointAtPixel(camera, fromX, fromY, kViewW, kViewH, settings(), globe(), before));

    const ivevsg::GlobeCamera moved = ivevsg::spinGlobeByPixels(camera, fromX, fromY, toX, toY, kViewW, kViewH, settings(), globe());
    ivevsg::GeoPoint after;
    ASSERT_TRUE(ivevsg::globePointAtPixel(moved, toX, toY, kViewW, kViewH, settings(), globe(), after));
    // a lon/lat offset is not a rigid rotation on a tilted view, so allow a small residual
    EXPECT_NEAR(after.longitude, before.longitude, 0.05);
    EXPECT_NEAR(after.latitude, before.latitude, 0.05);
}

TEST(GlobeCameraMath, DraggingOffTheGlobeStillSpinsIt)
{
    const ivevsg::GlobeCamera camera = over(0.0, 0.0, 6.0e7); // whole globe visible, corners are space
    const ivevsg::GlobeCamera moved = ivevsg::spinGlobeByPixels(camera, 10.0, 10.0, 60.0, 10.0, kViewW, kViewH, settings(), globe());
    EXPECT_LT(moved.longitude, 0.0);
    EXPECT_NEAR(moved.latitude, 0.0, 1e-9);
    const ivevsg::GlobeCamera rolled = ivevsg::spinGlobeByPixels(camera, 10.0, 10.0, 10.0, 60.0, kViewW, kViewH, settings(), globe());
    EXPECT_GT(rolled.latitude, 0.0);
    EXPECT_NEAR(rolled.longitude, 0.0, 1e-9);
}

TEST(GlobeCameraMath, ClampStopsAtThePolesAndWrapsLongitude)
{
    ivevsg::GlobeNavigationSettings s = settings();
    ivevsg::GlobeCamera camera = over(200.0, 95.0, 1.0, -190.0, 120.0);
    const ivevsg::GlobeCamera clamped = ivevsg::clampGlobeCamera(camera, s);
    EXPECT_DOUBLE_EQ(clamped.latitude, s.maxLatitude);
    EXPECT_NEAR(clamped.longitude, -160.0, 1e-12);
    EXPECT_NEAR(clamped.heading, 170.0, 1e-12);
    EXPECT_DOUBLE_EQ(clamped.tilt, s.maxTilt);
    EXPECT_DOUBLE_EQ(clamped.altitude, s.minAltitude);
    camera.altitude = 1.0e12;
    EXPECT_DOUBLE_EQ(ivevsg::clampGlobeCamera(camera, s).altitude, s.maxAltitude);
}

// ---- zooming -----------------------------------------------------------------------------------

TEST(GlobeCameraMath, ZoomAboutAPixelKeepsThatGroundUnderIt)
{
    const ivevsg::GlobeCamera camera = over(5.0, 40.0, 1.0e6);
    const double px = 600.0, py = 150.0;
    ivevsg::GeoPoint before;
    ASSERT_TRUE(ivevsg::globePointAtPixel(camera, px, py, kViewW, kViewH, settings(), globe(), before));
    const ivevsg::GlobeCamera zoomed = ivevsg::zoomGlobeAboutPixel(camera, 0.5, px, py, kViewW, kViewH, settings(), globe());
    EXPECT_DOUBLE_EQ(zoomed.altitude, 5.0e5);
    ivevsg::GeoPoint after;
    ASSERT_TRUE(ivevsg::globePointAtPixel(zoomed, px, py, kViewW, kViewH, settings(), globe(), after));
    EXPECT_NEAR(after.longitude, before.longitude, 0.02);
    EXPECT_NEAR(after.latitude, before.latitude, 0.02);
}

TEST(GlobeCameraMath, ZoomWithTheCursorInSpaceOnlyChangesTheAltitude)
{
    const ivevsg::GlobeCamera camera = over(5.0, 40.0, 6.0e7);
    const ivevsg::GlobeCamera zoomed = ivevsg::zoomGlobeAboutPixel(camera, 0.5, 0.0, 0.0, kViewW, kViewH, settings(), globe());
    EXPECT_DOUBLE_EQ(zoomed.altitude, 3.0e7);
    EXPECT_DOUBLE_EQ(zoomed.longitude, 5.0);
    EXPECT_DOUBLE_EQ(zoomed.latitude, 40.0);
}

// ---- handler -----------------------------------------------------------------------------------

TEST(GlobeNavigationHandler, StartsFromTheCamerasLookAt)
{
    auto camera = makeCamera(over(12.0, -8.0, 7.0e5, 30.0, 20.0));
    auto handler = ivevsg::GlobeNavigationHandler::create(camera, std::make_shared<ivevsg::GeocentricProjection>());
    EXPECT_NEAR(handler->globeCamera().longitude, 12.0, 1e-9);
    EXPECT_NEAR(handler->globeCamera().latitude, -8.0, 1e-9);
    EXPECT_NEAR(handler->globeCamera().altitude, 7.0e5, 1e-6);
    EXPECT_NEAR(handler->globeCamera().heading, 30.0, 1e-7);
    EXPECT_NEAR(handler->globeCamera().tilt, 20.0, 1e-7);
    EXPECT_DOUBLE_EQ(handler->settings().fieldOfViewDegrees, kFov);
}

TEST(GlobeNavigationHandler, LeftDragSpinsTheGlobeAndWritesTheLookAt)
{
    auto camera = makeCamera(over(0.0, 0.0, 5.0e6));
    auto handler = ivevsg::GlobeNavigationHandler::create(camera, std::make_shared<ivevsg::GeocentricProjection>());

    auto press = vsg::ButtonPressEvent::create(vsg::ref_ptr<vsg::Window>(), vsg::clock::now(), 400, 300, vsg::BUTTON_MASK_1, 1);
    press->accept(*handler);
    EXPECT_TRUE(press->handled);
    auto drag = move(460.0, 300.0, vsg::BUTTON_MASK_1);
    drag->accept(*handler);
    EXPECT_TRUE(drag->handled);

    const ivevsg::GlobeCamera& c = handler->globeCamera();
    EXPECT_LT(c.longitude, 0.0);
    EXPECT_NEAR(c.latitude, 0.0, 1e-9);
    EXPECT_NEAR(c.heading, 0.0, 1e-9);
    EXPECT_NEAR(c.tilt, 0.0, 1e-9);

    const ivevsg::CameraPose pose = ivevsg::globeCameraPose(c, globe());
    EXPECT_NEAR(vsg::length(lookAtOf(camera)->eye - pose.eye), 0.0, 1e-6);
    EXPECT_NEAR(vsg::length(lookAtOf(camera)->center - pose.centre), 0.0, 1e-6);

    auto release = vsg::ButtonReleaseEvent::create(vsg::ref_ptr<vsg::Window>(), vsg::clock::now(), 460, 300, vsg::BUTTON_MASK_OFF, 1);
    release->accept(*handler);
    const double longitude = c.longitude;
    move(500.0, 300.0, vsg::BUTTON_MASK_OFF)->accept(*handler); // no button: just tracks the pointer
    EXPECT_DOUBLE_EQ(c.longitude, longitude);
}

TEST(GlobeNavigationHandler, MiddleDragTiltsAndTurnsAboutTheCentre)
{
    auto camera = makeCamera(over(20.0, 10.0, 1.0e5));
    auto handler = ivevsg::GlobeNavigationHandler::create(camera, std::make_shared<ivevsg::GeocentricProjection>());
    vsg::ButtonPressEvent::create(vsg::ref_ptr<vsg::Window>(), vsg::clock::now(), 400, 300, vsg::BUTTON_MASK_2, 2)->accept(*handler);
    move(440.0, 260.0, vsg::BUTTON_MASK_2)->accept(*handler); // right and up
    const ivevsg::GlobeCamera& c = handler->globeCamera();
    EXPECT_NEAR(c.heading, -10.0, 1e-9); // 40 px * 0.25 deg
    EXPECT_NEAR(c.tilt, 10.0, 1e-9);
    EXPECT_NEAR(c.longitude, 20.0, 1e-9);
    EXPECT_NEAR(c.latitude, 10.0, 1e-9);
    EXPECT_NEAR(c.altitude, 1.0e5, 1e-6);

    handler->northUp();
    EXPECT_DOUBLE_EQ(c.heading, 0.0);
    EXPECT_DOUBLE_EQ(c.tilt, 0.0);
}

TEST(GlobeNavigationHandler, WheelZoomsAboutTheCursorAndRightDragZoomsAboutTheCentre)
{
    auto camera = makeCamera(over(0.0, 0.0, 1.0e6));
    auto handler = ivevsg::GlobeNavigationHandler::create(camera, std::make_shared<ivevsg::GeocentricProjection>());
    move(600.0, 300.0, vsg::BUTTON_MASK_OFF)->accept(*handler); // cursor east of the centre
    auto wheel = vsg::ScrollWheelEvent::create(vsg::ref_ptr<vsg::Window>(), vsg::clock::now(), vsg::vec3(0.0f, 1.0f, 0.0f));
    wheel->accept(*handler);
    EXPECT_TRUE(wheel->handled);
    EXPECT_NEAR(handler->globeCamera().altitude, 8.0e5, 1e-6);
    EXPECT_GT(handler->globeCamera().longitude, 0.0); // slid towards the cursor

    const double longitude = handler->globeCamera().longitude;
    vsg::ButtonPressEvent::create(vsg::ref_ptr<vsg::Window>(), vsg::clock::now(), 600, 300, vsg::BUTTON_MASK_3, 3)->accept(*handler);
    move(600.0, 500.0, vsg::BUTTON_MASK_3)->accept(*handler); // 200 px down = one octave out
    EXPECT_NEAR(handler->globeCamera().altitude, 1.6e6, 1e-6);
    EXPECT_DOUBLE_EQ(handler->globeCamera().longitude, longitude);
}

TEST(GlobeNavigationHandler, HandledEventsAreIgnored)
{
    auto camera = makeCamera(over(0.0, 0.0, 1.0e6));
    auto handler = ivevsg::GlobeNavigationHandler::create(camera, std::make_shared<ivevsg::GeocentricProjection>());
    auto press = vsg::ButtonPressEvent::create(vsg::ref_ptr<vsg::Window>(), vsg::clock::now(), 400, 300, vsg::BUTTON_MASK_1, 1);
    press->handled = true; // ImGui took it
    press->accept(*handler);
    move(460.0, 300.0, vsg::BUTTON_MASK_1)->accept(*handler);
    EXPECT_DOUBLE_EQ(handler->globeCamera().longitude, 0.0);

    auto wheel = vsg::ScrollWheelEvent::create(vsg::ref_ptr<vsg::Window>(), vsg::clock::now(), vsg::vec3(0.0f, 1.0f, 0.0f));
    wheel->handled = true;
    wheel->accept(*handler);
    EXPECT_DOUBLE_EQ(handler->globeCamera().altitude, 1.0e6);
}

TEST(GlobeNavigationHandler, KeysSpinZoomTiltAndReset)
{
    auto camera = makeCamera(over(0.0, 0.0, 1.0e6));
    auto handler = ivevsg::GlobeNavigationHandler::create(camera, std::make_shared<ivevsg::GeocentricProjection>());
    auto key = [&](vsg::KeySymbol symbol) {
        auto event = vsg::KeyPressEvent::create(vsg::ref_ptr<vsg::Window>(), vsg::clock::now(), symbol, symbol, vsg::MODKEY_OFF);
        event->accept(*handler);
        return event->handled;
    };
    EXPECT_TRUE(key(vsg::KEY_Left));
    EXPECT_LT(handler->globeCamera().longitude, 0.0); // the view moves west
    EXPECT_TRUE(key(vsg::KEY_Up));
    EXPECT_GT(handler->globeCamera().latitude, 0.0);
    EXPECT_TRUE(key(vsg::KEY_Plus));
    EXPECT_NEAR(handler->globeCamera().altitude, 5.0e5, 1e-6);
    EXPECT_TRUE(key(vsg::KEY_Page_Up));
    EXPECT_DOUBLE_EQ(handler->globeCamera().tilt, 10.0);
    EXPECT_TRUE(key(vsg::KEY_n));
    EXPECT_DOUBLE_EQ(handler->globeCamera().tilt, 0.0);
    EXPECT_TRUE(key(vsg::KEY_Home));
    EXPECT_DOUBLE_EQ(handler->globeCamera().longitude, 0.0);
    EXPECT_DOUBLE_EQ(handler->globeCamera().altitude, 1.0e6);
    EXPECT_FALSE(key(vsg::KEY_a));
}

TEST(GlobeNavigationHandler, ZoomLevelRangeNeverSendsTheGlobeOutOfSight)
{
    auto camera = makeCamera(over(0.0, 0.0, 1.0e6));
    auto handler = ivevsg::GlobeNavigationHandler::create(camera, std::make_shared<ivevsg::GeocentricProjection>());
    handler->setZoomLevelRange(0.0, 22.0);
    EXPECT_LE(handler->settings().maxAltitude, 6.0e7); // zoom 0 would be 3e8 m, far beyond the handler's ceiling
    EXPECT_GT(handler->minZoomLevel(), 0.0);
    handler->setZoomLevel(10.0);
    EXPECT_NEAR(handler->zoomLevel(), 10.0, 1e-9);
    EXPECT_NEAR(handler->maxZoomLevel(), 22.0, 1e-9);
}
