#include <ivevsg/CameraFraming.h>

#include <algorithm>
#include <cmath>

namespace ivevsg
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        /// eye distance / radius that fits the whole radius into a 30 degree vertical field of view
        constexpr double kDistancePerRadius = 2.55;
    } // namespace

    BoundsSummary summarize(const vsg::dbox& bounds)
    {
        BoundsSummary summary;
        if (!bounds.valid())
        {
            summary.centre = {0.0, 0.0, 0.0};
            summary.radius = 1.0;
            return summary;
        }
        summary.centre = (bounds.min + bounds.max) * 0.5;
        summary.radius = vsg::length(bounds.max - bounds.min) * 0.6;
        if (summary.radius <= 0.0) summary.radius = 1.0;
        return summary;
    }

    CameraPose computeFlatFramingPose(const vsg::dbox& bounds, double elevationDegrees, double nearFarRatio)
    {
        const BoundsSummary summary = summarize(bounds);
        const double clampedElevation = std::clamp(elevationDegrees, 1.0, 90.0);
        const double elevation = clampedElevation * kPi / 180.0;
        const double distance = summary.radius * kDistancePerRadius;

        CameraPose pose;
        pose.centre = summary.centre;
        pose.eye = summary.centre + vsg::dvec3(0.0, -distance * std::cos(elevation), distance * std::sin(elevation));
        // Looking straight down, +z is parallel to the view direction, so use north as the screen "up".
        pose.up = (clampedElevation >= 89.999) ? vsg::dvec3(0.0, 1.0, 0.0) : vsg::dvec3(0.0, 0.0, 1.0);
        pose.nearPlane = nearFarRatio * summary.radius;
        pose.farPlane = summary.radius * 10.0;
        return pose;
    }
} // namespace ivevsg
