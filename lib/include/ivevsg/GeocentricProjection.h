#pragma once

#include <ivevsg/Projection.h>

namespace ivevsg
{
    /// Whole-earth frame: WGS84 geocentric Cartesian coordinates (EPSG:4978, "ECEF"). x points to
    /// lon 0 / lat 0, z to the north pole, metres. Geographic input is EPSG:4326 longitude/latitude in
    /// degrees with ellipsoidal height in metres, so a Mercator database reprojected into this frame
    /// drapes over the globe.
    class GeocentricProjection : public Projection
    {
    public:
        static constexpr double kSemiMajor = 6378137.0;           // WGS84 a
        static constexpr double kSemiMinor = 6356752.314245179;   // WGS84 b
        static constexpr double kFlattening = 1.0 / 298.257223563;

        std::string name() const override { return "EPSG:4978 WGS84 geocentric (globe)"; }
        std::string crs() const override { return "EPSG:4978"; }
        bool geocentric() const override { return true; }

        vsg::dvec3 geoToWorld(const GeoPoint& geo) const override;
        GeoPoint worldToGeo(const vsg::dvec3& world) const override;
        vsg::dvec3 upVector(const vsg::dvec3& world) const override;
        CameraPose framingPose(const vsg::dbox& bounds, double elevationDegrees) const override;

        /// Radial scale about the earth's centre, so a layer sits a constant distance above/below the surface.
        vsg::dmat4 layerOffsetTransform(double offsetMetres) const override;

        /// Local north direction (tangent to the ellipsoid) at a world position; east at the poles.
        vsg::dvec3 northVector(const vsg::dvec3& world) const;

        double nearFarRatio = 0.0005;
    };
} // namespace ivevsg
