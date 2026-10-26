#include <ivevsg/CrsTransform.h>
#include <ivevsg/LoaderKeys.h>
#include <ivevsg/VsgReproject.h>

#include <vsg/all.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <set>
#include <vector>

namespace ivevsg
{
    namespace
    {
        /// Which array of a draw holds positions and which normals, per the pipeline that draws it.
        struct ArrayRoles
        {
            int vertices = 0; // binding index, -1 = none
            int normals = -1;
        };

        ArrayRoles rolesOf(const vsg::BindGraphicsPipeline* bind)
        {
            ArrayRoles roles;
            if (!bind || !bind->pipeline) return roles;
            for (const auto& state : bind->pipeline->pipelineStates)
            {
                auto input = state.cast<vsg::VertexInputState>();
                if (!input) continue;
                roles.vertices = -1;
                for (const auto& attribute : input->vertexAttributeDescriptions)
                {
                    if (attribute.location == 0) roles.vertices = static_cast<int>(attribute.binding);
                    if (attribute.location == 1 && attribute.format == VK_FORMAT_R32G32B32_SFLOAT) roles.normals = static_cast<int>(attribute.binding);
                }
            }
            return roles;
        }

        class Reprojector : public vsg::Visitor
        {
        public:
            Reprojector(const CrsTransform& crs, const Projection& target, std::optional<double> referenceLongitude, vsg::ref_ptr<const vsg::Options> childOptions) :
                _crs(crs),
                _target(target),
                _referenceLongitude(referenceLongitude),
                _childOptions(childOptions)
            {
                _pipelines.push_back(nullptr);
            }

            ReprojectReport report;

            /// The root frame is the world itself: geometry that sits under no MatrixTransform keeps
            /// absolute coordinates, exactly as the OSG reprojector leaves it, so children paged in
            /// below (reprojected into absolute coordinates by their own reader) line up with it.
            void run(vsg::Node& root)
            {
                _frames.push_back(Frame{vsg::dmat4(), vsg::dvec3(0.0, 0.0, 0.0)});
                root.accept(*this);
                _frames.pop_back();
            }

            void apply(vsg::Node& node) override { node.traverse(*this); }

            void apply(vsg::StateGroup& group) override
            {
                vsg::BindGraphicsPipeline* pipeline = _pipelines.back();
                for (const auto& command : group.stateCommands)
                    if (auto bind = command.cast<vsg::BindGraphicsPipeline>()) pipeline = bind.get();
                _pipelines.push_back(pipeline);
                group.traverse(*this);
                _pipelines.pop_back();
            }

            void apply(vsg::MatrixTransform& transform) override
            {
                const Frame& parent = _frames.back();
                Frame frame;
                frame.oldAccumulated = parent.oldAccumulated * transform.matrix;
                frame.newOrigin = toTarget(frame.oldAccumulated * vsg::dvec3(0.0, 0.0, 0.0));
                transform.matrix = vsg::translate(frame.newOrigin - parent.newOrigin);
                ++report.transforms;
                _frames.push_back(frame);
                transform.traverse(*this);
                _frames.pop_back();
            }

            void apply(vsg::VertexIndexDraw& draw) override { transformArrays(draw.arrays); }
            void apply(vsg::VertexDraw& draw) override { transformArrays(draw.arrays); }
            void apply(vsg::Geometry& geometry) override { transformArrays(geometry.arrays); }

            void apply(vsg::LOD& lod) override
            {
                moveSphere(lod.bound);
                lod.traverse(*this);
                vsg::dsphere children = boundOfChildren(lod);
                if (children.valid()) enclose(lod.bound, children);
                ++report.lods;
            }

            void apply(vsg::PagedLOD& plod) override
            {
                moveSphere(plod.bound);
                if (_childOptions) plod.options = vsg::Options::create(*_childOptions);
                plod.traverse(*this);
                vsg::dsphere children = boundOfChildren(plod);
                if (children.valid()) enclose(plod.bound, children);
                ++report.lods;
            }

            void apply(vsg::CullNode& cull) override
            {
                moveSphere(cull.bound);
                cull.traverse(*this);
                if (cull.child)
                {
                    vsg::ComputeBounds bounds;
                    cull.child->accept(bounds);
                    if (bounds.bounds.valid()) enclose(cull.bound, sphereOf(bounds.bounds));
                }
            }

            void apply(vsg::CullGroup& cull) override
            {
                moveSphere(cull.bound);
                cull.traverse(*this);
                vsg::ComputeBounds bounds;
                for (auto& child : cull.children) child->accept(bounds);
                if (bounds.bounds.valid()) enclose(cull.bound, sphereOf(bounds.bounds));
            }

        private:
            struct Frame
            {
                vsg::dmat4 oldAccumulated; // local -> source world
                vsg::dvec3 newOrigin;      // target world position of this frame's origin
            };

            vsg::dvec3 toTarget(const vsg::dvec3& sourceWorld)
            {
                GeoPoint geo;
                if (!_crs.toGeographic(sourceWorld.x, sourceWorld.y, sourceWorld.z, geo))
                {
                    if (report.error.empty()) report.error = "coordinate transformation failed";
                    return sourceWorld;
                }
                if (_referenceLongitude) geo.longitude = *_referenceLongitude + std::remainder(geo.longitude - *_referenceLongitude, 360.0);
                return _target.geoToWorld(geo);
            }

            double localScale(const vsg::dvec3& oldWorld, const vsg::dvec3& newWorld, double probe)
            {
                double best = 0.0;
                for (const vsg::dvec3& axis : {vsg::dvec3(1.0, 0.0, 0.0), vsg::dvec3(0.0, 1.0, 0.0)})
                {
                    const vsg::dvec3 moved = toTarget(oldWorld + axis * probe);
                    best = std::max(best, vsg::length(moved - newWorld) / probe);
                }
                return best > 0.0 ? best : 1.0;
            }

            static vsg::dsphere sphereOf(const vsg::dbox& box)
            {
                return vsg::dsphere((box.min + box.max) * 0.5, vsg::length(box.max - box.min) * 0.5);
            }

            static void enclose(vsg::dsphere& sphere, const vsg::dsphere& other)
            {
                if (!sphere.valid())
                {
                    sphere = other;
                    return;
                }
                sphere.radius = std::max(sphere.radius, vsg::length(other.center - sphere.center) + other.radius);
            }

            /// Sphere around the (now reprojected) resident children of an LOD, in the LOD's frame.
            template<typename Lod>
            vsg::dsphere boundOfChildren(Lod& lod)
            {
                vsg::ComputeBounds bounds;
                for (auto& child : lod.children)
                    if (child.node) child.node->accept(bounds);
                return bounds.bounds.valid() ? sphereOf(bounds.bounds) : vsg::dsphere();
            }

            void moveSphere(vsg::dsphere& sphere)
            {
                if (!sphere.valid()) return;
                const Frame& frame = _frames.back();
                const vsg::dvec3 oldWorld = frame.oldAccumulated * sphere.center;
                const vsg::dvec3 newWorld = toTarget(oldWorld);
                const double scale = localScale(oldWorld, newWorld, std::max(sphere.radius, 1.0));
                sphere.center = newWorld - frame.newOrigin;
                sphere.radius = sphere.radius * scale;
            }

            void transformArrays(vsg::BufferInfoList& arrays)
            {
                const ArrayRoles roles = rolesOf(_pipelines.back());
                if (roles.vertices < 0 || roles.vertices >= static_cast<int>(arrays.size())) return;
                auto& info = arrays[static_cast<std::size_t>(roles.vertices)];
                if (!info || !info->data) return;
                auto vertices = info->data.cast<vsg::vec3Array>();
                if (!vertices) return;
                if (!_done.insert(vertices.get()).second) return; // shared between draws: already moved

                const Frame& frame = _frames.back();
                const std::size_t count = vertices->size();
                std::vector<vsg::dvec3> newWorld(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    const vsg::dvec3 local(vertices->at(i));
                    newWorld[i] = toTarget(frame.oldAccumulated * local);
                }
                for (std::size_t i = 0; i < count; ++i)
                {
                    const vsg::dvec3 local = newWorld[i] - frame.newOrigin;
                    vertices->set(i, vsg::vec3(static_cast<float>(local.x), static_cast<float>(local.y), static_cast<float>(local.z)));
                }
                vertices->dirty();

                if (roles.normals >= 0 && roles.normals < static_cast<int>(arrays.size()) && arrays[static_cast<std::size_t>(roles.normals)] && arrays[static_cast<std::size_t>(roles.normals)]->data)
                {
                    if (auto normals = arrays[static_cast<std::size_t>(roles.normals)]->data.cast<vsg::vec3Array>())
                    {
                        if (normals->size() == count)
                        {
                            for (std::size_t i = 0; i < count; ++i)
                            {
                                const vsg::dvec3 up = _target.upVector(newWorld[i]);
                                normals->set(i, vsg::vec3(static_cast<float>(up.x), static_cast<float>(up.y), static_cast<float>(up.z)));
                            }
                        }
                        else if (normals->size() == 1)
                        {
                            const vsg::dvec3 up = _target.upVector(frame.newOrigin);
                            normals->set(0, vsg::vec3(static_cast<float>(up.x), static_cast<float>(up.y), static_cast<float>(up.z)));
                        }
                        normals->dirty();
                    }
                }
                ++report.geometries;
                report.vertices += count;
            }

            const CrsTransform& _crs;
            const Projection& _target;
            std::optional<double> _referenceLongitude;
            vsg::ref_ptr<const vsg::Options> _childOptions;
            std::vector<Frame> _frames;
            std::vector<vsg::BindGraphicsPipeline*> _pipelines;
            std::set<const vsg::Data*> _done;
        };
    } // namespace

    ReprojectReport reprojectVsgScene(vsg::ref_ptr<vsg::Node>& scene, const ReprojectSettings& settings, vsg::ref_ptr<const vsg::Options> childOptions)
    {
        ReprojectReport report;
        if (!scene)
        {
            report.error = "no scene";
            return report;
        }
        if (!settings.target)
        {
            report.error = "no target projection";
            return report;
        }
        if (settings.sourceCrs.empty())
        {
            report.error = "no source CRS";
            return report;
        }
        if (CrsTransform::sameCrs(settings.sourceCrs, settings.target->crs()))
        {
            report.skipped = true;
            return report;
        }
        std::string error;
        std::unique_ptr<CrsTransform> crs = CrsTransform::create(settings.sourceCrs, &error);
        if (!crs)
        {
            report.error = error;
            return report;
        }

        // the longitude everything is kept close to on a wrapping map: that of the file's centre
        std::optional<double> referenceLongitude;
        if (settings.target->wrapWidth() > 0.0)
        {
            vsg::ComputeBounds bounds;
            scene->accept(bounds);
            if (bounds.bounds.valid())
            {
                const vsg::dvec3 centre = (bounds.bounds.min + bounds.bounds.max) * 0.5;
                GeoPoint geo;
                if (crs->toGeographic(centre.x, centre.y, centre.z, geo) && std::abs(geo.longitude) <= 180.0) referenceLongitude = geo.longitude;
            }
        }

        Reprojector visitor(*crs, *settings.target, referenceLongitude, childOptions);
        visitor.run(*scene);
        return visitor.report;
    }

    void assignPagedLodOptions(vsg::Node& scene, vsg::ref_ptr<const vsg::Options> options)
    {
        struct Assign : public vsg::Visitor
        {
            vsg::ref_ptr<const vsg::Options> options;
            void apply(vsg::Node& node) override { node.traverse(*this); }
            void apply(vsg::PagedLOD& plod) override
            {
                plod.options = vsg::Options::create(*options);
                plod.traverse(*this);
            }
        } assign;
        assign.options = options;
        if (options) scene.accept(assign);
    }

    // ---- reader ---------------------------------------------------------------------------------------

    void tagCrs(vsg::Node& scene, const std::string& crs)
    {
        scene.setValue(loaderKeys::fileCrs, crs);
    }

    std::string taggedCrs(const vsg::Node& scene)
    {
        std::string crs;
        scene.getValue(loaderKeys::fileCrs, crs);
        return crs;
    }

    VsgSceneReader::VsgSceneReader() :
        _native(vsg::VSG::create())
    {
    }

    bool VsgSceneReader::handles(const vsg::Path& filename)
    {
        const vsg::Path ext = vsg::lowerCaseFileExtension(filename);
        return ext == ".vsgb" || ext == ".vsgt" || ext == ".vsga";
    }

    bool VsgSceneReader::getFeatures(Features& features) const
    {
        features.extensionFeatureMap[".vsgb"] = vsg::ReaderWriter::READ_FILENAME;
        features.extensionFeatureMap[".vsgt"] = vsg::ReaderWriter::READ_FILENAME;
        return true;
    }

    vsg::ref_ptr<vsg::Object> VsgSceneReader::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
    {
        if (!handles(filename)) return {};
        vsg::ref_ptr<vsg::Object> object = _native->read(filename, options);
        auto node = object.cast<vsg::Node>();
        if (!node) return object; // data, or nothing: not ours to place

        std::string sourceCrs, targetCrs;
        if (options)
        {
            options->getValue(loaderKeys::sourceCrs, sourceCrs);
            options->getValue(loaderKeys::targetCrs, targetCrs);
        }
        if (sourceCrs.empty()) sourceCrs = taggedCrs(*node);
        if (sourceCrs.empty() && options) options->getValue(loaderKeys::defaultSourceCrs, sourceCrs);
        vsg::ref_ptr<vsg::Options> childOptions = options ? vsg::Options::create(*options) : vsg::Options::create();
        if (!sourceCrs.empty()) childOptions->setValue(loaderKeys::sourceCrs, sourceCrs);

        bool reprojected = false;
        if (!targetCrs.empty())
        {
            ReprojectSettings settings;
            settings.sourceCrs = sourceCrs;
            settings.target = createProjection(targetCrs);
            if (!settings.target)
            {
                vsg::warn("VsgSceneReader: unknown target projection '", targetCrs, "', leaving ", filename, " as read");
            }
            else if (sourceCrs.empty())
            {
                vsg::warn("VsgSceneReader: no source CRS known for ", filename, ", leaving it as read");
            }
            else
            {
                const ReprojectReport report = reprojectVsgScene(node, settings, childOptions);
                if (!report) vsg::warn("VsgSceneReader: reprojection of ", filename, " failed: ", report.error);
                reprojected = report && !report.skipped;
            }
        }
        // paged children are read with these options either way, so they are taken to be in the
        // same CRS as this file (they rarely say themselves) and treated the same way
        if (!reprojected) assignPagedLodOptions(*node, childOptions);
        node->setValue(loaderKeys::nodeSourceCrs, sourceCrs);
        node->setValue(loaderKeys::nodeTargetCrs, reprojected ? targetCrs : std::string());
        return node;
    }
} // namespace ivevsg
