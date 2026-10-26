#pragma once

#include <ivevsg/Projection.h>

#include <vsg/core/Array.h>
#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/io/Options.h>
#include <vsg/maths/box.h>
#include <vsg/maths/sphere.h>
#include <vsg/nodes/Node.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/Descriptor.h>
#include <vsg/state/Sampler.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ivevsg
{
    class CrsTransform;

    /// A textured tile's geometry in the display frame: float vertices around `origin`, with the
    /// sphere that encloses them in world coordinates.
    struct TileMesh
    {
        vsg::ref_ptr<vsg::vec3Array> vertices;
        vsg::ref_ptr<vsg::vec3Array> normals;
        vsg::ref_ptr<vsg::vec2Array> texcoords;
        vsg::ref_ptr<vsg::ushortArray> indices;
        vsg::dvec3 origin;
        vsg::dsphere bound;
    };

    /// Cells per side for a tile spanning the given geographic corners: about one per 4 degrees so a
    /// whole-world tile still follows the globe, never below `minimum`, at most 64.
    int tileCellsFor(const GeoPoint& southWest, const GeoPoint& northEast, int minimum);

    /// The mesh for a tile footprint: an N x N grid whose points are taken from the footprint's CRS to
    /// geographic coordinates by `toGeographic` and placed with the projection (null `toGeographic` =
    /// the footprint already is in the projection's own world units, which only a flat map offers).
    /// Rows run south to north; texture t runs from 1 at the south edge to 0 at the north, so an
    /// image whose first row is the north edge maps straight on.
    TileMesh buildTileMesh(const Projection& projection, const vsg::dbox& extent, int cells, const CrsTransform* toGeographic);

    /// The screen height osg2vsg's PagedLOD ratios assume; tile hand-over ratios use it too, so
    /// pyramids of every format page at the same size on screen.
    constexpr double tileReferenceScreenHeight = 1080.0;
    /// PagedLOD minimumScreenHeightRatio at which a tile of the given size yields to its children.
    double tileHandOverRatio(int tileSizePixels, double detailBias = 1.0);

    /// Graphics state shared by every tile of one layer: a flat-shaded, alpha-blended, textured
    /// pipeline bound once at the root, with one descriptor set per tile for its texture.
    class TileStateFactory
    {
    public:
        explicit TileStateFactory(vsg::ref_ptr<const vsg::Options> options = {});

        /// The node to put tiles under: binds the pipeline, which the tiles inherit.
        vsg::ref_ptr<vsg::StateGroup> createRoot() const;

        /// A drawable tile: its texture bound in a StateGroup, the mesh under a transform to its origin.
        /// The image is RGBA8 or block compressed (TextureCompress.h), with or without a mip chain.
        vsg::ref_ptr<vsg::Node> createTile(const TileMesh& mesh, vsg::ref_ptr<vsg::Data> image) const;

    private:
        vsg::ref_ptr<vsg::GraphicsPipelineConfigurator> _pipeline;
        vsg::ref_ptr<vsg::Sampler> _sampler;
        std::vector<std::pair<uint32_t, vsg::Descriptors>> _fixedDescriptors; // per set: material and friends
        uint32_t _diffuseSet = 0, _diffuseBinding = 0;
        vsg::ref_ptr<vsg::vec4Array> _colours;
    };

    /// Wraps a tile for paging: a PagedLOD that loads `childFile` (relative to the file it is in, or a
    /// virtual name) once the sphere grows past `handOverRatio` of the screen height, or, with an
    /// empty childFile, a CullNode around a leaf.
    vsg::ref_ptr<vsg::Node> makePagedTile(vsg::ref_ptr<vsg::Node> tile, const vsg::dsphere& bound, const std::string& childFile, double handOverRatio, vsg::ref_ptr<vsg::Options> childOptions = {});
} // namespace ivevsg
