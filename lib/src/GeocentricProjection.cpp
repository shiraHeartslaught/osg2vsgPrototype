#include <ivevsg/CameraFraming.h>
#include <ivevsg/GeocentricProjection.h>

#include <vsg/maths/transform.h>

#include <algorithm>
#include <cmath>

namespace ivevsg
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kA = GeocentricProjection::kSemiMajor;
        constexpr double kB = GeocentricProjection::kSemiMinor;
        constexpr double kE2 = 1.0 - (kB * kB) / (kA * kA);            // first eccentricity squared
        constexpr double kEp2 = (kA * kA - kB * kB) / (kB * kB);       // second eccentricity squared
        double radians(double degrees) { return degrees * kPi / 180.0; }
        double degrees(double radians) { return radians * 180.0 / kPi; }
    } // namespace

    vsg::dvec3 GeocentricProjection::geoToWorld(const GeoPoint& geo) const
    {
        const double lat = radians(geo.latitude), lon = radians(geo.longitude);
        const double sinLat = std::sin(lat), cosLat = std::cos(lat);
        const double n = kA / std::sqrt(1.0 - kE2 * sinLat * sinLat); // prime vertical radius
        return {(n + geo.altitude) * cosLat * std::cos(lon),
                (n + geo.altitude) * cosLat * std::sin(lon),
                (n * (1.0 - kE2) + geo.altitude) * sinLat};
    }

    GeoPoint GeocentricProjection::worldToGeo(const vsg::dvec3& w) const
    {
        // Bowring's closed form as the starting point (exact on the surface, a few 1e-7 degrees off
        // at 20 000 km up), then a few fixed-point iterations to converge everywhere
        const double p = std::sqrt(w.x * w.x + w.y * w.y);
        const double theta = std::atan2(w.z * kA, p * kB);
        const double sinT = std::sin(theta), cosT = std::cos(theta);
        double lat = std::atan2(w.z + kEp2 * kB * sinT * sinT * sinT, p - kE2 * kA * cosT * cosT * cosT);
        const double lon = std::atan2(w.y, w.x);
        double n = kA, altitude = 0.0;
        for (int i = 0; i < 6; ++i)
        {
            const double sinLat = std::sin(lat), cosLat = std::cos(lat);
            n = kA / std::sqrt(1.0 - kE2 * sinLat * sinLat);
            altitude = (std::abs(cosLat) > 1e-10) ? p / cosLat - n : std::abs(w.z) - kB;
            if (std::abs(cosLat) <= 1e-10) break;
            lat = std::atan2(w.z, p * (1.0 - kE2 * n / (n + altitude)));
        }
        GeoPoint geo;
        geo.longitude = degrees(lon);
        geo.latitude = degrees(lat);
        geo.altitude = altitude;
        return geo;
    }

    vsg::dvec3 GeocentricProjection::upVector(const vsg::dvec3& world) const
    {
        const GeoPoint geo = worldToGeo(world);
        const double lat = radians(geo.latitude), lon = radians(geo.longitude);
        return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)}; // ellipsoid normal
    }

    vsg::dvec3 GeocentricProjection::northVector(const vsg::dvec3& world) const
    {
        const vsg::dvec3 up = upVector(world);
        vsg::dvec3 north = vsg::dvec3(0.0, 0.0, 1.0) - up * up.z;
        if (vsg::length(north) < 1e-9) return {-1.0, 0.0, 0.0}; // at a pole: any tangent will do
        return vsg::normalize(north);
    }

    CameraPose GeocentricProjection::framingPose(const vsg::dbox& bounds, double elevationDegrees) const
    {
        const BoundsSummary summary = summarize(bounds);
        // look at the point on the surface under the bounds' centre, from along its normal
        GeoPoint centreGeo = worldToGeo(summary.centre);
        centreGeo.altitude = 0.0;
        const vsg::dvec3 surface = geoToWorld(centreGeo);
        const vsg::dvec3 up = upVector(surface);
        const vsg::dvec3 north = northVector(surface);
        const double elevation = radians(std::clamp(elevationDegrees, 1.0, 90.0));
        const double distance = summary.radius * 2.55;

        CameraPose pose;
        pose.centre = surface;
        // tilt from straight above towards the south, like the flat framing: at 90 degrees straight down
        pose.eye = surface + up * (distance * std::sin(elevation)) - north * (distance * std::cos(elevation));
        pose.up = (elevationDegrees >= 89.999) ? north : up;
        pose.nearPlane = std::max(1.0, nearFarRatio * summary.radius);
        pose.farPlane = distance + summary.radius + kSemiMajor * 2.0; // always past the far side of the earth
        return pose;
    }

    vsg::dmat4 GeocentricProjection::layerOffsetTransform(double offsetMetres) const
    {
        const double s = 1.0 + offsetMetres / kSemiMajor;
        return vsg::scale(s, s, s);
    }
} // namespace ivevsg
