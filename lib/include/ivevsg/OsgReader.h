#pragma once

#include <vsg/core/Inherit.h>
#include <vsg/io/ReaderWriter.h>

#include <cstddef>
#include <string>

namespace osg
{
    class Node;
}

namespace ivevsg
{
    /// Rewrites byte colour arrays (osg::Vec4ubArray and osg::Vec3ubArray, the form VPB and osgconv's
    /// optimiser write) into float osg::Vec4Array normalised to 0..1, keeping their binding, so that
    /// osg2vsg's converter, which only understands float arrays, binds them as the colour attribute.
    /// Without this a geometry gets a shader expecting a colour input with no buffer behind it and
    /// draws nothing. Returns the number of arrays rewritten.
    std::size_t normaliseByteColourArrays(osg::Node& scene);

    /// Reads OpenSceneGraph scene formats (.ive, .osgb, .osgt, .osg) with osgDB, applies the fix-ups
    /// above, optionally reprojects the scene into the current projection (see the option values
    /// below), and converts the result with osg2vsg. SceneLoader puts it ahead of vsgXchange, so
    /// these adjustments live here instead of as patches to third-party code. Anything it does not
    /// handle (other extensions, images) is declined and falls through to the next reader.
    class OsgReader : public vsg::Inherit<vsg::ReaderWriter, OsgReader>
    {
    public:
        OsgReader();

        /// vsg::Options string/int values that control reprojection (set by SceneLoader):
        static const char* const kTargetCrs;        // e.g. "EPSG:3857" or "EPSG:4978"; empty = no reprojection
        static const char* const kSourceCrs;        // per-load override of the file's CRS
        static const char* const kDefaultSourceCrs; // used when the file has no CoordinateSystemNode
        static const char* const kQuadTessellation; // int, see ReprojectSettings
        /// Set on the returned node (vsg::Object values): the CRS the file turned out to be in and the
        /// frame it was converted to ("" when left as read), so a viewer can show both.
        static const char* const kNodeSourceCrs;
        static const char* const kNodeTargetCrs;

        /// The CRS a file declares through its osg::CoordinateSystemNode, or "" when it has none. A node
        /// carrying an EllipsoidModel means geocentric coordinates (VirtualPlanetBuilder's whole-earth
        /// databases), reported as EPSG:4978 whatever geographic system the node names.
        static std::string declaredCrs(osg::Node& scene);

        static bool handles(const vsg::Path& filename);

        vsg::ref_ptr<vsg::Object> read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options = {}) const override;
        bool getFeatures(Features& features) const override;
    };
} // namespace ivevsg
