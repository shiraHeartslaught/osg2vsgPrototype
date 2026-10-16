#include <ivevsg/CrsTransform.h>
#include <ivevsg/OsgReproject.h>

#include <osg/Geometry>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/PagedLOD>
#include <osg/PrimitiveSet>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ivevsg
{
    namespace
    {
        class Reprojector : public osg::NodeVisitor
        {
        public:
            Reprojector(const CrsTransform& crs, const Projection& target, int quadTessellation) :
                osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
                _crs(crs),
                _target(target),
                _tessellation(std::max(1, quadTessellation))
            {
                _frames.push_back(Frame{osg::Matrixd::identity(), osg::Vec3d()});
            }

            ReprojectReport report;

            void apply(osg::MatrixTransform& transform) override
            {
                const Frame& parent = _frames.back();
                Frame frame;
                frame.oldAccumulated = transform.getMatrix() * parent.oldAccumulated; // OSG: child first, row vectors
                frame.newOrigin = toTarget(osg::Vec3d() * frame.oldAccumulated);
                transform.setMatrix(osg::Matrixd::translate(frame.newOrigin - parent.newOrigin));
                ++report.transforms;
                _frames.push_back(frame);
                traverse(transform);
                _frames.pop_back();
            }

            void apply(osg::Geometry& geometry) override
            {
                if (_tessellation > 1) tessellateQuad(geometry);
                transformGeometry(geometry);
                traverse(geometry);
            }

            void apply(osg::LOD& lod) override
            {
                // the bounding sphere drives LOD selection (and paging): move and rescale it
                const Frame& frame = _frames.back();
                const osg::Vec3d oldLocalCentre(lod.getBound().center());
                osg::Vec3d oldCentre = oldLocalCentre;
                if (lod.getCenterMode() == osg::LOD::USER_DEFINED_CENTER) oldCentre = osg::Vec3d(lod.getCenter());
                const double oldRadius = lod.getCenterMode() == osg::LOD::USER_DEFINED_CENTER ? lod.getRadius() : lod.getBound().radius();
                const osg::Vec3d oldWorld = oldCentre * frame.oldAccumulated;
                const osg::Vec3d newWorld = toTarget(oldWorld);
                const double scale = localScale(oldWorld, newWorld, std::max(oldRadius, 1.0));

                lod.setCenterMode(osg::LOD::USER_DEFINED_CENTER);
                lod.setCenter(newWorld - frame.newOrigin);
                if (oldRadius > 0.0) lod.setRadius(oldRadius * scale);
                if (lod.getRangeMode() == osg::LOD::DISTANCE_FROM_EYE_POINT)
                {
                    for (unsigned int i = 0; i < lod.getNumRanges(); ++i)
                        lod.setRange(i, static_cast<float>(lod.getMinRange(i) * scale), static_cast<float>(lod.getMaxRange(i) * scale));
                }
                ++report.lods;
                traverse(lod);
            }

        private:
            struct Frame
            {
                osg::Matrixd oldAccumulated; // local -> source world
                osg::Vec3d newOrigin;        // target world position of this frame's origin
            };

            osg::Vec3d toTarget(const osg::Vec3d& sourceWorld)
            {
                GeoPoint geo;
                if (!_crs.toGeographic(sourceWorld.x(), sourceWorld.y(), sourceWorld.z(), geo))
                {
                    if (report.error.empty()) report.error = "coordinate transformation failed";
                    return sourceWorld;
                }
                const vsg::dvec3 w = _target.geoToWorld(geo);
                return {w.x, w.y, w.z};
            }

            /// how much a short distance around `oldWorld` grows or shrinks when reprojected
            double localScale(const osg::Vec3d& oldWorld, const osg::Vec3d& newWorld, double probe)
            {
                double best = 0.0;
                for (const osg::Vec3d& axis : {osg::Vec3d(1.0, 0.0, 0.0), osg::Vec3d(0.0, 1.0, 0.0)})
                {
                    const osg::Vec3d moved = toTarget(oldWorld + axis * probe);
                    best = std::max(best, (moved - newWorld).length() / probe);
                }
                return best > 0.0 ? best : 1.0;
            }

            /// A geometry made of one quad (four corners) becomes an N x N grid, interpolated in local
            /// coordinates and texture coordinates, so it can bend once reprojected.
            void tessellateQuad(osg::Geometry& geometry)
            {
                auto* vertices = dynamic_cast<osg::Vec3Array*>(geometry.getVertexArray());
                if (!vertices || vertices->size() != 4 || geometry.getNumPrimitiveSets() != 1) return;
                auto* texcoords = dynamic_cast<osg::Vec2Array*>(geometry.getTexCoordArray(0));
                if (texcoords && texcoords->size() != 4) return;
                if (geometry.getColorArray() && geometry.getColorArray()->getBinding() == osg::Array::BIND_PER_VERTEX) return;

                const int n = _tessellation;
                const osg::Vec3& v0 = (*vertices)[0]; // corner order as written: v0, v1 along one edge, v3 adjacent to v0
                const osg::Vec3& v1 = (*vertices)[1];
                const osg::Vec3& v2 = (*vertices)[2];
                const osg::Vec3& v3 = (*vertices)[3];

                osg::ref_ptr<osg::Vec3Array> gridVertices = new osg::Vec3Array;
                osg::ref_ptr<osg::Vec2Array> gridTexcoords = texcoords ? new osg::Vec2Array : nullptr;
                for (int j = 0; j <= n; ++j)
                {
                    const float t = static_cast<float>(j) / static_cast<float>(n);
                    for (int i = 0; i <= n; ++i)
                    {
                        const float s = static_cast<float>(i) / static_cast<float>(n);
                        const osg::Vec3 bottom = v0 * (1.0f - s) + v1 * s;
                        const osg::Vec3 top = v3 * (1.0f - s) + v2 * s;
                        gridVertices->push_back(bottom * (1.0f - t) + top * t);
                        if (gridTexcoords)
                        {
                            const osg::Vec2 tb = (*texcoords)[0] * (1.0f - s) + (*texcoords)[1] * s;
                            const osg::Vec2 tt = (*texcoords)[3] * (1.0f - s) + (*texcoords)[2] * s;
                            gridTexcoords->push_back(tb * (1.0f - t) + tt * t);
                        }
                    }
                }
                osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
                const auto index = [n](int i, int j) { return static_cast<unsigned int>(j * (n + 1) + i); };
                for (int j = 0; j < n; ++j)
                    for (int i = 0; i < n; ++i)
                    {
                        indices->push_back(index(i, j));
                        indices->push_back(index(i + 1, j));
                        indices->push_back(index(i + 1, j + 1));
                        indices->push_back(index(i, j));
                        indices->push_back(index(i + 1, j + 1));
                        indices->push_back(index(i, j + 1));
                    }
                geometry.setVertexArray(gridVertices);
                if (gridTexcoords) geometry.setTexCoordArray(0, gridTexcoords);
                geometry.removePrimitiveSet(0, geometry.getNumPrimitiveSets());
                geometry.addPrimitiveSet(indices);
            }

            void transformGeometry(osg::Geometry& geometry)
            {
                const Frame& frame = _frames.back();
                osg::Array* array = geometry.getVertexArray();
                auto* floats = dynamic_cast<osg::Vec3Array*>(array);
                auto* doubles = dynamic_cast<osg::Vec3dArray*>(array);
                if (!floats && !doubles) return;
                const std::size_t count = floats ? floats->size() : doubles->size();

                std::vector<osg::Vec3d> newWorld(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    const osg::Vec3d local = floats ? osg::Vec3d((*floats)[i]) : (*doubles)[i];
                    newWorld[i] = toTarget(local * frame.oldAccumulated);
                }
                for (std::size_t i = 0; i < count; ++i)
                {
                    const osg::Vec3d local = newWorld[i] - frame.newOrigin;
                    if (floats)
                        (*floats)[i] = osg::Vec3(static_cast<float>(local.x()), static_cast<float>(local.y()), static_cast<float>(local.z()));
                    else
                        (*doubles)[i] = local;
                }
                if (floats) floats->dirty();
                if (doubles) doubles->dirty();

                // normals follow the surface of the target frame
                if (osg::Array* normals = geometry.getNormalArray())
                {
                    if (normals->getBinding() == osg::Array::BIND_PER_VERTEX && count > 0)
                    {
                        osg::ref_ptr<osg::Vec3Array> up = new osg::Vec3Array;
                        up->reserve(count);
                        for (const osg::Vec3d& w : newWorld)
                        {
                            const vsg::dvec3 n = _target.upVector(vsg::dvec3(w.x(), w.y(), w.z()));
                            up->push_back(osg::Vec3(static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)));
                        }
                        geometry.setNormalArray(up, osg::Array::BIND_PER_VERTEX);
                    }
                    else if (normals->getBinding() == osg::Array::BIND_OVERALL)
                    {
                        const vsg::dvec3 n = _target.upVector(vsg::dvec3(frame.newOrigin.x(), frame.newOrigin.y(), frame.newOrigin.z()));
                        osg::ref_ptr<osg::Vec3Array> overall = new osg::Vec3Array;
                        overall->push_back(osg::Vec3(static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)));
                        geometry.setNormalArray(overall, osg::Array::BIND_OVERALL);
                    }
                }

                geometry.dirtyBound();
                geometry.dirtyGLObjects();
                ++report.geometries;
                report.vertices += count;
            }

            const CrsTransform& _crs;
            const Projection& _target;
            int _tessellation;
            std::vector<Frame> _frames;
        };
    } // namespace

    ReprojectReport reprojectScene(osg::Node& scene, const ReprojectSettings& settings)
    {
        ReprojectReport report;
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
        Reprojector visitor(*crs, *settings.target, settings.quadTessellation);
        scene.accept(visitor);
        return visitor.report;
    }
} // namespace ivevsg
