#include <ivevsg/CameraFraming.h>
#include <ivevsg/MercatorProjection.h>

#include <algorithm>
#include <cmath>

namespace ivevsg
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        double degreesToRadians(double degrees) { return degrees * kPi / 180.0; }
        double radiansToDegrees(double radians) { return radians * 180.0 / kPi; }
    } // namespace

    vsg::dvec3 MercatorProjection::geoToWorld(const GeoPoint& geo) const
    {
        const double latitude = std::clamp(geo.latitude, -kMaxLatitude, kMaxLatitude);
        const double x = kEarthRadius * degreesToRadians(geo.longitude);
        const double y = kEarthRadius * std::log(std::tan(kPi / 4.0 + degreesToRadians(latitude) / 2.0));
        return {x, y, geo.altitude};
    }

    GeoPoint MercatorProjection::worldToGeo(const vsg::dvec3& world) const
    {
        GeoPoint geo;
        geo.longitude = radiansToDegrees(world.x / kEarthRadius);
        geo.latitude = radiansToDegrees(2.0 * std::atan(std::exp(world.y / kEarthRadius)) - kPi / 2.0);
        geo.altitude = world.z;
        return geo;
    }

    CameraPose MercatorProjection::framingPose(const vsg::dbox& bounds, double elevationDegrees) const
    {
        return computeFlatFramingPose(bounds, elevationDegrees, nearFarRatio);
    }
} // namespace ivevsg
