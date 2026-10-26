#include <ivevsg/CrsTransform.h>
#include <ivevsg/LoaderKeys.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/TileGeometry.h>
#include <ivevsg/TileLayerReader.h>

#include <vsg/all.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <vector>

namespace ivevsg
{
    namespace
    {
        constexpr const char* kVirtualPrefix = "ivevsg-tile://";
        constexpr const char* kLayerHolder = "ivevsg.tile_layer";
        std::atomic<unsigned> nextLayerId{1}; // across every reader, so a child request can never reach the wrong layer
        constexpr std::array<const char*, 18> kRasterExtensions = {".tif", ".tiff", ".gtiff", ".vrt", ".jp2", ".j2k", ".img", ".ecw", ".sid",
                                                                   ".ntf", ".nitf", ".dem", ".hgt", ".bil", ".asc", ".kap", ".dt0", ".dt1"};

        std::string virtualName(const std::string& id, const TileIndex& tile)
        {
            return std::string(kVirtualPrefix) + id + "/" + std::to_string(tile.level) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y);
        }

        bool parseVirtualName(const std::string& name, std::string& id, TileIndex& tile)
        {
            const std::string prefix(kVirtualPrefix);
            if (name.compare(0, prefix.size(), prefix) != 0) return false;
            const std::string rest = name.substr(prefix.size());
            std::array<std::string, 4> parts;
            std::size_t from = 0;
            for (std::size_t i = 0; i < 4; ++i)
            {
                const std::size_t slash = (i == 3) ? std::string::npos : rest.find('/', from);
                if (i < 3 && slash == std::string::npos) return false;
                parts[i] = rest.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
                from = slash + 1;
            }
            id = parts[0];
            tile.level = std::atoi(parts[1].c_str());
            tile.x = std::atoi(parts[2].c_str());
            tile.y = std::atoi(parts[3].c_str());
            return !id.empty();
        }
    } // namespace

    /// Everything one opened tile set needs to make its tiles: the source, the frame they are drawn
    /// in, and the graphics state they share.
    struct TileLayerReader::Layer
    {
        std::string id;
        std::shared_ptr<TileSource> source;
        std::shared_ptr<const Projection> projection;
        std::string sourceCrs;    // what the data is in (for the panel)
        bool reprojected = false; // the data's CRS differs from the display frame
        bool planar = false;      // grid units are the display's world units: no transformation at all
        int tessellation = 1;
        double handOverRatio = 0.0;
        std::unique_ptr<TileStateFactory> state;
        vsg::ref_ptr<vsg::Options> childOptions;

        int cellsFor(const vsg::dbox& extent, const CrsTransform* toGeographic) const
        {
            if (planar) return std::max(1, tessellation);
            GeoPoint sw{extent.min.x, extent.min.y, 0.0}, ne{extent.max.x, extent.max.y, 0.0};
            if (toGeographic)
            {
                toGeographic->toGeographic(extent.min.x, extent.min.y, 0.0, sw);
                toGeographic->toGeographic(extent.max.x, extent.max.y, 0.0, ne);
            }
            return tileCellsFor(sw, ne, tessellation);
        }

        bool subdividable(const TileIndex& tile) const
        {
            if (tile.level >= source->maxLevel()) return false;
            for (const TileIndex& child : TileGrid::children(tile))
                if (source->mayHave(child)) return true;
            return false;
        }

        /// A tile with its texture, as a PagedLOD when finer tiles exist below it, else culled by its sphere.
        vsg::ref_ptr<vsg::Node> makeTile(const TileIndex& tile, const CrsTransform* toGeographic, vsg::ref_ptr<const vsg::Options> options) const
        {
            vsg::ref_ptr<vsg::Data> image = source->readTile(tile, options);
            if (!image) return {};
            const vsg::dbox extent = source->grid().tileExtent(tile);
            const TileMesh mesh = buildTileMesh(*projection, extent, cellsFor(extent, toGeographic), planar ? nullptr : toGeographic);
            return makePagedTile(state->createTile(mesh, image), mesh.bound, subdividable(tile) ? virtualName(id, tile) : std::string(), handOverRatio, childOptions);
        }
    };

    /// Keeps a layer alive from the node it was read into: when the scene drops the node, the
    /// layer (and its raster) goes with it.
    class TileLayerHolder : public vsg::Inherit<vsg::Object, TileLayerHolder>
    {
    public:
        explicit TileLayerHolder(std::shared_ptr<TileLayerReader::Layer> in_layer) :
            layer(std::move(in_layer)) {}
        std::shared_ptr<TileLayerReader::Layer> layer;
    };

    TileLayerReader::TileLayerReader() = default;
    TileLayerReader::~TileLayerReader() = default;

    bool TileLayerReader::handlesRaster(const vsg::Path& filename)
    {
        const vsg::Path ext = vsg::lowerCaseFileExtension(filename);
        for (const char* candidate : kRasterExtensions)
            if (ext == candidate) return true;
        return false;
    }

    bool TileLayerReader::handlesTileSet(const vsg::Path& filename)
    {
        return looksLikeTileSet(filename.string());
    }

    bool TileLayerReader::isVirtualTile(const vsg::Path& filename)
    {
        const std::string name = filename.string();
        return name.compare(0, std::string(kVirtualPrefix).size(), kVirtualPrefix) == 0;
    }

    bool TileLayerReader::getFeatures(Features& features) const
    {
        for (const char* ext : kRasterExtensions) features.extensionFeatureMap[ext] = vsg::ReaderWriter::READ_FILENAME;
        features.extensionFeatureMap[".xml"] = vsg::ReaderWriter::READ_FILENAME;
        return true;
    }

    std::size_t TileLayerReader::openLayerCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::size_t alive = 0;
        for (const auto& [id, weak] : _layers)
            if (!weak.expired()) ++alive;
        return alive;
    }

    std::shared_ptr<TileLayerReader::Layer> TileLayerReader::openLayer(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options, std::string& error) const
    {
        auto layer = std::make_shared<Layer>();

        std::string sourceCrs, targetCrs;
        int tessellation = 1;
        double detailBias = 1.0;
        if (options)
        {
            options->getValue(loaderKeys::sourceCrs, sourceCrs);
            options->getValue(loaderKeys::targetCrs, targetCrs);
            options->getValue(loaderKeys::quadTessellation, tessellation);
            options->getValue(loaderKeys::detailBias, detailBias);
        }

        if (handlesTileSet(filename))
        {
            TileSetLayout layout;
            if (!describeTileSet(filename.string(), layout, error)) return nullptr;
            if (!sourceCrs.empty()) layout.grid.crs = sourceCrs; // the user knows better than the directory
            layer->source = std::make_shared<DirectoryTileSource>(layout);
            layer->sourceCrs = layout.grid.crs;
        }
        else
        {
            std::shared_ptr<RasterTileSource> raster = RasterTileSource::open(filename.string(), sourceCrs, 256, error);
            if (!raster) return nullptr;
            layer->source = raster;
            layer->sourceCrs = raster->sourceCrs();
        }

        const TileGrid& grid = layer->source->grid();
        layer->projection = targetCrs.empty() ? nullptr : createProjection(targetCrs);
        if (!layer->projection) layer->projection = createProjection(grid.crs);
        if (!layer->projection) layer->projection = std::make_shared<MercatorProjection>();
        // the data counts as reprojected when its own CRS is not the display frame (a GeoTIFF in
        // degrees is warped into Mercator); whether the tile footprints need transforming is a
        // separate question, answered by the grid's CRS
        layer->reprojected = !CrsTransform::sameCrs(layer->sourceCrs, layer->projection->crs());
        const bool gridMatches = CrsTransform::sameCrs(grid.crs, layer->projection->crs());
        layer->planar = gridMatches && !layer->projection->geocentric();
        // a flat tile in its own plane can stay one quad; anything that bends needs a grid
        layer->tessellation = layer->planar ? std::max(1, tessellation) : std::max(8, tessellation);
        layer->handOverRatio = tileHandOverRatio(grid.tileSizePixels, detailBias);
        layer->state = std::make_unique<TileStateFactory>(options);
        layer->childOptions = options ? vsg::Options::create(*options) : vsg::Options::create();
        layer->childOptions->setValue(loaderKeys::sourceCrs, layer->sourceCrs);

        std::lock_guard<std::mutex> lock(_mutex);
        for (auto it = _layers.begin(); it != _layers.end();) it = it->second.expired() ? _layers.erase(it) : std::next(it);
        layer->id = std::to_string(nextLayerId++);
        _layers[layer->id] = layer;
        return layer;
    }

    vsg::ref_ptr<vsg::Object> TileLayerReader::readRoot(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
    {
        std::string error;
        std::shared_ptr<Layer> layer = openLayer(filename, options, error);
        if (!layer) return vsg::ReadError::create(error);

        std::unique_ptr<CrsTransform> toGeographic;
        if (!layer->planar)
        {
            toGeographic = CrsTransform::create(layer->source->grid().crs, &error);
            if (!toGeographic) return vsg::ReadError::create(error);
        }

        const TileSource& source = *layer->source;
        vsg::dbox bounds = source.bounds().valid() ? source.bounds() : source.grid().extent;
        std::vector<TileIndex> roots = source.grid().rootTilesFor(bounds, source.maxLevel());
        // a tile set may start below level 0 (gdal2tiles --zoom 8-14): descend to its first level
        while (!roots.empty() && roots.front().level < source.minLevel())
        {
            std::vector<TileIndex> finer;
            for (const TileIndex& tile : roots)
                for (const TileIndex& child : TileGrid::children(tile))
                    if (source.mayHave(child) || child.level < source.minLevel()) finer.push_back(child);
            roots.swap(finer);
        }

        auto root = layer->state->createRoot();
        for (const TileIndex& tile : roots)
        {
            if (auto node = layer->makeTile(tile, toGeographic.get(), layer->childOptions)) root->addChild(node);
        }
        if (root->children.empty())
        {
            return vsg::ReadError::create("no tile of " + filename.string() + " holds any data (" + source.description() + ")");
        }
        root->setValue(loaderKeys::nodeSourceCrs, layer->sourceCrs);
        root->setValue(loaderKeys::nodeTargetCrs, layer->reprojected ? layer->projection->crs() : std::string());
        root->setValue("ivevsg.tile_source", source.description());
        root->setObject(kLayerHolder, TileLayerHolder::create(layer)); // the node owns the layer
        return root;
    }

    vsg::ref_ptr<vsg::Object> TileLayerReader::readChildren(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
    {
        std::string id;
        TileIndex parent;
        if (!parseVirtualName(filename.string(), id, parent)) return vsg::ReadError::create("malformed tile request " + filename.string());
        std::shared_ptr<Layer> layer;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _layers.find(id);
            if (it != _layers.end()) layer = it->second.lock(); // null once the scene dropped the layer's node
        }
        if (!layer) return vsg::ReadError::create("tile layer " + id + " is no longer open");

        std::unique_ptr<CrsTransform> toGeographic;
        if (!layer->planar)
        {
            std::string error;
            toGeographic = CrsTransform::create(layer->source->grid().crs, &error);
            if (!toGeographic) return vsg::ReadError::create(error);
        }
        auto group = vsg::Group::create();
        vsg::ref_ptr<const vsg::Options> tileOptions = options ? options : vsg::ref_ptr<const vsg::Options>(layer->childOptions);
        for (const TileIndex& child : TileGrid::children(parent))
        {
            if (auto node = layer->makeTile(child, toGeographic.get(), tileOptions)) group->addChild(node);
        }
        return group;
    }

    vsg::ref_ptr<vsg::Object> TileLayerReader::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
    {
        if (isVirtualTile(filename)) return readChildren(filename, options);
        if (handlesRaster(filename) || handlesTileSet(filename)) return readRoot(filename, options);
        return {};
    }
} // namespace ivevsg
