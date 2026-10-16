#include <ivevsg/CameraFraming.h>
#include <ivevsg/SceneBounds.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/ViewerApp.h>

#include <vsg/all.h>
#include <vsgImGui/RenderImGui.h>
#include <vsgImGui/SendEventsToImGui.h>

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

    ViewerApp::~ViewerApp() = default;

    void ViewerApp::addUi(std::function<bool()> panel)
    {
        if (!panel) return;
        if (_imgui)
            _imgui->add(panel);
        else
            _pendingPanels.push_back(std::move(panel));
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

            // the same graph vsg::createCommandGraphForView() builds (view + headlight), with the ImGui
            // overlay drawn after the view inside the same render pass
            auto renderGraph = vsg::createRenderGraphForView(_window, _camera, _scene);
            auto commandGraph = vsg::CommandGraph::create(_window, renderGraph);
            if (_settings.ui)
            {
                _imgui = vsgImGui::RenderImGui::create(_window);
                for (auto& panel : _pendingPanels) _imgui->add(panel);
                _pendingPanels.clear();
                renderGraph->addChild(_imgui);
            }
            _viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});

            if (_imgui) _viewer->addEventHandler(vsgImGui::SendEventsToImGui::create()); // first: marks events ImGui consumes as handled
            _viewer->addEventHandler(vsg::CloseHandler::create(_viewer));              // Esc / close button
            if (mapMode)
            {
                MapNavigationSettings navigation = _settings.mapNavigation;
                if (!navigation.viewLimits.valid()) navigation.viewLimits = _projection->viewLimits();
                if (navigation.wrapWidth <= 0.0) navigation.wrapWidth = _projection->wrapWidth();
                _mapNavigation = MapNavigationHandler::create(_camera, navigation); // drag pan, wheel zoom
                _viewer->addEventHandler(_mapNavigation);
            }
            else if (_settings.navigation == NavigationMode::Globe && _projection->geocentric())
            {
                _globeNavigation = GlobeNavigationHandler::create(_camera, _projection, _settings.globeNavigation); // drag spins the globe
                _viewer->addEventHandler(_globeNavigation);
            }
            else if (_projection->geocentric())
            {
                _viewer->addEventHandler(vsg::Trackball::create(_camera, vsg::EllipsoidModel::create())); // orbit about the look-at point
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

    bool ViewerApp::loadLayer(const std::string& path, const std::string& name)
    {
        _lastLayerError.clear();
        if (!_layers || !_loader)
        {
            _lastLayerError = "no layer stack or loader attached";
            return false;
        }
        const LoadResult loaded = _loader->load(path);
        if (!loaded)
        {
            _lastLayerError = loaded.error;
            return false;
        }
        if (_viewer && _viewer->compileManager)
        {
            try
            {
                vsg::CompileResult result = _viewer->compileManager->compile(loaded.node);
                if (result) vsg::updateViewer(*_viewer, result); // registers new PagedLODs with the pager etc.
            }
            catch (const vsg::Exception& e)
            {
                _lastLayerError = "Vulkan error compiling " + path + ": " + e.message;
                return false;
            }
        }
        std::string layerName = name;
        if (layerName.empty()) layerName = vsg::simpleFilename(vsg::Path(path)).string();
        _layers->addLayer(layerName, loaded.node);
        return true;
    }

    void ViewerApp::queueLayerLoad(const std::string& path, const std::string& name)
    {
        _pendingLayerLoads.emplace_back(path, name);
    }

    std::vector<std::string> ViewerApp::loadingLayers() const
    {
        std::vector<std::string> names;
        for (const auto& [path, name] : _pendingLayerLoads) names.push_back(name.empty() ? vsg::simpleFilename(vsg::Path(path)).string() : name);
        if (_asyncLoader)
        {
            for (auto& name : _asyncLoader->inFlight()) names.push_back(name);
        }
        return names;
    }

    void ViewerApp::frameNode(vsg::ref_ptr<vsg::Node> node)
    {
        if (!node || !_projection) return;
        const vsg::dbox bounds = computeGeometryBounds(*node);
        if (!bounds.valid()) return;
        if (_mapNavigation)
        {
            const CameraPose pose = _projection->framingPose(bounds, 90.0);
            _mapNavigation->setMapCamera(MapCamera{{pose.centre.x, pose.centre.y}, pose.eye.z - _mapNavigation->settings().groundHeight});
            return;
        }
        if (_globeNavigation)
        {
            const CameraPose pose = _projection->framingPose(bounds, _settings.framingElevationDegrees);
            _globeNavigation->setGlobeCamera(globeCameraFromLookAt(pose.eye, pose.centre, pose.up, *_projection));
            return;
        }
        applyPose(_projection->framingPose(bounds, _settings.framingElevationDegrees));
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
        if (_globeNavigation)
        {
            _globeNavigation->home();
            return;
        }
        applyPose(_projection->framingPose(_bounds, _settings.framingElevationDegrees));
    }

    double ViewerApp::zoomLevel() const
    {
        if (_mapNavigation) return _mapNavigation->zoomLevel();
        if (_globeNavigation) return _globeNavigation->zoomLevel();
        return 0.0;
    }

    void ViewerApp::lookAt(const GeoPoint& target, double altitudeMetres)
    {
        const vsg::dvec3 centre = _projection->geoToWorld(target);
        if (_mapNavigation)
        {
            _mapNavigation->setMapCamera(MapCamera{{centre.x, centre.y}, altitudeMetres});
            return;
        }
        if (_globeNavigation)
        {
            GlobeCamera camera;
            camera.longitude = target.longitude;
            camera.latitude = target.latitude;
            camera.altitude = altitudeMetres;
            _globeNavigation->setGlobeCamera(camera); // straight down, north up
            return;
        }
        const vsg::dvec3 up = _projection->upVector(centre);

        CameraPose pose;
        pose.centre = centre;
        pose.eye = centre + up * altitudeMetres;
        // Looking along -up, so the screen "up" must be perpendicular to it: north on a map, the
        // earth's axis (projected) on a globe, east when that degenerates.
        vsg::dvec3 screenUp = _projection->geocentric() ? vsg::dvec3(0.0, 0.0, 1.0) : vsg::dvec3(0.0, 1.0, 0.0);
        screenUp = screenUp - up * vsg::dot(screenUp, up);
        pose.up = (vsg::length(screenUp) > 1e-6) ? vsg::normalize(screenUp) : vsg::dvec3(1.0, 0.0, 0.0);
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
        // hand queued layer loads to worker threads once the viewer can compile, and merge finished ones
        if (!_pendingLayerLoads.empty() && _loader && _viewer->compileManager)
        {
            if (!_asyncLoader)
            {
                vsg::ref_ptr<vsg::Viewer> viewer = _viewer;
                _asyncLoader = std::make_unique<AsyncLayerLoader>(_loader, [viewer](vsg::ref_ptr<vsg::Node> node) {
                    return viewer->compileManager->compile(node); // CompileManager is thread safe
                });
            }
            for (const auto& [path, name] : _pendingLayerLoads) _asyncLoader->request(path, name);
            _pendingLayerLoads.clear();
        }
        if (_asyncLoader)
        {
            for (LayerLoadOutcome& outcome : _asyncLoader->takeCompleted())
            {
                if (outcome.ok())
                {
                    if (outcome.compileResult) vsg::updateViewer(*_viewer, outcome.compileResult); // main thread: registers pager/descriptor needs
                    if (_layers) _layers->addLayer(outcome.name, outcome.node);
                    _lastLayerError.clear();
                }
                else
                {
                    _lastLayerError = outcome.error;
                    std::cerr << "layer load failed: " << outcome.path << ": " << outcome.error << "\n";
                }
                if (onLayerLoaded) onLayerLoaded(outcome.name, outcome.ok(), outcome.error, outcome.seconds);
            }
        }
        {
            double altitude = 1.0;
            if (_mapNavigation) altitude = _mapNavigation->mapCamera().altitude;
            else if (_globeNavigation) altitude = _globeNavigation->globeCamera().altitude;
            else if (_lookAt && _projection) altitude = std::max(1.0, _projection->worldToGeo(_lookAt->eye).altitude);
            // keep the underlay a fixed fraction of the eye height below the data surface (z on a map,
            // radially on a globe) ...
            if (_underlay && _projection) _underlay->matrix = _projection->layerOffsetTransform(-std::max(1.0, altitude * _underlayDepthFraction));
            // ... and each further data layer a little above the previous one, so overlaps never z-fight
            if (_layers && _projection)
            {
                for (std::size_t i = 1; i < _layers->layerCount(); ++i)
                    _layers->setLayerOffset(i, _projection->layerOffsetTransform(std::max(0.5, altitude * layerSeparationFraction) * static_cast<double>(i)));
            }
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
