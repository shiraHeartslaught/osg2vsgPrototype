#include <ivevsg/LoaderKeys.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/TerrainLayerReader.h>
#include <ivevsg/TileGrid.h>

#include <vsg/all.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace ivevsg
{
    namespace
    {
        constexpr const char* kVirtualPrefix = "ivevsg-terrain://";
        constexpr const char* kLayerHolder = "ivevsg.terrain_layer";
        constexpr int kTilePixels = 256; // hand-over size on screen, shared with the imagery pyramids
        constexpr double kPi = 3.14159265358979323846;
        std::atomic<unsigned> nextLayerId{1};

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

        vsg::dbox intersect(const vsg::dbox& a, const vsg::dbox& b)
        {
            vsg::dbox box;
            if (!(a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y)) return box;
            box.min = vsg::dvec3(std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), 0.0);
            box.max = vsg::dvec3(std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), 0.0);
            return box;
        }

        // Hypsometric tint for a height between the data's minimum and maximum (sea for h <= 0), RGB in [0, 1].
        vsg::vec3 hypsometricTint(double height, double minHeight, double maxHeight)
        {
            if (!(height > 0.0)) return {0.25f, 0.45f, 0.70f}; // sea, or a void
            const double low = std::max(0.0, minHeight), high = std::max(low + 1.0, maxHeight);
            const double t = std::clamp((height - low) / (high - low), 0.0, 1.0);
            struct Stop
            {
                double at;
                vsg::vec3 colour;
            };
            static const Stop stops[] = {{0.0, {0.33f, 0.55f, 0.25f}}, {0.35, {0.76f, 0.72f, 0.38f}}, {0.7, {0.56f, 0.39f, 0.23f}}, {1.0, {0.95f, 0.95f, 0.95f}}};
            for (std::size_t i = 1; i < 4; ++i)
            {
                if (t <= stops[i].at)
                {
                    const float f = static_cast<float>((t - stops[i - 1].at) / (stops[i].at - stops[i - 1].at));
                    return stops[i - 1].colour * (1.0f - f) + stops[i].colour * f;
                }
            }
            return stops[3].colour;
        }
    } // namespace

    int TerrainLayerReader::nativeLevelFor(double postSpacingMetres, int cellsPerTile)
    {
        if (!(postSpacingMetres > 0.0)) return 0;
        const TileGrid grid = TileGrid::webMercatorQuad(kTilePixels);
        const double cellAtZero = grid.tileWidth(0) / std::max(1, cellsPerTile);
        return std::max(0, static_cast<int>(std::ceil(std::log2(cellAtZero / postSpacingMetres) - 1e-9)));
    }

    /// One opened elevation set and everything its tiles share.
    struct TerrainLayerReader::Layer
    {
        std::string id;
        std::shared_ptr<ElevationSource> source;
        std::shared_ptr<const Projection> projection;
        MercatorProjection mercator; // the tile grid's frame
        TileGrid grid = TileGrid::webMercatorQuad(kTilePixels);
        TerrainStyle style;
        int nativeLevel = 0;
        int maxLevel = 0;
        double handOverRatio = 0.0;
        vsg::dbox bounds; // the data's footprint in grid (Mercator) units
        std::unique_ptr<TileStateFactory> state;
        vsg::ref_ptr<vsg::Options> childOptions;
        vsg::ref_ptr<ElevationRegistry> registry;
        unsigned registryId = 0;

        GeoPoint geo(double x, double y) const { return mercator.worldToGeo(vsg::dvec3(x, y, 0.0)); }

        bool subdividable(const TileIndex& tile) const { return tile.level < maxLevel; }

        /// The heights the tile needs: native posts with a margin (then interpolated), or, for tiles
        /// spanning many posts, the posts averaged down to about two per texel.
        bool heightsFor(const vsg::dbox& box, HeightGrid& grid) const
        {
            const GeoPoint sw = geo(box.min.x, box.min.y), ne = geo(box.max.x, box.max.y);
            GeoExtent extent;
            extent.minLongitude = sw.longitude;
            extent.maxLongitude = ne.longitude;
            extent.minLatitude = sw.latitude;
            extent.maxLatitude = ne.latitude;
            const int maxPosts = 2 * std::max(style.texturePixels, style.cellsPerTile + 1) + 4;
            return source->readWindow(extent, 2, maxPosts, grid);
        }

        vsg::ref_ptr<vsg::Data> makeTexture(const vsg::dbox& box, const HeightGrid& heights) const
        {
            const int n = std::clamp(style.texturePixels, 8, 1024);
            auto image = vsg::ubvec4Array2D::create(static_cast<uint32_t>(n), static_cast<uint32_t>(n), vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});
            const double w = box.max.x - box.min.x, h = box.max.y - box.min.y;
            const double texelX = w / n, texelY = h / n;
            const vsg::dvec3 light = vsg::normalize(vsg::dvec3(-0.5, 0.5, 0.7071)); // from the north-west, 45 degrees up
            const double minH = source->minHeight(), maxH = source->maxHeight();
            const double exaggeration = style.exaggeration;
            auto heightAt = [&](double x, double y) {
                const GeoPoint g = geo(x, y);
                return heights.sample(g.longitude, g.latitude, style.interpolation);
            };
            for (int j = 0; j < n; ++j)
            {
                const double y = box.max.y - (j + 0.5) * texelY; // rows from the north
                const double latitude = geo(box.min.x, y).latitude;
                const double cosLat = std::max(0.01, std::cos(latitude * kPi / 180.0));
                for (int i = 0; i < n; ++i)
                {
                    const double x = box.min.x + (i + 0.5) * texelX;
                    const double height = heightAt(x, y);
                    vsg::vec3 colour = hypsometricTint(height, minH, maxH);
                    if (style.hillshade)
                    {
                        const double dzdx = (heightAt(x + texelX, y) - heightAt(x - texelX, y)) * exaggeration / (2.0 * texelX * cosLat);
                        const double dzdy = (heightAt(x, y + texelY) - heightAt(x, y - texelY)) * exaggeration / (2.0 * texelY * cosLat);
                        const vsg::dvec3 normal = vsg::normalize(vsg::dvec3(-dzdx, -dzdy, 1.0));
                        const double shade = std::max(0.0, vsg::dot(normal, light));
                        colour = colour * static_cast<float>(0.3 + 0.7 * shade);
                    }
                    auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    image->set(static_cast<uint32_t>(i), static_cast<uint32_t>(j), vsg::ubvec4(byte(colour.r), byte(colour.g), byte(colour.b), 255));
                }
            }
            return image;
        }

        TileMesh makeMesh(const vsg::dbox& box, const HeightGrid& heights) const
        {
            const int n = std::clamp(style.cellsPerTile, 1, 254);
            const std::size_t count = static_cast<std::size_t>(n + 1) * (n + 1);
            std::vector<vsg::dvec3> world(count);
            vsg::dbox worldBox;
            for (int j = 0; j <= n; ++j)
                for (int i = 0; i <= n; ++i)
                {
                    const double x = box.min.x + (box.max.x - box.min.x) * i / n;
                    const double y = box.min.y + (box.max.y - box.min.y) * j / n; // south to north
                    GeoPoint g = geo(x, y);
                    g.altitude = heights.sample(g.longitude, g.latitude, style.interpolation) * style.exaggeration;
                    vsg::dvec3& w = world[static_cast<std::size_t>(j) * (n + 1) + i];
                    w = projection->geoToWorld(g);
                    worldBox.add(w);
                }
            TileMesh mesh;
            mesh.origin = (worldBox.min + worldBox.max) * 0.5;
            double radius = 0.0;
            for (const vsg::dvec3& w : world) radius = std::max(radius, vsg::length(w - mesh.origin));
            mesh.bound = vsg::dsphere(mesh.origin, radius * 1.001 + 1.0);
            mesh.vertices = vsg::vec3Array::create(static_cast<uint32_t>(count));
            mesh.normals = vsg::vec3Array::create(static_cast<uint32_t>(count));
            mesh.texcoords = vsg::vec2Array::create(static_cast<uint32_t>(count));
            for (int j = 0; j <= n; ++j)
                for (int i = 0; i <= n; ++i)
                {
                    const std::size_t k = static_cast<std::size_t>(j) * (n + 1) + i;
                    const vsg::dvec3 local = world[k] - mesh.origin;
                    mesh.vertices->set(k, vsg::vec3(static_cast<float>(local.x), static_cast<float>(local.y), static_cast<float>(local.z)));
                    const vsg::dvec3 up = projection->upVector(world[k]);
                    mesh.normals->set(k, vsg::vec3(static_cast<float>(up.x), static_cast<float>(up.y), static_cast<float>(up.z)));
                    mesh.texcoords->set(k, vsg::vec2(static_cast<float>(i) / n, 1.0f - static_cast<float>(j) / n));
                }
            mesh.indices = vsg::ushortArray::create(static_cast<uint32_t>(n) * n * 6);
            const auto index = [n](int i, int j) { return static_cast<uint16_t>(j * (n + 1) + i); };
            std::size_t at = 0;
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i)
                {
                    mesh.indices->set(at++, index(i, j));
                    mesh.indices->set(at++, index(i + 1, j));
                    mesh.indices->set(at++, index(i + 1, j + 1));
                    mesh.indices->set(at++, index(i, j));
                    mesh.indices->set(at++, index(i + 1, j + 1));
                    mesh.indices->set(at++, index(i, j + 1));
                }
            return mesh;
        }

        vsg::ref_ptr<vsg::Node> makeTile(const TileIndex& tile) const
        {
            const vsg::dbox box = intersect(grid.tileExtent(tile), bounds);
            if (!box.valid()) return {};
            HeightGrid heights;
            if (!heightsFor(box, heights)) return {};
            const TileMesh mesh = makeMesh(box, heights);
            vsg::ref_ptr<vsg::Data> image = makeTexture(box, heights);
            return makePagedTile(state->createTile(mesh, image), mesh.bound, subdividable(tile) ? virtualName(id, tile) : std::string(), handOverRatio, childOptions);
        }
    };

    /// Owned by the root node: keeps the layer alive and its source registered exactly that long.
    class TerrainLayerHolder : public vsg::Inherit<vsg::Object, TerrainLayerHolder>
    {
    public:
        explicit TerrainLayerHolder(std::shared_ptr<TerrainLayerReader::Layer> in_layer) :
            layer(std::move(in_layer)) {}
        ~TerrainLayerHolder() override
        {
            if (layer && layer->registry && layer->registryId) layer->registry->remove(layer->registryId);
        }
        std::shared_ptr<TerrainLayerReader::Layer> layer;
    };

    TerrainLayerReader::TerrainLayerReader() = default;
    TerrainLayerReader::~TerrainLayerReader() = default;

    bool TerrainLayerReader::handles(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options)
    {
        std::string kind;
        if (options) options->getValue(loaderKeys::layerKind, kind);
        if (kind == "terrain") return true;
        if (!kind.empty() && kind != "auto") return false;
        return ElevationSource::looksLikeElevation(filename.string());
    }

    bool TerrainLayerReader::isVirtualTile(const vsg::Path& filename)
    {
        const std::string name = filename.string();
        return name.compare(0, std::string(kVirtualPrefix).size(), kVirtualPrefix) == 0;
    }

    bool TerrainLayerReader::getFeatures(Features& features) const
    {
        for (const char* ext : {".dt0", ".dt1", ".dt2", ".dt3", ".hgt"}) features.extensionFeatureMap[ext] = vsg::ReaderWriter::READ_FILENAME;
        return true;
    }

    std::size_t TerrainLayerReader::openLayerCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::size_t alive = 0;
        for (const auto& [id, weak] : _layers)
            if (!weak.expired()) ++alive;
        return alive;
    }

    std::shared_ptr<TerrainLayerReader::Layer> TerrainLayerReader::openLayer(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options, std::string& error) const
    {
        auto layer = std::make_shared<Layer>();
        std::string sourceCrs, targetCrs;
        double detailBias = 1.0;
        int cap = -1, interpolation = static_cast<int>(HeightInterpolation::Bicubic);
        if (options)
        {
            options->getValue(loaderKeys::sourceCrs, sourceCrs);
            options->getValue(loaderKeys::targetCrs, targetCrs);
            options->getValue(loaderKeys::detailBias, detailBias);
            options->getValue(loaderKeys::detailLevelCap, cap);
            options->getValue(loaderKeys::terrainExaggeration, layer->style.exaggeration);
            options->getValue(loaderKeys::terrainInterpolation, interpolation);
            options->getValue(loaderKeys::terrainOversample, layer->style.oversampleLevels);
            options->getValue(loaderKeys::terrainCells, layer->style.cellsPerTile);
            options->getValue(loaderKeys::terrainTexture, layer->style.texturePixels);
            options->getValue(loaderKeys::terrainHillshade, layer->style.hillshade);
            layer->registry = const_cast<vsg::Options*>(options.get())->getRefObject<ElevationRegistry>(loaderKeys::elevationRegistry);
        }
        layer->style.interpolation = static_cast<HeightInterpolation>(std::clamp(interpolation, 0, 2));
        layer->style.cellsPerTile = std::clamp(layer->style.cellsPerTile, 2, 128);
        layer->style.texturePixels = std::clamp(layer->style.texturePixels, 8, 512);
        layer->style.oversampleLevels = std::clamp(layer->style.oversampleLevels, 0, 12);
        if (!(layer->style.exaggeration > 0.0)) layer->style.exaggeration = 1.0;

        layer->source = ElevationSource::open(filename.string(), sourceCrs, error);
        if (!layer->source) return nullptr;
        layer->projection = targetCrs.empty() ? nullptr : createProjection(targetCrs);
        if (!layer->projection) layer->projection = std::make_shared<MercatorProjection>();

        // the footprint on the grid: longitudes as they are, latitudes clamped to the Mercator square
        const GeoExtent& extent = layer->source->extent();
        const vsg::dvec3 sw = layer->mercator.geoToWorld(GeoPoint{extent.minLongitude, std::max(extent.minLatitude, -MercatorProjection::kMaxLatitude), 0.0});
        const vsg::dvec3 ne = layer->mercator.geoToWorld(GeoPoint{extent.maxLongitude, std::min(extent.maxLatitude, MercatorProjection::kMaxLatitude), 0.0});
        layer->bounds = intersect(vsg::dbox(vsg::dvec3(sw.x, sw.y, 0.0), vsg::dvec3(ne.x, ne.y, 0.0)), layer->grid.extent);
        if (!layer->bounds.valid())
        {
            error = filename.string() + " lies entirely outside the Mercator world";
            return nullptr;
        }

        // the post spacing in the grid's metres: Mercator stretches north-south by 1 / cos(latitude)
        const double centreLatitude = 0.5 * (extent.minLatitude + extent.maxLatitude);
        const double cosLat = std::max(0.01, std::cos(centreLatitude * kPi / 180.0));
        const double metresPerDegree = 111319.49079327358;
        const double postMetres = std::min(layer->source->postSpacingLon() * metresPerDegree, layer->source->postSpacingLat() * metresPerDegree / cosLat);
        layer->nativeLevel = nativeLevelFor(postMetres, layer->style.cellsPerTile);
        layer->maxLevel = std::min(24, layer->nativeLevel + layer->style.oversampleLevels);
        if (cap >= 0) layer->maxLevel = std::min(layer->maxLevel, cap);
        layer->handOverRatio = tileHandOverRatio(kTilePixels, detailBias);
        layer->state = std::make_unique<TileStateFactory>(options);
        layer->childOptions = options ? vsg::Options::create(*options) : vsg::Options::create();
        layer->childOptions->setValue(loaderKeys::sourceCrs, layer->source->sourceCrs());
        if (layer->registry) layer->registryId = layer->registry->add(layer->source, layer->style.exaggeration);

        std::lock_guard<std::mutex> lock(_mutex);
        for (auto it = _layers.begin(); it != _layers.end();) it = it->second.expired() ? _layers.erase(it) : std::next(it);
        layer->id = std::to_string(nextLayerId++);
        _layers[layer->id] = layer;
        return layer;
    }

    vsg::ref_ptr<vsg::Object> TerrainLayerReader::readRoot(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
    {
        std::string error;
        std::shared_ptr<Layer> layer = openLayer(filename, options, error);
        if (!layer) return vsg::ReadError::create(error);

        std::vector<TileIndex> roots = layer->grid.rootTilesFor(layer->bounds, layer->maxLevel);
        auto root = layer->state->createRoot();
        for (const TileIndex& tile : roots)
        {
            if (auto node = layer->makeTile(tile)) root->addChild(node);
        }
        if (root->children.empty()) return vsg::ReadError::create("no terrain tile of " + filename.string() + " holds any data");
        root->setValue(loaderKeys::nodeSourceCrs, layer->source->sourceCrs());
        root->setValue(loaderKeys::nodeTargetCrs, layer->projection->crs());
        root->setValue(loaderKeys::nodeLayerKind, std::string("terrain"));
        root->setValue(loaderKeys::nodeInfo, layer->source->description() + ", native level " + std::to_string(layer->nativeLevel) + ", pages to " + std::to_string(layer->maxLevel) +
                                                 ", " + heightInterpolationName(layer->style.interpolation) + " x" + std::to_string(layer->style.exaggeration).substr(0, 4));
        root->setValue(loaderKeys::nodeNativeLevel, layer->nativeLevel);
        root->setValue(loaderKeys::nodeMaxLevel, layer->maxLevel);
        root->setValue(loaderKeys::nodeTerrainRegistryId, layer->registryId);
        root->setObject(kLayerHolder, TerrainLayerHolder::create(layer));
        return root;
    }

    vsg::ref_ptr<vsg::Object> TerrainLayerReader::readChildren(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options>) const
    {
        std::string id;
        TileIndex parent;
        if (!parseVirtualName(filename.string(), id, parent)) return vsg::ReadError::create("malformed terrain tile request " + filename.string());
        std::shared_ptr<Layer> layer;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _layers.find(id);
            if (it != _layers.end()) layer = it->second.lock();
        }
        if (!layer) return vsg::ReadError::create("terrain layer " + id + " is no longer open");
        auto group = vsg::Group::create();
        for (const TileIndex& child : TileGrid::children(parent))
        {
            if (auto node = layer->makeTile(child)) group->addChild(node);
        }
        return group;
    }

    vsg::ref_ptr<vsg::Object> TerrainLayerReader::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
    {
        if (isVirtualTile(filename)) return readChildren(filename, options);
        if (handles(filename, options)) return readRoot(filename, options);
        return {};
    }
} // namespace ivevsg
