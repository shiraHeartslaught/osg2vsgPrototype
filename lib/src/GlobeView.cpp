#include <ivevsg/GlobeView.h>

#include <vsg/all.h>

namespace ivevsg
{
    GlobeView::GlobeView(GlobeNavigationSettings navigation) :
        _projection(std::make_shared<GeocentricProjection>()),
        _settings(std::move(navigation))
    {
    }

    void GlobeView::attach(vsg::ref_ptr<vsg::Camera> camera, const vsg::dbox& bounds, double elevationDegrees)
    {
        detach();
        _camera = camera;
        _elevationDegrees = elevationDegrees;
        if (!_camera) return;
        const CameraPose pose = _projection->framingPose(bounds, elevationDegrees);
        if (auto lookAt = _camera->viewMatrix.cast<vsg::LookAt>())
        {
            lookAt->eye = pose.eye;
            lookAt->center = pose.centre;
            lookAt->up = pose.up;
        }
        if (auto perspective = _camera->projectionMatrix.cast<vsg::Perspective>())
        {
            perspective->nearDistance = pose.nearPlane;
            perspective->farDistance = pose.farPlane;
        }
        _navigation = GlobeNavigationHandler::create(_camera, _projection, _settings); // reads the pose back as its start and home
    }

    void GlobeView::detach()
    {
        _navigation = {};
        _camera = {};
    }

    vsg::ref_ptr<vsg::Visitor> GlobeView::eventHandler() const
    {
        return _navigation;
    }

    void GlobeView::frame(const vsg::dbox& bounds)
    {
        if (!_navigation || !bounds.valid()) return;
        const CameraPose pose = _projection->framingPose(bounds, _elevationDegrees);
        _navigation->setGlobeCamera(globeCameraFromLookAt(pose.eye, pose.centre, pose.up, *_projection));
        _navigation->setHome(_navigation->globeCamera());
    }

    void GlobeView::home()
    {
        if (_navigation) _navigation->home();
    }

    void GlobeView::lookAt(const GeoPoint& target, double altitudeMetres)
    {
        if (!_navigation) return;
        GlobeCamera camera;
        camera.longitude = target.longitude;
        camera.latitude = target.latitude;
        camera.altitude = altitudeMetres;
        _navigation->setGlobeCamera(camera); // straight down, north up
    }

    ViewState GlobeView::state() const
    {
        ViewState state;
        if (!_navigation) return state;
        const GlobeCamera& camera = _navigation->globeCamera();
        state.centre = GeoPoint{camera.longitude, camera.latitude, 0.0};
        state.altitude = camera.altitude;
        state.zoomLevel = _navigation->zoomLevel();
        state.metersPerPixel = metersPerPixel(camera.altitude, _navigation->settings().fieldOfViewDegrees, _navigation->viewportHeight());
        state.heading = camera.heading;
        state.tilt = camera.tilt;
        return state;
    }

    void GlobeView::setOrientation(double headingDegrees, double tiltDegrees)
    {
        if (!_navigation) return;
        GlobeCamera camera = _navigation->globeCamera();
        camera.heading = headingDegrees;
        camera.tilt = tiltDegrees;
        _navigation->setGlobeCamera(camera);
    }

    std::optional<GeoPoint> GlobeView::groundAtPixel(double px, double py) const
    {
        if (!_navigation) return std::nullopt;
        GeoPoint hit;
        if (!globePointAtPixel(_navigation->globeCamera(), px, py, _navigation->viewportWidth(), _navigation->viewportHeight(), _navigation->settings(), *_projection, hit)) return std::nullopt;
        return hit;
    }

    std::optional<GeoPoint> GlobeView::groundAtPointer() const
    {
        if (!_navigation) return std::nullopt;
        const GlobeNavigationHandler::PointerPosition pointer = _navigation->lastPointer();
        if (!pointer.valid) return std::nullopt;
        return groundAtPixel(pointer.x, pointer.y);
    }

    void GlobeView::setZoomLevelRange(double minZoomLevel, double maxZoomLevel)
    {
        if (_navigation) _navigation->setZoomLevelRange(minZoomLevel, maxZoomLevel);
    }

    double GlobeView::minZoomLevel() const
    {
        return _navigation ? _navigation->minZoomLevel() : 0.0;
    }

    double GlobeView::maxZoomLevel() const
    {
        return _navigation ? _navigation->maxZoomLevel() : 28.0;
    }

    std::vector<std::string> GlobeView::controls() const
    {
        return {"Drag = spin the globe, wheel = zoom at cursor",
                "Middle drag = tilt / turn, right drag = zoom",
                "PgUp/PgDn = tilt, N = north up, Home = start view"};
    }
} // namespace ivevsg
