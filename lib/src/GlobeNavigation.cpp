#include <ivevsg/GlobeNavigation.h>

#include <vsg/all.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ivevsg
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        double radians(double degrees) { return degrees * kPi / 180.0; }
        double degrees(double radians) { return radians * 180.0 / kPi; }
        double wrapDegrees(double value) { return std::remainder(value, 360.0); } // -> [-180, 180]

        /// Local east / north / up at the surface point under the screen centre, and the camera basis
        /// the heading and tilt build on it.
        struct GlobeFrame
        {
            vsg::dvec3 centre, east, north, up;
            vsg::dvec3 eye, forward, right, cameraUp;
        };

        void tangentFrame(const Projection& globe, const vsg::dvec3& surface, vsg::dvec3& east, vsg::dvec3& north, vsg::dvec3& up)
        {
            up = globe.upVector(surface);
            north = vsg::dvec3(0.0, 0.0, 1.0) - up * up.z; // the earth's axis projected onto the tangent plane
            north = (vsg::length(north) > 1e-9) ? vsg::normalize(north) : vsg::dvec3(-1.0, 0.0, 0.0); // at a pole any tangent will do
            east = vsg::normalize(vsg::cross(north, up));
        }

        GlobeFrame frameOf(const GlobeCamera& camera, const Projection& globe)
        {
            GlobeFrame f;
            f.centre = globe.geoToWorld(GeoPoint{camera.longitude, camera.latitude, 0.0});
            tangentFrame(globe, f.centre, f.east, f.north, f.up);
            const double h = radians(camera.heading), t = radians(camera.tilt);
            const vsg::dvec3 ahead = f.north * std::cos(h) + f.east * std::sin(h); // ground direction the top of the screen points along
            const vsg::dvec3 offset = f.up * std::cos(t) - ahead * std::sin(t);    // from the centre towards the eye
            f.eye = f.centre + offset * camera.altitude;
            f.forward = -offset;
            f.cameraUp = ahead * std::cos(t) + f.up * std::sin(t); // perpendicular to forward for every tilt
            f.right = vsg::normalize(vsg::cross(f.forward, f.cameraUp));
            return f;
        }

        vsg::dvec3 pixelRay(const GlobeFrame& f, double px, double py, double viewportWidth, double viewportHeight, double fieldOfViewDegrees)
        {
            const double tanHalf = std::tan(radians(fieldOfViewDegrees) * 0.5); // vertical field of view
            const double sx = (px - viewportWidth * 0.5) / (viewportHeight * 0.5) * tanHalf;
            const double sy = -(py - viewportHeight * 0.5) / (viewportHeight * 0.5) * tanHalf; // pixel y runs down
            return vsg::normalize(f.forward + f.right * sx + f.cameraUp * sy);
        }

        bool intersectEllipsoid(const vsg::dvec3& origin, const vsg::dvec3& direction, double a, double b, vsg::dvec3& hit)
        {
            // scale the ellipsoid to the unit sphere and solve |o + t d|^2 = 1 for the nearer root
            const vsg::dvec3 o(origin.x / a, origin.y / a, origin.z / b);
            const vsg::dvec3 d(direction.x / a, direction.y / a, direction.z / b);
            const double A = vsg::dot(d, d), B = 2.0 * vsg::dot(o, d), C = vsg::dot(o, o) - 1.0;
            const double discriminant = B * B - 4.0 * A * C;
            if (discriminant < 0.0 || A <= 0.0) return false;
            const double t = (-B - std::sqrt(discriminant)) / (2.0 * A);
            if (t < 0.0) return false; // the globe is behind the eye
            hit = origin + direction * t;
            return true;
        }
    } // namespace

    CameraPose globeCameraPose(const GlobeCamera& camera, const Projection& globe)
    {
        const GlobeFrame f = frameOf(camera, globe);
        CameraPose pose;
        pose.centre = f.centre;
        pose.eye = f.eye;
        pose.up = f.cameraUp;
        const double eyeHeight = std::max(1.0, globe.worldToGeo(f.eye).altitude);
        const double equatorialRadius = vsg::length(globe.geoToWorld(GeoPoint{0.0, 0.0, 0.0}));
        pose.nearPlane = std::max(1.0, eyeHeight * 0.001);
        pose.farPlane = vsg::length(f.eye) + equatorialRadius * 1.1; // past the far side of the globe
        return pose;
    }

    GlobeCamera globeCameraFromLookAt(const vsg::dvec3& eye, const vsg::dvec3& centre, const vsg::dvec3& up, const Projection& globe)
    {
        GlobeCamera camera;
        const GeoPoint geo = globe.worldToGeo(centre);
        camera.longitude = geo.longitude;
        camera.latitude = geo.latitude;
        const vsg::dvec3 surface = globe.geoToWorld(GeoPoint{geo.longitude, geo.latitude, 0.0});
        const vsg::dvec3 offset = eye - surface;
        camera.altitude = vsg::length(offset);
        if (camera.altitude < 1e-9) return camera;

        vsg::dvec3 east, north, surfaceUp;
        tangentFrame(globe, surface, east, north, surfaceUp);
        const vsg::dvec3 o = offset / camera.altitude;
        const double cosTilt = std::clamp(vsg::dot(o, surfaceUp), -1.0, 1.0);
        camera.tilt = degrees(std::acos(cosTilt));
        // the ground direction the top of the screen points along: opposite the eye's sideways offset,
        // or the given up vector when looking straight down
        vsg::dvec3 ahead = (o - surfaceUp * cosTilt) * -1.0;
        if (vsg::length(ahead) < 1e-6) ahead = up - surfaceUp * vsg::dot(up, surfaceUp);
        if (vsg::length(ahead) < 1e-9) return camera; // heading undefined: keep north
        ahead = vsg::normalize(ahead);
        camera.heading = degrees(std::atan2(vsg::dot(ahead, east), vsg::dot(ahead, north)));
        return camera;
    }

    bool globePointAtPixel(const GlobeCamera& camera, double px, double py, double viewportWidth, double viewportHeight,
                           const GlobeNavigationSettings& settings, const Projection& globe, GeoPoint& hit)
    {
        if (viewportWidth <= 0.0 || viewportHeight <= 0.0) return false;
        const GlobeFrame f = frameOf(camera, globe);
        const vsg::dvec3 direction = pixelRay(f, px, py, viewportWidth, viewportHeight, settings.fieldOfViewDegrees);
        vsg::dvec3 world;
        if (!intersectEllipsoid(f.eye, direction, settings.semiMajorAxis, settings.semiMinorAxis, world)) return false;
        hit = globe.worldToGeo(world);
        hit.altitude = 0.0;
        return true;
    }

    GlobeCamera spinGlobeByPixels(const GlobeCamera& camera, double fromX, double fromY, double toX, double toY,
                                  double viewportWidth, double viewportHeight, const GlobeNavigationSettings& settings, const Projection& globe)
    {
        // The drag is read as ground motion in the tangent plane of the point it started on: its east
        // component spins the globe about the polar axis (longitude), its north component rolls it about
        // the centre (latitude). Working in the tangent plane rather than with raw hit-point latitudes
        // keeps a sideways drag a pure spin: the ground along a screen row lies on a great circle that
        // dips towards the equator, and following that dip would roll the globe as well.
        double east = 0.0, north = 0.0, latitude = camera.latitude;
        GeoPoint from, to;
        if (globePointAtPixel(camera, fromX, fromY, viewportWidth, viewportHeight, settings, globe, from) &&
            globePointAtPixel(camera, toX, toY, viewportWidth, viewportHeight, settings, globe, to))
        {
            const vsg::dvec3 fromWorld = globe.geoToWorld(from);
            const vsg::dvec3 motion = globe.geoToWorld(to) - fromWorld;
            vsg::dvec3 e, n, u;
            tangentFrame(globe, fromWorld, e, n, u);
            east = vsg::dot(motion, e);
            north = vsg::dot(motion, n);
            latitude = from.latitude;
        }
        else
        {
            // off the globe: the drag covers ground at the scale of the point under the screen centre;
            // screen-right is the ground direction at azimuth heading + 90, screen-down the opposite of heading
            const double mpp = metersPerPixel(camera.altitude, settings.fieldOfViewDegrees, viewportHeight);
            const double h = radians(camera.heading);
            const double dx = (toX - fromX) * mpp, dy = (toY - fromY) * mpp;
            east = dx * std::cos(h) - dy * std::sin(h);
            north = -dx * std::sin(h) - dy * std::cos(h);
        }

        // metres to degrees along the parallel and the meridian at this latitude (WGS84 radii of curvature)
        const double a = settings.semiMajorAxis, b = settings.semiMinorAxis;
        const double e2 = 1.0 - (b * b) / (a * a);
        const double sinLat = std::sin(radians(latitude)), cosLat = std::max(std::cos(radians(latitude)), 0.01);
        const double w = std::sqrt(1.0 - e2 * sinLat * sinLat);
        const double parallelRadius = a / w * cosLat;              // N cos(lat)
        const double meridianRadius = a * (1.0 - e2) / (w * w * w); // M

        // the ground follows the cursor, so the camera's centre moves the other way
        GlobeCamera moved = camera;
        moved.longitude -= degrees(east / parallelRadius);
        moved.latitude -= degrees(north / meridianRadius);
        return moved;
    }

    GlobeCamera zoomGlobeAboutPixel(const GlobeCamera& camera, double factor, double px, double py,
                                    double viewportWidth, double viewportHeight, const GlobeNavigationSettings& settings, const Projection& globe)
    {
        GlobeCamera zoomed = camera;
        zoomed.altitude = camera.altitude * factor;
        GeoPoint anchor;
        if (globePointAtPixel(camera, px, py, viewportWidth, viewportHeight, settings, globe, anchor))
        {
            // the centre slides towards the anchor as the view shrinks, so the anchor stays under the
            // cursor (exact for a straight-down view, close enough per wheel notch when tilted)
            zoomed.longitude = anchor.longitude + wrapDegrees(camera.longitude - anchor.longitude) * factor;
            zoomed.latitude = anchor.latitude + (camera.latitude - anchor.latitude) * factor;
        }
        return zoomed;
    }

    GlobeCamera clampGlobeCamera(const GlobeCamera& camera, const GlobeNavigationSettings& settings)
    {
        GlobeCamera clamped = camera;
        clamped.altitude = std::clamp(clamped.altitude, settings.minAltitude, settings.maxAltitude);
        clamped.tilt = std::clamp(clamped.tilt, 0.0, settings.maxTilt);
        clamped.latitude = std::clamp(clamped.latitude, -settings.maxLatitude, settings.maxLatitude);
        clamped.longitude = wrapDegrees(clamped.longitude);
        clamped.heading = wrapDegrees(clamped.heading);
        return clamped;
    }

    // ---- handler -----------------------------------------------------------------------------------

    GlobeNavigationHandler::GlobeNavigationHandler(vsg::ref_ptr<vsg::Camera> camera, std::shared_ptr<const Projection> globe, GlobeNavigationSettings settings) :
        _camera(camera),
        _globe(std::move(globe)),
        _settings(std::move(settings))
    {
        if (_camera)
        {
            _lookAt = _camera->viewMatrix.cast<vsg::LookAt>();
            _perspective = _camera->projectionMatrix.cast<vsg::Perspective>();
        }
        if (_perspective) _settings.fieldOfViewDegrees = _perspective->fieldOfViewY;
        _farthestAltitude = _settings.maxAltitude;
        if (_lookAt && _globe) _current = globeCameraFromLookAt(_lookAt->eye, _lookAt->center, _lookAt->up, *_globe);
        _current = clampGlobeCamera(_current, _settings);
        _home = _current;
        applyToCamera();
    }

    double GlobeNavigationHandler::viewportWidth() const
    {
        return _camera ? static_cast<double>(_camera->getViewport().width) : 0.0;
    }

    double GlobeNavigationHandler::viewportHeight() const
    {
        return _camera ? static_cast<double>(_camera->getViewport().height) : 0.0;
    }

    void GlobeNavigationHandler::setGlobeCamera(const GlobeCamera& camera)
    {
        _current = clampGlobeCamera(camera, _settings);
        applyToCamera();
    }

    void GlobeNavigationHandler::northUp()
    {
        GlobeCamera camera = _current;
        camera.heading = 0.0;
        camera.tilt = 0.0;
        setGlobeCamera(camera);
    }

    double GlobeNavigationHandler::zoomLevel() const
    {
        return zoomLevelForAltitude(_current.altitude, _settings.fieldOfViewDegrees, viewportHeight(), _settings.worldWidthMetres);
    }

    void GlobeNavigationHandler::setZoomLevel(double zoomLevel)
    {
        GlobeCamera camera = _current;
        camera.altitude = altitudeForZoomLevel(zoomLevel, _settings.fieldOfViewDegrees, viewportHeight(), _settings.worldWidthMetres);
        setGlobeCamera(camera);
    }

    void GlobeNavigationHandler::setZoomLevelRange(double minZoomLevel, double maxZoomLevel)
    {
        if (maxZoomLevel < minZoomLevel) std::swap(minZoomLevel, maxZoomLevel);
        const double height = viewportHeight() > 0.0 ? viewportHeight() : 1024.0;
        _settings.maxAltitude = std::min(_farthestAltitude, altitudeForZoomLevel(minZoomLevel, _settings.fieldOfViewDegrees, height, _settings.worldWidthMetres));
        _settings.minAltitude = std::min(_settings.maxAltitude, altitudeForZoomLevel(maxZoomLevel, _settings.fieldOfViewDegrees, height, _settings.worldWidthMetres));
        setGlobeCamera(_current); // re-clamp
    }

    double GlobeNavigationHandler::minZoomLevel() const
    {
        return zoomLevelForAltitude(_settings.maxAltitude, _settings.fieldOfViewDegrees, viewportHeight(), _settings.worldWidthMetres);
    }

    double GlobeNavigationHandler::maxZoomLevel() const
    {
        return zoomLevelForAltitude(_settings.minAltitude, _settings.fieldOfViewDegrees, viewportHeight(), _settings.worldWidthMetres);
    }

    void GlobeNavigationHandler::applyToCamera()
    {
        if (!_lookAt || !_perspective || !_globe) return;
        const CameraPose pose = globeCameraPose(_current, *_globe);
        _lookAt->eye = pose.eye;
        _lookAt->center = pose.centre;
        _lookAt->up = pose.up;
        _perspective->nearDistance = pose.nearPlane;
        _perspective->farDistance = pose.farPlane;
    }

    void GlobeNavigationHandler::apply(vsg::ButtonPressEvent& buttonPress)
    {
        _lastX = buttonPress.x;
        _lastY = buttonPress.y;
        _havePointer = true;
        if (buttonPress.handled) return; // e.g. a click on an ImGui window
        switch (buttonPress.button)
        {
        case 1: _drag = Drag::Spin; break;
        case 2: _drag = Drag::Orbit; break;
        case 3: _drag = Drag::Zoom; break;
        default: return;
        }
        buttonPress.handled = true;
    }

    void GlobeNavigationHandler::apply(vsg::ButtonReleaseEvent& buttonRelease)
    {
        _lastX = buttonRelease.x;
        _lastY = buttonRelease.y;
        if (buttonRelease.button >= 1 && buttonRelease.button <= 3 && _drag != Drag::None)
        {
            _drag = Drag::None;
            buttonRelease.handled = true;
        }
    }

    void GlobeNavigationHandler::apply(vsg::MoveEvent& moveEvent)
    {
        const double fromX = _lastX, fromY = _lastY;
        const double dx = moveEvent.x - _lastX;
        const double dy = moveEvent.y - _lastY;
        _lastX = moveEvent.x;
        _lastY = moveEvent.y;
        const bool hadPointer = _havePointer;
        _havePointer = true;

        if (moveEvent.handled || !hadPointer || _drag == Drag::None) return;
        GlobeCamera camera = _current;
        if (_drag == Drag::Spin && (moveEvent.mask & vsg::BUTTON_MASK_1))
        {
            camera = spinGlobeByPixels(_current, fromX, fromY, moveEvent.x, moveEvent.y, viewportWidth(), viewportHeight(), _settings, *_globe);
        }
        else if (_drag == Drag::Orbit && (moveEvent.mask & vsg::BUTTON_MASK_2))
        {
            camera.heading -= dx * _settings.degreesPerPixelOrbit; // the ground at the top of the screen follows the cursor
            camera.tilt -= dy * _settings.degreesPerPixelOrbit;    // drag up = look towards the horizon
        }
        else if (_drag == Drag::Zoom && (moveEvent.mask & vsg::BUTTON_MASK_3))
        {
            camera.altitude *= std::pow(2.0, dy / _settings.zoomDragPixelsPerOctave); // drag down = zoom out
        }
        else
        {
            return;
        }
        _current = clampGlobeCamera(camera, _settings);
        applyToCamera();
        moveEvent.handled = true;
    }

    void GlobeNavigationHandler::apply(vsg::ScrollWheelEvent& scrollWheel)
    {
        if (scrollWheel.handled || scrollWheel.delta.y == 0.0f) return;
        const double factor = scrollWheel.delta.y > 0.0f ? _settings.scrollZoomFactor : 1.0 / _settings.scrollZoomFactor;
        const double px = _havePointer ? _lastX : viewportWidth() * 0.5;
        const double py = _havePointer ? _lastY : viewportHeight() * 0.5;
        _current = clampGlobeCamera(zoomGlobeAboutPixel(_current, factor, px, py, viewportWidth(), viewportHeight(), _settings, *_globe), _settings);
        applyToCamera();
        scrollWheel.handled = true;
    }

    void GlobeNavigationHandler::apply(vsg::KeyPressEvent& keyPress)
    {
        if (keyPress.handled) return; // e.g. typing into an ImGui field
        const double spinPixels = _settings.keySpinFraction * viewportHeight();
        const double cx = viewportWidth() * 0.5;
        const double cy = viewportHeight() * 0.5;
        const double vw = viewportWidth(), vh = viewportHeight();
        GlobeCamera camera = _current;

        switch (keyPress.keyBase)
        {
        case vsg::KEY_Left: camera = spinGlobeByPixels(_current, cx, cy, cx + spinPixels, cy, vw, vh, _settings, *_globe); break;
        case vsg::KEY_Right: camera = spinGlobeByPixels(_current, cx, cy, cx - spinPixels, cy, vw, vh, _settings, *_globe); break;
        case vsg::KEY_Up: camera = spinGlobeByPixels(_current, cx, cy, cx, cy + spinPixels, vw, vh, _settings, *_globe); break;
        case vsg::KEY_Down: camera = spinGlobeByPixels(_current, cx, cy, cx, cy - spinPixels, vw, vh, _settings, *_globe); break;
        case vsg::KEY_Plus:
        case vsg::KEY_Equals:
        case vsg::KEY_KP_Add: camera = zoomGlobeAboutPixel(_current, _settings.keyZoomFactor, cx, cy, vw, vh, _settings, *_globe); break;
        case vsg::KEY_Minus:
        case vsg::KEY_KP_Subtract: camera = zoomGlobeAboutPixel(_current, 1.0 / _settings.keyZoomFactor, cx, cy, vw, vh, _settings, *_globe); break;
        case vsg::KEY_Page_Up: camera.tilt += _settings.keyTiltStep; break;
        case vsg::KEY_Page_Down: camera.tilt -= _settings.keyTiltStep; break;
        case vsg::KEY_n:
        case vsg::KEY_N:
            camera.heading = 0.0;
            camera.tilt = 0.0;
            break;
        case vsg::KEY_Home:
        case vsg::KEY_Space: camera = _home; break;
        default: return;
        }
        _current = clampGlobeCamera(camera, _settings);
        applyToCamera();
        keyPress.handled = true;
    }
} // namespace ivevsg
