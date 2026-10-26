#pragma once

#include <ivevsg/MercatorProjection.h>
#include <ivevsg/TileScheme.h>

#include <vsg/maths/box.h>

#include <array>
#include <string>
#include <vector>

namespace ivevsg
{
    /// A quadtree tile matrix set in any CRS: the extent its level-0 tiles cover, how many tiles sit
    /// across and down at level 0 (every further level doubles both), and the tile size in pixels.
    /// Tile indices always count rows from the top (the XYZ / OGC convention); a TMS layout on disk,
    /// whose rows count from the bottom, is converted when its tile paths are made.
    struct TileGrid
    {
        std::string crs = "EPSG:3857";
        vsg::dbox extent{vsg::dvec3(-MercatorProjection::kMaxExtent, -MercatorProjection::kMaxExtent, 0.0),
                         vsg::dvec3(MercatorProjection::kMaxExtent, MercatorProjection::kMaxExtent, 0.0)};
        int tilesAcrossAtLevel0 = 1;
        int tilesDownAtLevel0 = 1;
        int tileSizePixels = 256;

        /// OGC WebMercatorQuad: the EPSG:3857 world square, one tile at level 0 (TileScheme's layout).
        static TileGrid webMercatorQuad(int tileSizePixels = 256);
        /// EPSG:4326 with two 180 x 180 degree tiles at level 0 (OGC WorldCRS84Quad, OSGeo global-geodetic).
        static TileGrid worldCrs84Quad(int tileSizePixels = 256);

        int tilesAcross(int level) const { return tilesAcrossAtLevel0 << level; }
        int tilesDown(int level) const { return tilesDownAtLevel0 << level; }
        double tileWidth(int level) const { return (extent.max.x - extent.min.x) / tilesAcross(level); }
        double tileHeight(int level) const { return (extent.max.y - extent.min.y) / tilesDown(level); }
        /// CRS units per texel along x at a level.
        double unitsPerPixel(int level) const { return tileWidth(level) / tileSizePixels; }

        bool contains(const TileIndex& tile) const;
        /// Footprint of a tile in CRS units (z = 0).
        vsg::dbox tileExtent(const TileIndex& tile) const;
        /// Tile holding a position at a level (clamped to the grid).
        TileIndex tileAt(double x, double y, int level) const;
        static std::array<TileIndex, 4> children(const TileIndex& tile) { return TileScheme::children(tile); }

        /// Smallest level whose texels are at least as fine as the given resolution.
        int levelForUnitsPerPixel(double unitsPerPixel) const;

        /// Tiles at a level whose footprint overlaps a box (empty when the box misses the grid).
        std::vector<TileIndex> tilesOverlapping(const vsg::dbox& box, int level) const;
        /// The fewest tiles under which everything inside `box` sits: the overlapping level-0 tiles,
        /// descended while a single tile still covers the box (and the level stays <= maxLevel).
        std::vector<TileIndex> rootTilesFor(const vsg::dbox& box, int maxLevel) const;
    };

    /// Where a tile set lives on disk and how its files are named: a grid plus a path template.
    struct TileSetLayout
    {
        TileGrid grid;
        /// Path template with {z}, {x}, {y} (and optionally {-y} for rows counted from the bottom),
        /// e.g. "/data/tiles/{z}/{x}/{y}.png".
        std::string pathTemplate;
        /// Rows on disk count from the south (OSGeo TMS) rather than the north (XYZ).
        bool rowsFromBottom = false;
        int minLevel = 0;
        int maxLevel = 0;
        /// Extent the tiles cover, in grid units; an invalid box means the whole grid.
        vsg::dbox bounds;
        std::string description;

        /// File for a tile (indices with rows from the top, as everywhere else).
        std::string tilePath(const TileIndex& tile) const;
    };

    /// Reads an OSGeo TMS "tilemapresource.xml" (what gdal2tiles writes): SRS, origin, tile size,
    /// bounding box and the levels present. The template is built from the file's directory.
    bool parseTileMapResource(const std::string& xml, const std::string& directory, TileSetLayout& layout, std::string& error);

    /// Describes a tile set given its directory, its tilemapresource.xml, or a path template with
    /// {z}: a tilemapresource.xml beside the tiles is used when present, otherwise the directory is
    /// taken as an XYZ (slippy map, EPSG:3857, rows from the top) tree and its levels are scanned.
    bool describeTileSet(const std::string& path, TileSetLayout& layout, std::string& error);

    /// True for a directory, a tilemapresource.xml, or a template naming {z}.
    bool looksLikeTileSet(const std::string& path);
} // namespace ivevsg
