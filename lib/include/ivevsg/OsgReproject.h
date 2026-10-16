#pragma once

#include <ivevsg/Projection.h>

#include <cstddef>
#include <memory>
#include <string>

namespace osg
{
    class Node;
}

namespace ivevsg
{
    struct ReprojectSettings
    {
        /// CRS the scene's coordinates are in (GDAL user input: "EPSG:3857", WKT, ...).
        std::string sourceCrs;
        /// Frame to move the scene into.
        std::shared_ptr<const Projection> target;
        /// > 1 splits geometries that are a single quad (one tile = four corners) into an N x N grid
        /// before reprojecting, so they follow the curvature of a globe instead of cutting through it.
        int quadTessellation = 1;
    };

    struct ReprojectReport
    {
        bool skipped = false; // source and target are the same CRS: nothing was touched
        std::size_t transforms = 0;
        std::size_t geometries = 0;
        std::size_t vertices = 0;
        std::size_t lods = 0;
        std::string error;

        explicit operator bool() const { return error.empty(); }
    };

    /// Rewrites an OpenSceneGraph scene from its source CRS into the target projection's world frame:
    /// every vertex is taken to world coordinates through the accumulated transforms, converted
    /// through geographic WGS84 into the target frame, and stored again as a float offset from its
    /// tile's new origin (each MatrixTransform becomes a translation to the reprojected position of its
    /// old origin, keeping float precision). LOD / PagedLOD centres move with their content and their
    /// radii and distance ranges are scaled by the local change of scale, so paging keeps working.
    /// Normals are recomputed as the target's up vector. Works on paged databases file by file, since
    /// each file is a self-contained subtree.
    ReprojectReport reprojectScene(osg::Node& scene, const ReprojectSettings& settings);
} // namespace ivevsg
