#pragma once

#include <ivevsg/ElevationSource.h>
#include <ivevsg/LayerOptions.h>
#include <ivevsg/TileGeometry.h>

#include <vsg/core/Inherit.h>
#include <vsg/io/ReaderWriter.h>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ivevsg
{
    /// Shows an elevation raster (DTED, SRTM .hgt, any GDAL DEM when loaded as terrain, or a directory
    /// of cells) as a surface: a quadtree of vsg::PagedLOD tiles on the WebMercatorQuad grid, each an
    /// N x N mesh whose vertices carry the sampled heights (times the exaggeration) placed in the display
    /// frame (above the ellipsoid on the globe, as z on the map), textured with a hillshaded hypsometric
    /// tint computed from the same heights. Tiles keep subdividing `oversampleLevels` past the level at
    /// which a mesh cell equals a post, with heights interpolated between the posts (bicubic by default),
    /// so zooming in past the data's resolution shows smooth slopes rather than facets.
    ///
    /// The source is registered with the loader's ElevationRegistry while the layer's root node lives,
    /// so vector layers can drape over it and the viewer can keep the camera above it.
    class TerrainLayerReader : public vsg::Inherit<vsg::ReaderWriter, TerrainLayerReader>
    {
    public:
        TerrainLayerReader();
        ~TerrainLayerReader();

        /// .dt0 / .dt1 / .dt2 / .hgt files and directories of them, by convention; any GDAL raster when
        /// the options say loaderKeys::layerKind = "terrain".
        static bool handles(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options);
        static bool isVirtualTile(const vsg::Path& filename);

        vsg::ref_ptr<vsg::Object> read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options = {}) const override;
        bool getFeatures(Features& features) const override;

        std::size_t openLayerCount() const;

        /// Tile level at which a mesh cell of `cellsPerTile` per tile is as fine as posts `postSpacingMetres` apart.
        static int nativeLevelFor(double postSpacingMetres, int cellsPerTile);

        struct Layer;

    private:
        std::shared_ptr<Layer> openLayer(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options, std::string& error) const;
        vsg::ref_ptr<vsg::Object> readRoot(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const;
        vsg::ref_ptr<vsg::Object> readChildren(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const;

        mutable std::mutex _mutex;
        mutable std::map<std::string, std::weak_ptr<Layer>> _layers;
    };

    /// Hypsometric tint for a height between the data's minimum and maximum (sea for h <= 0), RGB in [0, 1].
    vsg::vec3 hypsometricTint(double height, double minHeight, double maxHeight);
} // namespace ivevsg
