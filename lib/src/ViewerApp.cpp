#include <ivevsg/BaseMap.h>
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
    ViewerApp::ViewerApp(ViewerSettings settings, std::unique_ptr<WorldView> view) :
        _settings(std::move(settings)),
        _view(std::move(view))
    {
        if (!_view) _view = createWorldView("mercator", _settings.views);
    }

    ViewerApp::~ViewerApp()
    {
        uninstallView();
    }

    // ---- what to show ----------------------------------------------------------------------------

    void ViewerApp::setBaseSource(const std::string& viewKey, BaseSource source)
    {
        _baseSources[viewKey] = std::move(source);
    }

    BaseSource ViewerApp::baseSource(const std::string& viewKey) const
    {
        auto it = _baseSources.find(viewKey);
        return it == _baseSources.end() ? BaseSource{} : it->second;
    }

    bool ViewerApp::addLayer(LayerSource source)
    {
        if (source.name.empty()) source.name = vsg::simpleFilename(vsg::Path(source.path)).string();
        if (!_viewer)
        {
            _startupLayers.push_back(std::move(source));
            return true;
        }
        _lastLayerError.clear();
        return loadLayerNow(source, _lastLayerError);
    }

    void ViewerApp::queueLayerLoad(LayerSource source)
    {
        if (source.name.empty()) source.name = vsg::simpleFilename(vsg::Path(source.path)).string();
        _pendingLayerLoads.push_back(std::move(source));
    }

    bool ViewerApp::removeLayer(std::size_t index)
    {
        return _layers && _layers->removeLayer(index);
    }

    std::vector<std::string> ViewerApp::loadingLayers() const
    {
        std::vector<std::string> names;
        for (const LayerSource& source : _pendingLayerLoads) names.push_back(source.name);
        if (_asyncLoader)
        {
            for (auto& name : _asyncLoader->inFlight()) names.push_back(name);
        }
        return names;
    }

    bool ViewerApp::loadLayerNow(const LayerSource& source, std::string& error)
    {
        if (!_layers || !_loader)
        {
            error = "no scene yet";
            return false;
        }
        const LoadResult loaded = _loader->load(source.path, source.sourceCrs);
        if (!loaded)
        {
            error = loaded.error;
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
                error = "Vulkan error compiling " + source.path + ": " + e.message;
                return false;
            }
        }
        Layer layer;
        layer.name = source.name;
        layer.node = loaded.node;
        layer.source = source;
        layer.sourceCrs = loaded.sourceCrs;
        layer.targetCrs = loaded.targetCrs;
        _layers->addLayer(std::move(layer));
        return true;
    }

    // ---- scene assembly --------------------------------------------------------------------------

    double ViewerApp::viewportAspect() const
    {
        if (_window)
        {
            const auto extent = _window->extent2D();
            if (extent.height > 0) return static_cast<double>(extent.width) / static_cast<double>(extent.height);
        }
        return static_cast<double>(_settings.width) / static_cast<double>(std::max<uint32_t>(1, _settings.height));
    }

    bool ViewerApp::buildScene(std::string& error)
    {
        // a loader targeting this view's frame; paged tiles of the scene it loads inherit its options
        LoaderSettings loaderSettings = SceneLoader::defaultSettings();
        loaderSettings.logReads = _settings.logReads;
        loaderSettings.targetCrs = _view->projection()->crs();
        loaderSettings.defaultSourceCrs = _settings.defaultSourceCrs;
        loaderSettings.quadTessellation = _view->quadTessellation();
        _loader = std::make_shared<SceneLoader>(loaderSettings);
        _layers = std::make_shared<LayerStack>();
        _underlay = vsg::MatrixTransform::create();
        _baseInfo = BaseInfo{};

        // Base map: the pyramid or image registered for this view. It hangs under the "underlay"
        // transform that the frame loop keeps a small, altitude-scaled distance below the data
        // surface, so data always draws on top without z-fighting and without disturbing the tiles'
        // LOD choice.
        const BaseSource base = baseSource(_view->key());
        vsg::ref_ptr<vsg::Node> baseNode;
        if (!base.tiles.empty())
        {
            const LoadResult tiles = _loader->load(base.tiles);
            if (!tiles)
            {
                error = "Failed to load tile set " + base.tiles + ": " + tiles.error;
                return false;
            }
            baseNode = tiles.node;
            _baseInfo = BaseInfo{true, base.tiles, "tile pyramid", tiles.sourceCrs, tiles.targetCrs};
        }
        else if (!base.image.empty() && _view->supportsFlatBaseImage())
        {
            BaseMapSettings imageSettings;
            imageSettings.imagePath = base.image;
            const BaseMapResult image = createBaseMap(imageSettings, _loader->options());
            baseNode = image.node;
            _baseInfo = BaseInfo{true, base.image, "image", _view->projection()->crs(), ""};
        }
        else
        {
            _baseInfo.kind = "none";
        }
        if (baseNode)
        {
            if (_viewer && _viewer->compileManager)
            {
                try
                {
                    vsg::CompileResult result = _viewer->compileManager->compile(baseNode);
                    if (result) vsg::updateViewer(*_viewer, result);
                }
                catch (const vsg::Exception& e)
                {
                    error = "Vulkan error compiling the base map: " + e.message;
                    return false;
                }
            }
            _underlay->addChild(baseNode);
            _layers->setBase(_underlay);
        }
        _world = _layers->node();
        _bounds = baseNode ? computeGeometryBounds(*baseNode) : vsg::dbox();
        return true;
    }

    void ViewerApp::startAsyncLoader()
    {
        if (!_viewer || !_viewer->compileManager || !_loader) return;
        vsg::ref_ptr<vsg::Viewer> viewer = _viewer;
        _asyncLoader = std::make_unique<AsyncLayerLoader>(_loader, [viewer](vsg::ref_ptr<vsg::Node> node) {
            return viewer->compileManager->compile(node); // CompileManager is thread safe
        });
    }

    // ---- window and views ------------------------------------------------------------------------

    void ViewerApp::addUi(std::function<bool()> panel)
    {
        if (!panel) return;
        if (_imgui)
            _imgui->add(panel);
        else
            _pendingPanels.push_back(std::move(panel));
    }

    void ViewerApp::installView(const ViewState* carryOver)
    {
        if (!_viewer || !_camera) return;
        _view->attach(_camera, _bounds, _settings.framingElevationDegrees);
        if (carryOver)
        {
            _view->lookAt(carryOver->centre, carryOver->altitude);
            if (_view->hasOrientation()) _view->setOrientation(carryOver->heading, carryOver->tilt);
        }
        if (auto handler = _view->eventHandler()) _viewer->addEventHandler(handler); // after ImGui and the close handler
        _reportedOnce = false;
    }

    void ViewerApp::uninstallView()
    {
        if (!_view) return;
        if (_viewer)
        {
            if (auto handler = _view->eventHandler())
            {
                auto& handlers = _viewer->getEventHandlers();
                handlers.erase(std::remove(handlers.begin(), handlers.end(), handler), handlers.end());
            }
        }
        _view->detach();
    }

    bool ViewerApp::initialize()
    {
        if (!_view)
        {
            _error = "No view set";
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

            std::string error;
            if (!buildScene(error))
            {
                _error = error;
                _viewer = {};
                return false;
            }
            // start-up layers load now, on this thread; the first one frames the start view
            vsg::ref_ptr<vsg::Node> focus;
            for (const LayerSource& source : _startupLayers)
            {
                if (!loadLayerNow(source, error))
                {
                    _error = "Failed to load " + source.path + ": " + error;
                    _viewer = {};
                    return false;
                }
                if (!focus) focus = _layers->layer(_layers->layerCount() - 1).node;
            }
            _startupLayers.clear();
            if (focus) _bounds = computeGeometryBounds(*focus);
            if (!_bounds.valid() && _world) _bounds = computeGeometryBounds(*_world);

            _sceneRoot = vsg::Group::create();
            _sceneRoot->addChild(_view->assembleScene(_world, viewportAspect()));

            _lookAt = vsg::LookAt::create();
            const auto extent = _window->extent2D();
            const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
            _perspective = vsg::Perspective::create(_settings.fieldOfViewDegrees, aspect, 1.0, 1.0e8);
            _camera = vsg::Camera::create(_perspective, _lookAt, vsg::ViewportState::create(extent));

            // the same graph vsg::createCommandGraphForView() builds (view + headlight), with the ImGui
            // overlay drawn after the view inside the same render pass
            auto renderGraph = vsg::createRenderGraphForView(_window, _camera, _sceneRoot);
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
            installView(nullptr);

            // compile() uploads GPU resources and, when the scene contains PagedLOD nodes,
            // starts the database pager that streams tiles in through the loader's options.
            _viewer->compile();
            startAsyncLoader();
        }
        catch (const vsg::Exception& e)
        {
            _error = "Vulkan error during initialization: " + e.message;
            _viewer = {};
            return false;
        }
        return true;
    }

    bool ViewerApp::switchView(const std::string& viewKey)
    {
        std::unique_ptr<WorldView> view = createWorldView(viewKey, _settings.views);
        if (!view)
        {
            _error = "unknown view '" + viewKey + "'";
            return false;
        }
        return switchView(std::move(view));
    }

    bool ViewerApp::switchView(std::unique_ptr<WorldView> view)
    {
        if (!view) return false;
        if (!_viewer)
        {
            _view = std::move(view); // nothing built yet: simply start with this one
            return true;
        }

        // the sources of everything shown, to reload in the new frame
        std::vector<LayerSource> sources = _layers ? _layers->sources() : std::vector<LayerSource>{};
        if (_asyncLoader)
        {
            for (const LayerSource& source : _asyncLoader->inFlightSources()) sources.push_back(source);
        }
        for (const LayerSource& source : _pendingLayerLoads) sources.push_back(source);
        _pendingLayerLoads.clear();

        // build the new scene first so a failing base keeps the current view intact
        const ViewState carryOver = _view->state();
        std::unique_ptr<WorldView> previous = std::move(_view);
        _view = std::move(view);
        std::shared_ptr<SceneLoader> previousLoader = _loader;
        std::shared_ptr<LayerStack> previousLayers = _layers;
        vsg::ref_ptr<vsg::MatrixTransform> previousUnderlay = _underlay;
        vsg::ref_ptr<vsg::Node> previousWorld = _world;
        const vsg::dbox previousBounds = _bounds;
        std::string error;
        if (!buildScene(error))
        {
            _error = error;
            _view = std::move(previous);
            _loader = previousLoader;
            _layers = previousLayers;
            _underlay = previousUnderlay;
            _world = previousWorld;
            _bounds = previousBounds;
            for (const LayerSource& source : sources) _pendingLayerLoads.push_back(source); // keep what was queued
            return false;
        }

        // hand over: old navigation out, new scene in, new navigation in at the same place
        if (previous)
        {
            if (_viewer)
            {
                if (auto handler = previous->eventHandler())
                {
                    auto& handlers = _viewer->getEventHandlers();
                    handlers.erase(std::remove(handlers.begin(), handlers.end(), handler), handlers.end());
                }
            }
            previous->detach();
        }
        _sceneRoot->children.clear();
        _sceneRoot->addChild(_view->assembleScene(_world, viewportAspect()));
        installView(&carryOver);

        // layers of the old frame are dropped; the same sources reload on worker threads in the new one
        if (_asyncLoader) _retiredLoaders.push_back(std::move(_asyncLoader));
        startAsyncLoader();
        for (const LayerSource& source : sources) _pendingLayerLoads.push_back(source);
        _lastLayerError.clear();
        if (onViewSwitched) onViewSwitched(*_view);
        return true;
    }

    void ViewerApp::frameNode(vsg::ref_ptr<vsg::Node> node)
    {
        if (!node) return;
        const vsg::dbox bounds = computeGeometryBounds(*node);
        if (bounds.valid()) _view->frame(bounds);
    }

    void ViewerApp::frameScene()
    {
        _view->home();
    }

    double ViewerApp::zoomLevel() const
    {
        return _view->state().zoomLevel;
    }

    ViewState ViewerApp::viewState() const
    {
        return _view->state();
    }

    void ViewerApp::lookAt(const GeoPoint& target, double altitudeMetres)
    {
        _view->lookAt(target, altitudeMetres);
    }

    // ---- frame loop ------------------------------------------------------------------------------

    bool ViewerApp::frame()
    {
        if (!_viewer) return false;
        if (_settings.maxFrames >= 0 && _frameCount >= _settings.maxFrames) return false;
        if (!_viewer->advanceToNextFrame()) return false;

        _viewer->handleEvents();

        // hand queued layer loads to worker threads, and merge finished ones
        if (!_pendingLayerLoads.empty() && _asyncLoader)
        {
            for (const LayerSource& source : _pendingLayerLoads) _asyncLoader->request(source);
            _pendingLayerLoads.clear();
        }
        if (_asyncLoader)
        {
            for (LayerLoadOutcome& outcome : _asyncLoader->takeCompleted())
            {
                if (outcome.ok())
                {
                    if (outcome.compileResult) vsg::updateViewer(*_viewer, outcome.compileResult); // main thread: registers pager/descriptor needs
                    Layer layer;
                    layer.name = outcome.name;
                    layer.node = outcome.node;
                    layer.source = outcome.source;
                    layer.sourceCrs = outcome.sourceCrs;
                    layer.targetCrs = outcome.targetCrs;
                    if (_layers) _layers->addLayer(std::move(layer));
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
        // loads started for a view that is gone finish on their threads and are thrown away
        for (auto it = _retiredLoaders.begin(); it != _retiredLoaders.end();)
        {
            (*it)->takeCompleted();
            it = ((*it)->pendingCount() == 0) ? _retiredLoaders.erase(it) : it + 1;
        }

        const ViewState state = _view->state();
        {
            const double altitude = std::max(1.0, state.altitude);
            const Projection& projection = *_view->projection();
            // keep the underlay a fixed fraction of the eye height below the data surface (z on a map,
            // radially on a globe) ...
            if (_underlay) _underlay->matrix = projection.layerOffsetTransform(-std::max(1.0, altitude * underlayDepthFraction));
            // ... and each further data layer a little above the previous one, so overlaps never z-fight
            if (_layers)
            {
                for (std::size_t i = 1; i < _layers->layerCount(); ++i)
                    _layers->setLayerOffset(i, projection.layerOffsetTransform(std::max(0.5, altitude * layerSeparationFraction) * static_cast<double>(i)));
            }
        }
        if (onViewChanged)
        {
            const ViewState& last = _lastReportedState;
            if (!_reportedOnce || state.centre.longitude != last.centre.longitude || state.centre.latitude != last.centre.latitude ||
                state.altitude != last.altitude || state.heading != last.heading || state.tilt != last.tilt)
            {
                _lastReportedState = state;
                _reportedOnce = true;
                onViewChanged(state);
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
