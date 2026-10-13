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
