#pragma once

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

    /// Where a layer comes from, kept so the scene can be rebuilt in another projection.
    struct LayerSource
    {
        std::string path;
        std::string name;      // "" = the file's simple name
        std::string sourceCrs; // CRS the file is to be taken as; "" = what the file declares, else the loader default
    };

    struct Layer
    {
        std::string name;
        vsg::ref_ptr<vsg::Node> node;                 // the loaded content
        vsg::ref_ptr<vsg::MatrixTransform> placement; // what is actually in the switch; see setLayerHeight()
        LayerSource source;
        std::string sourceCrs; // the CRS the file was actually taken as ("" when the format carries none)
        std::string targetCrs; // the frame it was reprojected into ("" = shown as read)
    };

    /// The base map plus any number of named data layers drawn above it, each switchable at runtime.
    /// Backed by one vsg::Switch (base first, then the layers in order) that can be shared by the
    /// wrapped world copies; hidden layers are skipped by every traversal.
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
        void setLayerVisible(std::size_t index, bool visible);
        bool layerVisible(std::size_t index) const;

        /// Lifts a layer above the data surface by a z translation (flat maps). Layers that overlap on
        /// the same surface would z-fight; ViewerApp raises layer i by i * a small fraction of the
        /// camera altitude every frame, so later layers draw on top without changing what they look like.
        void setLayerHeight(std::size_t index, double z);
        /// Same, with an arbitrary transform (e.g. a radial scale on a globe, see Projection::layerOffsetTransform).
        void setLayerOffset(std::size_t index, const vsg::dmat4& transform);

    private:
        std::size_t switchIndex(std::size_t layer) const { return (_base ? 1 : 0) + layer; }

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
