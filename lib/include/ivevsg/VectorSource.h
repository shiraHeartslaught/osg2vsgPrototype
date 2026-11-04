#pragma once

#include <ivevsg/Projection.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ivevsg
{
    enum class FeatureType
    {
        Point,
        Line,
        Polygon
    };

    /// One OGR feature's geometry in geographic WGS84 degrees: points, line strings, or polygon rings
    /// (closed; the first is the outer ring).
    struct Feature
    {
        FeatureType type = FeatureType::Line;
        std::vector<std::vector<GeoPoint>> parts;
        GeoExtent extent;
    };

    /// Everything read from a vector file (every layer of the data source), in longitude / latitude.
    struct VectorData
    {
        std::vector<Feature> features;
        GeoExtent extent;
        std::string sourceCrs; // the CRS the coordinates were taken to be in
        std::size_t vertexCount = 0;
        std::size_t layerCount = 0;
        std::string description;
    };

    /// Formats OGR reads that this viewer offers as layers, by extension (.shp, .geojson, .gpkg, .kml, ...).
    bool looksLikeVector(const std::string& path);

    /// Reads a vector file through OGR into WGS84 degrees. sourceCrs overrides the CRS the file declares;
    /// a file naming none (a shapefile without .prj) is taken as EPSG:4326. Curves are linearised,
    /// z is dropped, empty geometries skipped.
    bool readVectorData(const std::string& path, const std::string& sourceCrs, VectorData& out, std::string& error);

    // ---- geometry helpers (pure, unit tested) ------------------------------------------------------

    GeoExtent extentOf(const std::vector<GeoPoint>& points);
    bool extentsOverlap(const GeoExtent& a, const GeoExtent& b);
    bool extentContains(const GeoExtent& extent, double longitude, double latitude);
    GeoExtent expanded(const GeoExtent& extent, double degrees);

    /// Inserts points so no segment spans more than maxSegmentDegrees along either axis, so a long
    /// edge follows the globe (and the terrain) instead of cutting straight through it.
    std::vector<GeoPoint> densifyLine(const std::vector<GeoPoint>& line, double maxSegmentDegrees);

    /// Douglas-Peucker: drops points that lie within toleranceDegrees of the line through their
    /// neighbours. End points always stay.
    std::vector<GeoPoint> simplifyLine(const std::vector<GeoPoint>& line, double toleranceDegrees);

    /// The parts of a polyline inside a box (Liang-Barsky per segment, consecutive kept segments joined).
    std::vector<std::vector<GeoPoint>> clipLine(const std::vector<GeoPoint>& line, const GeoExtent& box);
} // namespace ivevsg
