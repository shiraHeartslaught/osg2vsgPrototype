#include <ivevsg/BaseMap.h>
#include <ivevsg/SceneBounds.h>
#include <ivevsg/SceneLoader.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

#include <filesystem>

namespace
{
    const vsg::ubvec4* texelsOf(const vsg::Data& data) { return static_cast<const vsg::ubvec4*>(data.dataPointer()); }
} // namespace

TEST(BaseMapMipmaps, GeneratesEveryLevelDownToOneByOne)
{
    auto level0 = vsg::ubvec4Array2D::create(8, 4, vsg::ubvec4(10, 20, 30, 255), vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});
    auto chain = ivevsg::generateMipmaps(*level0);
    ASSERT_TRUE(chain);
    EXPECT_EQ(chain->width(), 8u);
    EXPECT_EQ(chain->height(), 4u);
    EXPECT_EQ(chain->properties.mipLevels, 4); // 8x4, 4x2, 2x1, 1x1
    EXPECT_EQ(chain->properties.format, VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(chain->computeValueCountIncludingMipmaps(), 32u + 8u + 2u + 1u);
    EXPECT_EQ(chain->dataSize(), (32u + 8u + 2u + 1u) * sizeof(vsg::ubvec4));
    // every level is a uniform colour, so the last texel of the chain must still be the input colour
    const vsg::ubvec4 last = texelsOf(*chain)[32 + 8 + 2];
    EXPECT_EQ(last.r, 10);
    EXPECT_EQ(last.a, 255);
}

TEST(BaseMapMipmaps, LevelsAreBoxFilteredAverages)
{
    auto level0 = vsg::ubvec4Array2D::create(2, 2, vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});
    level0->at(0, 0) = vsg::ubvec4(0, 0, 0, 255);
    level0->at(1, 0) = vsg::ubvec4(255, 255, 255, 255);
    level0->at(0, 1) = vsg::ubvec4(255, 255, 255, 255);
    level0->at(1, 1) = vsg::ubvec4(0, 0, 0, 255);
    auto chain = ivevsg::generateMipmaps(*level0);
    ASSERT_EQ(chain->properties.mipLevels, 2);
    const vsg::ubvec4 average = texelsOf(*chain)[4];
    EXPECT_NEAR(average.r, 127.5, 1.0);
    EXPECT_NEAR(average.g, 127.5, 1.0);
    EXPECT_EQ(average.a, 255);
    EXPECT_EQ(chain->properties.origin, level0->properties.origin);
}

TEST(BaseMap, NoImageGivesASolidColourQuadCoveringTheWorldSquare)
{
    ivevsg::BaseMapSettings settings;
    settings.imagePath.clear();
    const ivevsg::BaseMapResult result = ivevsg::createBaseMap(settings, vsg::Options::create());
    ASSERT_TRUE(result.node);
    EXPECT_FALSE(result.textured);
    EXPECT_FALSE(result.message.empty());

    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*result.node);
    ASSERT_TRUE(bounds.valid());
    const double e = settings.halfExtent;
    EXPECT_NEAR(bounds.min.x, -e, e * 1e-6);
    EXPECT_NEAR(bounds.max.x, e, e * 1e-6);
    EXPECT_NEAR(bounds.min.y, -e, e * 1e-6);
    EXPECT_NEAR(bounds.max.y, e, e * 1e-6);
    EXPECT_NEAR(bounds.min.z, settings.zOffset, 1.0);
    EXPECT_NEAR(bounds.max.z, settings.zOffset, 1.0);
}

TEST(BaseMap, MissingImageFallsBackToSolidColour)
{
    ivevsg::BaseMapSettings settings;
    settings.imagePath = "/nonexistent/bluemarble.jpg";
    const ivevsg::BaseMapResult result = ivevsg::createBaseMap(settings, vsg::Options::create());
    ASSERT_TRUE(result.node);
    EXPECT_FALSE(result.textured);
    EXPECT_NE(result.message.find("not found"), std::string::npos);
}

TEST(BaseMap, TexturesTheQuadFromAnImageFile)
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ivevsg_basemap_test.png";
    auto image = vsg::ubvec4Array2D::create(16, 16, vsg::ubvec4(20, 60, 120, 255), vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});
    ivevsg::SceneLoader loader; // its options carry the vsgXchange image writers/readers
    ASSERT_TRUE(vsg::write(image, vsg::Path(file.string()), loader.options()));

    ivevsg::BaseMapSettings settings;
    settings.imagePath = file.string();
    const ivevsg::BaseMapResult result = ivevsg::createBaseMap(settings, loader.options());
    ASSERT_TRUE(result.node);
    EXPECT_TRUE(result.textured) << result.message;
    EXPECT_NE(result.message.find("mip levels"), std::string::npos);
    EXPECT_TRUE(ivevsg::computeGeometryBounds(*result.node).valid());

    std::filesystem::remove(file);
}

TEST(BaseMap, DefaultImageShipsWithTheProject)
{
    const std::string path = ivevsg::BaseMapSettings::defaultBaseMapImagePath();
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(std::filesystem::exists(path)) << path << " (run data/fetch_bluemarble.sh)";
}
