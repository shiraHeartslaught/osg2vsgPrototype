#include <ivevsg/CrsTransform.h>
#include <ivevsg/VectorSource.h>

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>

namespace ivevsg
{
    namespace
    {
        void ensureGdal()
        {
            ensureGdalRegistered();
        }

        std::string lowerExtension(const std::string& path)
        {
            std::string ext = std::filesystem::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        void addExtent(GeoExtent& into, const GeoExtent& other, bool& first)
        {
            if (first)
            {
                into = other;
                first = false;
                return;
            }
            into.minLongitude = std::min(into.minLongitude, other.minLongitude);
            into.maxLongitude = std::max(into.maxLongitude, other.maxLongitude);
            into.minLatitude = std::min(into.minLatitude, other.minLatitude);
            into.maxLatitude = std::max(into.maxLatitude, other.maxLatitude);
        }

        std::vector<GeoPoint> ringPoints(OGRGeometryH geometry)
        {
            std::vector<GeoPoint> points;
            const int count = OGR_G_GetPointCount(geometry);
            points.reserve(static_cast<std::size_t>(std::max(0, count)));
            for (int i = 0; i < count; ++i) points.push_back(GeoPoint{OGR_G_GetX(geometry, i), OGR_G_GetY(geometry, i), 0.0});
            return points;
        }

        void collect(OGRGeometryH geometry, std::vector<Feature>& features)
        {
            if (!geometry || OGR_G_IsEmpty(geometry)) return;
            const OGRwkbGeometryType type = wkbFlatten(OGR_G_GetGeometryType(geometry));
            switch (type)
            {
            case wkbPoint: {
                Feature f;
                f.type = FeatureType::Point;
                f.parts.push_back({GeoPoint{OGR_G_GetX(geometry, 0), OGR_G_GetY(geometry, 0), 0.0}});
                features.push_back(std::move(f));
                break;
            }
            case wkbLineString:
            case wkbLinearRing: {
                Feature f;
                f.type = FeatureType::Line;
                f.parts.push_back(ringPoints(geometry));
                if (f.parts.back().size() >= 2) features.push_back(std::move(f));
                break;
            }
            case wkbPolygon: {
                Feature f;
                f.type = FeatureType::Polygon;
                const int rings = OGR_G_GetGeometryCount(geometry);
                for (int r = 0; r < rings; ++r)
                {
                    std::vector<GeoPoint> ring = ringPoints(OGR_G_GetGeometryRef(geometry, r));
                    if (ring.size() >= 3) f.parts.push_back(std::move(ring));
                }
                if (!f.parts.empty()) features.push_back(std::move(f));
                break;
            }
            case wkbMultiPoint: {
                Feature f;
                f.type = FeatureType::Point;
                const int n = OGR_G_GetGeometryCount(geometry);
                for (int i = 0; i < n; ++i)
                {
                    OGRGeometryH point = OGR_G_GetGeometryRef(geometry, i);
                    if (point && !OGR_G_IsEmpty(point)) f.parts.push_back({GeoPoint{OGR_G_GetX(point, 0), OGR_G_GetY(point, 0), 0.0}});
                }
                if (!f.parts.empty()) features.push_back(std::move(f));
                break;
            }
            case wkbMultiLineString: {
                Feature f;
                f.type = FeatureType::Line;
                const int n = OGR_G_GetGeometryCount(geometry);
                for (int i = 0; i < n; ++i)
                {
                    std::vector<GeoPoint> line = ringPoints(OGR_G_GetGeometryRef(geometry, i));
                    if (line.size() >= 2) f.parts.push_back(std::move(line));
                }
                if (!f.parts.empty()) features.push_back(std::move(f));
                break;
            }
            default: {
                // multipolygons and collections: each member is its own feature
                const int n = OGR_G_GetGeometryCount(geometry);
                for (int i = 0; i < n; ++i) collect(OGR_G_GetGeometryRef(geometry, i), features);
                break;
            }
            }
        }
    } // namespace

    bool looksLikeVector(const std::string& path)
    {
        const std::string ext = lowerExtension(path);
        for (const char* candidate : {".shp", ".shz", ".geojson", ".geojsonl", ".gpkg", ".kml", ".kmz", ".gml", ".fgb", ".tab", ".mif", ".gpx", ".sqlite", ".dxf"})
            if (ext == candidate) return true;
        return false;
    }

    bool readVectorData(const std::string& path, const std::string& sourceCrs, VectorData& out, std::string& error)
    {
        ensureGdal();
        out = VectorData{};
        CPLErrorReset();
        GDALDatasetH dataset = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!dataset)
        {
            const char* message = CPLGetLastErrorMsg();
            error = "OGR cannot open " + path + (message && *message ? std::string(": ") + message : "");
            return false;
        }
        OGRSpatialReference wgs84;
        wgs84.SetWellKnownGeogCS("WGS84");
        wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        OGRSpatialReference override;
        if (!sourceCrs.empty() && override.SetFromUserInput(sourceCrs.c_str()) != OGRERR_NONE)
        {
            error = "unrecognised coordinate reference system '" + sourceCrs + "'";
            GDALClose(dataset);
            return false;
        }

        bool firstExtent = true;
        const int layerCount = GDALDatasetGetLayerCount(dataset);
        for (int l = 0; l < layerCount; ++l)
        {
            OGRLayerH layer = GDALDatasetGetLayer(dataset, l);
            if (!layer) continue;
            ++out.layerCount;
            OGRSpatialReference source;
            if (!sourceCrs.empty())
            {
                source = override;
            }
            else if (OGRSpatialReferenceH declared = OGR_L_GetSpatialRef(layer))
            {
                source = *OGRSpatialReference::FromHandle(declared);
            }
            else
            {
                source.importFromEPSG(4326); // a shapefile without .prj: degrees by far the most likely
            }
            source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            if (out.sourceCrs.empty())
            {
                if (!sourceCrs.empty())
                {
                    out.sourceCrs = sourceCrs;
                }
                else
                {
                    char* wkt = nullptr;
                    if (source.exportToWkt(&wkt) == OGRERR_NONE && wkt) out.sourceCrs = wkt;
                    CPLFree(wkt);
                }
            }
            OGRCoordinateTransformation* transform = nullptr;
            const bool alreadyGeographic = source.IsGeographic() == TRUE && source.IsSame(&wgs84) == TRUE;
            if (!alreadyGeographic)
            {
                transform = OGRCreateCoordinateTransformation(&source, &wgs84);
                if (!transform)
                {
                    error = "no transformation from the CRS of " + path + " to WGS84";
                    GDALClose(dataset);
                    return false;
                }
            }
            OGR_L_ResetReading(layer);
            while (OGRFeatureH feature = OGR_L_GetNextFeature(layer))
            {
                OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
                if (geometry && !OGR_G_IsEmpty(geometry))
                {
                    OGRGeometryH linear = OGR_G_HasCurveGeometry(geometry, TRUE) ? OGR_G_GetLinearGeometry(geometry, 0.0, nullptr) : OGR_G_Clone(geometry);
                    if (linear)
                    {
                        bool ok = true;
                        if (transform) ok = OGR_G_Transform(linear, OGRCoordinateTransformation::ToHandle(transform)) == OGRERR_NONE;
                        if (ok)
                        {
                            std::vector<Feature> collected;
                            collect(linear, collected);
                            for (Feature& f : collected)
                            {
                                bool first = true;
                                for (const auto& part : f.parts)
                                {
                                    out.vertexCount += part.size();
                                    addExtent(f.extent, extentOf(part), first);
                                }
                                addExtent(out.extent, f.extent, firstExtent);
                                out.features.push_back(std::move(f));
                            }
                        }
                        OGR_G_DestroyGeometry(linear);
                    }
                }
                OGR_F_Destroy(feature);
            }
            if (transform) OGRCoordinateTransformation::DestroyCT(transform);
        }
        GDALClose(dataset);
        if (out.features.empty())
        {
            error = path + " holds no usable geometry";
            return false;
        }
        std::size_t points = 0, lines = 0, polygons = 0;
        for (const Feature& f : out.features)
        {
            if (f.type == FeatureType::Point) ++points;
            else if (f.type == FeatureType::Line) ++lines;
            else ++polygons;
        }
        out.description = std::filesystem::path(path).filename().string() + ": " + std::to_string(out.features.size()) + " features (" + std::to_string(points) + " points, " +
                          std::to_string(lines) + " lines, " + std::to_string(polygons) + " polygons), " + std::to_string(out.vertexCount) + " vertices";
        return true;
    }

    // ---- geometry helpers ---------------------------------------------------------------------------

    GeoExtent extentOf(const std::vector<GeoPoint>& points)
    {
        GeoExtent e;
        if (points.empty()) return e;
        e.minLongitude = e.maxLongitude = points.front().longitude;
        e.minLatitude = e.maxLatitude = points.front().latitude;
        for (const GeoPoint& p : points)
        {
            e.minLongitude = std::min(e.minLongitude, p.longitude);
            e.maxLongitude = std::max(e.maxLongitude, p.longitude);
            e.minLatitude = std::min(e.minLatitude, p.latitude);
            e.maxLatitude = std::max(e.maxLatitude, p.latitude);
        }
        return e;
    }

    bool extentsOverlap(const GeoExtent& a, const GeoExtent& b)
    {
        return a.minLongitude <= b.maxLongitude && a.maxLongitude >= b.minLongitude && a.minLatitude <= b.maxLatitude && a.maxLatitude >= b.minLatitude;
    }

    bool extentContains(const GeoExtent& e, double longitude, double latitude)
    {
        return longitude >= e.minLongitude && longitude <= e.maxLongitude && latitude >= e.minLatitude && latitude <= e.maxLatitude;
    }

    GeoExtent expanded(const GeoExtent& e, double degrees)
    {
        return GeoExtent{e.minLongitude - degrees, e.minLatitude - degrees, e.maxLongitude + degrees, e.maxLatitude + degrees};
    }

    std::vector<GeoPoint> densifyLine(const std::vector<GeoPoint>& line, double maxSegmentDegrees)
    {
        if (line.size() < 2 || !(maxSegmentDegrees > 0.0)) return line;
        std::vector<GeoPoint> out;
        out.reserve(line.size());
        out.push_back(line.front());
        for (std::size_t i = 1; i < line.size(); ++i)
        {
            const GeoPoint& a = line[i - 1];
            const GeoPoint& b = line[i];
            const double span = std::max(std::abs(b.longitude - a.longitude), std::abs(b.latitude - a.latitude));
            const int pieces = std::max(1, static_cast<int>(std::ceil(span / maxSegmentDegrees - 1e-9)));
            for (int k = 1; k < pieces; ++k)
            {
                const double t = static_cast<double>(k) / pieces;
                out.push_back(GeoPoint{a.longitude + (b.longitude - a.longitude) * t, a.latitude + (b.latitude - a.latitude) * t, a.altitude + (b.altitude - a.altitude) * t});
            }
            out.push_back(b);
        }
        return out;
    }

    namespace
    {
        double pointSegmentDistance(const GeoPoint& p, const GeoPoint& a, const GeoPoint& b)
        {
            const double dx = b.longitude - a.longitude, dy = b.latitude - a.latitude;
            const double len2 = dx * dx + dy * dy;
            double t = 0.0;
            if (len2 > 0.0) t = std::clamp(((p.longitude - a.longitude) * dx + (p.latitude - a.latitude) * dy) / len2, 0.0, 1.0);
            const double px = a.longitude + dx * t - p.longitude, py = a.latitude + dy * t - p.latitude;
            return std::sqrt(px * px + py * py);
        }
    } // namespace

    std::vector<GeoPoint> simplifyLine(const std::vector<GeoPoint>& line, double toleranceDegrees)
    {
        if (line.size() < 3 || !(toleranceDegrees > 0.0)) return line;
        std::vector<char> keep(line.size(), 0);
        keep.front() = keep.back() = 1;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{0, line.size() - 1}};
        while (!stack.empty())
        {
            const auto [first, last] = stack.back();
            stack.pop_back();
            if (last <= first + 1) continue;
            double worst = -1.0;
            std::size_t at = first;
            for (std::size_t i = first + 1; i < last; ++i)
            {
                const double d = pointSegmentDistance(line[i], line[first], line[last]);
                if (d > worst)
                {
                    worst = d;
                    at = i;
                }
            }
            if (worst > toleranceDegrees)
            {
                keep[at] = 1;
                stack.emplace_back(first, at);
                stack.emplace_back(at, last);
            }
        }
        std::vector<GeoPoint> out;
        for (std::size_t i = 0; i < line.size(); ++i)
            if (keep[i]) out.push_back(line[i]);
        return out;
    }

    std::vector<std::vector<GeoPoint>> clipLine(const std::vector<GeoPoint>& line, const GeoExtent& box)
    {
        std::vector<std::vector<GeoPoint>> runs;
        if (line.size() < 2) return runs;
        auto clipSegment = [&box](GeoPoint a, GeoPoint b, GeoPoint& outA, GeoPoint& outB) {
            double t0 = 0.0, t1 = 1.0;
            const double dx = b.longitude - a.longitude, dy = b.latitude - a.latitude;
            const double p[4] = {-dx, dx, -dy, dy};
            const double q[4] = {a.longitude - box.minLongitude, box.maxLongitude - a.longitude, a.latitude - box.minLatitude, box.maxLatitude - a.latitude};
            for (int i = 0; i < 4; ++i)
            {
                if (p[i] == 0.0)
                {
                    if (q[i] < 0.0) return false;
                    continue;
                }
                const double t = q[i] / p[i];
                if (p[i] < 0.0)
                    t0 = std::max(t0, t);
                else
                    t1 = std::min(t1, t);
                if (t0 > t1) return false;
            }
            outA = GeoPoint{a.longitude + dx * t0, a.latitude + dy * t0, a.altitude + (b.altitude - a.altitude) * t0};
            outB = GeoPoint{a.longitude + dx * t1, a.latitude + dy * t1, a.altitude + (b.altitude - a.altitude) * t1};
            return true;
        };
        bool continuing = false;
        for (std::size_t i = 1; i < line.size(); ++i)
        {
            GeoPoint a, b;
            if (!clipSegment(line[i - 1], line[i], a, b))
            {
                continuing = false;
                continue;
            }
            const bool aInside = a.longitude == line[i - 1].longitude && a.latitude == line[i - 1].latitude;
            if (!continuing || !aInside || runs.empty())
            {
                runs.push_back({a});
            }
            runs.back().push_back(b);
            continuing = b.longitude == line[i].longitude && b.latitude == line[i].latitude; // ended inside: the next segment continues it
        }
        runs.erase(std::remove_if(runs.begin(), runs.end(), [](const std::vector<GeoPoint>& r) { return r.size() < 2; }), runs.end());
        return runs;
    }
} // namespace ivevsg
