#include <ivevsg/CameraFraming.h>
#include <ivevsg/MapNavigation.h>
#include <ivevsg/TrackballView.h>

#include <vsg/all.h>

#include <algorithm>
#include <cmath>

namespace ivevsg
{
    TrackballView::TrackballView(std::shared_ptr<const Projection> projection, std::string key) :
        _projection(std::move(projection)),
        _key(std::move(key))
    {
    }

    void TrackballView::applyPose(const CameraPose& pose)
    {
        if (!_camera) return;
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
    }

    void TrackballView::attach(vsg::ref_ptr<vsg::Camera> camera, const vsg::dbox& bounds, double elevationDegrees)
    {
        detach();
        _camera = camera;
        _elevationDegrees = elevationDegrees;
        _bounds = bounds;
        if (!_camera) return;
        _home = _projection->framingPose(bounds, elevationDegrees);
        applyPose(_home);
        _trackball = _projection->geocentric() ? vsg::Trackball::create(_camera, vsg::EllipsoidModel::create()) : vsg::Trackball::create(_camera);
    }

    void TrackballView::detach()
    {
        _trackball = {};
        _camera = {};
    }

    vsg::ref_ptr<vsg::Visitor> TrackballView::eventHandler() const
    {
        return _trackball;
    }

    void TrackballView::frame(const vsg::dbox& bounds)
    {
        if (!bounds.valid()) return;
        _bounds = bounds;
        _home = _projection->framingPose(bounds, _elevationDegrees);
        applyPose(_home);
    }

    void TrackballView::home()
    {
        applyPose(_home);
    }

    void TrackballView::lookAt(const GeoPoint& target, double altitudeMetres)
    {
        const vsg::dvec3 centre = _projection->geoToWorld(target);
        const vsg::dvec3 up = _projection->upVector(centre);
        CameraPose pose;
        pose.centre = centre;
        pose.eye = centre + up * altitudeMetres;
        // looking along -up, so the screen "up" must be perpendicular to it: north on a map, the
        // earth's axis (projected) on a globe, east when that degenerates
        vsg::dvec3 screenUp = _projection->geocentric() ? vsg::dvec3(0.0, 0.0, 1.0) : vsg::dvec3(0.0, 1.0, 0.0);
        screenUp = screenUp - up * vsg::dot(screenUp, up);
        pose.up = (vsg::length(screenUp) > 1e-6) ? vsg::normalize(screenUp) : vsg::dvec3(1.0, 0.0, 0.0);
        pose.nearPlane = std::max(1.0, altitudeMetres * 0.0005);
        pose.farPlane = altitudeMetres * 10.0 + (_bounds.valid() ? summarize(_bounds).radius : 1.0e7);
        applyPose(pose);
    }

    ViewState TrackballView::state() const
    {
        ViewState state;
        if (!_camera) return state;
        auto lookAt = _camera->viewMatrix.cast<vsg::LookAt>();
        auto perspective = _camera->projectionMatrix.cast<vsg::Perspective>();
        if (!lookAt) return state;
        state.centre = _projection->worldToGeo(lookAt->center);
        state.centre.altitude = 0.0;
        state.altitude = vsg::length(lookAt->eye - lookAt->center);
        const double fov = perspective ? perspective->fieldOfViewY : 30.0;
        const double height = static_cast<double>(_camera->getViewport().height);
        state.metersPerPixel = metersPerPixel(state.altitude, fov, height);
        state.zoomLevel = zoomLevelForAltitude(state.altitude, fov, height, 40075016.685578488);
        return state;
    }

    std::optional<GeoPoint> TrackballView::groundAtPixel(double, double) const
    {
        return std::nullopt;
    }

    std::vector<std::string> TrackballView::controls() const
    {
        return {"Trackball: drag = orbit, right drag / wheel = zoom, middle = pan"};
    }
} // namespace ivevsg
