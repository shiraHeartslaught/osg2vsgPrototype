#pragma once

#include <ivevsg/TileGrid.h>

#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/io/Options.h>

#include <memory>
#include <string>

namespace ivevsg
{
    /// Supplies the images of a quadtree of tiles on some TileGrid: a georeferenced raster cut on the
    /// fly, or a tile directory on disk. TileLayerReader turns one into a paged scene in the display
    /// projection. Implementations are used from the database pager's threads, so readTile() must be
    /// safe to call concurrently.
    class TileSource
    {
    public:
        virtual ~TileSource() = default;

        virtual const TileGrid& grid() const = 0;
        /// CRS the tile footprints are in (the grid's).
        std::string crs() const { return grid().crs; }
        /// Human readable summary for logs and panels.
        virtual std::string description() const = 0;

        virtual int minLevel() const = 0;
        virtual int maxLevel() const = 0;
        /// Extent holding data, in grid units; an invalid box means the whole grid.
        virtual vsg::dbox bounds() const = 0;

        /// Whether a tile can have content at all (inside the bounds, its file present, ...).
        virtual bool mayHave(const TileIndex& tile) const = 0;
        /// The tile's image as RGBA8 (vsg::ubvec4Array2D, first row at the north edge), or null when
        /// the tile holds nothing. Pixels outside the data carry alpha 0.
        virtual vsg::ref_ptr<vsg::Data> readTile(const TileIndex& tile, vsg::ref_ptr<const vsg::Options> options) = 0;
    };

    /// A GDAL-readable raster (GeoTIFF, JPEG2000, VRT, ...) served as WebMercatorQuad tiles: the raster
    /// is warped from its own CRS (or an override, for files that name none or name it wrongly) into
    /// EPSG:3857 on demand, so any projection GDAL knows can be shown. The source alpha band or nodata
    /// value, and everything outside the raster's footprint, become transparent.
    ///
    /// Large rasters should carry overviews (gdaladdo); without them the coarse tiles read the whole
    /// file. The deepest level matches the raster's own resolution.
    class RasterTileSource : public TileSource
    {
    public:
        ~RasterTileSource() override;

        /// Opens the raster; sourceCrs overrides the CRS the file declares ("" = use the file's).
        static std::shared_ptr<RasterTileSource> open(const std::string& path, const std::string& sourceCrs, int tileSizePixels, std::string& error);

        const TileGrid& grid() const override { return _grid; }
        std::string description() const override;
        int minLevel() const override { return 0; }
        int maxLevel() const override { return _maxLevel; }
        vsg::dbox bounds() const override { return _bounds; }
        bool mayHave(const TileIndex& tile) const override;
        vsg::ref_ptr<vsg::Data> readTile(const TileIndex& tile, vsg::ref_ptr<const vsg::Options> options) override;

        /// The CRS the raster was taken to be in (its own, or the override).
        const std::string& sourceCrs() const { return _sourceCrs; }
        const std::string& path() const { return _path; }
        /// Resolution of the warped raster in EPSG:3857 metres per pixel.
        double metersPerPixel() const { return _metersPerPixel; }
        bool hasOverviews() const { return _hasOverviews; }
        int width() const { return _sourceWidth; }
        int height() const { return _sourceHeight; }

    private:
        RasterTileSource() = default;
        struct Impl;
        std::unique_ptr<Impl> _impl;
        TileGrid _grid;
        std::string _path;
        std::string _sourceCrs;
        vsg::dbox _bounds;
        int _maxLevel = 0;
        double _metersPerPixel = 0.0;
        bool _hasOverviews = false;
        int _sourceWidth = 0, _sourceHeight = 0;
    };

    /// Tiles already cut and stored as images: a gdal2tiles / MapTiler style TMS or XYZ directory, or
    /// any {z}/{x}/{y} template. Images are read with the loader's own readers (PNG, JPEG, ...).
    class DirectoryTileSource : public TileSource
    {
    public:
        explicit DirectoryTileSource(TileSetLayout layout);

        const TileGrid& grid() const override { return _layout.grid; }
        std::string description() const override { return _layout.description; }
        int minLevel() const override { return _layout.minLevel; }
        int maxLevel() const override { return _layout.maxLevel; }
        vsg::dbox bounds() const override { return _layout.bounds; }
        bool mayHave(const TileIndex& tile) const override;
        vsg::ref_ptr<vsg::Data> readTile(const TileIndex& tile, vsg::ref_ptr<const vsg::Options> options) override;

        const TileSetLayout& layout() const { return _layout; }

    private:
        TileSetLayout _layout;
    };

    /// Any 2D image data as RGBA8 with the first row at the top (null when the format is not handled).
    vsg::ref_ptr<vsg::Data> toRgba8(vsg::ref_ptr<vsg::Data> image);
} // namespace ivevsg
