#pragma once

#include <ivevsg/MapNavigation.h>
#include <ivevsg/Projection.h>

#include <vsg/app/Camera.h>
#include <vsg/app/Viewer.h>
#include <vsg/app/Window.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/nodes/Node.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ivevsg
{
    enum class NavigationMode
    {
        Map,      ///< 2D web-map style: straight down, north up, drag to pan, wheel to zoom (MapNavigationHandler)
        Trackball ///< free 3D orbit with vsg::Trackball, framed from an oblique elevation
    };

    struct ViewerSettings
    {
        std::string windowTitle = "ive PoC viewer";
        uint32_t width = 1280;
        uint32_t height = 1024;
        bool debugLayer = false; // enable the Vulkan validation layer
        double fieldOfViewDegrees = 30.0;
        double framingElevationDegrees = 45.0; // Trackball mode only; Map mode always looks straight down
        NavigationMode navigation = NavigationMode::Map;
        MapNavigationSettings mapNavigation;
        int maxFrames = -1; // >= 0 stops after that many frames (smoke tests, screenshots)
    };

    /// Window + camera + event handling around a vsg::Viewer, with camera placement delegated to a Projection.
    class ViewerApp
    {
    public:
        ViewerApp(ViewerSettings settings, std::shared_ptr<const Projection> projection);

        /// Creates the window, camera and event handlers and compiles the scene (which also starts the
        /// database pager when the scene contains PagedLODs). Returns false, with error() set, on failure.
        /// The initial view frames `focus` when given (e.g. the terrain), otherwise the whole scene.
        bool initialize(vsg::ref_ptr<vsg::Node> scene, vsg::ref_ptr<vsg::Node> focus = {});

        /// Re-frames the whole scene using the projection's framing pose.
        void frameScene();

        /// Looks straight down at a geographic position from the given altitude in metres.
        void lookAt(const GeoPoint& target, double altitudeMetres);

        /// Web Mercator zoom level of the current view (Map mode only, 0 otherwise).
        double zoomLevel() const;

        /// Called from frame() whenever the map camera has changed (Map mode only).
        std::function<void(const MapCamera& camera, double zoomLevel)> onMapViewChanged;

        /// A layer (e.g. the base map) kept a small distance below the data plane to avoid z-fighting.
        /// The distance is scaled with the camera altitude every frame, so it never changes what level
        /// of detail the layer's tiles select, and is still far enough to separate the depth values.
        /// A fixed offset would not work: 400 km is nothing at zoom 2 but swamps an 18 km altitude at zoom 14.
        void setUnderlay(vsg::ref_ptr<vsg::MatrixTransform> underlay, double depthFraction = 0.02);
        vsg::ref_ptr<vsg::MatrixTransform> underlay() const { return _underlay; }

        /// Runs one frame. Returns false once the viewer closes or maxFrames is reached.
        bool frame();

        /// Runs frames until the viewer closes. Returns a process exit code.
        int run();

        const ViewerSettings& settings() const { return _settings; }
        const Projection& projection() const { return *_projection; }
        vsg::ref_ptr<vsg::Viewer> viewer() const { return _viewer; }
        vsg::ref_ptr<vsg::Window> window() const { return _window; }
        vsg::ref_ptr<vsg::Camera> camera() const { return _camera; }
        vsg::ref_ptr<MapNavigationHandler> mapNavigation() const { return _mapNavigation; }
        vsg::ref_ptr<vsg::Node> scene() const { return _scene; }
        const vsg::dbox& sceneBounds() const { return _bounds; }
        int frameCount() const { return _frameCount; }
        const std::string& error() const { return _error; }

    private:
        void applyPose(const CameraPose& pose);

        ViewerSettings _settings;
        std::shared_ptr<const Projection> _projection;
        vsg::ref_ptr<vsg::Node> _scene;
        vsg::ref_ptr<vsg::Viewer> _viewer;
        vsg::ref_ptr<vsg::Window> _window;
        vsg::ref_ptr<vsg::Camera> _camera;
        vsg::ref_ptr<vsg::LookAt> _lookAt;
        vsg::ref_ptr<vsg::Perspective> _perspective;
        vsg::ref_ptr<MapNavigationHandler> _mapNavigation;
        vsg::ref_ptr<vsg::MatrixTransform> _underlay;
        double _underlayDepthFraction = 0.02;
        vsg::dbox _bounds;
        MapCamera _lastReportedCamera;
        bool _reportedOnce = false;
        int _frameCount = 0;
        std::string _error;
    };
} // namespace ivevsg
