#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/Projection.h>

#include <vsg/maths/transform.h>

#include <algorithm>
#include <cctype>

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

    vsg::dmat4 Projection::layerOffsetTransform(double offsetMetres) const
    {
        return vsg::translate(0.0, 0.0, offsetMetres);
    }

    std::shared_ptr<Projection> createProjection(const std::string& crsOrName)
    {
        std::string key;
        for (char c : crsOrName) key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == "epsg:3857" || key == "mercator" || key == "webmercator" || key == "epsg:900913") return std::make_shared<MercatorProjection>();
        if (key == "epsg:4978" || key == "globe" || key == "geocentric" || key == "ecef") return std::make_shared<GeocentricProjection>();
        return {};
    }
} // namespace ivevsg
