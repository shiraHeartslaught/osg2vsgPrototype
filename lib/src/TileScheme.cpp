#include <ivevsg/TileScheme.h>

#include <algorithm>
#include <cmath>

namespace ivevsg
{
    vsg::dbox TileScheme::tileExtent(const TileIndex& tile) const
    {
        const double side = tileSideMetres(tile.level);
        const double minX = -worldHalfExtent + side * tile.x;
        const double maxY = worldHalfExtent - side * tile.y; // rows count down from the north edge
        return vsg::dbox(vsg::dvec3(minX, maxY - side, 0.0), vsg::dvec3(minX + side, maxY, 0.0));
    }

    vsg::dvec3 TileScheme::tileCentre(const TileIndex& tile) const
    {
        const vsg::dbox extent = tileExtent(tile);
        return (extent.min + extent.max) * 0.5;
    }

    double TileScheme::tileRadius(const TileIndex& tile) const
    {
        return tileSideMetres(tile.level) * std::sqrt(0.5);
    }

    TileIndex TileScheme::tileAt(double worldX, double worldY, int level) const
    {
        const int n = tilesAcross(level);
        const double side = tileSideMetres(level);
        const int x = static_cast<int>(std::floor((worldX + worldHalfExtent) / side));
        const int y = static_cast<int>(std::floor((worldHalfExtent - worldY) / side));
        return {level, std::clamp(x, 0, n - 1), std::clamp(y, 0, n - 1)};
    }

    double TileScheme::levelForMetersPerPixel(double mpp) const
    {
        if (mpp <= 0.0) return 0.0;
        return std::log2(metersPerPixel(0) / mpp);
    }

    int TileScheme::nativeLevelFor(double sourceMetersPerPixel) const
    {
        if (sourceMetersPerPixel <= 0.0) return 0;
        return std::max(0, static_cast<int>(std::ceil(levelForMetersPerPixel(sourceMetersPerPixel) - 1e-9)));
    }

    std::array<TileIndex, 4> TileScheme::children(const TileIndex& tile)
    {
        const int l = tile.level + 1, x = tile.x * 2, y = tile.y * 2;
        return {TileIndex{l, x, y}, TileIndex{l, x + 1, y}, TileIndex{l, x, y + 1}, TileIndex{l, x + 1, y + 1}};
    }

    TileIndex TileScheme::parent(const TileIndex& tile)
    {
        if (tile.level <= 0) return tile;
        return {tile.level - 1, tile.x / 2, tile.y / 2};
    }

    std::string TileScheme::relativePath(const TileIndex& tile, const std::string& extension)
    {
        return std::to_string(tile.level) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y) + extension;
    }
} // namespace ivevsg
