#include <ivevsg/MapNavigation.h>

#include <vsg/all.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ivevsg
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        double halfFovTangent(double fieldOfViewDegrees) { return std::tan(fieldOfViewDegrees * kPi / 360.0); }
    } // namespace

    double metersPerPixel(double altitude, double fieldOfViewDegrees, double viewportHeightPixels)
    {
        if (viewportHeightPixels <= 0.0) return 1.0;
        return 2.0 * altitude * halfFovTangent(fieldOfViewDegrees) / viewportHeightPixels;
    }

    double zoomLevelForAltitude(double altitude, double fieldOfViewDegrees, double viewportHeightPixels, double worldWidthMetres)
    {
        const double worldWidthPixels = worldWidthMetres / metersPerPixel(altitude, fieldOfViewDegrees, viewportHeightPixels);
        return std::log2(worldWidthPixels / 256.0);
    }

    double altitudeForZoomLevel(double zoomLevel, double fieldOfViewDegrees, double viewportHeightPixels, double worldWidthMetres)
    {
        const double worldWidthPixels = 256.0 * std::pow(2.0, zoomLevel);
        const double mpp = worldWidthMetres / worldWidthPixels;
        return mpp * viewportHeightPixels / (2.0 * halfFovTangent(fieldOfViewDegrees));
    }

    vsg::dvec2 groundPointAtPixel(const MapCamera& camera, double px, double py, double viewportWidth, double viewportHeight, double fieldOfViewDegrees)
    {
        const double mpp = metersPerPixel(camera.altitude, fieldOfViewDegrees, viewportHeight);
        return {camera.centre.x + (px - viewportWidth * 0.5) * mpp,
                camera.centre.y - (py - viewportHeight * 0.5) * mpp};
    }

    MapCamera panByPixels(const MapCamera& camera, double dxPixels, double dyPixels, double viewportHeight, double fieldOfViewDegrees)
    {
        const double mpp = metersPerPixel(camera.altitude, fieldOfViewDegrees, viewportHeight);
        MapCamera moved = camera;
        moved.centre.x -= dxPixels * mpp;
        moved.centre.y += dyPixels * mpp;
        return moved;
    }

    MapCamera zoomAboutPixel(const MapCamera& camera, double factor, double px, double py, double viewportWidth, double viewportHeight, double fieldOfViewDegrees)
    {
        const vsg::dvec2 anchor = groundPointAtPixel(camera, px, py, viewportWidth, viewportHeight, fieldOfViewDegrees);
        MapCamera zoomed = camera;
        zoomed.altitude = camera.altitude * factor;
        zoomed.centre = anchor + (camera.centre - anchor) * factor;
        return zoomed;
    }

    vsg::dvec2 viewHalfExtents(const MapCamera& camera, double fieldOfViewDegrees, double viewportWidth, double viewportHeight)
    {
        const double halfHeight = camera.altitude * halfFovTangent(fieldOfViewDegrees);
        const double aspect = (viewportHeight > 0.0) ? viewportWidth / viewportHeight : 1.0;
        return {halfHeight * aspect, halfHeight};
    }

    namespace
    {
        // keep value within [low, high]; if the range is inverted (the view is larger than the limits), centre it
        double clampRange(double value, double low, double high)
        {
            if (low > high) return (low + high) * 0.5;
            return std::clamp(value, low, high);
        }
    } // namespace

    MapCamera clampCamera(const MapCamera& camera, const MapNavigationSettings& settings, double viewportWidth, double viewportHeight)
    {
        MapCamera clamped = camera;
        clamped.altitude = std::clamp(clamped.altitude, settings.minAltitude, settings.maxAltitude);

        const double tanHalfFov = halfFovTangent(settings.fieldOfViewDegrees);
        const bool wraps = settings.wrapWidth > 0.0;
        const bool haveViewport = viewportWidth > 0.0 && viewportHeight > 0.0 && tanHalfFov > 0.0;

        if (settings.viewLimits.valid() && haveViewport)
        {
            const double aspect = viewportWidth / viewportHeight;
            const double limitHalfHeight = (settings.viewLimits.max.y - settings.viewLimits.min.y) * 0.5;
            const double limitHalfWidth = (settings.viewLimits.max.x - settings.viewLimits.min.x) * 0.5;

            // Never look past the map's top/bottom edge: cap the altitude so the view is at most as
            // tall as the map (and, unless the world repeats sideways, at most as wide as the map).
            double maxAltitude = limitHalfHeight / tanHalfFov;
            if (!wraps) maxAltitude = std::min(maxAltitude, limitHalfWidth / (tanHalfFov * aspect));
            clamped.altitude = std::min(clamped.altitude, std::max(maxAltitude, settings.minAltitude));

            const vsg::dvec2 half = viewHalfExtents(clamped, settings.fieldOfViewDegrees, viewportWidth, viewportHeight);
            clamped.centre.y = clampRange(clamped.centre.y, settings.viewLimits.min.y + half.y, settings.viewLimits.max.y - half.y);
            if (!wraps) clamped.centre.x = clampRange(clamped.centre.x, settings.viewLimits.min.x + half.x, settings.viewLimits.max.x - half.x);
        }

        if (wraps) clamped.centre.x = std::remainder(clamped.centre.x, settings.wrapWidth); // -> [-w/2, w/2]
        return clamped;
    }

    // ---- handler -----------------------------------------------------------------------------------

    MapNavigationHandler::MapNavigationHandler(vsg::ref_ptr<vsg::Camera> camera, MapNavigationSettings settings) :
        _camera(camera),
        _settings(std::move(settings))
    {
        if (_camera)
        {
            _lookAt = _camera->viewMatrix.cast<vsg::LookAt>();
            _perspective = _camera->projectionMatrix.cast<vsg::Perspective>();
        }
        if (_perspective) _settings.fieldOfViewDegrees = _perspective->fieldOfViewY;
        if (_lookAt)
        {
            _current.centre = {_lookAt->center.x, _lookAt->center.y};
            _current.altitude = _lookAt->eye.z - _settings.groundHeight;
        }
        _current = clampCamera(_current, _settings, viewportWidth(), viewportHeight());
        _home = _current;
        applyToCamera();
    }

    double MapNavigationHandler::viewportWidth() const
    {
        return _camera ? static_cast<double>(_camera->getViewport().width) : 0.0;
    }

    double MapNavigationHandler::viewportHeight() const
    {
        return _camera ? static_cast<double>(_camera->getViewport().height) : 0.0;
    }

    void MapNavigationHandler::setMapCamera(const MapCamera& camera)
    {
        _current = clampCamera(camera, _settings, viewportWidth(), viewportHeight());
        applyToCamera();
    }

    double MapNavigationHandler::zoomLevel() const
    {
        return zoomLevelForAltitude(_current.altitude, _settings.fieldOfViewDegrees, viewportHeight(), _settings.worldWidthMetres);
    }

    void MapNavigationHandler::setZoomLevel(double zoomLevel)
    {
        MapCamera camera = _current;
        camera.altitude = altitudeForZoomLevel(zoomLevel, _settings.fieldOfViewDegrees, viewportHeight(), _settings.worldWidthMetres);
        setMapCamera(camera);
    }

    void MapNavigationHandler::setZoomLevelRange(double minZoomLevel, double maxZoomLevel)
    {
        if (maxZoomLevel < minZoomLevel) std::swap(minZoomLevel, maxZoomLevel);
        const double height = viewportHeight() > 0.0 ? viewportHeight() : 1024.0;
        _settings.maxAltitude = altitudeForZoomLevel(minZoomLevel, _settings.fieldOfViewDegrees, height, _settings.worldWidthMetres);
        _settings.minAltitude = altitudeForZoomLevel(maxZoomLevel, _settings.fieldOfViewDegrees, height, _settings.worldWidthMetres);
        setMapCamera(_current); // re-clamp
    }

    double MapNavigationHandler::minZoomLevel() const
    {
        return zoomLevelForAltitude(_settings.maxAltitude, _settings.fieldOfViewDegrees, viewportHeight(), _settings.worldWidthMetres);
    }

    double MapNavigationHandler::maxZoomLevel() const
    {
        return zoomLevelForAltitude(_settings.minAltitude, _settings.fieldOfViewDegrees, viewportHeight(), _settings.worldWidthMetres);
    }

    void MapNavigationHandler::applyToCamera()
    {
        if (!_lookAt || !_perspective) return;
        const double ground = _settings.groundHeight;
        _lookAt->eye = vsg::dvec3(_current.centre.x, _current.centre.y, ground + _current.altitude);
        _lookAt->center = vsg::dvec3(_current.centre.x, _current.centre.y, ground);
        _lookAt->up = vsg::dvec3(0.0, 1.0, 0.0); // north up
        _perspective->nearDistance = std::max(1.0, _current.altitude * 0.001);
        _perspective->farDistance = _current.altitude * 10.0 + _settings.farPlanePadding;
    }

    void MapNavigationHandler::apply(vsg::ButtonPressEvent& buttonPress)
    {
        _lastX = buttonPress.x;
        _lastY = buttonPress.y;
        _havePointer = true;
        if (buttonPress.button == 1)
        {
            _dragging = true;
            buttonPress.handled = true;
        }
    }

    void MapNavigationHandler::apply(vsg::ButtonReleaseEvent& buttonRelease)
    {
        _lastX = buttonRelease.x;
        _lastY = buttonRelease.y;
        if (buttonRelease.button == 1)
        {
            _dragging = false;
            buttonRelease.handled = true;
        }
    }

    void MapNavigationHandler::apply(vsg::MoveEvent& moveEvent)
    {
        const double dx = moveEvent.x - _lastX;
        const double dy = moveEvent.y - _lastY;
        _lastX = moveEvent.x;
        _lastY = moveEvent.y;
        _havePointer = true;

        if (_dragging && (moveEvent.mask & vsg::BUTTON_MASK_1))
        {
            _current = clampCamera(panByPixels(_current, dx, dy, viewportHeight(), _settings.fieldOfViewDegrees), _settings, viewportWidth(), viewportHeight());
            applyToCamera();
            moveEvent.handled = true;
        }
    }

    void MapNavigationHandler::apply(vsg::ScrollWheelEvent& scrollWheel)
    {
        if (scrollWheel.delta.y == 0.0f) return;
        const double factor = scrollWheel.delta.y > 0.0f ? _settings.scrollZoomFactor : 1.0 / _settings.scrollZoomFactor;
        const double px = _havePointer ? _lastX : viewportWidth() * 0.5;
        const double py = _havePointer ? _lastY : viewportHeight() * 0.5;
        _current = clampCamera(zoomAboutPixel(_current, factor, px, py, viewportWidth(), viewportHeight(), _settings.fieldOfViewDegrees), _settings, viewportWidth(), viewportHeight());
        applyToCamera();
        scrollWheel.handled = true;
    }

    void MapNavigationHandler::apply(vsg::KeyPressEvent& keyPress)
    {
        const double panPixels = _settings.keyPanFraction * viewportHeight();
        const double cx = viewportWidth() * 0.5;
        const double cy = viewportHeight() * 0.5;
        const double fov = _settings.fieldOfViewDegrees;

        switch (keyPress.keyBase)
        {
        case vsg::KEY_Left: _current = panByPixels(_current, panPixels, 0.0, viewportHeight(), fov); break;
        case vsg::KEY_Right: _current = panByPixels(_current, -panPixels, 0.0, viewportHeight(), fov); break;
        case vsg::KEY_Up: _current = panByPixels(_current, 0.0, panPixels, viewportHeight(), fov); break;
        case vsg::KEY_Down: _current = panByPixels(_current, 0.0, -panPixels, viewportHeight(), fov); break;
        case vsg::KEY_Plus:
        case vsg::KEY_Equals:
        case vsg::KEY_KP_Add: _current = zoomAboutPixel(_current, _settings.keyZoomFactor, cx, cy, viewportWidth(), viewportHeight(), fov); break;
        case vsg::KEY_Minus:
        case vsg::KEY_KP_Subtract: _current = zoomAboutPixel(_current, 1.0 / _settings.keyZoomFactor, cx, cy, viewportWidth(), viewportHeight(), fov); break;
        case vsg::KEY_Home:
        case vsg::KEY_Space: _current = _home; break;
        default: return;
        }
        _current = clampCamera(_current, _settings, viewportWidth(), viewportHeight());
        applyToCamera();
        keyPress.handled = true;
    }
} // namespace ivevsg
