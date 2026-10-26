#include <ivevsg/TextureCompress.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

#include <cmath>
#include <cstdlib>

namespace
{
    vsg::ref_ptr<vsg::ubvec4Array2D> solid(uint32_t w, uint32_t h, vsg::ubvec4 colour)
    {
        return vsg::ubvec4Array2D::create(w, h, colour, vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});
    }
} // namespace

TEST(TextureCompress, ASolidBlockComesBackExactlyWhenItsColourFitsRgb565)
{
    auto image = solid(4, 4, vsg::ubvec4(247, 251, 247, 255)); // what 5-6-5 bits expand to exactly (30, 62, 30)
    auto blocks = ivevsg::compressBc1(*image, false);
    ASSERT_TRUE(blocks);
    EXPECT_EQ(blocks->width(), 1u);
    EXPECT_EQ(blocks->height(), 1u);
    EXPECT_EQ(blocks->properties.format, VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    EXPECT_EQ(blocks->properties.blockWidth, 4);
    EXPECT_EQ(blocks->properties.blockHeight, 4);
    EXPECT_EQ(blocks->properties.mipLevels, 1);
    vsg::ubvec4 pixels[16];
    ivevsg::decodeBc1Block(blocks->at(0, 0), pixels);
    for (const vsg::ubvec4& p : pixels)
    {
        EXPECT_EQ(p.r, 247);
        EXPECT_EQ(p.g, 251);
        EXPECT_EQ(p.b, 247);
        EXPECT_EQ(p.a, 255);
    }
}

TEST(TextureCompress, AGradientBlockIsCloseAndTransparencySurvives)
{
    auto image = solid(4, 4, vsg::ubvec4(0, 0, 0, 255));
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
        {
            const uint8_t v = static_cast<uint8_t>(40 + (x + y * 4) * 10); // 40..190
            image->at(x, y) = vsg::ubvec4(v, static_cast<uint8_t>(200 - v / 2), 90, 255);
        }
    image->at(3, 3) = vsg::ubvec4(0, 0, 0, 0); // one transparent texel
    auto blocks = ivevsg::compressBc1(*image, false);
    ASSERT_TRUE(blocks);
    vsg::ubvec4 pixels[16];
    ivevsg::decodeBc1Block(blocks->at(0, 0), pixels);
    EXPECT_EQ(pixels[15].a, 0) << "the transparent texel";
    for (int i = 0; i < 15; ++i)
    {
        const vsg::ubvec4& want = image->at(static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
        EXPECT_EQ(pixels[i].a, 255);
        EXPECT_LE(std::abs(int(pixels[i].r) - int(want.r)), 40) << "texel " << i;
        EXPECT_LE(std::abs(int(pixels[i].g) - int(want.g)), 40) << "texel " << i;
        EXPECT_LE(std::abs(int(pixels[i].b) - int(want.b)), 40) << "texel " << i;
    }
}

TEST(TextureCompress, MipChainMatchesVsgsExpectedLayout)
{
    auto image = solid(256, 128, vsg::ubvec4(10, 200, 30, 255));
    image->at(0, 0) = vsg::ubvec4(255, 255, 255, 255);
    auto blocks = ivevsg::compressBc1(*image, true);
    ASSERT_TRUE(blocks);
    EXPECT_EQ(blocks->width(), 64u);
    EXPECT_EQ(blocks->height(), 32u);
    EXPECT_EQ(blocks->properties.mipLevels, 9); // 256 -> 1 along the wide side
    // level sizes in blocks: 64x32, 32x16, 16x8, 8x4, 4x2, 2x1, 1x1, 1x1, 1x1
    const std::size_t expected = 64 * 32 + 32 * 16 + 16 * 8 + 8 * 4 + 4 * 2 + 2 * 1 + 1 + 1 + 1;
    EXPECT_EQ(blocks->valueCount(), expected);
    EXPECT_EQ(blocks->computeValueCountIncludingMipmaps(), expected) << "what VSG will upload must be what was allocated";
    EXPECT_EQ(blocks->dataSize(), expected * 8);

    // the last level is one block of (nearly) the average colour
    vsg::ubvec4 pixels[16];
    ivevsg::decodeBc1Block(blocks->data()[expected - 1], pixels);
    EXPECT_LE(std::abs(int(pixels[0].g) - 200), 12);
    EXPECT_EQ(pixels[0].a, 255);
}

TEST(TextureCompress, AWholeTileEncodesToAnEighthOfRgba)
{
    auto image = solid(256, 256, vsg::ubvec4(120, 130, 140, 255));
    auto noMips = ivevsg::compressBc1(*image, false);
    ASSERT_TRUE(noMips);
    EXPECT_EQ(noMips->dataSize(), 256u * 256u * 4u / 8u);
    auto withMips = ivevsg::compressBc1(*image, true);
    ASSERT_TRUE(withMips);
    EXPECT_LT(withMips->dataSize(), 256u * 256u * 4u / 8u * 4u / 3u + 64u);
}
