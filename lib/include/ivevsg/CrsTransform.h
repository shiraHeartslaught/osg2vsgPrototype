#pragma once

#include <ivevsg/Projection.h>

#include <memory>
#include <string>

namespace ivevsg
{
    /// Converts coordinates of any GDAL-known coordinate reference system ("EPSG:3857", "EPSG:4978",
    /// a WKT string, ...) to geographic WGS84 (longitude, latitude in degrees, height in metres),
    /// which every Projection accepts. Built on OGR, so it understands whatever GDAL does.
    class CrsTransform
    {
    public:
        /// nullptr, with error filled in, when OGR cannot interpret the CRS.
        static std::unique_ptr<CrsTransform> create(const std::string& sourceCrs, std::string* error = nullptr);
        ~CrsTransform();
        CrsTransform(const CrsTransform&) = delete;
        CrsTransform& operator=(const CrsTransform&) = delete;

        const std::string& sourceCrs() const { return _sourceCrs; }

        /// True when the source already is geographic WGS84 (nothing to do).
        bool isGeographic() const { return _geographic; }

        /// The source's z (height) is carried through as ellipsoidal height in metres.
        bool toGeographic(double x, double y, double z, GeoPoint& out) const;

        /// Thread-safe helper: OGR transformations are not thread safe, so this object must only be
        /// used from one thread at a time; create one per worker.
        static bool sameCrs(const std::string& a, const std::string& b);

        /// A short label for a CRS given in any form: "EPSG:3857" for anything carrying an authority
        /// code, else the CRS's own name (e.g. "WGS 84 / Pseudo-Mercator"), else the text unchanged.
        static std::string displayName(const std::string& crs);

    private:
        CrsTransform() = default;
        struct Impl;
        std::unique_ptr<Impl> _impl;
        std::string _sourceCrs;
        bool _geographic = false;
    };
} // namespace ivevsg
