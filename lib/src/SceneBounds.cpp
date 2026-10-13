#include <ivevsg/SceneBounds.h>

#include <vsg/utils/ComputeBounds.h>

namespace ivevsg
{
    vsg::dbox computeGeometryBounds(vsg::Node& scene)
    {
        vsg::ComputeBounds computeBounds;
        computeBounds.useNodeBounds = false;
        scene.accept(computeBounds);
        return computeBounds.bounds;
    }
} // namespace ivevsg
