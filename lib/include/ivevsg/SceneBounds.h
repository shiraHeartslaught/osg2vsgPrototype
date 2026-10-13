#pragma once

#include <vsg/maths/box.h>
#include <vsg/nodes/Node.h>

namespace ivevsg
{
    /// Tight bounds of the geometry actually present in the (loaded part of the) scene graph.
    /// Unlike a default vsg::ComputeBounds this ignores CullNode/PagedLOD bounding spheres; for a paged
    /// terrain those make a flat map report a z-extent as large as its footprint.
    vsg::dbox computeGeometryBounds(vsg::Node& scene);
} // namespace ivevsg
