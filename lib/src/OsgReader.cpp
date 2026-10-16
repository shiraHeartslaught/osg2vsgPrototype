#include <ivevsg/OsgReader.h>
#include <ivevsg/OsgReproject.h>

#include <osg/Array>
#include <osg/CoordinateSystemNode>
#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osgDB/Options>
#include <osgDB/ReadFile>
#include <osgDB/Registry>

#include <osg2vsg/convert.h>

#include <vsg/all.h>

#include <array>

namespace ivevsg
{
    namespace
    {
        constexpr std::array<const char*, 4> kExtensions = {".ive", ".osgb", ".osgt", ".osg"};

        osg::ref_ptr<osg::Vec4Array> normalised(const osg::Vec4ubArray& bytes)
        {
            osg::ref_ptr<osg::Vec4Array> floats = new osg::Vec4Array;
            floats->reserve(bytes.size());
            for (const osg::Vec4ub& c : bytes) floats->push_back(osg::Vec4(c.r() / 255.0f, c.g() / 255.0f, c.b() / 255.0f, c.a() / 255.0f));
            floats->setBinding(bytes.getBinding());
            return floats;
        }

        osg::ref_ptr<osg::Vec4Array> normalised(const osg::Vec3ubArray& bytes)
        {
            osg::ref_ptr<osg::Vec4Array> floats = new osg::Vec4Array;
            floats->reserve(bytes.size());
            for (const osg::Vec3ub& c : bytes) floats->push_back(osg::Vec4(c.r() / 255.0f, c.g() / 255.0f, c.b() / 255.0f, 1.0f));
            floats->setBinding(bytes.getBinding());
            return floats;
        }

        /// First osg::CoordinateSystemNode in the scene (VPB writes one at a database root).
        class FindCoordinateSystem : public osg::NodeVisitor
        {
        public:
            FindCoordinateSystem() :
                osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN) {}
            std::string crs;
            void apply(osg::CoordinateSystemNode& csn) override
            {
                if (crs.empty()) crs = csn.getCoordinateSystem();
                traverse(csn);
            }
        };

        class ByteColourNormaliser : public osg::NodeVisitor
        {
        public:
            ByteColourNormaliser() :
                osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN) {}

            std::size_t rewritten = 0;

            void apply(osg::Geometry& geometry) override
            {
                osg::Array* colours = geometry.getColorArray();
                if (auto* bytes4 = dynamic_cast<osg::Vec4ubArray*>(colours))
                {
                    geometry.setColorArray(normalised(*bytes4), bytes4->getBinding());
                    ++rewritten;
                }
                else if (auto* bytes3 = dynamic_cast<osg::Vec3ubArray*>(colours))
                {
                    geometry.setColorArray(normalised(*bytes3), bytes3->getBinding());
                    ++rewritten;
                }
                traverse(geometry);
            }
        };
    } // namespace

    std::size_t normaliseByteColourArrays(osg::Node& scene)
    {
        ByteColourNormaliser visitor;
        scene.accept(visitor);
        return visitor.rewritten;
    }

    const char* const OsgReader::kTargetCrs = "ivevsg.target_crs";
    const char* const OsgReader::kSourceCrs = "ivevsg.source_crs";
    const char* const OsgReader::kDefaultSourceCrs = "ivevsg.default_source_crs";
    const char* const OsgReader::kQuadTessellation = "ivevsg.quad_tessellation";

    OsgReader::OsgReader() = default;

    bool OsgReader::handles(const vsg::Path& filename)
    {
        const vsg::Path ext = vsg::lowerCaseFileExtension(filename);
        for (const char* candidate : kExtensions)
        {
            if (ext == candidate) return true;
        }
        return false;
    }

    vsg::ref_ptr<vsg::Object> OsgReader::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
    {
        if (!handles(filename)) return {};

        // the same database-path plumbing osg2vsg's own reader uses, so relative PagedLOD names resolve
        osg::ref_ptr<osgDB::Options> osgOptions;
        if (osgDB::Registry::instance()->getOptions())
            osgOptions = osgDB::Registry::instance()->getOptions()->cloneOptions();
        else
            osgOptions = new osgDB::Options();
        if (options)
        {
            for (const auto& path : options->paths) osgOptions->getDatabasePathList().push_back(path.string());
        }

        osgDB::ReaderWriter::ReadResult result = osgDB::Registry::instance()->readNode(filename.string(), osgOptions.get());
        if (!result.validNode()) return {};
        osg::ref_ptr<osg::Node> scene = result.takeNode();

        normaliseByteColourArrays(*scene);

        // Reproject into the current projection's frame when the options ask for it. The source CRS
        // comes from the load request, else from the file's CoordinateSystemNode, else from the
        // loader's default. The values travel with the options into every PagedLOD child, so paged
        // tiles are reprojected the same way as their root.
        std::string targetCrs;
        if (options && options->getValue(OsgReader::kTargetCrs, targetCrs) && !targetCrs.empty())
        {
            std::string sourceCrs;
            options->getValue(OsgReader::kSourceCrs, sourceCrs);
            if (sourceCrs.empty())
            {
                FindCoordinateSystem finder;
                scene->accept(finder);
                sourceCrs = finder.crs;
            }
            if (sourceCrs.empty()) options->getValue(OsgReader::kDefaultSourceCrs, sourceCrs);

            ReprojectSettings settings;
            settings.sourceCrs = sourceCrs;
            settings.target = createProjection(targetCrs);
            options->getValue(OsgReader::kQuadTessellation, settings.quadTessellation);
            if (!settings.target)
            {
                vsg::warn("OsgReader: unknown target projection '", targetCrs, "', leaving ", filename, " unprojected");
            }
            else if (sourceCrs.empty())
            {
                vsg::warn("OsgReader: no source CRS known for ", filename, ", leaving it unprojected");
            }
            else
            {
                const ReprojectReport report = reprojectScene(*scene, settings);
                if (!report) vsg::warn("OsgReader: reprojection of ", filename, " failed: ", report.error);
            }
        }

        return osg2vsg::convert(*scene, options);
    }

    bool OsgReader::getFeatures(Features& features) const
    {
        for (const char* ext : kExtensions) features.extensionFeatureMap[ext] = vsg::ReaderWriter::READ_FILENAME;
        return true;
    }
} // namespace ivevsg
