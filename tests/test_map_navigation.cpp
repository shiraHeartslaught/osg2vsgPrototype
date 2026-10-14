#include <ivevsg/MapNavigation.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

#include <cmath>

namespace
{
    constexpr double kFov = 30.0;
    constexpr double kWorldWidth = 40075016.685578488;
    constexpr double kViewW = 800.0;
    constexpr double kViewH = 600.0;

    vsg::ref_ptr<vsg::Camera> makeTopDownCamera(double altitude)
    {
        auto lookAt = vsg::LookAt::create(vsg::dvec3(0.0, 0.0, altitude), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 1.0, 0.0));
        auto perspective = vsg::Perspective::create(kFov, kViewW / kViewH, 1.0, 1.0e8);
        return vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(VkExtent2D{static_cast<uint32_t>(kViewW), static_cast<uint32_t>(kViewH)}));
    }

    vsg::ref_ptr<vsg::LookAt> lookAtOf(vsg::ref_ptr<vsg::Camera> camera) { return camera->viewMatrix.cast<vsg::LookAt>(); }
} // namespace

TEST(MapNavigationMath, MetersPerPixelFollowsTheFrustumGeometry)
{
    // 90 degree fov, 1000 m up: the view is 2000 m tall; over 1000 pixels that is 2 m per pixel.
    EXPECT_NEAR(ivevsg::metersPerPixel(1000.0, 90.0, 1000.0), 2.0, 1e-12);
    EXPECT_DOUBLE_EQ(ivevsg::metersPerPixel(1000.0, 90.0, 0.0), 1.0); // degenerate viewport is harmless
}

TEST(MapNavigationMath, ZoomLevelZeroShowsTheWorldIn256Pixels)
{
    const double altitude = ivevsg::altitudeForZoomLevel(0.0, kFov, kViewH, kWorldWidth);
    EXPECT_NEAR(ivevsg::metersPerPixel(altitude, kFov, kViewH), kWorldWidth / 256.0, 1e-6);
}

TEST(MapNavigationMath, ZoomLevelRoundTrips)
{
    for (double z : {0.0, 3.0, 7.5, 12.0, 18.0})
    {
        const double altitude = ivevsg::altitudeForZoomLevel(z, kFov, kViewH, kWorldWidth);
        EXPECT_NEAR(ivevsg::zoomLevelForAltitude(altitude, kFov, kViewH, kWorldWidth), z, 1e-9);
    }
}

TEST(MapNavigationMath, EachZoomLevelHalvesTheAltitude)
{
    const double z5 = ivevsg::altitudeForZoomLevel(5.0, kFov, kViewH, kWorldWidth);
    const double z6 = ivevsg::altitudeForZoomLevel(6.0, kFov, kViewH, kWorldWidth);
    EXPECT_NEAR(z6 * 2.0, z5, 1e-6);
}

TEST(MapNavigationMath, ViewportCentreIsTheCameraCentre)
{
    const ivevsg::MapCamera camera{{1000.0, -2000.0}, 5000.0};
    const vsg::dvec2 ground = ivevsg::groundPointAtPixel(camera, kViewW / 2.0, kViewH / 2.0, kViewW, kViewH, kFov);
    EXPECT_DOUBLE_EQ(ground.x, 1000.0);
    EXPECT_DOUBLE_EQ(ground.y, -2000.0);
}

TEST(MapNavigationMath, PixelRightIsEastAndPixelDownIsSouth)
{
    const ivevsg::MapCamera camera{{0.0, 0.0}, 5000.0};
    const double mpp = ivevsg::metersPerPixel(camera.altitude, kFov, kViewH);
    const vsg::dvec2 right = ivevsg::groundPointAtPixel(camera, kViewW / 2.0 + 10.0, kViewH / 2.0, kViewW, kViewH, kFov);
    const vsg::dvec2 down = ivevsg::groundPointAtPixel(camera, kViewW / 2.0, kViewH / 2.0 + 10.0, kViewW, kViewH, kFov);
    EXPECT_NEAR(right.x, 10.0 * mpp, 1e-9);
    EXPECT_NEAR(right.y, 0.0, 1e-9);
    EXPECT_NEAR(down.x, 0.0, 1e-9);
    EXPECT_NEAR(down.y, -10.0 * mpp, 1e-9);
}

TEST(MapNavigationMath, PanByPixelsMovesTheMapWithTheCursor)
{
    const ivevsg::MapCamera camera{{0.0, 0.0}, 5000.0};
    const double mpp = ivevsg::metersPerPixel(camera.altitude, kFov, kViewH);
    // dragging the mouse right and down must move the camera left (west) and up (north)
    const ivevsg::MapCamera moved = ivevsg::panByPixels(camera, 100.0, 50.0, kViewH, kFov);
    EXPECT_NEAR(moved.centre.x, -100.0 * mpp, 1e-9);
    EXPECT_NEAR(moved.centre.y, 50.0 * mpp, 1e-9);
    EXPECT_DOUBLE_EQ(moved.altitude, camera.altitude);
}

TEST(MapNavigationMath, ZoomAboutPixelKeepsTheAnchorUnderTheCursor)
{
    const ivevsg::MapCamera camera{{1.0e6, 2.0e6}, 1.0e5};
    const double px = 650.0, py = 120.0;
    const vsg::dvec2 before = ivevsg::groundPointAtPixel(camera, px, py, kViewW, kViewH, kFov);
    const ivevsg::MapCamera zoomed = ivevsg::zoomAboutPixel(camera, 0.5, px, py, kViewW, kViewH, kFov);
    const vsg::dvec2 after = ivevsg::groundPointAtPixel(zoomed, px, py, kViewW, kViewH, kFov);
    EXPECT_NEAR(after.x, before.x, 1e-6);
    EXPECT_NEAR(after.y, before.y, 1e-6);
    EXPECT_DOUBLE_EQ(zoomed.altitude, 0.5e5);
}

TEST(MapNavigationMath, ZoomAboutTheCentreDoesNotPan)
{
    const ivevsg::MapCamera camera{{123.0, 456.0}, 1.0e5};
    const ivevsg::MapCamera zoomed = ivevsg::zoomAboutPixel(camera, 2.0, kViewW / 2.0, kViewH / 2.0, kViewW, kViewH, kFov);
    EXPECT_DOUBLE_EQ(zoomed.centre.x, 123.0);
    EXPECT_DOUBLE_EQ(zoomed.centre.y, 456.0);
    EXPECT_DOUBLE_EQ(zoomed.altitude, 2.0e5);
}

TEST(MapNavigationMath, ClampLimitsAltitude)
{
    ivevsg::MapNavigationSettings settings;
    settings.minAltitude = 100.0;
    settings.maxAltitude = 1000.0;
    EXPECT_DOUBLE_EQ(ivevsg::clampCamera({{0.0, 0.0}, 1.0}, settings).altitude, 100.0);
    EXPECT_DOUBLE_EQ(ivevsg::clampCamera({{0.0, 0.0}, 1.0e9}, settings).altitude, 1000.0);
    // without limits or wrapping the position is untouched
    const ivevsg::MapCamera anywhere = ivevsg::clampCamera({{1.0e12, -1.0e12}, 500.0}, settings, kViewW, kViewH);
    EXPECT_DOUBLE_EQ(anywhere.centre.x, 1.0e12);
    EXPECT_DOUBLE_EQ(anywhere.centre.y, -1.0e12);
}

TEST(MapNavigationMath, WrapFoldsXIntoOneWorldWidth)
{
    ivevsg::MapNavigationSettings settings;
    settings.wrapWidth = 1000.0;
    EXPECT_DOUBLE_EQ(ivevsg::clampCamera({{1200.0, 7.0}, 50.0}, settings).centre.x, 200.0);
    EXPECT_DOUBLE_EQ(ivevsg::clampCamera({{-1200.0, 7.0}, 50.0}, settings).centre.x, -200.0);
    EXPECT_DOUBLE_EQ(ivevsg::clampCamera({{2600.0, 7.0}, 50.0}, settings).centre.x, -400.0);
    EXPECT_DOUBLE_EQ(ivevsg::clampCamera({{300.0, 7.0}, 50.0}, settings).centre.x, 300.0);
    EXPECT_DOUBLE_EQ(ivevsg::clampCamera({{300.0, 7.0}, 50.0}, settings).centre.y, 7.0); // y untouched
}

TEST(MapNavigationMath, ViewHalfExtentsFollowAltitudeAndAspect)
{
    const ivevsg::MapCamera camera{{0.0, 0.0}, 1000.0};
    const vsg::dvec2 half = ivevsg::viewHalfExtents(camera, 90.0, 800.0, 400.0);
    EXPECT_NEAR(half.y, 1000.0, 1e-9); // tan(45) * altitude
    EXPECT_NEAR(half.x, 2000.0, 1e-9); // times aspect 2
}

TEST(MapNavigationMath, ViewLimitsStopTheCameraAtTheMapEdge)
{
    ivevsg::MapNavigationSettings settings;
    settings.fieldOfViewDegrees = 90.0;
    settings.viewLimits = vsg::dbox(vsg::dvec3(-10000.0, -10000.0, 0.0), vsg::dvec3(10000.0, 10000.0, 0.0));
    const double vw = 800.0, vh = 800.0; // square viewport: view half extent == altitude

    // comfortably inside: untouched
    const ivevsg::MapCamera inside = ivevsg::clampCamera({{0.0, 0.0}, 1000.0}, settings, vw, vh);
    EXPECT_DOUBLE_EQ(inside.centre.y, 0.0);
    EXPECT_DOUBLE_EQ(inside.altitude, 1000.0);

    // the moment the top edge of the view would pass the map's top edge, the camera is held back
    const ivevsg::MapCamera north = ivevsg::clampCamera({{0.0, 9800.0}, 1000.0}, settings, vw, vh);
    EXPECT_DOUBLE_EQ(north.centre.y, 9000.0);
    const ivevsg::MapCamera south = ivevsg::clampCamera({{0.0, -20000.0}, 1000.0}, settings, vw, vh);
    EXPECT_DOUBLE_EQ(south.centre.y, -9000.0);

    // without wrapping the same applies east/west
    const ivevsg::MapCamera east = ivevsg::clampCamera({{30000.0, 0.0}, 1000.0}, settings, vw, vh);
    EXPECT_DOUBLE_EQ(east.centre.x, 9000.0);
}

TEST(MapNavigationMath, ViewLimitsCapTheAltitudeSoTheWholeMapJustFits)
{
    ivevsg::MapNavigationSettings settings;
    settings.fieldOfViewDegrees = 90.0;
    settings.viewLimits = vsg::dbox(vsg::dvec3(-10000.0, -10000.0, 0.0), vsg::dvec3(10000.0, 10000.0, 0.0));

    // zoomed out far beyond the map: altitude drops to show exactly the map's height, centred vertically
    const ivevsg::MapCamera far = ivevsg::clampCamera({{0.0, 5000.0}, 1.0e6}, settings, 800.0, 800.0);
    EXPECT_NEAR(far.altitude, 10000.0, 1e-9);
    EXPECT_NEAR(far.centre.y, 0.0, 1e-9);

    // a wide viewport without wrapping is also limited by the map's width
    const ivevsg::MapCamera wide = ivevsg::clampCamera({{0.0, 0.0}, 1.0e6}, settings, 1600.0, 800.0);
    EXPECT_NEAR(wide.altitude, 5000.0, 1e-9);

    // with wrapping only the height limits the altitude, and x is free (folded, not clamped)
    settings.wrapWidth = 20000.0;
    const ivevsg::MapCamera wrapped = ivevsg::clampCamera({{25000.0, 0.0}, 1.0e6}, settings, 1600.0, 800.0);
    EXPECT_NEAR(wrapped.altitude, 10000.0, 1e-9);
    EXPECT_NEAR(wrapped.centre.x, 5000.0, 1e-9);
}

TEST(MapNavigationHandler, TakesItsInitialStateFromTheCamera)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera);
    EXPECT_DOUBLE_EQ(handler->mapCamera().altitude, 2.0e6);
    EXPECT_DOUBLE_EQ(handler->settings().fieldOfViewDegrees, kFov);
    EXPECT_DOUBLE_EQ(handler->viewportWidth(), kViewW);
    EXPECT_DOUBLE_EQ(handler->viewportHeight(), kViewH);
}

TEST(MapNavigationHandler, LeftDragPansTheCamera)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera);
    const double mpp = ivevsg::metersPerPixel(2.0e6, kFov, kViewH);
    const auto now = vsg::clock::now();

    vsg::ButtonPressEvent::create(nullptr, now, 400, 300, vsg::BUTTON_MASK_1, 1)->accept(*handler);
    vsg::MoveEvent::create(nullptr, now, 500, 340, vsg::BUTTON_MASK_1)->accept(*handler);
    vsg::ButtonReleaseEvent::create(nullptr, now, 500, 340, vsg::BUTTON_MASK_OFF, 1)->accept(*handler);

    auto lookAt = lookAtOf(camera);
    EXPECT_NEAR(lookAt->eye.x, -100.0 * mpp, 1e-6);
    EXPECT_NEAR(lookAt->eye.y, 40.0 * mpp, 1e-6);
    EXPECT_DOUBLE_EQ(lookAt->eye.z, 2.0e6);
    EXPECT_DOUBLE_EQ(lookAt->center.z, 0.0);
    EXPECT_DOUBLE_EQ(lookAt->up.y, 1.0);

    // moving without the button held must not pan
    vsg::MoveEvent::create(nullptr, now, 600, 340, vsg::BUTTON_MASK_OFF)->accept(*handler);
    EXPECT_NEAR(lookAtOf(camera)->eye.x, -100.0 * mpp, 1e-6);
}

TEST(MapNavigationHandler, WheelZoomsAboutTheCursorAndKeepsDepthRangeSane)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera);
    const auto now = vsg::clock::now();

    vsg::MoveEvent::create(nullptr, now, 700, 100, vsg::BUTTON_MASK_OFF)->accept(*handler); // park the cursor
    const vsg::dvec2 anchorBefore = ivevsg::groundPointAtPixel(handler->mapCamera(), 700.0, 100.0, kViewW, kViewH, kFov);

    vsg::ScrollWheelEvent::create(nullptr, now, vsg::vec3(0.0f, 1.0f, 0.0f))->accept(*handler); // scroll towards the map
    EXPECT_NEAR(handler->mapCamera().altitude, 2.0e6 * handler->settings().scrollZoomFactor, 1e-6);
    const vsg::dvec2 anchorAfter = ivevsg::groundPointAtPixel(handler->mapCamera(), 700.0, 100.0, kViewW, kViewH, kFov);
    EXPECT_NEAR(anchorAfter.x, anchorBefore.x, 1e-6);
    EXPECT_NEAR(anchorAfter.y, anchorBefore.y, 1e-6);

    auto perspective = camera->projectionMatrix.cast<vsg::Perspective>();
    EXPECT_GT(perspective->nearDistance, 0.0);
    EXPECT_GT(perspective->farDistance, handler->mapCamera().altitude + handler->settings().farPlanePadding * 0.99);

    vsg::ScrollWheelEvent::create(nullptr, now, vsg::vec3(0.0f, -1.0f, 0.0f))->accept(*handler); // and back out
    EXPECT_NEAR(handler->mapCamera().altitude, 2.0e6, 1e-6);
}

TEST(MapNavigationHandler, KeysPanZoomAndGoHome)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera);
    const auto now = vsg::clock::now();
    const ivevsg::MapCamera home = handler->mapCamera();

    vsg::KeyPressEvent::create(nullptr, now, vsg::KEY_Plus, vsg::KEY_Plus, vsg::MODKEY_OFF)->accept(*handler);
    EXPECT_NEAR(handler->mapCamera().altitude, 2.0e6 * handler->settings().keyZoomFactor, 1e-6);

    vsg::KeyPressEvent::create(nullptr, now, vsg::KEY_Right, vsg::KEY_Right, vsg::MODKEY_OFF)->accept(*handler);
    EXPECT_GT(handler->mapCamera().centre.x, 0.0); // the view moved east

    vsg::KeyPressEvent::create(nullptr, now, vsg::KEY_Home, vsg::KEY_Home, vsg::MODKEY_OFF)->accept(*handler);
    EXPECT_DOUBLE_EQ(handler->mapCamera().altitude, home.altitude);
    EXPECT_DOUBLE_EQ(handler->mapCamera().centre.x, home.centre.x);
}

TEST(MapNavigationHandler, ZoomLevelIsConsistentWithAltitude)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera);
    handler->setZoomLevel(4.0);
    EXPECT_NEAR(handler->zoomLevel(), 4.0, 1e-9);
    EXPECT_NEAR(lookAtOf(camera)->eye.z, ivevsg::altitudeForZoomLevel(4.0, kFov, kViewH, handler->settings().worldWidthMetres), 1e-6);
}

namespace
{
    ivevsg::MapNavigationSettings mercatorLikeSettings()
    {
        ivevsg::MapNavigationSettings settings;
        settings.viewLimits = vsg::dbox(vsg::dvec3(-kWorldWidth / 2.0, -kWorldWidth / 2.0, 0.0), vsg::dvec3(kWorldWidth / 2.0, kWorldWidth / 2.0, 0.0));
        settings.wrapWidth = kWorldWidth;
        return settings;
    }
} // namespace

TEST(MapNavigationHandler, PanningEastForeverWrapsAroundTheWorld)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera, mercatorLikeSettings());
    const auto now = vsg::clock::now();
    const double mpp = ivevsg::metersPerPixel(2.0e6, kFov, kViewH);
    const int dragsNeeded = static_cast<int>(kWorldWidth / (400.0 * mpp)) + 2; // each drag moves 400 px west->east

    for (int i = 0; i < dragsNeeded; ++i)
    {
        vsg::ButtonPressEvent::create(nullptr, now, 600, 300, vsg::BUTTON_MASK_1, 1)->accept(*handler);
        vsg::MoveEvent::create(nullptr, now, 200, 300, vsg::BUTTON_MASK_1)->accept(*handler); // drag left = look east
        vsg::ButtonReleaseEvent::create(nullptr, now, 200, 300, vsg::BUTTON_MASK_OFF, 1)->accept(*handler);
    }
    const double x = handler->mapCamera().centre.x;
    EXPECT_GE(x, -kWorldWidth / 2.0);
    EXPECT_LE(x, kWorldWidth / 2.0);
    EXPECT_DOUBLE_EQ(lookAtOf(camera)->eye.x, x);
}

TEST(MapNavigationHandler, ZoomingOutStopsWhenThePolesReachTheViewEdge)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera, mercatorLikeSettings());
    const auto now = vsg::clock::now();

    for (int i = 0; i < 100; ++i) vsg::ScrollWheelEvent::create(nullptr, now, vsg::vec3(0.0f, -1.0f, 0.0f))->accept(*handler);

    const vsg::dvec2 half = ivevsg::viewHalfExtents(handler->mapCamera(), kFov, kViewW, kViewH);
    EXPECT_NEAR(half.y, kWorldWidth / 2.0, 1.0); // the view is exactly as tall as the map ...
    EXPECT_NEAR(handler->mapCamera().centre.y, 0.0, 1e-6); // ... and centred on it
    EXPECT_GT(half.x, kWorldWidth / 2.0); // wider than the map: the wrapped copies fill the sides
}

TEST(MapNavigationHandler, PanningNorthStopsAtTheMapEdge)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera, mercatorLikeSettings());
    const auto now = vsg::clock::now();

    for (int i = 0; i < 200; ++i) vsg::KeyPressEvent::create(nullptr, now, vsg::KEY_Up, vsg::KEY_Up, vsg::MODKEY_OFF)->accept(*handler);

    const vsg::dvec2 half = ivevsg::viewHalfExtents(handler->mapCamera(), kFov, kViewW, kViewH);
    EXPECT_NEAR(handler->mapCamera().centre.y + half.y, kWorldWidth / 2.0, 1e-6);
}

TEST(MapNavigationHandler, ZoomLevelRangeBoundsTheZoomNotTheData)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera);
    handler->setZoomLevelRange(1.0, 20.0);
    EXPECT_NEAR(handler->minZoomLevel(), 1.0, 1e-9);
    EXPECT_NEAR(handler->maxZoomLevel(), 20.0, 1e-9);

    // asking for more zoom than the ceiling stops at the ceiling, and the camera still looks straight down
    handler->setZoomLevel(25.0);
    EXPECT_NEAR(handler->zoomLevel(), 20.0, 1e-9);
    EXPECT_DOUBLE_EQ(lookAtOf(camera)->center.z, 0.0);
    EXPECT_GT(lookAtOf(camera)->eye.z, 0.0);

    // well beyond any tile level is still allowed when the range says so
    handler->setZoomLevelRange(0.0, 28.0);
    handler->setZoomLevel(24.0);
    EXPECT_NEAR(handler->zoomLevel(), 24.0, 1e-9);
    EXPECT_LT(lookAtOf(camera)->eye.z, 20.0); // a few metres up at zoom 24

    // the floor works the same way
    handler->setZoomLevel(-3.0);
    EXPECT_NEAR(handler->zoomLevel(), 0.0, 1e-9);
}

TEST(MapNavigationHandler, LeavesEventsAlreadyHandledAlone)
{
    auto camera = makeTopDownCamera(2.0e6);
    auto handler = ivevsg::MapNavigationHandler::create(camera);
    const auto now = vsg::clock::now();
    const ivevsg::MapCamera before = handler->mapCamera();

    auto press = vsg::ButtonPressEvent::create(nullptr, now, 400, 300, vsg::BUTTON_MASK_1, 1);
    press->handled = true; // an ImGui window took the click
    press->accept(*handler);
    vsg::MoveEvent::create(nullptr, now, 500, 340, vsg::BUTTON_MASK_1)->accept(*handler);
    EXPECT_DOUBLE_EQ(handler->mapCamera().centre.x, before.centre.x);
    EXPECT_DOUBLE_EQ(handler->mapCamera().centre.y, before.centre.y);

    auto wheel = vsg::ScrollWheelEvent::create(nullptr, now, vsg::vec3(0.0f, 1.0f, 0.0f));
    wheel->handled = true;
    wheel->accept(*handler);
    EXPECT_DOUBLE_EQ(handler->mapCamera().altitude, before.altitude);

    auto key = vsg::KeyPressEvent::create(nullptr, now, vsg::KEY_Plus, vsg::KEY_Plus, vsg::MODKEY_OFF);
    key->handled = true;
    key->accept(*handler);
    EXPECT_DOUBLE_EQ(handler->mapCamera().altitude, before.altitude);

    // the pointer position is still tracked, so a cursor readout keeps working over the UI
    const auto pointer = handler->lastPointer();
    EXPECT_TRUE(pointer.valid);
    EXPECT_DOUBLE_EQ(pointer.x, 500.0);
    EXPECT_DOUBLE_EQ(pointer.y, 340.0);
}
