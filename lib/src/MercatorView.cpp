#include <ivevsg/MapScene.h>
#include <ivevsg/MercatorView.h>

#include <vsg/all.h>

#include <algorithm>
#include <cmath>

namespace ivevsg
{
    MercatorView::MercatorView(MapNavigationSettings navigation) :
        _projection(std::make_shared<MercatorProjection>()),
        _settings(std::move(navigation))
    {
        if (!_settings.viewLimits.valid()) _settings.viewLimits = _projection->viewLimits(); // stop at the poles
        if (_settings.wrapWidth <= 0.0) _settings.wrapWidth = _projection->wrapWidth();       // wrap sideways
    }

    vsg::ref_ptr<vsg::Node> MercatorView::assembleScene(vsg::ref_ptr<vsg::Node> world, double viewportAspect) const
    {
        // the world drawn again on each side so the screen is always covered while the camera's x wraps;
        // one copy each side covers windows up to twice as wide as they are tall
        const int copiesEachSide = std::max(1, static_cast<int>(std::ceil(std::max(viewportAspect, 1.0) / 2.0)));
        return createWrappedScene(world, _projection->wrapWidth(), copiesEachSide);
    }

    void MercatorView::attach(vsg::ref_ptr<vsg::Camera> camera, const vsg::dbox& bounds, double /*elevationDegrees*/)
    {
        detach();
        _camera = camera;
        if (!_camera) return;
        const CameraPose pose = _projection->framingPose(bounds, 90.0); // a map is always seen straight down
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
        _navigation = MapNavigationHandler::create(_camera, _settings); // reads the pose back as its start and home
    }

    void MercatorView::detach()
    {
        _navigation = {};
        _camera = {};
    }

    vsg::ref_ptr<vsg::Visitor> MercatorView::eventHandler() const
    {
        return _navigation;
    }

    void MercatorView::frame(const vsg::dbox& bounds)
    {
        if (!_navigation || !bounds.valid()) return;
        const CameraPose pose = _projection->framingPose(bounds, 90.0);
        _navigation->setMapCamera(MapCamera{{pose.centre.x, pose.centre.y}, pose.eye.z - _navigation->settings().groundHeight});
        _navigation->setHome(_navigation->mapCamera());
    }

    void MercatorView::home()
    {
        if (_navigation) _navigation->home();
    }

    void MercatorView::lookAt(const GeoPoint& target, double altitudeMetres)
    {
        if (!_navigation) return;
        const vsg::dvec3 centre = _projection->geoToWorld(target);
        _navigation->setMapCamera(MapCamera{{centre.x, centre.y}, altitudeMetres});
    }

    ViewState MercatorView::state() const
    {
        ViewState state;
        if (!_navigation) return state;
        const MapCamera camera = _navigation->mapCamera();
        state.centre = _projection->worldToGeo(vsg::dvec3(camera.centre.x, camera.centre.y, 0.0));
        state.centre.altitude = 0.0;
        state.altitude = camera.altitude;
        state.zoomLevel = _navigation->zoomLevel();
        state.metersPerPixel = metersPerPixel(camera.altitude, _navigation->settings().fieldOfViewDegrees, _navigation->viewportHeight());
        return state;
    }

    std::optional<GeoPoint> MercatorView::groundAtPixel(double px, double py) const
    {
        if (!_navigation || _navigation->viewportHeight() <= 0.0) return std::nullopt;
        const vsg::dvec2 ground = groundPointAtPixel(_navigation->mapCamera(), px, py, _navigation->viewportWidth(), _navigation->viewportHeight(), _navigation->settings().fieldOfViewDegrees);
        GeoPoint geo = _projection->worldToGeo(vsg::dvec3(ground.x, ground.y, 0.0));
        geo.longitude = std::remainder(geo.longitude, 360.0); // the map repeats sideways
        geo.altitude = 0.0;
        return geo;
    }

    std::optional<GeoPoint> MercatorView::groundAtPointer() const
    {
        if (!_navigation) return std::nullopt;
        const MapNavigationHandler::PointerPosition pointer = _navigation->lastPointer();
        if (!pointer.valid) return std::nullopt;
        return groundAtPixel(pointer.x, pointer.y);
    }

    void MercatorView::setZoomLevelRange(double minZoomLevel, double maxZoomLevel)
    {
        if (_navigation) _navigation->setZoomLevelRange(minZoomLevel, maxZoomLevel);
    }

    double MercatorView::minZoomLevel() const
    {
        return _navigation ? _navigation->minZoomLevel() : 0.0;
    }

    double MercatorView::maxZoomLevel() const
    {
        return _navigation ? _navigation->maxZoomLevel() : 28.0;
    }

    std::vector<std::string> MercatorView::controls() const
    {
        return {"Drag = pan, wheel = zoom at cursor", "Arrows = pan, +/- = zoom, Home/Space = start view"};
    }
} // namespace ivevsg
