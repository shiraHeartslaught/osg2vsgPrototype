#pragma once

#include <ivevsg/TileSource.h>

#include <vsg/core/Inherit.h>
#include <vsg/io/ReaderWriter.h>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ivevsg
{
    /// Turns tile sources into paged scenes in the display projection. Reads
    ///
    ///   - georeferenced rasters (GeoTIFF and the other formats GDAL knows, by extension), cut into
    ///     WebMercatorQuad tiles on the fly (RasterTileSource);
    ///   - tile directories: a TMS tree with its tilemapresource.xml, an XYZ tree, or a {z}/{x}/{y}
    ///     template (DirectoryTileSource).
    ///
    /// The root is a quadtree of vsg::PagedLOD nodes whose children are generated on request: their
    /// "filenames" are virtual ("ivevsg-tile://<layer>/z/x/y") and come back to this reader through the
    /// database pager, so a 100 000 pixel raster pages exactly like a pre-built .ive pyramid. Each tile
    /// is a textured grid on the display surface: flat on the Mercator plane, on the ellipsoid on the
    /// globe (the loader's quadTessellation option sets the grid), with alpha for transparency. The
    /// target frame is the loader's target CRS (LoaderKeys.h); the source CRS override applies to
    /// rasters that name no CRS (or name it wrongly).
    class TileLayerReader : public vsg::Inherit<vsg::ReaderWriter, TileLayerReader>
    {
    public:
        TileLayerReader();
        ~TileLayerReader();

        /// Raster formats handled by extension (".tif", ".tiff", ".vrt", ".jp2", ".img", ...).
        static bool handlesRaster(const vsg::Path& filename);
        /// Tile directories, tilemapresource.xml files and {z} templates.
        static bool handlesTileSet(const vsg::Path& filename);
        /// A virtual child name made by this reader.
        static bool isVirtualTile(const vsg::Path& filename);

        vsg::ref_ptr<vsg::Object> read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options = {}) const override;
        bool getFeatures(Features& features) const override;

        /// Layers still alive: a layer lives as long as the root node it was read into (the node
        /// holds it), so removing a layer from the scene closes its raster or directory; a child
        /// request that arrives after that is answered with a ReadError, never served from a freed layer.
        std::size_t openLayerCount() const;

        struct Layer; // one opened tile set and its shared graphics state (TileLayerReader.cpp)

    private:
        std::shared_ptr<Layer> openLayer(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options, std::string& error) const;
        vsg::ref_ptr<vsg::Object> readRoot(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const;
        vsg::ref_ptr<vsg::Object> readChildren(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const;

        mutable std::mutex _mutex;
        mutable std::map<std::string, std::weak_ptr<Layer>> _layers;
    };
} // namespace ivevsg
