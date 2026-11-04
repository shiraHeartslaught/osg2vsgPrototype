#pragma once

#include <ivevsg/AsyncLayerLoader.h>
#include <ivevsg/ElevationSource.h>
#include <ivevsg/LayerVisibility.h>
#include <ivevsg/MapScene.h>
#include <ivevsg/Projection.h>
#include <ivevsg/WorldView.h>

#include <vsg/app/Camera.h>
#include <vsg/app/Viewer.h>
#include <vsg/app/Window.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/nodes/Node.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vsgImGui
{
    class RenderImGui;
}

namespace ivevsg
{
    class SceneLoader;

    struct ViewerSettings
    {
        std::string windowTitle = "ive PoC viewer";
        uint32_t width = 1280;
        uint32_t height = 1024;
        bool debugLayer = false; // enable the Vulkan validation layer
        double fieldOfViewDegrees = 30.0;
        double framingElevationDegrees = 90.0; // start angle above the ground for views with tilt (90 = straight down)
        WorldViewSettings views;               // navigation settings for the views made by key
        bool ui = true;                        // render Dear ImGui panels registered with addUi() (vsgImGui)
        int maxFrames = -1;                    // >= 0 stops after that many frames (smoke tests, screenshots)
        std::string defaultSourceCrs = "EPSG:3857"; // CRS assumed for OSG files that do not name one
        bool logReads = false;                      // print every file the loaders read
        VisibilitySettings visibility;              // global zoom floor, draping, resolution tolerance
        int vectorMaxLevel = 16;                    // deepest tile level vector layers page to
    };

    /// The base map for one view: a tile pyramid root, or (flat maps only) one image. Both empty = none.
    struct BaseSource
    {
        std::string tiles;
        std::string image;
        bool empty() const { return tiles.empty() && image.empty(); }
    };

    /// What the base map currently shown came from and how it was treated.
    struct BaseInfo
    {
        bool present = false;
        std::string path;
        std::string kind;      // "tile pyramid", "image", "none"
        std::string sourceCrs; // CRS the file was taken as ("" = n/a)
        std::string targetCrs; // frame it was reprojected into ("" = shown as read)
    };

    /// Window + camera + event handling around a vsg::Viewer, showing a base map and a stack of layers
    /// in whichever WorldView is current. The view (projection, navigation, scene assembly) can be
    /// swapped at runtime: the layers are reloaded into the new frame from their recorded sources, the
    /// camera is handed over at the same place and altitude, and nothing else needs to change.
    ///
    /// Every frame the zoom rules decide which layers are drawn (zoom windows, the global floor, LOD
    /// groups of declared scales), and the camera is kept above whatever terrain layers are loaded.
    class ViewerApp
    {
    public:
        ViewerApp(ViewerSettings settings, std::unique_ptr<WorldView> view);
        ~ViewerApp();

        // ---- what to show ------------------------------------------------------------------------

        /// The base map used when the view with this key is current (set before or after initialize()).
        void setBaseSource(const std::string& viewKey, BaseSource source);
        BaseSource baseSource(const std::string& viewKey) const;
        const BaseInfo& baseInfo() const { return _baseInfo; }

        /// Adds a layer above the base. Before initialize() it is loaded during initialize() (the first
        /// one frames the start view); afterwards it loads on the calling thread, pausing rendering.
        /// The path may carry options ("file.shp?minzoom=3&scale=50m", see parseLayerSpec).
        /// Returns false with lastLayerError() set when the load fails.
        bool addLayer(LayerSource source);

        /// Loads a file as a new layer on a worker thread (read, convert and GPU compile all off the
        /// main thread); the layer joins the scene at the start of the frame after it is ready, so
        /// rendering is never interrupted. Progress is visible through loadingLayers(), completion
        /// through onLayerLoaded and lastLayerError().
        void queueLayerLoad(LayerSource source);
        void queueLayerLoad(const std::string& path, const std::string& name = {}) { queueLayerLoad(LayerSource{path, name, {}, {}}); }
        bool removeLayer(std::size_t index);
        /// Removes a layer and loads it again from its (possibly edited) source, on a worker thread.
        bool reloadLayer(std::size_t index);
        /// Reloads every vector layer (after terrain or draping changed).
        void reloadVectorLayers();

        /// Live edits of a vector layer's look (also recorded in its source for reloads).
        void setLayerColour(std::size_t index, const vsg::vec4& colour);
        void setLayerLineWidth(std::size_t index, float pixels);

        std::vector<std::string> loadingLayers() const;
        const std::string& lastLayerError() const { return _lastLayerError; }
        std::function<void(const std::string& name, bool ok, const std::string& error, double seconds)> onLayerLoaded;

        // ---- visibility rules and terrain --------------------------------------------------------

        const VisibilitySettings& visibility() const { return _settings.visibility; }
        /// Changing draping (on/off or its level) reloads the vector layers so their tiles pick it up.
        void setVisibility(const VisibilitySettings& settings);
        /// The zoom rules' verdict for each layer, as of the last frame.
        const std::vector<LayerVisibilityResult>& layerVisibility() const { return _layerVisibility; }

        /// The terrain layers currently loaded (shared with the loaders).
        vsg::ref_ptr<ElevationRegistry> elevation() const { return _elevation; }
        /// Height (exaggerated) of the terrain under a point, if any terrain covers it.
        std::optional<double> terrainHeightAt(double longitude, double latitude) const;
        /// Whether the Vulkan device draws lines wider than one pixel.
        bool wideLinesSupported() const { return _wideLines; }

        // ---- window and views --------------------------------------------------------------------

        /// Registers an ImGui panel (a callable that issues ImGui::Begin/End and returns true to keep
        /// drawing). ImGui gets events before the navigation, so interacting with a panel never moves
        /// the view. Call before or after initialize().
        void addUi(std::function<bool()> panel);

        /// Creates the window, assembles the scene for the current view from the registered base and
        /// layers, attaches the view's navigation and compiles. Returns false, with error() set, on failure.
        bool initialize();

        /// Switches to another view at runtime: the base map registered for it is loaded, every layer is
        /// reloaded into the new frame on worker threads, and the camera continues over the same ground
        /// at the same altitude. Returns false (and keeps the current view) when the new base cannot load.
        bool switchView(std::unique_ptr<WorldView> view);
        bool switchView(const std::string& viewKey);
        /// Switches at the start of the next frame. This is the form for UI code: panels are drawn
        /// while the frame is being recorded, so a switch made from one would rebuild the scene
        /// mid-record and destroy the view the panel is still reading. A failure is reported through
        /// lastSwitchError().
        void requestViewSwitch(const std::string& viewKey);
        bool viewSwitchPending() const { return !_pendingViewKey.empty(); }
        const std::string& lastSwitchError() const { return _lastSwitchError; }
        std::function<void(const WorldView& view)> onViewSwitched;

        WorldView& view() { return *_view; }
        const WorldView& view() const { return *_view; }
        const Projection& projection() const { return *_view->projection(); }

        void frameScene();                            // home view
        void frameNode(vsg::ref_ptr<vsg::Node> node); // frame one layer
        void lookAt(const GeoPoint& target, double altitudeMetres);
        double zoomLevel() const;
        /// Zoom limits, kept and applied to every view the app switches to.
        void setZoomLevelRange(double minZoomLevel, double maxZoomLevel);
        /// A ceiling for one view only ("globe": how far in the globe may be zoomed), over the shared range.
        void setViewMaxZoomLevel(const std::string& viewKey, double maxZoomLevel);
        double viewMaxZoomLevel(const std::string& viewKey) const;
        double minZoomLevel() const { return _minZoomLevel; }
        ViewState viewState() const;
        /// Called from frame() whenever the view's state changed.
        std::function<void(const ViewState& state)> onViewChanged;

        /// Ground pixels (at the view's current resolution) each successive data layer is lifted by, so
        /// coincident layers never z-fight yet stay where they belong even on a tilted globe (see
        /// LayerStack::setLayerHeight), and the fraction of the camera altitude the base map is kept
        /// below the data surface (see the frame loop).
        double layerSeparationPixels = 2.0;
        double underlayDepthFraction = 0.02;

        /// Runs one frame. Returns false once the viewer closes or maxFrames is reached.
        bool frame();
        /// Runs frames until the viewer closes. Returns a process exit code.
        int run();

        const ViewerSettings& settings() const { return _settings; }
        std::shared_ptr<LayerStack> layers() const { return _layers; }
        std::shared_ptr<SceneLoader> loader() const { return _loader; }
        vsg::ref_ptr<vsg::Viewer> viewer() const { return _viewer; }
        vsg::ref_ptr<vsg::Window> window() const { return _window; }
        vsg::ref_ptr<vsg::Camera> camera() const { return _camera; }
        vsg::ref_ptr<vsg::Node> scene() const { return _sceneRoot; }
        const vsg::dbox& sceneBounds() const { return _bounds; }
        int frameCount() const { return _frameCount; }
        const std::string& error() const { return _error; }

    private:
        bool buildScene(std::string& error);                       // loader, layer stack and base for the current view
        bool loadLayerNow(const LayerSource& source, std::string& error);
        void installView(const ViewState* carryOver);
        void uninstallView();
        void startAsyncLoader();
        void applyZoomRange();
        void applyVisibility(double zoom);
        void followTerrain(const ViewState& state);
        void addLoadedLayer(const LayerSource& source, const std::string& name, vsg::ref_ptr<vsg::Node> node, const std::string& sourceCrs, const std::string& targetCrs,
                            const std::string& kind, const std::string& info, double declaredScale, int nativeLevel, int maxLevel, unsigned terrainRegistryId);
        double viewportAspect() const;

        ViewerSettings _settings;
        std::unique_ptr<WorldView> _view;
        std::map<std::string, BaseSource> _baseSources;
        BaseInfo _baseInfo;
        std::vector<LayerSource> _startupLayers; // added before initialize()
        std::string _pendingViewKey;             // requestViewSwitch(), applied in frame()
        bool _haveZoomRange = false;
        double _minZoomLevel = 0.0, _maxZoomLevel = 28.0;
        std::map<std::string, double> _viewMaxZoom; // per-view ceilings
        std::string _lastSwitchError;

        vsg::ref_ptr<vsg::Viewer> _viewer;
        vsg::ref_ptr<vsg::Window> _window;
        vsg::ref_ptr<vsg::Camera> _camera;
        vsg::ref_ptr<vsg::LookAt> _lookAt;
        vsg::ref_ptr<vsg::Perspective> _perspective;
        vsg::ref_ptr<vsg::Group> _sceneRoot; // what the vsg::View renders; its one child is the assembled scene
        vsg::ref_ptr<vsg::Node> _world;      // the layer stack in the view's world frame
        vsg::ref_ptr<vsg::MatrixTransform> _underlay;
        vsg::ref_ptr<vsgImGui::RenderImGui> _imgui;
        std::vector<std::function<bool()>> _pendingPanels;
        bool _wideLines = false;

        std::shared_ptr<SceneLoader> _loader;
        std::shared_ptr<LayerStack> _layers;
        vsg::ref_ptr<ElevationRegistry> _elevation; // outlives every loader, so terrain survives view switches
        std::vector<LayerSource> _pendingLayerLoads; // queued until the viewer can compile
        std::unique_ptr<AsyncLayerLoader> _asyncLoader;
        std::vector<std::unique_ptr<AsyncLayerLoader>> _retiredLoaders; // loads of a previous view, drained and dropped
        /// Nodes taken out of the scene stay alive for a few frames before they are released: the
        /// GPU may still be executing a frame that binds their pipelines and textures.
        static constexpr int kRetireFrames = 8;
        std::vector<std::pair<int, vsg::ref_ptr<vsg::Object>>> _retiredNodes;
        void retire(vsg::ref_ptr<vsg::Object> node);
        std::string _lastLayerError;
        std::vector<LayerVisibilityResult> _layerVisibility;
        bool _terrainChanged = false; // a terrain layer came or went: vector layers re-drape next frame
        double _lastTerrainLongitude = 1e9, _lastTerrainLatitude = 1e9;

        vsg::dbox _bounds;
        ViewState _lastReportedState;
        bool _reportedOnce = false;
        int _frameCount = 0;
        std::string _error;
    };
} // namespace ivevsg
