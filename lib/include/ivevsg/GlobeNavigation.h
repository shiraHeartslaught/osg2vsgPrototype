#pragma once

#include <ivevsg/MapNavigation.h>
#include <ivevsg/Projection.h>

#include <vsg/app/Camera.h>
#include <vsg/core/Inherit.h>
#include <vsg/core/Visitor.h>
#include <vsg/ui/KeyEvent.h>
#include <vsg/ui/PointerEvent.h>
#include <vsg/ui/ScrollWheelEvent.h>

#include <memory>

namespace ivevsg
{
    /// A camera orbiting the globe, described the way a desk globe is handled: which point of the
    /// surface sits under the middle of the screen, how far away the eye is, and how the view is
    /// turned about that point. Spinning changes longitude and latitude only, so north stays up.
    struct GlobeCamera
    {
        double longitude = 0.0;  // degrees, of the surface point under the screen centre
        double latitude = 0.0;
        double altitude = 2.0e7; // metres from that point to the eye
        double heading = 0.0;    // degrees clockwise from north that the top of the screen points to
        double tilt = 0.0;       // degrees the view is tipped from straight down (0) towards the horizon
    };

    struct GlobeNavigationSettings
    {
        double fieldOfViewDegrees = 30.0;   // taken from the camera's Perspective when a handler is created
        double minAltitude = 2.0;           // metres; see setZoomLevelRange()
        double maxAltitude = 6.0e7;         // metres: the globe is about 40% of the view height from here
        double maxTilt = 80.0;              // degrees
        double maxLatitude = 89.99;         // degrees: the view never passes over a pole
        double scrollZoomFactor = 0.8;      // altitude multiplier for one wheel notch towards the globe
        double keyZoomFactor = 0.5;         // altitude multiplier for + / -
        double keySpinFraction = 0.1;       // of the view height per arrow key press
        double keyTiltStep = 10.0;          // degrees per Page Up / Page Down
        double degreesPerPixelOrbit = 0.25; // heading / tilt change per pixel of middle-button drag
        double zoomDragPixelsPerOctave = 200.0; // right-button drag distance that doubles / halves the altitude
        double semiMajorAxis = 6378137.0;         // WGS84, for the ray / ellipsoid intersection
        double semiMinorAxis = 6356752.314245179;
        double worldWidthMetres = 40075016.685578488; // equator length, for Web Mercator zoom level numbers
    };

    // ---- pure camera arithmetic (unit tested) -------------------------------------------------------

    /// Eye, centre (on the ellipsoid) and up of a GlobeCamera, with near / far planes that keep the
    /// whole globe inside the frustum.
    CameraPose globeCameraPose(const GlobeCamera& camera, const Projection& globe);

    /// The GlobeCamera that reproduces a LookAt whose centre is on (or near) the ellipsoid.
    GlobeCamera globeCameraFromLookAt(const vsg::dvec3& eye, const vsg::dvec3& centre, const vsg::dvec3& up, const Projection& globe);

    /// Where the ray through a viewport pixel meets the ellipsoid; false when it misses (space).
    bool globePointAtPixel(const GlobeCamera& camera, double px, double py, double viewportWidth, double viewportHeight,
                           const GlobeNavigationSettings& settings, const Projection& globe, GeoPoint& hit);

    /// Drag the globe so the ground that was under (fromX, fromY) ends up under (toX, toY): the
    /// longitude difference spins the globe about its polar axis, the latitude difference rolls it
    /// about its centre, so north stays up. With the cursor off the globe the drag turns it by the
    /// angle the pixels subtend at the point under the screen centre.
    GlobeCamera spinGlobeByPixels(const GlobeCamera& camera, double fromX, double fromY, double toX, double toY,
                                  double viewportWidth, double viewportHeight, const GlobeNavigationSettings& settings, const Projection& globe);

    /// Scale the altitude by factor (< 1 zooms in) keeping the ground under the given pixel in place.
    GlobeCamera zoomGlobeAboutPixel(const GlobeCamera& camera, double factor, double px, double py,
                                    double viewportWidth, double viewportHeight, const GlobeNavigationSettings& settings, const Projection& globe);

    /// Altitude, tilt and latitude limits; longitude and heading folded into [-180, 180].
    GlobeCamera clampGlobeCamera(const GlobeCamera& camera, const GlobeNavigationSettings& settings);

    // ---- VSG event handler -------------------------------------------------------------------------

    /// Desk-globe navigation for a geocentric projection: left-drag spins the globe about its axis
    /// (the ground follows the cursor, north stays up), the wheel zooms about the cursor, middle-drag
    /// tilts and turns the view about the point under the screen centre, right-drag zooms, arrow keys
    /// spin, + / - zoom, Page Up / Down tilt, N restores north-up / straight-down, Home or Space returns
    /// to the home view. Events already marked handled (e.g. by an ImGui handler) are left alone.
    class GlobeNavigationHandler : public vsg::Inherit<vsg::Visitor, GlobeNavigationHandler>
    {
    public:
        GlobeNavigationHandler(vsg::ref_ptr<vsg::Camera> camera, std::shared_ptr<const Projection> globe, GlobeNavigationSettings settings = {});

        void apply(vsg::KeyPressEvent& keyPress) override;
        void apply(vsg::ButtonPressEvent& buttonPress) override;
        void apply(vsg::ButtonReleaseEvent& buttonRelease) override;
        void apply(vsg::MoveEvent& moveEvent) override;
        void apply(vsg::ScrollWheelEvent& scrollWheel) override;

        const GlobeCamera& globeCamera() const { return _current; }
        void setGlobeCamera(const GlobeCamera& camera);
        void setHome(const GlobeCamera& camera) { _home = camera; }
        void home() { setGlobeCamera(_home); }
        /// Straight down with north at the top of the screen, keeping position and altitude.
        void northUp();

        /// Web Mercator zoom level equivalent of the ground resolution under the screen centre.
        double zoomLevel() const;
        void setZoomLevel(double zoomLevel);
        /// Derives the altitude limits from zoom levels for the current viewport; the far limit never
        /// exceeds the maxAltitude the handler was created with, so the globe stays a usable size.
        void setZoomLevelRange(double minZoomLevel, double maxZoomLevel);
        double minZoomLevel() const;
        double maxZoomLevel() const;

        const GlobeNavigationSettings& settings() const { return _settings; }
        const Projection& globe() const { return *_globe; }
        double viewportWidth() const;
        double viewportHeight() const;

        using PointerPosition = MapNavigationHandler::PointerPosition;
        PointerPosition lastPointer() const { return {_lastX, _lastY, _havePointer}; }

    private:
        enum class Drag
        {
            None,
            Spin,
            Orbit,
            Zoom
        };

        void applyToCamera();

        vsg::ref_ptr<vsg::Camera> _camera;
        vsg::ref_ptr<vsg::LookAt> _lookAt;
        vsg::ref_ptr<vsg::Perspective> _perspective;
        std::shared_ptr<const Projection> _globe;
        GlobeNavigationSettings _settings;
        double _farthestAltitude = 6.0e7;
        GlobeCamera _current;
        GlobeCamera _home;
        Drag _drag = Drag::None;
        bool _havePointer = false;
        double _lastX = 0.0;
        double _lastY = 0.0;
    };
} // namespace ivevsg
