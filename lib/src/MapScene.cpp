#include <ivevsg/MapScene.h>

#include <vsg/all.h>

namespace ivevsg
{
    vsg::ref_ptr<vsg::Group> createLayeredScene(vsg::ref_ptr<vsg::Node> baseMap, vsg::ref_ptr<vsg::Node> overlay)
    {
        auto group = vsg::Group::create();
        if (baseMap) group->addChild(baseMap);
        if (overlay) group->addChild(overlay);
        return group;
    }

    namespace
    {
        void setMask(vsg::ref_ptr<vsg::Switch> node, std::size_t index, bool visible)
        {
            if (node && index < node->children.size()) node->children[index].mask = visible ? vsg::MASK_ALL : vsg::MASK_OFF;
        }
        bool maskOn(vsg::ref_ptr<vsg::Switch> node, std::size_t index)
        {
            return node && index < node->children.size() && node->children[index].mask != vsg::MASK_OFF;
        }
    } // namespace

    LayerStack::LayerStack() :
        _switch(vsg::Switch::create())
    {
    }

    void LayerStack::setBase(vsg::ref_ptr<vsg::Node> base)
    {
        if (_base) _switch->children.erase(_switch->children.begin());
        _base = base;
        if (_base) _switch->children.insert(_switch->children.begin(), vsg::Switch::Child{vsg::MASK_ALL, _base});
    }

    void LayerStack::setBaseVisible(bool visible)
    {
        if (_base) setMask(_switch, 0, visible);
    }

    bool LayerStack::baseVisible() const
    {
        return _base && maskOn(_switch, 0);
    }

    std::size_t LayerStack::addLayer(const std::string& name, vsg::ref_ptr<vsg::Node> node)
    {
        Layer layer;
        layer.name = name;
        layer.node = node;
        return addLayer(std::move(layer));
    }

    std::size_t LayerStack::addLayer(Layer layer)
    {
        layer.placement = vsg::MatrixTransform::create();
        if (layer.node) layer.placement->addChild(layer.node);
        if (layer.source.name.empty()) layer.source.name = layer.name;
        _switch->addChild(true, layer.placement);
        _layers.push_back(std::move(layer));
        return _layers.size() - 1;
    }

    std::vector<LayerSource> LayerStack::sources() const
    {
        std::vector<LayerSource> result;
        for (const Layer& layer : _layers) result.push_back(layer.source);
        return result;
    }

    void LayerStack::setLayerHeight(std::size_t index, double z)
    {
        setLayerOffset(index, vsg::translate(0.0, 0.0, z));
    }

    void LayerStack::setLayerOffset(std::size_t index, const vsg::dmat4& transform)
    {
        if (index < _layers.size()) _layers[index].placement->matrix = transform;
    }

    bool LayerStack::removeLayer(std::size_t index)
    {
        if (index >= _layers.size()) return false;
        _switch->children.erase(_switch->children.begin() + static_cast<std::ptrdiff_t>(switchIndex(index)));
        _layers.erase(_layers.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    void LayerStack::setLayerVisible(std::size_t index, bool visible)
    {
        if (index < _layers.size()) setMask(_switch, switchIndex(index), visible);
    }

    bool LayerStack::layerVisible(std::size_t index) const
    {
        return index < _layers.size() && maskOn(_switch, switchIndex(index));
    }

    LayerStack createLayerStack(vsg::ref_ptr<vsg::Node> baseMap, vsg::ref_ptr<vsg::Node> overlay)
    {
        LayerStack stack;
        if (baseMap) stack.setBase(baseMap);
        if (overlay) stack.addLayer("data", overlay);
        return stack;
    }

    vsg::ref_ptr<vsg::Group> createWrappedScene(vsg::ref_ptr<vsg::Node> world, double worldWidth, int copiesEachSide)
    {
        auto group = vsg::Group::create();
        if (!world) return group;
        if (worldWidth <= 0.0 || copiesEachSide <= 0)
        {
            group->addChild(world);
            return group;
        }
        for (int i = -copiesEachSide; i <= copiesEachSide; ++i)
        {
            auto copy = vsg::MatrixTransform::create(vsg::translate(worldWidth * i, 0.0, 0.0));
            copy->addChild(world);
            group->addChild(copy);
        }
        return group;
    }
} // namespace ivevsg
