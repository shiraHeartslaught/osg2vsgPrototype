#include <ivevsg/TileScheme.h>

#include <gtest/gtest.h>

using ivevsg::TileIndex;
using ivevsg::TileScheme;

TEST(TileScheme, MatchesTheOgcWebMercatorQuadDefinition)
{
    TileScheme scheme;
    EXPECT_EQ(scheme.tilesAcross(0), 1);
    EXPECT_EQ(scheme.tilesAcross(9), 512);
    // OGC TMS "WebMercatorQuad" level 0: cell size 156543.0339280410 m, scale denominator 559082264.0287178
    EXPECT_NEAR(scheme.metersPerPixel(0), 156543.0339280410, 1e-6);
    EXPECT_NEAR(scheme.scaleDenominator(0), 559082264.0287178, 1e-3);
    EXPECT_NEAR(scheme.metersPerPixel(9), 305.7481, 1e-4);
    EXPECT_NEAR(scheme.scaleDenominator(9), 1091957.5468, 1e-3);
}

TEST(TileScheme, LevelZeroIsTheWholeWorldAndLevelOneQuartersIt)
{
    TileScheme scheme;
    const vsg::dbox world = scheme.tileExtent({0, 0, 0});
    EXPECT_DOUBLE_EQ(world.min.x, -scheme.worldHalfExtent);
    EXPECT_DOUBLE_EQ(world.max.y, scheme.worldHalfExtent);

    const vsg::dbox northWest = scheme.tileExtent({1, 0, 0});
    EXPECT_DOUBLE_EQ(northWest.min.x, -scheme.worldHalfExtent);
    EXPECT_DOUBLE_EQ(northWest.max.x, 0.0);
    EXPECT_DOUBLE_EQ(northWest.min.y, 0.0);
    EXPECT_DOUBLE_EQ(northWest.max.y, scheme.worldHalfExtent);

    const vsg::dbox southEast = scheme.tileExtent({1, 1, 1});
    EXPECT_DOUBLE_EQ(southEast.min.x, 0.0);
    EXPECT_DOUBLE_EQ(southEast.max.y, 0.0);
}

TEST(TileScheme, CentreAndRadiusDescribeTheTileSphere)
{
    TileScheme scheme;
    const TileIndex tile{2, 3, 0};
    const vsg::dbox extent = scheme.tileExtent(tile);
    const vsg::dvec3 centre = scheme.tileCentre(tile);
    EXPECT_DOUBLE_EQ(centre.x, (extent.min.x + extent.max.x) * 0.5);
    EXPECT_DOUBLE_EQ(centre.y, (extent.min.y + extent.max.y) * 0.5);
    EXPECT_NEAR(scheme.tileRadius(tile), vsg::length(extent.max - extent.min) * 0.5, 1e-6);
}

TEST(TileScheme, TileAtFindsTheContainingTile)
{
    TileScheme scheme;
    EXPECT_EQ(scheme.tileAt(0.0, 0.0, 0), (TileIndex{0, 0, 0}));
    EXPECT_EQ(scheme.tileAt(1.0, 1.0, 1), (TileIndex{1, 1, 0}));    // just north-east of the origin
    EXPECT_EQ(scheme.tileAt(-1.0, -1.0, 1), (TileIndex{1, 0, 1}));  // just south-west
    EXPECT_EQ(scheme.tileAt(-1.0e9, 1.0e9, 3), (TileIndex{3, 0, 0})); // clamped to the world square
    EXPECT_EQ(scheme.tileAt(1.0e9, -1.0e9, 3), (TileIndex{3, 7, 7}));
    // a point inside a tile's extent maps back to that tile
    const TileIndex tile{5, 17, 9};
    const vsg::dvec3 centre = scheme.tileCentre(tile);
    EXPECT_EQ(scheme.tileAt(centre.x, centre.y, 5), tile);
}

TEST(TileScheme, LevelForMetersPerPixelIsTheInverseOfTheResolutionTable)
{
    TileScheme scheme;
    for (int level = 0; level <= 12; ++level)
    {
        EXPECT_NEAR(scheme.levelForMetersPerPixel(scheme.metersPerPixel(level)), level, 1e-9);
    }
    EXPECT_NEAR(scheme.levelForMetersPerPixel(scheme.metersPerPixel(3) * 0.5), 4.0, 1e-9);
}

TEST(TileScheme, NativeLevelIsTheFirstLevelAtLeastAsFineAsTheSource)
{
    TileScheme scheme;
    EXPECT_EQ(scheme.nativeLevelFor(scheme.metersPerPixel(5)), 5);
    EXPECT_EQ(scheme.nativeLevelFor(scheme.metersPerPixel(5) * 1.3), 5); // coarser than 5 but finer than 4
    EXPECT_EQ(scheme.nativeLevelFor(scheme.metersPerPixel(5) * 0.9), 6);
    EXPECT_EQ(scheme.nativeLevelFor(4891.969810251280), 5);              // the 8192 px Blue Marble source
    EXPECT_EQ(scheme.nativeLevelFor(1.0e9), 0);
}

TEST(TileScheme, ChildrenParentsAndPaths)
{
    const TileIndex tile{3, 5, 2};
    const auto kids = TileScheme::children(tile);
    EXPECT_EQ(kids[0], (TileIndex{4, 10, 4}));
    EXPECT_EQ(kids[1], (TileIndex{4, 11, 4}));
    EXPECT_EQ(kids[2], (TileIndex{4, 10, 5}));
    EXPECT_EQ(kids[3], (TileIndex{4, 11, 5}));
    for (const auto& kid : kids) EXPECT_EQ(TileScheme::parent(kid), tile);
    EXPECT_EQ(TileScheme::parent({0, 0, 0}), (TileIndex{0, 0, 0}));
    EXPECT_EQ(TileScheme::relativePath(tile), "3/5/2.ive");
    EXPECT_EQ(TileScheme::relativePath(tile, ".jpg"), "3/5/2.jpg");

    // children tile the parent exactly
    TileScheme scheme;
    const vsg::dbox parentExtent = scheme.tileExtent(tile);
    EXPECT_DOUBLE_EQ(scheme.tileExtent(kids[0]).min.x, parentExtent.min.x);
    EXPECT_DOUBLE_EQ(scheme.tileExtent(kids[0]).max.y, parentExtent.max.y);
    EXPECT_DOUBLE_EQ(scheme.tileExtent(kids[3]).max.x, parentExtent.max.x);
    EXPECT_DOUBLE_EQ(scheme.tileExtent(kids[3]).min.y, parentExtent.min.y);
}
