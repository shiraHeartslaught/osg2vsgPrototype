#pragma once

#include <ivevsg/LayerOptions.h>

#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/nodes/Node.h>
#include <vsg/nodes/Switch.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ivevsg
{
    /// Group drawing the base map first and the overlay (e.g. terrain) on top of it.
    vsg::ref_ptr<vsg::Group> createLayeredScene(vsg::ref_ptr<vsg::Node> baseMap, vsg::ref_ptr<vsg::Node> overlay);

    /// Where a layer comes from and how it is to be shown, kept so the scene can be rebuilt in another
    /// projection exactly as it was.
    struct LayerSource
    {
        std::string path;
        std::string name;      // "" = the file's simple name
        std::string sourceCrs; // CRS the file is to be taken as; "" = what the file declares, else the loader default
        LayerOptions options;  // kind, zoom window, declared scale, LOD group, styles
    };

    /// Splits a "PATH?key=value&..." spec held in source.path into the path and its options (see
    /// parseLayerSpec); a plain path is left alone. Returns false with error set for a bad option.
    bool applyLayerSpec(LayerSource& source, std::string& error);

    struct Layer
    {
        std::string name;
        vsg::ref_ptr<vsg::Node> node;                 // the loaded content
        vsg::ref_ptr<vsg::MatrixTransform> placement; // what is actually in the switch; see setLayerHeight()
        LayerSource source;
        std::string sourceCrs; // the CRS the file was actually taken as ("" when the format carries none)
        std::string targetCrs; // the frame it was reprojected into ("" = shown as read)
        std::string kind;      // what the reader made of it: "terrain", "vector", "raster", "scene", "" = unknown
        std::string info;      // one line from the reader for panels
        bool visible = true;     // the user's toggle
        bool zoomAllowed = true; // the zoom rules' verdict (set every frame by the viewer)
        double declaredScale = 0.0;    // 1:S the reader found in the file's name (0 = none)
        int nativeLevel = -1;          // tile level matching the data's own resolution (-1 = n/a)
        int maxLevel = -1;             // deepest tile level the layer pages to (-1 = n/a)
        unsigned terrainRegistryId = 0; // the layer's ElevationRegistry entry (terrain layers)

        /// The declared scale in force: the one set in the options, else the one found in the name.
        double effectiveScaleDenominator() const { return source.options.scaleDenominator > 0.0 ? source.options.scaleDenominator : declaredScale; }
        /// The LOD group in force: the one set in the options, else (when a scale is known) the name without its scale prefix.
        std::string effectiveLodGroup() const;
    };

    /// The base map plus any number of named data layers drawn above it, each switchable at runtime.
    /// Backed by one vsg::Switch (base first, then the layers in order) that can be shared by the
    /// wrapped world copies; hidden layers are skipped by every traversal. A layer is drawn when the
    /// user has it on (setLayerVisible) and the zoom rules allow it (setLayerZoomAllowed).
    class LayerStack
    {
    public:
        LayerStack();

        vsg::ref_ptr<vsg::Switch> node() const { return _switch; }

        void setBase(vsg::ref_ptr<vsg::Node> base); // replaces any previous base
        bool hasBase() const { return _base.valid(); }
        void setBaseVisible(bool visible);
        bool baseVisible() const;

        std::size_t addLayer(const std::string& name, vsg::ref_ptr<vsg::Node> node); // returns its index
        std::size_t addLayer(Layer layer);                                             // with its source / CRS record
        /// The sources of the layers in order (to reload them elsewhere).
        std::vector<LayerSource> sources() const;
        bool removeLayer(std::size_t index);
        std::size_t layerCount() const { return _layers.size(); }
        const Layer& layer(std::size_t index) const { return _layers.at(index); }
        Layer& layer(std::size_t index) { return _layers.at(index); }
        /// The user's toggle.
        void setLayerVisible(std::size_t index, bool visible);
        bool layerVisible(std::size_t index) const;
        /// The zoom rules' verdict; the layer is drawn when both are true.
        void setLayerZoomAllowed(std::size_t index, bool allowed);
        bool layerZoomAllowed(std::size_t index) const;
        /// Whether the layer is actually drawn.
        bool layerShown(std::size_t index) const;

        /// Lifts a layer above the data surface by a z translation (flat maps). Layers that overlap on
        /// the same surface would z-fight; ViewerApp raises layer i by i * two ground pixels every
        /// frame, so later layers draw on top without changing what they look like.
        void setLayerHeight(std::size_t index, double z);
        /// Same, with an arbitrary transform (e.g. a radial scale on a globe, see Projection::layerOffsetTransform).
        void setLayerOffset(std::size_t index, const vsg::dmat4& transform);

    private:
        std::size_t switchIndex(std::size_t layer) const { return (_base ? 1 : 0) + layer; }
        void applyMask(std::size_t index);

        vsg::ref_ptr<vsg::Switch> _switch;
        vsg::ref_ptr<vsg::Node> _base;
        std::vector<Layer> _layers;
    };

    /// A stack with the base map and, when given, one layer named "data".
    LayerStack createLayerStack(vsg::ref_ptr<vsg::Node> baseMap, vsg::ref_ptr<vsg::Node> overlay = {});

    /// The world drawn 2 * copiesEachSide + 1 times, shifted by multiples of worldWidth along x, so a
    /// map that repeats sideways (Mercator longitude) always fills the screen while the camera wraps.
    /// The nodes are shared, not copied, so paged terrain is loaded once. One copy each side covers
    /// viewports up to twice as wide as they are tall; use ceil(aspect / 2) for wider windows.
    vsg::ref_ptr<vsg::Group> createWrappedScene(vsg::ref_ptr<vsg::Node> world, double worldWidth, int copiesEachSide = 1);
} // namespace ivevsg
