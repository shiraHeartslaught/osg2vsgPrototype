#include <ivevsg/Projection.h>

#include <algorithm>

namespace ivevsg
{
    GeoExtent Projection::extentOf(const vsg::dbox& bounds) const
    {
        const GeoPoint a = worldToGeo(bounds.min);
        const GeoPoint b = worldToGeo(bounds.max);

        GeoExtent extent;
        extent.minLongitude = std::min(a.longitude, b.longitude);
        extent.maxLongitude = std::max(a.longitude, b.longitude);
        extent.minLatitude = std::min(a.latitude, b.latitude);
        extent.maxLatitude = std::max(a.latitude, b.latitude);
        return extent;
    }
} // namespace ivevsg
