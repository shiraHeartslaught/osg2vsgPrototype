#include <ivevsg/CrsTransform.h>

#include <cpl_conv.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#ifndef IVEVSG_PROJ_DATA_DIR
#    define IVEVSG_PROJ_DATA_DIR ""
#endif
#ifndef IVEVSG_GDAL_DATA_DIR
#    define IVEVSG_GDAL_DATA_DIR ""
#endif

namespace ivevsg
{
    namespace
    {
        /// PROJ needs proj.db and GDAL its data files. The vcpkg builds do not know where they end up
        /// installed, so unless PROJ_DATA / PROJ_LIB and GDAL_DATA are set (they are exported globally
        /// on this machine), point them at the vcpkg share folders recorded at configure time.
        void ensureGisDataPaths()
        {
            static std::once_flag once;
            std::call_once(once, [] {
                std::error_code ec;
                if (!std::getenv("PROJ_DATA") && !std::getenv("PROJ_LIB") && std::filesystem::is_directory(IVEVSG_PROJ_DATA_DIR, ec))
                {
                    const char* paths[] = {IVEVSG_PROJ_DATA_DIR, nullptr};
                    OSRSetPROJSearchPaths(paths);
                }
                if (!std::getenv("GDAL_DATA") && std::filesystem::is_directory(IVEVSG_GDAL_DATA_DIR, ec))
                {
                    CPLSetConfigOption("GDAL_DATA", IVEVSG_GDAL_DATA_DIR);
                }
            });
        }
    } // namespace

    struct CrsTransform::Impl
    {
        OGRSpatialReference source;
        OGRSpatialReference target;
        OGRCoordinateTransformation* transform = nullptr;
        ~Impl()
        {
            if (transform) OGRCoordinateTransformation::DestroyCT(transform);
        }
    };

    CrsTransform::~CrsTransform() = default;

    bool CrsTransform::sameCrs(const std::string& a, const std::string& b)
    {
        ensureGisDataPaths();
        auto normalise = [](std::string s) {
            std::string out;
            for (char c : s)
                if (!std::isspace(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        };
        const std::string na = normalise(a), nb = normalise(b);
        if (na == nb) return true;
        // the same CRS can be spelt as an EPSG code or as WKT; let OGR decide
        OGRSpatialReference sa, sb;
        if (sa.SetFromUserInput(a.c_str()) != OGRERR_NONE || sb.SetFromUserInput(b.c_str()) != OGRERR_NONE) return false;
        return sa.IsSame(&sb) == TRUE;
    }

    std::unique_ptr<CrsTransform> CrsTransform::create(const std::string& sourceCrs, std::string* error)
    {
        ensureGisDataPaths();
        auto impl = std::make_unique<Impl>();
        if (sourceCrs.empty() || impl->source.SetFromUserInput(sourceCrs.c_str()) != OGRERR_NONE)
        {
            if (error) *error = "unrecognised coordinate reference system: '" + sourceCrs + "'";
            return nullptr;
        }
        impl->target.SetWellKnownGeogCS("WGS84");
        // longitude, latitude order regardless of what the EPSG registry says about axis order
        impl->source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        impl->target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        std::unique_ptr<CrsTransform> result(new CrsTransform());
        result->_sourceCrs = sourceCrs;
        result->_geographic = impl->source.IsGeographic() == TRUE && impl->source.IsSame(&impl->target) == TRUE;
        if (!result->_geographic)
        {
            impl->transform = OGRCreateCoordinateTransformation(&impl->source, &impl->target);
            if (!impl->transform)
            {
                if (error) *error = "no transformation from '" + sourceCrs + "' to WGS84";
                return nullptr;
            }
        }
        result->_impl = std::move(impl);
        return result;
    }

    bool CrsTransform::toGeographic(double x, double y, double z, GeoPoint& out) const
    {
        if (_geographic)
        {
            out.longitude = x;
            out.latitude = y;
            out.altitude = z;
            return true;
        }
        if (!_impl || !_impl->transform) return false;
        double tx = x, ty = y, tz = z;
        if (!_impl->transform->Transform(1, &tx, &ty, &tz)) return false;
        out.longitude = tx;
        out.latitude = ty;
        out.altitude = tz;
        return true;
    }
    std::string CrsTransform::displayName(const std::string& crs)
    {
        if (crs.empty()) return crs;
        ensureGisDataPaths();
        OGRSpatialReference srs;
        if (srs.SetFromUserInput(crs.c_str()) != OGRERR_NONE) return crs;
        const char* authority = srs.GetAuthorityName(nullptr);
        const char* code = srs.GetAuthorityCode(nullptr);
        if (authority && code) return std::string(authority) + ":" + code;
        const char* name = srs.GetName();
        return name ? std::string(name) : crs;
    }
} // namespace ivevsg
