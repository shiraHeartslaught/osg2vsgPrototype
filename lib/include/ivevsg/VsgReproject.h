#pragma once

#include <ivevsg/OsgReproject.h> // ReprojectSettings / ReprojectReport

#include <vsg/core/Inherit.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/io/Options.h>
#include <vsg/io/ReaderWriter.h>
#include <vsg/nodes/Node.h>

#include <string>

namespace ivevsg
{
    /// The VSG counterpart of reprojectScene(): rewrites a VSG scene graph from its source CRS into the
    /// target projection's frame. Vertex arrays (found through each StateGroup's pipeline: the array
    /// bound to vsg_Vertex, and vsg_Normal for normals) are taken to world coordinates through the
    /// accumulated MatrixTransforms, converted through geographic WGS84 into the target frame, and
    /// stored again as float offsets from their transform's new origin; every MatrixTransform becomes a
    /// translation to the reprojected position of its old origin. LOD, PagedLOD, CullNode and
    /// CullGroup spheres move with their content and grow to enclose it. PagedLODs are given
    /// `childOptions`, so their children are read (and reprojected) the same way.
    ///
    /// Geometry under no MatrixTransform at all keeps absolute (float) coordinates, like the OSG
    /// reprojector leaves it: children paged in below are reprojected into absolute coordinates by
    /// their own reader and have to line up with their parent.
    ReprojectReport reprojectVsgScene(vsg::ref_ptr<vsg::Node>& scene, const ReprojectSettings& settings, vsg::ref_ptr<const vsg::Options> childOptions = {});

    /// Reads VSG native files (.vsgb, .vsgt) and places them in the display frame. The file's CRS is
    /// the loader's source override, else the value stored under loaderKeys::fileCrs in the file (see
    /// tagCrs()), else the loader's default; when it differs from the target CRS the scene is
    /// reprojected, paged children included. The returned node carries loaderKeys::nodeSourceCrs /
    /// nodeTargetCrs like every other reader's.
    class VsgSceneReader : public vsg::Inherit<vsg::ReaderWriter, VsgSceneReader>
    {
    public:
        VsgSceneReader();

        static bool handles(const vsg::Path& filename);

        vsg::ref_ptr<vsg::Object> read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options = {}) const override;
        bool getFeatures(Features& features) const override;

    private:
        vsg::ref_ptr<vsg::ReaderWriter> _native;
    };

    /// Gives every PagedLOD below `scene` a copy of `options`, so its children are read the same way.
    void assignPagedLodOptions(vsg::Node& scene, vsg::ref_ptr<const vsg::Options> options);

    /// Records the CRS of a scene's coordinates on its root node, where VsgSceneReader finds it after
    /// the node has been written to a .vsgb / .vsgt file.
    void tagCrs(vsg::Node& scene, const std::string& crs);
    /// The CRS recorded with tagCrs(), or "".
    std::string taggedCrs(const vsg::Node& scene);
} // namespace ivevsg
