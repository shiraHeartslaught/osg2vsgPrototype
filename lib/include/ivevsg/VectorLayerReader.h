#pragma once

#include <ivevsg/ElevationSource.h>
#include <ivevsg/LayerOptions.h>
#include <ivevsg/VectorSource.h>

#include <vsg/core/Inherit.h>
#include <vsg/io/ReaderWriter.h>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ivevsg
{
    /// Draws OGR vector files (shapefiles, GeoJSON, GeoPackage, KML, ...) as lines and point markers in
    /// the display frame. The features are read once, then served as a quadtree of vsg::PagedLOD tiles
    /// on the WebMercatorQuad grid: each tile holds the features clipped to its footprint, simplified to
    /// half a pixel at the tile's hand-over size and densified so long edges follow the globe, so a
    /// boundary file gains detail as the view zooms in and costs little when zoomed out. Tiles at or
    /// past the drape level (loaderKeys::drapeMinZoom) put every vertex on the terrain registered in
    /// the loader's ElevationRegistry when draping is on, so borders follow valleys and ridges.
    ///
    /// Lines are drawn through one pipeline per layer with a dynamic line width (vsg::SetLineWidth
    /// at the root, editable at runtime) and one shared, dynamic colour array; both are exposed on the
    /// root node (loaderKeys::nodeVectorLineWidth / nodeVectorColour) for panels.
    class VectorLayerReader : public vsg::Inherit<vsg::ReaderWriter, VectorLayerReader>
    {
    public:
        VectorLayerReader();
        ~VectorLayerReader() override;

        /// OGR formats by extension (looksLikeVector), or anything when loaderKeys::layerKind = "vector".
        static bool handles(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options);
        static bool isVirtualTile(const vsg::Path& filename);

        vsg::ref_ptr<vsg::Object> read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options = {}) const override;
        bool getFeatures(Features& features) const override;

        std::size_t openLayerCount() const;

        struct Layer;

    private:
        std::shared_ptr<Layer> openLayer(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options, std::string& error) const;
        vsg::ref_ptr<vsg::Object> readRoot(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const;
        vsg::ref_ptr<vsg::Object> readChildren(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const;

        mutable std::mutex _mutex;
        mutable std::map<std::string, std::weak_ptr<Layer>> _layers;
    };
} // namespace ivevsg
