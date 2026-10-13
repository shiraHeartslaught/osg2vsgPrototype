#pragma once

#include <ivevsg/Projection.h>

namespace ivevsg
{
    /// WGS84 Pseudo-Mercator (EPSG:3857), the frame VPB databases built from Web Mercator imagery use:
    /// world x/y are metres on the Mercator plane, z is altitude, up is +z.
    class MercatorProjection : public Projection
    {
    public:
        static constexpr double kEarthRadius = 6378137.0;        // metres, spherical
        static constexpr double kMaxLatitude = 85.05112878;      // degrees, edge of the square world
        static constexpr double kMaxExtent = 20037508.342789244; // metres, pi * kEarthRadius

        std::string name() const override { return "EPSG:3857 WGS84 Pseudo-Mercator"; }

        vsg::dvec3 geoToWorld(const GeoPoint& geo) const override;
        GeoPoint worldToGeo(const vsg::dvec3& world) const override;
        vsg::dvec3 upVector(const vsg::dvec3& /*world*/) const override { return {0.0, 0.0, 1.0}; }
        CameraPose framingPose(const vsg::dbox& bounds, double elevationDegrees) const override;

        /// The Mercator world square: x and y in [-kMaxExtent, kMaxExtent]. Its top and bottom edges are
        /// latitude +-kMaxLatitude; the poles themselves are at infinity and cannot be shown.
        vsg::dbox viewLimits() const override
        {
            return vsg::dbox(vsg::dvec3(-kMaxExtent, -kMaxExtent, 0.0), vsg::dvec3(kMaxExtent, kMaxExtent, 0.0));
        }

        /// Longitude wraps: the world repeats every 2 * kMaxExtent metres along x.
        double wrapWidth() const override { return 2.0 * kMaxExtent; }

        /// near plane as a fraction of the scene radius
        double nearFarRatio = 0.0005;
    };
} // namespace ivevsg
