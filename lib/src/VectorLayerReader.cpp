#include <ivevsg/LoaderKeys.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/TileGeometry.h>
#include <ivevsg/TileGrid.h>
#include <ivevsg/VectorLayerReader.h>

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
        constexpr const char* kVirtualPrefix = "ivevsg-vector://";
        constexpr const char* kLayerHolder = "ivevsg.vector_layer";
        constexpr int kTilePixels = 256;
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

        /// The graphics state of one vector layer: a flat-shaded line pipeline with a dynamic line width,
        /// its fixed descriptors bound once at the root, and one colour shared by every tile.
        class VectorStateFactory
        {
        public:
            VectorStateFactory(const vsg::ref_ptr<const vsg::Options>& options, const vsg::vec4& colour, float lineWidth)
            {
                auto shaderSet = vsg::createFlatShadedShaderSet(options);
                _pipeline = vsg::GraphicsPipelineConfigurator::create(shaderSet);
                if (options) _pipeline->assignInheritedState(options->inheritedState);
                _pipeline->enableDescriptor("material");
                const bool haveTexCoordIndices = static_cast<bool>(shaderSet->getDescriptorBinding("texCoordIndices"));
                if (haveTexCoordIndices) _pipeline->enableDescriptor("texCoordIndices");
                _pipeline->enableArray("vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, 12, VK_FORMAT_R32G32B32_SFLOAT);
                _pipeline->enableArray("vsg_Normal", VK_VERTEX_INPUT_RATE_INSTANCE, 12, VK_FORMAT_R32G32B32_SFLOAT);
                _pipeline->enableArray("vsg_TexCoord0", VK_VERTEX_INPUT_RATE_INSTANCE, 8, VK_FORMAT_R32G32_SFLOAT);
                _pipeline->enableArray("vsg_Color", VK_VERTEX_INPUT_RATE_INSTANCE, 16, VK_FORMAT_R32G32B32A32_SFLOAT);
                struct LineState : public vsg::Visitor
                {
                    void apply(vsg::Object& object) override { object.traverse(*this); }
                    void apply(vsg::ColorBlendState& state) override { state.configureAttachments(true); }
                    void apply(vsg::InputAssemblyState& state) override { state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; }
                    void apply(vsg::RasterizationState& state) override
                    {
                        state.cullMode = VK_CULL_MODE_NONE;
                        state.lineWidth = 1.0f;
                    }
                } lineState;
                _pipeline->accept(lineState);
                _pipeline->pipelineStates.push_back(vsg::DynamicState::create(VK_DYNAMIC_STATE_LINE_WIDTH));
                _pipeline->init();

                auto addFixed = [this](uint32_t set, const vsg::ref_ptr<vsg::Descriptor>& descriptor) {
                    for (auto& [s, descriptors] : _fixedDescriptors)
                        if (s == set)
                        {
                            descriptors.push_back(descriptor);
                            return;
                        }
                    _fixedDescriptors.emplace_back(set, vsg::Descriptors{descriptor});
                };
                if (const vsg::DescriptorBinding& material = shaderSet->getDescriptorBinding("material"); material)
                {
                    vsg::ref_ptr<vsg::Data> data = material.data;
                    if (!data) data = vsg::PhongMaterialValue::create();
                    addFixed(material.set, vsg::DescriptorBuffer::create(data, material.binding, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
                }
                if (haveTexCoordIndices)
                {
                    const vsg::DescriptorBinding& indices = shaderSet->getDescriptorBinding("texCoordIndices");
                    vsg::ref_ptr<vsg::Data> data = indices.data;
                    if (!data) data = vsg::TexCoordIndicesValue::create();
                    addFixed(indices.set, vsg::DescriptorBuffer::create(data, indices.binding, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
                }
                _colours = vsg::vec4Array::create({colour});
                _colours->properties.dataVariance = vsg::DYNAMIC_DATA; // the panel recolours the layer in place
                _normals = vsg::vec3Array::create({vsg::vec3(0.0f, 0.0f, 1.0f)});
                _texcoords = vsg::vec2Array::create({vsg::vec2(0.0f, 0.0f)});
                _lineWidth = vsg::SetLineWidth::create(lineWidth);
            }

            vsg::ref_ptr<vsg::StateGroup> createRoot() const
            {
                auto root = vsg::StateGroup::create();
                _pipeline->copyTo(root);
                for (const auto& [set, descriptors] : _fixedDescriptors)
                    root->add(vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline->layout, set, descriptors));
                root->addChild(_lineWidth); // recorded before the tiles: they inherit the width
                return root;
            }

            /// A line list (pairs of vertices around `origin`) culled by its sphere.
            vsg::ref_ptr<vsg::Node> createTile(const vsg::ref_ptr<vsg::vec3Array>& vertices, const vsg::dvec3& origin) const
            {
                auto draw = vsg::VertexDraw::create();
                draw->assignArrays(vsg::DataList{vertices, _normals, _texcoords, _colours});
                draw->vertexCount = static_cast<uint32_t>(vertices->size());
                draw->instanceCount = 1;
                auto transform = vsg::MatrixTransform::create(vsg::translate(origin));
                transform->addChild(draw);
                return transform;
            }

            vsg::ref_ptr<vsg::vec4Array> colours() const { return _colours; }
            vsg::ref_ptr<vsg::SetLineWidth> lineWidth() const { return _lineWidth; }

        private:
            vsg::ref_ptr<vsg::GraphicsPipelineConfigurator> _pipeline;
            std::vector<std::pair<uint32_t, vsg::Descriptors>> _fixedDescriptors;
            vsg::ref_ptr<vsg::vec4Array> _colours;
            vsg::ref_ptr<vsg::vec3Array> _normals;
            vsg::ref_ptr<vsg::vec2Array> _texcoords;
            vsg::ref_ptr<vsg::SetLineWidth> _lineWidth;
        };
    } // namespace

    struct VectorLayerReader::Layer
    {
        std::string id;
        VectorData data;
        std::shared_ptr<const Projection> projection;
        MercatorProjection mercator;
        TileGrid grid = TileGrid::webMercatorQuad(kTilePixels);
        VectorStyle style;
        int maxLevel = 16;
        double handOverRatio = 0.0;
        vsg::dbox bounds; // the data's footprint in grid units
        bool drape = true;
        double drapeMinZoom = 8.0;
        vsg::ref_ptr<ElevationRegistry> registry;
        std::unique_ptr<VectorStateFactory> state;
        vsg::ref_ptr<vsg::Options> childOptions;

        GeoPoint geo(double x, double y) const { return mercator.worldToGeo(vsg::dvec3(x, y, 0.0)); }
        bool subdividable(const TileIndex& tile) const { return tile.level < maxLevel; }

        vsg::ref_ptr<vsg::Node> makeTile(const TileIndex& tile) const
        {
            const vsg::dbox box = grid.tileExtent(tile);
            if (!(box.min.x < bounds.max.x && box.max.x > bounds.min.x && box.min.y < bounds.max.y && box.max.y > bounds.min.y)) return {};
            const GeoPoint sw = geo(box.min.x, box.min.y), ne = geo(box.max.x, box.max.y);
            GeoExtent footprint{sw.longitude, sw.latitude, ne.longitude, ne.latitude};
            const double widthDegrees = footprint.maxLongitude - footprint.minLongitude;
            const GeoExtent clipBox = expanded(footprint, widthDegrees * 0.01); // a hair of overlap hides seams between tiles
            const double tolerance = widthDegrees / (kTilePixels * 2.0);           // half a pixel when the tile hands over
            const double maxSegment = widthDegrees / 32.0;                          // edges follow the globe and the terrain
            const double tileWidthMetres = (box.max.x - box.min.x) * std::max(0.01, std::cos(0.5 * (sw.latitude + ne.latitude) * kPi / 180.0));
            const bool drapeHere = drape && tile.level >= drapeMinZoom && registry && registry->hasTerrain();
            const double clearance = std::max(2.0, 0.001 * tileWidthMetres);
            const double markerHalf = 0.5 * style.pointSize / kTilePixels * (box.max.x - box.min.x); // in grid metres

            auto heightAt = [&](const GeoPoint& g) {
                if (!drapeHere) return 0.0;
                std::optional<double> h = registry->heightAt(g.longitude, g.latitude);
                return h ? *h + clearance : 0.0;
            };
            std::vector<vsg::dvec3> world;
            auto addSegment = [&](const GeoPoint& a, const GeoPoint& b) {
                world.push_back(projection->geoToWorld(GeoPoint{a.longitude, a.latitude, heightAt(a)}));
                world.push_back(projection->geoToWorld(GeoPoint{b.longitude, b.latitude, heightAt(b)}));
            };
            for (const Feature& feature : data.features)
            {
                if (!extentsOverlap(feature.extent, clipBox)) continue;
                if (feature.type == FeatureType::Point)
                {
                    for (const auto& part : feature.parts)
                    {
                        if (part.empty() || !extentContains(footprint, part.front().longitude, part.front().latitude)) continue;
                        const vsg::dvec3 c = mercator.geoToWorld(part.front());
                        addSegment(geo(c.x - markerHalf, c.y), geo(c.x + markerHalf, c.y));
                        addSegment(geo(c.x, c.y - markerHalf), geo(c.x, c.y + markerHalf));
                    }
                    continue;
                }
                for (const auto& part : feature.parts)
                {
                    for (const std::vector<GeoPoint>& run : clipLine(part, clipBox))
                    {
                        const std::vector<GeoPoint> line = densifyLine(simplifyLine(run, tolerance), maxSegment);
                        for (std::size_t i = 1; i < line.size(); ++i) addSegment(line[i - 1], line[i]);
                    }
                }
            }
            if (world.empty()) return {};
            vsg::dbox worldBox;
            for (const vsg::dvec3& w : world) worldBox.add(w);
            const vsg::dvec3 origin = (worldBox.min + worldBox.max) * 0.5;
            double radius = 0.0;
            auto vertices = vsg::vec3Array::create(static_cast<uint32_t>(world.size()));
            for (std::size_t i = 0; i < world.size(); ++i)
            {
                const vsg::dvec3 local = world[i] - origin;
                radius = std::max(radius, vsg::length(local));
                vertices->set(i, vsg::vec3(static_cast<float>(local.x), static_cast<float>(local.y), static_cast<float>(local.z)));
            }
            // the sphere spans the whole tile footprint even when its lines sit in a corner, so hand-over
            // happens at the same screen size for every tile of a level
            vsg::dsphere bound(origin, radius * 1.001 + 1.0);
            {
                vsg::dbox tileBox;
                for (double x : {box.min.x, box.max.x})
                    for (double y : {box.min.y, box.max.y}) tileBox.add(projection->geoToWorld(geo(x, y)));
                const vsg::dvec3 tileCentre = (tileBox.min + tileBox.max) * 0.5;
                const double tileRadius = vsg::length(tileBox.max - tileCentre);
                if (tileRadius > bound.radius) bound = vsg::dsphere(tileCentre, tileRadius * 1.001 + 1.0);
            }
            return makePagedTile(state->createTile(vertices, origin), bound, subdividable(tile) ? virtualName(id, tile) : std::string(), handOverRatio, childOptions);
        }
    };

    class VectorLayerHolder : public vsg::Inherit<vsg::Object, VectorLayerHolder>
    {
    public:
        explicit VectorLayerHolder(std::shared_ptr<VectorLayerReader::Layer> in_layer) :
            layer(std::move(in_layer)) {}
        std::shared_ptr<VectorLayerReader::Layer> layer;
    };

    VectorLayerReader::VectorLayerReader() = default;
    VectorLayerReader::~VectorLayerReader() = default;

    bool VectorLayerReader::handles(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options)
    {
        std::string kind;
        if (options) options->getValue(loaderKeys::layerKind, kind);
        if (kind == "vector") return true;
        if (!kind.empty() && kind != "auto") return false;
        return looksLikeVector(filename.string());
    }

    bool VectorLayerReader::isVirtualTile(const vsg::Path& filename)
    {
        const std::string name = filename.string();
        return name.compare(0, std::string(kVirtualPrefix).size(), kVirtualPrefix) == 0;
    }

    bool VectorLayerReader::getFeatures(Features& features) const
    {
        for (const char* ext : {".shp", ".shz", ".geojson", ".gpkg", ".kml", ".kmz", ".gml", ".fgb", ".tab", ".mif", ".gpx"}) features.extensionFeatureMap[ext] = vsg::ReaderWriter::READ_FILENAME;
        return true;
    }

    std::size_t VectorLayerReader::openLayerCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::size_t alive = 0;
        for (const auto& [id, weak] : _layers)
            if (!weak.expired()) ++alive;
        return alive;
    }

    std::shared_ptr<VectorLayerReader::Layer> VectorLayerReader::openLayer(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options, std::string& error) const
    {
        auto layer = std::make_shared<Layer>();
        std::string sourceCrs, targetCrs;
        double detailBias = 1.0;
        int cap = -1;
        bool wideLines = false;
        if (options)
        {
            options->getValue(loaderKeys::sourceCrs, sourceCrs);
            options->getValue(loaderKeys::targetCrs, targetCrs);
            options->getValue(loaderKeys::detailBias, detailBias);
            options->getValue(loaderKeys::detailLevelCap, cap);
            options->getValue(loaderKeys::vectorColour, layer->style.colour);
            options->getValue(loaderKeys::vectorLineWidth, layer->style.lineWidth);
            options->getValue(loaderKeys::vectorPointSize, layer->style.pointSize);
            options->getValue(loaderKeys::vectorMaxLevel, layer->maxLevel);
            options->getValue(loaderKeys::drape, layer->drape);
            options->getValue(loaderKeys::drapeMinZoom, layer->drapeMinZoom);
            options->getValue(loaderKeys::wideLines, wideLines);
            layer->registry = const_cast<vsg::Options*>(options.get())->getRefObject<ElevationRegistry>(loaderKeys::elevationRegistry);
        }
        if (!wideLines) layer->style.lineWidth = 1.0f; // vkCmdSetLineWidth != 1 needs the wideLines feature
        layer->style.lineWidth = std::clamp(layer->style.lineWidth, 1.0f, 32.0f);
        layer->style.pointSize = std::clamp(layer->style.pointSize, 1.0f, 64.0f);
        if (!readVectorData(filename.string(), sourceCrs, layer->data, error)) return nullptr;
        layer->projection = targetCrs.empty() ? nullptr : createProjection(targetCrs);
        if (!layer->projection) layer->projection = std::make_shared<MercatorProjection>();
        const GeoExtent& e = layer->data.extent;
        const vsg::dvec3 sw = layer->mercator.geoToWorld(GeoPoint{std::max(-180.0, e.minLongitude), std::max(e.minLatitude, -MercatorProjection::kMaxLatitude), 0.0});
        const vsg::dvec3 ne = layer->mercator.geoToWorld(GeoPoint{std::min(180.0, e.maxLongitude), std::min(e.maxLatitude, MercatorProjection::kMaxLatitude), 0.0});
        layer->bounds = vsg::dbox(vsg::dvec3(sw.x, sw.y, 0.0), vsg::dvec3(std::max(ne.x, sw.x + 1.0), std::max(ne.y, sw.y + 1.0), 0.0));
        layer->maxLevel = std::clamp(layer->maxLevel, 0, 24);
        if (cap >= 0) layer->maxLevel = std::min(layer->maxLevel, cap);
        layer->handOverRatio = tileHandOverRatio(kTilePixels, detailBias);
        layer->state = std::make_unique<VectorStateFactory>(options, layer->style.colour, layer->style.lineWidth);
        layer->childOptions = options ? vsg::Options::create(*options) : vsg::Options::create();

        std::lock_guard<std::mutex> lock(_mutex);
        for (auto it = _layers.begin(); it != _layers.end();) it = it->second.expired() ? _layers.erase(it) : std::next(it);
        layer->id = std::to_string(nextLayerId++);
        _layers[layer->id] = layer;
        return layer;
    }

    vsg::ref_ptr<vsg::Object> VectorLayerReader::readRoot(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
    {
        std::string error;
        std::shared_ptr<Layer> layer = openLayer(filename, options, error);
        if (!layer) return vsg::ReadError::create(error);

        auto root = layer->state->createRoot();
        for (const TileIndex& tile : layer->grid.rootTilesFor(layer->bounds, layer->maxLevel))
        {
            if (auto node = layer->makeTile(tile)) root->addChild(node);
        }
        if (root->children.size() <= 1) return vsg::ReadError::create("no feature of " + filename.string() + " falls inside the Mercator world");
        root->setValue(loaderKeys::nodeSourceCrs, layer->data.sourceCrs);
        root->setValue(loaderKeys::nodeTargetCrs, layer->projection->crs());
        root->setValue(loaderKeys::nodeLayerKind, std::string("vector"));
        root->setValue(loaderKeys::nodeInfo, layer->data.description + ", pages to level " + std::to_string(layer->maxLevel));
        root->setValue(loaderKeys::nodeMaxLevel, layer->maxLevel);
        if (std::optional<double> scale = scaleDenominatorFromName(filename.string())) root->setValue(loaderKeys::nodeDeclaredScale, *scale);
        root->setObject(loaderKeys::nodeVectorColour, layer->state->colours());
        root->setObject(loaderKeys::nodeVectorLineWidth, layer->state->lineWidth());
        root->setObject(kLayerHolder, VectorLayerHolder::create(layer));
        return root;
    }

    vsg::ref_ptr<vsg::Object> VectorLayerReader::readChildren(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options>) const
    {
        std::string id;
        TileIndex parent;
        if (!parseVirtualName(filename.string(), id, parent)) return vsg::ReadError::create("malformed vector tile request " + filename.string());
        std::shared_ptr<Layer> layer;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _layers.find(id);
            if (it != _layers.end()) layer = it->second.lock();
        }
        if (!layer) return vsg::ReadError::create("vector layer " + id + " is no longer open");
        auto group = vsg::Group::create();
        for (const TileIndex& child : TileGrid::children(parent))
        {
            if (auto node = layer->makeTile(child)) group->addChild(node);
        }
        return group;
    }

    vsg::ref_ptr<vsg::Object> VectorLayerReader::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
    {
        if (isVirtualTile(filename)) return readChildren(filename, options);
        if (handles(filename, options)) return readRoot(filename, options);
        return {};
    }
} // namespace ivevsg
