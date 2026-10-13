#pragma once

#include <ivevsg/Projection.h>

#include <vsg/app/Camera.h>
#include <vsg/core/Inherit.h>
#include <vsg/core/Visitor.h>
#include <vsg/maths/box.h>
#include <vsg/maths/vec2.h>
#include <vsg/ui/KeyEvent.h>
#include <vsg/ui/PointerEvent.h>
#include <vsg/ui/ScrollWheelEvent.h>

namespace ivevsg
{
    /// A straight-down camera over a flat, z-up map: where it is on the plane and how high above it.
    struct MapCamera
    {
        vsg::dvec2 centre{0.0, 0.0}; // world x/y (Mercator metres) under the middle of the screen
        double altitude = 1.0e7;     // metres above the ground plane
    };

    struct MapNavigationSettings
    {
        double fieldOfViewDegrees = 30.0; // taken from the camera's Perspective when a handler is created
        double groundHeight = 0.0;        // z of the map plane
        double minAltitude = 1.0;         // metres (about zoom 28 on a 1024 px tall window); see setZoomLevelRange()
        double maxAltitude = 1.5e8;       // metres, comfortably shows the whole Mercator world
        double scrollZoomFactor = 0.8;    // altitude multiplier for one wheel notch towards the map
        double keyZoomFactor = 0.5;       // altitude multiplier for + / -
        double keyPanFraction = 0.1;      // of the view height per arrow key press
        double farPlanePadding = 1.0e6;   // metres kept beyond the ground so layers below it stay visible
        /// The visible area is kept inside this box: the camera stops at its edges and the altitude is
        /// reduced if the view would be taller (or, without wrapping, wider) than the box.
        /// An invalid box means unlimited. ViewerApp fills it from Projection::viewLimits() when unset.
        vsg::dbox viewLimits;
        /// Period along x for a world that repeats sideways; the camera's x is folded into
        /// [-wrapWidth/2, wrapWidth/2). 0 = no wrapping. Filled from Projection::wrapWidth() when unset.
        double wrapWidth = 0.0;
        double worldWidthMetres = 40075016.685578488; // 2 * pi * 6378137, for Web Mercator zoom levels
    };

    // ---- pure camera arithmetic (unit tested) -------------------------------------------------------

    /// metres of map per screen pixel for a straight-down perspective camera
    double metersPerPixel(double altitude, double fieldOfViewDegrees, double viewportHeightPixels);

    /// Web Mercator zoom level z, defined by the whole world being 256 * 2^z pixels wide on screen
    double zoomLevelForAltitude(double altitude, double fieldOfViewDegrees, double viewportHeightPixels, double worldWidthMetres);
    double altitudeForZoomLevel(double zoomLevel, double fieldOfViewDegrees, double viewportHeightPixels, double worldWidthMetres);

    /// map coordinates under a viewport pixel (pixel y runs downwards, map y runs north)
    vsg::dvec2 groundPointAtPixel(const MapCamera& camera, double px, double py, double viewportWidth, double viewportHeight, double fieldOfViewDegrees);

    /// drag the map by a pixel delta: the map follows the cursor, so the camera moves the other way
    MapCamera panByPixels(const MapCamera& camera, double dxPixels, double dyPixels, double viewportHeight, double fieldOfViewDegrees);

    /// scale the altitude by factor (< 1 zooms in) keeping the map point under the given pixel fixed
    MapCamera zoomAboutPixel(const MapCamera& camera, double factor, double px, double py, double viewportWidth, double viewportHeight, double fieldOfViewDegrees);

    /// half the width and height of the ground area visible from the camera
    vsg::dvec2 viewHalfExtents(const MapCamera& camera, double fieldOfViewDegrees, double viewportWidth, double viewportHeight);

    /// Applies altitude limits, keeps the visible area inside settings.viewLimits (needs the viewport size
    /// to know the view's footprint; pass 0 to skip that part) and wraps x when settings.wrapWidth > 0.
    MapCamera clampCamera(const MapCamera& camera, const MapNavigationSettings& settings, double viewportWidth = 0.0, double viewportHeight = 0.0);

    // ---- VSG event handler -------------------------------------------------------------------------

    /// Google-Maps style navigation for a flat map: left-drag pans, the scroll wheel zooms about the
    /// cursor, arrow keys pan, + / - zoom about the centre, Home or Space returns to the home view.
    /// The camera always looks straight down with north up; there is no rotation or tilt.
    class MapNavigationHandler : public vsg::Inherit<vsg::Visitor, MapNavigationHandler>
    {
    public:
        explicit MapNavigationHandler(vsg::ref_ptr<vsg::Camera> camera, MapNavigationSettings settings = {});

        void apply(vsg::KeyPressEvent& keyPress) override;
        void apply(vsg::ButtonPressEvent& buttonPress) override;
        void apply(vsg::ButtonReleaseEvent& buttonRelease) override;
        void apply(vsg::MoveEvent& moveEvent) override;
        void apply(vsg::ScrollWheelEvent& scrollWheel) override;

        const MapCamera& mapCamera() const { return _current; }
        void setMapCamera(const MapCamera& camera);
        void setHome(const MapCamera& camera) { _home = camera; }
        void home() { setMapCamera(_home); }

        double zoomLevel() const;
        void setZoomLevel(double zoomLevel);

        /// Limits zooming to [minZoomLevel, maxZoomLevel] by deriving the altitude limits from them for
        /// the current viewport. Zooming past the deepest tile level is allowed; the deepest tiles simply
        /// stay on screen magnified, so pick the ceiling by how far you want to be able to go, not by the data.
        void setZoomLevelRange(double minZoomLevel, double maxZoomLevel);
        double minZoomLevel() const;
        double maxZoomLevel() const;

        const MapNavigationSettings& settings() const { return _settings; }
        double viewportWidth() const;
        double viewportHeight() const;

    private:
        void applyToCamera();

        vsg::ref_ptr<vsg::Camera> _camera;
        vsg::ref_ptr<vsg::LookAt> _lookAt;
        vsg::ref_ptr<vsg::Perspective> _perspective;
        MapNavigationSettings _settings;
        MapCamera _current;
        MapCamera _home;
        bool _dragging = false;
        bool _havePointer = false;
        double _lastX = 0.0;
        double _lastY = 0.0;
    };
} // namespace ivevsg
