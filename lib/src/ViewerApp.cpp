#include <ivevsg/CameraFraming.h>
#include <ivevsg/SceneBounds.h>
#include <ivevsg/ViewerApp.h>

#include <vsg/all.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace ivevsg
{
    ViewerApp::ViewerApp(ViewerSettings settings, std::shared_ptr<const Projection> projection) :
        _settings(std::move(settings)),
        _projection(std::move(projection))
    {
    }

    bool ViewerApp::initialize(vsg::ref_ptr<vsg::Node> scene, vsg::ref_ptr<vsg::Node> focus)
    {
        _scene = scene;
        if (!_scene)
        {
            _error = "No scene to display";
            return false;
        }
        if (!_projection)
        {
            _error = "No projection set";
            return false;
        }

        try
        {
            auto traits = vsg::WindowTraits::create();
            traits->windowTitle = _settings.windowTitle;
            traits->width = _settings.width;
            traits->height = _settings.height;
            traits->debugLayer = _settings.debugLayer;

            _viewer = vsg::Viewer::create();
            _window = vsg::Window::create(traits);
            if (!_window)
            {
                _error = "Could not create window: is vsg built with the 'windowing' feature and is a Vulkan driver present?";
                _viewer = {};
                return false;
            }
            _viewer->addWindow(_window);

            _bounds = computeGeometryBounds(focus ? *focus : *_scene);
            const bool mapMode = _settings.navigation == NavigationMode::Map;
            const CameraPose pose = _projection->framingPose(_bounds, mapMode ? 90.0 : _settings.framingElevationDegrees);

            _lookAt = vsg::LookAt::create(pose.eye, pose.centre, pose.up);
            const auto extent = _window->extent2D();
            const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
            _perspective = vsg::Perspective::create(_settings.fieldOfViewDegrees, aspect, pose.nearPlane, pose.farPlane);
            _camera = vsg::Camera::create(_perspective, _lookAt, vsg::ViewportState::create(extent));

            auto commandGraph = vsg::createCommandGraphForView(_window, _camera, _scene);
            _viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});

            _viewer->addEventHandler(vsg::CloseHandler::create(_viewer)); // Esc / close button
            if (mapMode)
            {
                MapNavigationSettings navigation = _settings.mapNavigation;
                if (!navigation.viewLimits.valid()) navigation.viewLimits = _projection->viewLimits();
                if (navigation.wrapWidth <= 0.0) navigation.wrapWidth = _projection->wrapWidth();
                _mapNavigation = MapNavigationHandler::create(_camera, navigation); // drag pan, wheel zoom
                _viewer->addEventHandler(_mapNavigation);
            }
            else
            {
                _viewer->addEventHandler(vsg::Trackball::create(_camera)); // LMB orbit, RMB zoom, MMB pan
            }

            // compile() uploads GPU resources and, when the scene contains PagedLOD nodes,
            // starts the database pager that streams tiles in through the loader's options.
            _viewer->compile();
        }
        catch (const vsg::Exception& e)
        {
            _error = "Vulkan error during initialization: " + e.message;
            _viewer = {};
            return false;
        }
        return true;
    }

    void ViewerApp::setUnderlay(vsg::ref_ptr<vsg::MatrixTransform> underlay, double depthFraction)
    {
        _underlay = underlay;
        _underlayDepthFraction = depthFraction;
    }

    void ViewerApp::applyPose(const CameraPose& pose)
    {
        if (!_lookAt || !_perspective) return;
        _lookAt->eye = pose.eye;
        _lookAt->center = pose.centre;
        _lookAt->up = pose.up;
        _perspective->nearDistance = pose.nearPlane;
        _perspective->farDistance = pose.farPlane;
    }

    void ViewerApp::frameScene()
    {
        if (_mapNavigation)
        {
            _mapNavigation->home();
            return;
        }
        applyPose(_projection->framingPose(_bounds, _settings.framingElevationDegrees));
    }

    double ViewerApp::zoomLevel() const
    {
        return _mapNavigation ? _mapNavigation->zoomLevel() : 0.0;
    }

    void ViewerApp::lookAt(const GeoPoint& target, double altitudeMetres)
    {
        const vsg::dvec3 centre = _projection->geoToWorld(target);
        if (_mapNavigation)
        {
            _mapNavigation->setMapCamera(MapCamera{{centre.x, centre.y}, altitudeMetres});
            return;
        }
        const vsg::dvec3 up = _projection->upVector(centre);

        CameraPose pose;
        pose.centre = centre;
        pose.eye = centre + up * altitudeMetres;
        // Looking along -up, so the screen "up" must be perpendicular to it: prefer north, else east.
        pose.up = (std::abs(up.y) < 0.99) ? vsg::dvec3(0.0, 1.0, 0.0) : vsg::dvec3(1.0, 0.0, 0.0);
        pose.nearPlane = std::max(1.0, altitudeMetres * 0.0005);
        pose.farPlane = altitudeMetres * 10.0 + summarize(_bounds).radius;
        applyPose(pose);
    }

    bool ViewerApp::frame()
    {
        if (!_viewer) return false;
        if (_settings.maxFrames >= 0 && _frameCount >= _settings.maxFrames) return false;
        if (!_viewer->advanceToNextFrame()) return false;

        _viewer->handleEvents();
        if (_underlay)
        {
            // keep the underlay a fixed fraction of the eye height below the data plane
            double altitude = 1.0;
            if (_mapNavigation) altitude = _mapNavigation->mapCamera().altitude;
            else if (_lookAt) altitude = std::abs(_lookAt->eye.z);
            const double depth = std::max(1.0, altitude * _underlayDepthFraction);
            _underlay->matrix = vsg::translate(0.0, 0.0, -depth);
        }
        if (_mapNavigation && onMapViewChanged)
        {
            const MapCamera& camera = _mapNavigation->mapCamera();
            if (!_reportedOnce || camera.centre.x != _lastReportedCamera.centre.x || camera.centre.y != _lastReportedCamera.centre.y || camera.altitude != _lastReportedCamera.altitude)
            {
                _lastReportedCamera = camera;
                _reportedOnce = true;
                onMapViewChanged(camera, zoomLevel());
            }
        }
        _viewer->update();
        _viewer->recordAndSubmit();
        _viewer->present();
        ++_frameCount;
        return true;
    }

    int ViewerApp::run()
    {
        if (!_viewer)
        {
            std::cerr << (_error.empty() ? "ViewerApp::run() called before a successful initialize()" : _error) << "\n";
            return 1;
        }
        try
        {
            while (frame()) {}
        }
        catch (const vsg::Exception& e)
        {
            std::cerr << "Vulkan error while rendering: " << e.message << "\n";
            return 2;
        }
        return 0;
    }
} // namespace ivevsg
