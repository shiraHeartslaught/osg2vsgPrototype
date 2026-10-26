#include <ivevsg/CrsTransform.h>
#include <ivevsg/TileGeometry.h>
#include <ivevsg/TileSetBuilder.h>

#include <vsg/all.h>

#include <algorithm>
#include <cmath>

namespace ivevsg
{
    int tileCellsFor(const GeoPoint& southWest, const GeoPoint& northEast, int minimum)
    {
        const double span = std::max(std::abs(northEast.longitude - southWest.longitude), std::abs(northEast.latitude - southWest.latitude));
        return std::clamp(static_cast<int>(std::ceil(span / 4.0)), std::max(1, minimum), 64);
    }

    double tileHandOverRatio(int tileSizePixels, double detailBias)
    {
        return subdivisionPixelThreshold(tileSizePixels, detailBias) / tileReferenceScreenHeight;
    }

    TileMesh buildTileMesh(const Projection& projection, const vsg::dbox& extent, int cells, const CrsTransform* toGeographic)
    {
        const int n = std::clamp(cells, 1, 254); // (n + 1)^2 vertices must fit 16-bit indices
        const std::size_t count = static_cast<std::size_t>(n + 1) * (n + 1);
        auto toWorld = [&](double x, double y) {
            if (!toGeographic) return vsg::dvec3(x, y, 0.0);
            GeoPoint geo{x, y, 0.0};
            if (!toGeographic->toGeographic(x, y, 0.0, geo)) geo = GeoPoint{x, y, 0.0};
            return projection.geoToWorld(geo);
        };

        std::vector<vsg::dvec3> world(count);
        vsg::dbox box;
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
            {
                const double x = extent.min.x + (extent.max.x - extent.min.x) * i / n;
                const double y = extent.min.y + (extent.max.y - extent.min.y) * j / n; // south to north
                vsg::dvec3& w = world[static_cast<std::size_t>(j) * (n + 1) + i];
                w = toWorld(x, y);
                box.add(w);
            }

        TileMesh mesh;
        mesh.origin = (box.min + box.max) * 0.5;
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
                const vsg::dvec3 up = projection.upVector(world[k]);
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

    TileStateFactory::TileStateFactory(vsg::ref_ptr<const vsg::Options> options)
    {
        auto shaderSet = vsg::createFlatShadedShaderSet(options);
        _pipeline = vsg::GraphicsPipelineConfigurator::create(shaderSet);
        if (options) _pipeline->assignInheritedState(options->inheritedState);
        _pipeline->enableTexture("diffuseMap");
        _pipeline->enableDescriptor("material");
        const bool haveTexCoordIndices = static_cast<bool>(shaderSet->getDescriptorBinding("texCoordIndices"));
        if (haveTexCoordIndices) _pipeline->enableDescriptor("texCoordIndices");
        _pipeline->enableArray("vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, 12, VK_FORMAT_R32G32B32_SFLOAT);
        _pipeline->enableArray("vsg_Normal", VK_VERTEX_INPUT_RATE_VERTEX, 12, VK_FORMAT_R32G32B32_SFLOAT);
        _pipeline->enableArray("vsg_TexCoord0", VK_VERTEX_INPUT_RATE_VERTEX, 8, VK_FORMAT_R32G32_SFLOAT);
        _pipeline->enableArray("vsg_Color", VK_VERTEX_INPUT_RATE_INSTANCE, 16, VK_FORMAT_R32G32B32A32_SFLOAT);
        struct EnableBlending : public vsg::Visitor
        {
            void apply(vsg::Object& object) override { object.traverse(*this); }
            void apply(vsg::ColorBlendState& state) override { state.configureAttachments(true); }
        } blending;
        _pipeline->accept(blending);
        _pipeline->init();

        _sampler = vsg::Sampler::create();
        _sampler->addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        _sampler->addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        _sampler->addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        _sampler->anisotropyEnable = VK_TRUE;
        _sampler->maxAnisotropy = 8.0f;
        _sampler->maxLod = 16.0f; // a mip chain the image carries, or one made on the GPU

        const vsg::DescriptorBinding& diffuse = shaderSet->getDescriptorBinding("diffuseMap");
        _diffuseSet = diffuse.set;
        _diffuseBinding = diffuse.binding;
        auto addFixed = [this](uint32_t set, vsg::ref_ptr<vsg::Descriptor> descriptor) {
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
        bool haveDiffuseSet = false;
        for (const auto& [s, descriptors] : _fixedDescriptors) haveDiffuseSet = haveDiffuseSet || s == _diffuseSet;
        if (!haveDiffuseSet) _fixedDescriptors.emplace_back(_diffuseSet, vsg::Descriptors{});
        _colours = vsg::vec4Array::create({vsg::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
    }

    vsg::ref_ptr<vsg::StateGroup> TileStateFactory::createRoot() const
    {
        auto root = vsg::StateGroup::create();
        _pipeline->copyTo(root);
        return root;
    }

    vsg::ref_ptr<vsg::Node> TileStateFactory::createTile(const TileMesh& mesh, vsg::ref_ptr<vsg::Data> image) const
    {
        auto draw = vsg::VertexIndexDraw::create();
        draw->assignArrays(vsg::DataList{mesh.vertices, mesh.normals, mesh.texcoords, _colours});
        draw->assignIndices(mesh.indices);
        draw->indexCount = static_cast<uint32_t>(mesh.indices->size());
        draw->instanceCount = 1;

        auto transform = vsg::MatrixTransform::create(vsg::translate(mesh.origin));
        transform->addChild(draw);

        auto stateGroup = vsg::StateGroup::create();
        for (const auto& [set, descriptors] : _fixedDescriptors)
        {
            vsg::Descriptors all = descriptors;
            if (set == _diffuseSet) all.insert(all.begin(), vsg::DescriptorImage::create(_sampler, image, _diffuseBinding, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
            stateGroup->add(vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline->layout, set, all));
        }
        stateGroup->addChild(transform);
        return stateGroup;
    }

    vsg::ref_ptr<vsg::Node> makePagedTile(vsg::ref_ptr<vsg::Node> tile, const vsg::dsphere& bound, const std::string& childFile, double handOverRatio, vsg::ref_ptr<vsg::Options> childOptions)
    {
        if (childFile.empty()) return vsg::CullNode::create(bound, tile);
        auto plod = vsg::PagedLOD::create();
        plod->bound = bound;
        plod->filename = vsg::Path(childFile);
        plod->options = childOptions;
        plod->children[0] = vsg::PagedLOD::Child{handOverRatio, {}};
        plod->children[1] = vsg::PagedLOD::Child{0.0, tile};
        return plod;
    }
} // namespace ivevsg
