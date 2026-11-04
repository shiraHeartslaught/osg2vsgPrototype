#include <ivevsg/BaseMap.h>
#include <ivevsg/LoaderKeys.h>
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
        _view(std::move(view)),
        _elevation(ElevationRegistry::create())
    {
        if (!_view) _view = createWorldView("mercator", _settings.views);
    }

    ViewerApp::~ViewerApp()
    {
        uninstallView();
        if (_viewer) _viewer->deviceWaitIdle(); // nothing of the scene may be freed under a running frame
    }

    void ViewerApp::retire(vsg::ref_ptr<vsg::Object> node)
    {
        if (node) _retiredNodes.emplace_back(_frameCount, node);
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
        std::string specError;
        if (!applyLayerSpec(source, specError))
        {
            _lastLayerError = specError;
            std::cerr << "layer options: " << specError << "\n";
            return false;
        }
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
        std::string specError;
        if (!applyLayerSpec(source, specError))
        {
            _lastLayerError = specError;
            std::cerr << "layer options: " << specError << "\n";
            return;
        }
        if (source.name.empty()) source.name = vsg::simpleFilename(vsg::Path(source.path)).string();
        _pendingLayerLoads.push_back(std::move(source));
    }

    bool ViewerApp::removeLayer(std::size_t index)
    {
        if (!_layers || index >= _layers->layerCount()) return false;
        const Layer& layer = _layers->layer(index);
        if (layer.kind == "terrain") _terrainChanged = true; // vector layers lose their ground: re-drape
        retire(layer.placement); // its pipelines and textures outlive the frames still drawing them
        return _layers->removeLayer(index);
    }

    bool ViewerApp::reloadLayer(std::size_t index)
    {
        if (!_layers || index >= _layers->layerCount()) return false;
        LayerSource source = _layers->layer(index).source;
        if (!removeLayer(index)) return false;
        _pendingLayerLoads.push_back(std::move(source));
        return true;
    }

    void ViewerApp::reloadVectorLayers()
    {
        if (!_layers) return;
        std::vector<LayerSource> sources;
        for (std::size_t i = _layers->layerCount(); i-- > 0;)
        {
            if (_layers->layer(i).kind != "vector") continue;
            sources.insert(sources.begin(), _layers->layer(i).source);
            removeLayer(i);
        }
        for (LayerSource& source : sources) _pendingLayerLoads.push_back(std::move(source));
    }

    void ViewerApp::setLayerColour(std::size_t index, const vsg::vec4& colour)
    {
        if (!_layers || index >= _layers->layerCount()) return;
        Layer& layer = _layers->layer(index);
        layer.source.options.vector.colour = colour;
        if (!layer.node) return;
        if (auto colours = layer.node->getRefObject<vsg::vec4Array>(loaderKeys::nodeVectorColour))
        {
            colours->set(0, colour);
            colours->dirty();
        }
    }

    void ViewerApp::setLayerLineWidth(std::size_t index, float pixels)
    {
        if (!_layers || index >= _layers->layerCount()) return;
        Layer& layer = _layers->layer(index);
        layer.source.options.vector.lineWidth = pixels;
        if (!layer.node) return;
        if (auto width = layer.node->getRefObject<vsg::SetLineWidth>(loaderKeys::nodeVectorLineWidth))
            width->lineWidth = _wideLines ? std::clamp(pixels, 1.0f, 32.0f) : 1.0f;
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

    void ViewerApp::addLoadedLayer(const LayerSource& source, const std::string& name, vsg::ref_ptr<vsg::Node> node, const std::string& sourceCrs, const std::string& targetCrs,
                                   const std::string& kind, const std::string& info, double declaredScale, int nativeLevel, int maxLevel, unsigned terrainRegistryId)
    {
        if (!_layers) return;
        Layer layer;
        layer.name = name;
        layer.node = node;
        layer.source = source;
        layer.sourceCrs = sourceCrs;
        layer.targetCrs = targetCrs;
        layer.kind = kind;
        layer.info = info;
        layer.declaredScale = declaredScale;
        layer.nativeLevel = nativeLevel;
        layer.maxLevel = maxLevel;
        layer.terrainRegistryId = terrainRegistryId;
        _layers->addLayer(std::move(layer));
        if (kind == "terrain")
        {
            _terrainChanged = true; // vector layers already shown can now drape over it
            _lastTerrainLongitude = _lastTerrainLatitude = 1e9;
        }
    }

    bool ViewerApp::loadLayerNow(const LayerSource& source, std::string& error)
    {
        if (!_layers || !_loader)
        {
            error = "no scene yet";
            return false;
        }
        const LoadResult loaded = _loader->load(source);
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
        addLoadedLayer(source, source.name, loaded.node, loaded.sourceCrs, loaded.targetCrs, loaded.kind, loaded.info, loaded.declaredScale, loaded.nativeLevel, loaded.maxLevel, loaded.terrainRegistryId);
        return true;
    }

    // ---- visibility rules and terrain -----------------------------------------------------------

    void ViewerApp::setVisibility(const VisibilitySettings& settings)
    {
        const bool drapeChanged = settings.drape != _settings.visibility.drape || settings.drapeMinZoom != _settings.visibility.drapeMinZoom;
        _settings.visibility = settings;
        if (_loader)
        {
            _loader->options()->setValue(loaderKeys::drape, settings.drape);
            _loader->options()->setValue(loaderKeys::drapeMinZoom, settings.drapeMinZoom);
        }
        if (drapeChanged) reloadVectorLayers();
    }

    std::optional<double> ViewerApp::terrainHeightAt(double longitude, double latitude) const
    {
        return _elevation ? _elevation->heightAt(longitude, latitude) : std::nullopt;
    }

    void ViewerApp::applyVisibility(double zoom)
    {
        if (!_layers) return;
        std::vector<LayerVisibilityInput> inputs;
        inputs.reserve(_layers->layerCount());
        for (std::size_t i = 0; i < _layers->layerCount(); ++i)
        {
            const Layer& layer = _layers->layer(i);
            LayerVisibilityInput in;
            in.userVisible = layer.visible;
            in.minZoom = layer.source.options.minZoom;
            in.maxZoom = layer.source.options.maxZoom;
            in.scaleDenominator = layer.effectiveScaleDenominator();
            in.lodGroup = layer.effectiveLodGroup();
            inputs.push_back(in);
        }
        _layerVisibility = evaluateLayerVisibility(inputs, zoom, _settings.visibility);
        for (std::size_t i = 0; i < _layers->layerCount(); ++i)
        {
            // zoomAllowed carries the rules' verdict; the user's toggle is applied by the stack itself
            // (a layer the user switched off counts as allowed, so it reappears the moment it is switched on)
            _layers->setLayerZoomAllowed(i, inputs[i].userVisible ? _layerVisibility[i].shown : true);
            const Layer& layer = _layers->layer(i);
            if (layer.kind == "terrain" && layer.terrainRegistryId && _elevation) _elevation->setEnabled(layer.terrainRegistryId, _layers->layerShown(i));
        }
    }

    void ViewerApp::followTerrain(const ViewState& state)
    {
        if (!_view) return;
        if (!_elevation || !_elevation->hasTerrain())
        {
            if (_view->surfaceHeight() != 0.0) _view->setSurfaceHeight(0.0);
            _lastTerrainLongitude = _lastTerrainLatitude = 1e9;
            return;
        }
        if (state.centre.longitude == _lastTerrainLongitude && state.centre.latitude == _lastTerrainLatitude) return;
        _lastTerrainLongitude = state.centre.longitude;
        _lastTerrainLatitude = state.centre.latitude;
        const std::optional<double> height = _elevation->heightAt(state.centre.longitude, state.centre.latitude);
        _view->setSurfaceHeight(height ? *height : 0.0);
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
        loaderSettings.elevation = _elevation;
        loaderSettings.drape = _settings.visibility.drape;
        loaderSettings.drapeMinZoom = _settings.visibility.drapeMinZoom;
        loaderSettings.vectorMaxLevel = _settings.vectorMaxLevel;
        loaderSettings.wideLines = _wideLines;
        _loader = std::make_shared<SceneLoader>(loaderSettings);
        _layers = std::make_shared<LayerStack>();
        _layerVisibility.clear();
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

    void ViewerApp::applyZoomRange()
    {
        if (!_view) return;
        _haveZoomRange = true;
        _view->setZoomLevelRange(_minZoomLevel, viewMaxZoomLevel(_view->key()));
    }

    void ViewerApp::installView(const ViewState* carryOver)
    {
        if (!_viewer || !_camera) return;
        _view->attach(_camera, _bounds, _settings.framingElevationDegrees);
        if (_haveZoomRange) applyZoomRange();
        if (carryOver)
        {
            _view->lookAt(carryOver->centre, carryOver->altitude);
            if (_view->hasOrientation()) _view->setOrientation(carryOver->heading, carryOver->tilt);
        }
        if (auto handler = _view->eventHandler()) _viewer->addEventHandler(handler); // after ImGui and the close handler
        _reportedOnce = false;
        _lastTerrainLongitude = _lastTerrainLatitude = 1e9;
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

            // lines wider than a pixel need the wideLines device feature: ask for it when the device has
            // it (the logical device is created later, from these traits)
            if (auto physicalDevice = _window->getOrCreatePhysicalDevice())
            {
                if (physicalDevice->getFeatures().wideLines)
                {
                    if (!traits->deviceFeatures) traits->deviceFeatures = vsg::DeviceFeatures::create();
                    traits->deviceFeatures->get().wideLines = VK_TRUE;
                    _wideLines = true;
                }
            }

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

    void ViewerApp::requestViewSwitch(const std::string& viewKey)
    {
        _pendingViewKey = viewKey;
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
        for (auto& child : _sceneRoot->children) retire(child); // the old scene is still being drawn by frames in flight
        retire(previousWorld);
        _sceneRoot->children.clear();
        _sceneRoot->addChild(_view->assembleScene(_world, viewportAspect()));
        installView(&carryOver);

        // layers of the old frame are dropped; the same sources reload on worker threads in the new one
        if (_asyncLoader) _retiredLoaders.push_back(std::move(_asyncLoader));
        startAsyncLoader();
        for (const LayerSource& source : sources) _pendingLayerLoads.push_back(source);
        _lastLayerError.clear();
        _terrainChanged = false;
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

    void ViewerApp::setZoomLevelRange(double minZoomLevel, double maxZoomLevel)
    {
        _minZoomLevel = minZoomLevel;
        _maxZoomLevel = maxZoomLevel;
        applyZoomRange();
    }

    void ViewerApp::setViewMaxZoomLevel(const std::string& viewKey, double maxZoomLevel)
    {
        _viewMaxZoom[viewKey] = maxZoomLevel;
        if (_view && _view->key() == viewKey) applyZoomRange();
    }

    double ViewerApp::viewMaxZoomLevel(const std::string& viewKey) const
    {
        auto it = _viewMaxZoom.find(viewKey);
        return it == _viewMaxZoom.end() ? _maxZoomLevel : it->second;
    }

    // ---- frame loop ------------------------------------------------------------------------------

    bool ViewerApp::frame()
    {
        if (!_viewer) return false;
        if (_settings.maxFrames >= 0 && _frameCount >= _settings.maxFrames) return false;
        if (!_viewer->advanceToNextFrame()) return false;

        _viewer->handleEvents();

        // a switch asked for from a panel during the last frame happens here, between frames
        if (!_pendingViewKey.empty())
        {
            std::string key;
            key.swap(_pendingViewKey);
            _lastSwitchError.clear();
            if (!switchView(key))
            {
                _lastSwitchError = _error;
                std::cerr << "view switch to '" << key << "' failed: " << _error << "\n";
            }
        }

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
                    addLoadedLayer(outcome.source, outcome.name, outcome.node, outcome.sourceCrs, outcome.targetCrs, outcome.kind, outcome.info, outcome.declaredScale,
                                   outcome.nativeLevel, outcome.maxLevel, outcome.terrainRegistryId);
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
        // terrain came or went: vector layers are rebuilt so their tiles (re)drape
        if (_terrainChanged)
        {
            _terrainChanged = false;
            if (_settings.visibility.drape) reloadVectorLayers();
        }

        const ViewState state = _view->state();
        applyVisibility(state.zoomLevel);
        followTerrain(state);
        {
            const double altitude = std::max(1.0, state.altitude);
            const Projection& projection = *_view->projection();
            // keep the underlay a fixed fraction of the eye height below the data surface (z on a map,
            // radially on a globe) ...
            if (_underlay) _underlay->matrix = projection.layerOffsetTransform(-std::max(1.0, altitude * underlayDepthFraction));
            // ... and each further data layer a couple of ground pixels above the previous one, so overlaps
            // never z-fight (a fraction of the altitude would show as a visible lift on a tilted globe)
            if (_layers)
            {
                const double step = std::max(0.5, std::max(state.metersPerPixel, 1e-6) * layerSeparationPixels);
                for (std::size_t i = 1; i < _layers->layerCount(); ++i)
                    _layers->setLayerOffset(i, projection.layerOffsetTransform(step * static_cast<double>(i)));
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
        // release what left the scene long enough ago that no submitted frame can still refer to it
        _retiredNodes.erase(std::remove_if(_retiredNodes.begin(), _retiredNodes.end(), [this](const auto& entry) { return _frameCount - entry.first >= kRetireFrames; }), _retiredNodes.end());
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
