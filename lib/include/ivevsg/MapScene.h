#pragma once

#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>

namespace ivevsg
{
    /// Group drawing the base map first and the overlay (e.g. terrain) on top of it.
    vsg::ref_ptr<vsg::Group> createLayeredScene(vsg::ref_ptr<vsg::Node> baseMap, vsg::ref_ptr<vsg::Node> overlay);

    /// The world drawn 2 * copiesEachSide + 1 times, shifted by multiples of worldWidth along x, so a
    /// map that repeats sideways (Mercator longitude) always fills the screen while the camera wraps.
    /// The nodes are shared, not copied, so paged terrain is loaded once. One copy each side covers
    /// viewports up to twice as wide as they are tall; use ceil(aspect / 2) for wider windows.
    vsg::ref_ptr<vsg::Group> createWrappedScene(vsg::ref_ptr<vsg::Node> world, double worldWidth, int copiesEachSide = 1);
} // namespace ivevsg
