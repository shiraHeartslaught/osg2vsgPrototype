#pragma once

#include <vsg/maths/box.h>
#include <vsg/maths/vec3.h>

#include <string>

namespace ivevsg
{
    /// Geographic position: degrees on WGS84, altitude in metres above the projection surface.
    struct GeoPoint
    {
        double longitude = 0.0; // degrees, east positive
        double latitude = 0.0;  // degrees, north positive
        double altitude = 0.0;  // metres
    };

    /// Geographic bounding rectangle in degrees.
    struct GeoExtent
    {
        double minLongitude = 0.0;
        double minLatitude = 0.0;
        double maxLongitude = 0.0;
        double maxLatitude = 0.0;
    };

    /// Camera pose plus depth range, produced by a projection's framing logic.
    struct CameraPose
    {
        vsg::dvec3 eye;
        vsg::dvec3 centre;
        vsg::dvec3 up{0.0, 0.0, 1.0};
        double nearPlane = 1.0;
        double farPlane = 1000.0;
    };

    /// Maps between geographic coordinates and the scene's world coordinate frame, and knows how to
    /// place a camera in that frame. MercatorProjection is the first implementation; a geocentric (ECEF)
    /// projection for whole-earth databases can be added behind the same interface without changing
    /// the loader or the viewer.
    class Projection
    {
    public:
        virtual ~Projection() = default;

        virtual std::string name() const = 0;

        virtual vsg::dvec3 geoToWorld(const GeoPoint& geo) const = 0;
        virtual GeoPoint worldToGeo(const vsg::dvec3& world) const = 0;

        /// Local "up" direction at a world position (constant for flat projections).
        virtual vsg::dvec3 upVector(const vsg::dvec3& world) const = 0;

        /// Camera pose that frames the given world-space bounds, viewed from the given elevation angle.
        virtual CameraPose framingPose(const vsg::dbox& bounds, double elevationDegrees) const = 0;

        /// Area of the world plane the map exists on. Map navigation keeps the visible area inside it,
        /// so the camera stops at the map's edge instead of showing empty space beyond it.
        /// An invalid box (the default) means the projection is unbounded.
        virtual vsg::dbox viewLimits() const { return vsg::dbox(); }

        /// Period along x for projections whose world repeats sideways (0 = no repetition). When set,
        /// the scene is drawn again one period to each side and the camera's x wraps around.
        virtual double wrapWidth() const { return 0.0; }

        /// Geographic extent covered by a world-space bounding box.
        GeoExtent extentOf(const vsg::dbox& bounds) const;
    };
} // namespace ivevsg
