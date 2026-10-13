#pragma once

#include <ivevsg/Projection.h>

namespace ivevsg
{
    struct BoundsSummary
    {
        vsg::dvec3 centre;
        double radius = 1.0;
    };

    /// Centre and a view radius for a bounding box; an invalid or degenerate box yields a unit radius.
    BoundsSummary summarize(const vsg::dbox& bounds);

    /// Pose for a flat, z-up world: the eye sits south of the centre, raised by elevationDegrees
    /// (clamped to [1, 90]), looking at the centre, at a distance that fits a 30 degree field of view.
    CameraPose computeFlatFramingPose(const vsg::dbox& bounds, double elevationDegrees, double nearFarRatio = 0.0005);
} // namespace ivevsg
