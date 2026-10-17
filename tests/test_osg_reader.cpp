#include <ivevsg/OsgReader.h>
#include <ivevsg/SceneLoader.h>

#include <gtest/gtest.h>
#include <osg/CoordinateSystemNode>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osgDB/WriteFile>
#include <vsg/all.h>

#include <filesystem>

namespace
{
    osg::ref_ptr<osg::Geometry> makeQuad(osg::Array* colours, osg::Array::Binding binding)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3(0.0f, 0.0f, 0.0f));
        vertices->push_back(osg::Vec3(1.0f, 0.0f, 0.0f));
        vertices->push_back(osg::Vec3(1.0f, 1.0f, 0.0f));
        vertices->push_back(osg::Vec3(0.0f, 1.0f, 0.0f));
        geometry->setVertexArray(vertices);
        geometry->setColorArray(colours, binding);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN, 0, 4));
        return geometry;
    }

    struct CountArrays : public vsg::ConstVisitor
    {
        int draws = 0;
        int vec4Arrays = 0;
        void apply(const vsg::Node& node) override { node.traverse(*this); }
        void apply(const vsg::VertexIndexDraw& draw) override
        {
            ++draws;
            for (auto& array : draw.arrays)
                if (array->data && array->data.cast<vsg::vec4Array>()) ++vec4Arrays;
        }
        void apply(const vsg::VertexDraw& draw) override
        {
            ++draws;
            for (auto& array : draw.arrays)
                if (array->data && array->data.cast<vsg::vec4Array>()) ++vec4Arrays;
        }
    };
} // namespace

TEST(OsgReader, NormalisesByteColourArraysKeepingTheirBinding)
{
    osg::ref_ptr<osg::Vec4ubArray> overall = new osg::Vec4ubArray;
    overall->push_back(osg::Vec4ub(255, 128, 0, 255));
    osg::ref_ptr<osg::Vec3ubArray> perVertex = new osg::Vec3ubArray;
    for (int i = 0; i < 4; ++i) perVertex->push_back(osg::Vec3ub(static_cast<unsigned char>(i * 50), 0, 255));
    osg::ref_ptr<osg::Vec4Array> floats = new osg::Vec4Array;
    floats->push_back(osg::Vec4(0.5f, 0.5f, 0.5f, 1.0f));

    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(makeQuad(overall, osg::Array::BIND_OVERALL));
    geode->addDrawable(makeQuad(perVertex, osg::Array::BIND_PER_VERTEX));
    geode->addDrawable(makeQuad(floats, osg::Array::BIND_OVERALL));
    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->addChild(geode);

    EXPECT_EQ(ivevsg::normaliseByteColourArrays(*root), 2u);

    auto* a = dynamic_cast<osg::Vec4Array*>(geode->getDrawable(0)->asGeometry()->getColorArray());
    ASSERT_TRUE(a);
    ASSERT_EQ(a->size(), 1u);
    EXPECT_FLOAT_EQ((*a)[0].r(), 1.0f);
    EXPECT_NEAR((*a)[0].g(), 128.0f / 255.0f, 1e-6f);
    EXPECT_FLOAT_EQ((*a)[0].b(), 0.0f);
    EXPECT_FLOAT_EQ((*a)[0].a(), 1.0f);
    EXPECT_EQ(a->getBinding(), osg::Array::BIND_OVERALL);

    auto* b = dynamic_cast<osg::Vec4Array*>(geode->getDrawable(1)->asGeometry()->getColorArray());
    ASSERT_TRUE(b);
    ASSERT_EQ(b->size(), 4u);
    EXPECT_NEAR((*b)[3].r(), 150.0f / 255.0f, 1e-6f);
    EXPECT_FLOAT_EQ((*b)[3].b(), 1.0f);
    EXPECT_FLOAT_EQ((*b)[3].a(), 1.0f); // Vec3ub gets an opaque alpha
    EXPECT_EQ(b->getBinding(), osg::Array::BIND_PER_VERTEX);

    EXPECT_EQ(geode->getDrawable(2)->asGeometry()->getColorArray(), floats.get()); // float arrays untouched
    EXPECT_EQ(ivevsg::normaliseByteColourArrays(*root), 0u);                      // idempotent
}

TEST(OsgReader, HandlesOnlyOpenSceneGraphSceneFormats)
{
    EXPECT_TRUE(ivevsg::OsgReader::handles("tiles/root.ive"));
    EXPECT_TRUE(ivevsg::OsgReader::handles("model.OSGB"));
    EXPECT_TRUE(ivevsg::OsgReader::handles("a.osgt"));
    EXPECT_FALSE(ivevsg::OsgReader::handles("scene.vsgt"));
    EXPECT_FALSE(ivevsg::OsgReader::handles("image.png"));
    auto reader = ivevsg::OsgReader::create();
    EXPECT_FALSE(reader->read(vsg::Path("image.png"), vsg::Options::create()));
    vsg::ReaderWriter::Features features;
    EXPECT_TRUE(reader->getFeatures(features));
    EXPECT_TRUE(features.extensionFeatureMap.count(".ive"));
}

TEST(OsgReader, SceneLoaderUsesItAheadOfVsgXchangeAndByteColoursBecomeAVsgColourArray)
{
    ivevsg::SceneLoader loader; // registers the OSG plugin directory for the .osgt writer below
    ASSERT_GE(loader.options()->readerWriters.size(), 3u);
    EXPECT_TRUE(loader.options()->readerWriters[1].cast<ivevsg::OsgReader>()) << "OsgReader must come right after the load monitor";

    osg::ref_ptr<osg::Vec4ubArray> colours = new osg::Vec4ubArray;
    colours->push_back(osg::Vec4ub(10, 20, 30, 255));
    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(makeQuad(colours, osg::Array::BIND_OVERALL));
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ivevsg_bytecolours.osgt";
    ASSERT_TRUE(osgDB::writeNodeFile(*geode, file.string()));

    const ivevsg::LoadResult loaded = loader.load(file.string());
    ASSERT_TRUE(loaded) << loaded.error;
    CountArrays census;
    loaded.node->accept(census);
    EXPECT_EQ(census.draws, 1);
    EXPECT_EQ(census.vec4Arrays, 1) << "the byte colours must arrive as a bound float colour array";

    std::filesystem::remove(file);
}

TEST(OsgReader, ACoordinateSystemNodeWithAnEllipsoidModelMeansGeocentric)
{
    osg::ref_ptr<osg::CoordinateSystemNode> geocentric = new osg::CoordinateSystemNode;
    geocentric->setCoordinateSystem("GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]]]");
    geocentric->setEllipsoidModel(new osg::EllipsoidModel);
    EXPECT_EQ(ivevsg::OsgReader::declaredCrs(*geocentric), "EPSG:4978");

    osg::ref_ptr<osg::CoordinateSystemNode> flat = new osg::CoordinateSystemNode;
    flat->setCoordinateSystem("EPSG:3857");
    EXPECT_EQ(ivevsg::OsgReader::declaredCrs(*flat), "EPSG:3857");

    osg::ref_ptr<osg::Group> none = new osg::Group;
    EXPECT_TRUE(ivevsg::OsgReader::declaredCrs(*none).empty());
}
