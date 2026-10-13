#pragma once

#include <ivevsg/MercatorProjection.h>

#include <vsg/maths/box.h>
#include <vsg/maths/vec3.h>

#include <array>
#include <string>

namespace ivevsg
{
    /// XYZ tile address: level z, column x (west to east), row y (north to south).
    struct TileIndex
    {
        int level = 0;
        int x = 0;
        int y = 0;

        bool operator==(const TileIndex& rhs) const { return level == rhs.level && x == rhs.x && y == rhs.y; }
        bool operator!=(const TileIndex& rhs) const { return !(*this == rhs); }
    };

    /// The OGC Two Dimensional Tile Matrix Set "WebMercatorQuad": EPSG:3857, square world of
    /// +-20037508.34 m, 2^level tiles across, 256 px tiles, origin at the top-left (north-west).
    /// Level 0 is 156543.03 m/px, each level halves it. Scale denominators assume the OGC standard
    /// 0.28 mm rendering pixel.
    struct TileScheme
    {
        double worldHalfExtent = MercatorProjection::kMaxExtent;
        int tileSizePixels = 256;

        int tilesAcross(int level) const { return 1 << level; }
        double worldWidth() const { return 2.0 * worldHalfExtent; }
        double tileSideMetres(int level) const { return worldWidth() / tilesAcross(level); }
        double metersPerPixel(int level) const { return tileSideMetres(level) / tileSizePixels; }
        double scaleDenominator(int level) const { return metersPerPixel(level) / 0.00028; }

        /// World-space (z = 0) footprint of a tile.
        vsg::dbox tileExtent(const TileIndex& tile) const;
        vsg::dvec3 tileCentre(const TileIndex& tile) const;
        /// Radius of the sphere around the tile's footprint (half its diagonal).
        double tileRadius(const TileIndex& tile) const;

        /// Tile containing a world position at the given level (clamped to the world square).
        TileIndex tileAt(double worldX, double worldY, int level) const;

        /// Fractional level whose resolution matches the given metres per pixel (level 0 at 156543 m/px).
        double levelForMetersPerPixel(double metersPerPixel) const;

        /// Smallest level whose tiles are at least as fine as a source raster of the given resolution.
        int nativeLevelFor(double sourceMetersPerPixel) const;

        static std::array<TileIndex, 4> children(const TileIndex& tile);
        static TileIndex parent(const TileIndex& tile);

        /// "z/x/y<extension>", the usual XYZ directory layout.
        static std::string relativePath(const TileIndex& tile, const std::string& extension = ".ive");
    };
} // namespace ivevsg
