#include <ivevsg/TileGrid.h>
#include <ivevsg/TileScheme.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace
{
    constexpr double kE = 20037508.342789244;

    const char* kMercatorTileMap = R"(<?xml version="1.0" encoding="utf-8"?>
<TileMap version="1.0.0" tilemapservice="http://tms.osgeo.org/1.0.0">
  <Title>cyprus</Title>
  <Abstract></Abstract>
  <SRS>EPSG:3857</SRS>
  <BoundingBox minx="3600000.0" miny="4000000.0" maxx="3800000.0" maxy="4200000.0"/>
  <Origin x="-20037508.342789244" y="-20037508.342789244"/>
  <TileFormat width="256" height="256" mime-type="image/png" extension="png"/>
  <TileSets profile="mercator">
    <TileSet href="3" units-per-pixel="19567.87924100512" order="3"/>
    <TileSet href="4" units-per-pixel="9783.93962050256" order="4"/>
    <TileSet href="5" units-per-pixel="4891.96981025128" order="5"/>
  </TileSets>
</TileMap>)";

    const char* kGeodeticTileMap = R"(<TileMap version="1.0.0">
  <SRS>EPSG:4326</SRS>
  <BoundingBox minx="30.0" miny="33.0" maxx="36.0" maxy="37.0"/>
  <Origin x="-180.0" y="-90.0"/>
  <TileFormat width="256" height="256" mime-type="image/jpeg" extension="jpg"/>
  <TileSets profile="geodetic">
    <TileSet href="0" units-per-pixel="0.703125" order="0"/>
    <TileSet href="1" units-per-pixel="0.3515625" order="1"/>
  </TileSets>
</TileMap>)";
} // namespace

TEST(TileGrid, WebMercatorQuadMatchesTheTileScheme)
{
    const ivevsg::TileGrid grid = ivevsg::TileGrid::webMercatorQuad();
    const ivevsg::TileScheme scheme;
    for (const ivevsg::TileIndex tile : {ivevsg::TileIndex{0, 0, 0}, ivevsg::TileIndex{1, 1, 0}, ivevsg::TileIndex{3, 5, 6}, ivevsg::TileIndex{9, 300, 200}})
    {
        const vsg::dbox a = grid.tileExtent(tile), b = scheme.tileExtent(tile);
        EXPECT_NEAR(a.min.x, b.min.x, 1e-6);
        EXPECT_NEAR(a.min.y, b.min.y, 1e-6);
        EXPECT_NEAR(a.max.x, b.max.x, 1e-6);
        EXPECT_NEAR(a.max.y, b.max.y, 1e-6);
        EXPECT_NEAR(grid.unitsPerPixel(tile.level), scheme.metersPerPixel(tile.level), 1e-9);
    }
    EXPECT_EQ(grid.tilesAcross(5), 32);
    EXPECT_EQ(grid.tilesDown(5), 32);
    EXPECT_EQ(grid.levelForUnitsPerPixel(scheme.metersPerPixel(7)), 7);
    EXPECT_EQ(grid.levelForUnitsPerPixel(scheme.metersPerPixel(7) * 0.9), 8);
}

TEST(TileGrid, WorldCrs84QuadHasTwoTilesAtLevelZero)
{
    const ivevsg::TileGrid grid = ivevsg::TileGrid::worldCrs84Quad();
    EXPECT_EQ(grid.tilesAcross(0), 2);
    EXPECT_EQ(grid.tilesDown(0), 1);
    const vsg::dbox west = grid.tileExtent({0, 0, 0});
    EXPECT_DOUBLE_EQ(west.min.x, -180.0);
    EXPECT_DOUBLE_EQ(west.max.x, 0.0);
    EXPECT_DOUBLE_EQ(west.min.y, -90.0);
    EXPECT_DOUBLE_EQ(west.max.y, 90.0);
    const vsg::dbox southEast = grid.tileExtent({1, 3, 1});
    EXPECT_DOUBLE_EQ(southEast.min.x, 90.0);
    EXPECT_DOUBLE_EQ(southEast.max.x, 180.0);
    EXPECT_DOUBLE_EQ(southEast.min.y, -90.0);
    EXPECT_DOUBLE_EQ(southEast.max.y, 0.0);
    EXPECT_TRUE(grid.contains({1, 3, 1}));
    EXPECT_FALSE(grid.contains({1, 4, 0}));
    EXPECT_FALSE(grid.contains({1, 0, 2}));
}

TEST(TileGrid, RootTilesDescendWhileOneTileHoldsTheBox)
{
    const ivevsg::TileGrid grid = ivevsg::TileGrid::webMercatorQuad();
    const vsg::dbox box(vsg::dvec3(1.0e6, 3.0e6, 0.0), vsg::dvec3(2.0e6, 4.0e6, 0.0));
    std::vector<ivevsg::TileIndex> roots = grid.rootTilesFor(box, 10);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots.front(), (ivevsg::TileIndex{4, 8, 6}));
    roots = grid.rootTilesFor(box, 2);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots.front(), (ivevsg::TileIndex{2, 2, 1}));

    // a box across the prime meridian never leaves level 0
    const vsg::dbox astride(vsg::dvec3(-1.0e6, 3.0e6, 0.0), vsg::dvec3(1.0e6, 4.0e6, 0.0));
    roots = grid.rootTilesFor(astride, 10);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots.front(), (ivevsg::TileIndex{0, 0, 0}));

    EXPECT_EQ(grid.tilesOverlapping(box, 5).size(), 4u); // columns 16-17, rows 12-13
    EXPECT_TRUE(grid.tilesOverlapping(vsg::dbox(vsg::dvec3(kE + 1.0, 0.0, 0.0), vsg::dvec3(kE + 2.0, 1.0, 0.0)), 3).empty());
}

TEST(TileGrid, TilePathsFlipRowsForTmsLayouts)
{
    ivevsg::TileSetLayout layout;
    layout.grid = ivevsg::TileGrid::webMercatorQuad();
    layout.pathTemplate = "/tiles/{z}/{x}/{y}.png";
    layout.rowsFromBottom = false;
    EXPECT_EQ(layout.tilePath({2, 1, 0}), "/tiles/2/1/0.png");
    layout.rowsFromBottom = true;
    EXPECT_EQ(layout.tilePath({2, 1, 0}), "/tiles/2/1/3.png"); // the north row is the last one counted from the south
    layout.rowsFromBottom = false;
    layout.pathTemplate = "/tiles/{z}/{x}/{-y}.jpg";
    EXPECT_EQ(layout.tilePath({2, 1, 0}), "/tiles/2/1/3.jpg"); // the template may ask for the flip itself
}

TEST(TileGrid, ParsesAMercatorTileMapResource)
{
    ivevsg::TileSetLayout layout;
    std::string error;
    ASSERT_TRUE(ivevsg::parseTileMapResource(kMercatorTileMap, "/data/cyprus", layout, error)) << error;
    EXPECT_EQ(layout.grid.crs, "EPSG:3857");
    EXPECT_EQ(layout.grid.tilesAcrossAtLevel0, 1);
    EXPECT_EQ(layout.grid.tilesDownAtLevel0, 1);
    EXPECT_NEAR(layout.grid.extent.min.x, -kE, 1.0);
    EXPECT_NEAR(layout.grid.extent.max.y, kE, 1.0);
    EXPECT_EQ(layout.grid.tileSizePixels, 256);
    EXPECT_TRUE(layout.rowsFromBottom);
    EXPECT_EQ(layout.minLevel, 3);
    EXPECT_EQ(layout.maxLevel, 5);
    ASSERT_TRUE(layout.bounds.valid());
    EXPECT_DOUBLE_EQ(layout.bounds.min.x, 3600000.0);
    EXPECT_DOUBLE_EQ(layout.bounds.max.y, 4200000.0);
    EXPECT_EQ(layout.pathTemplate, "/data/cyprus/{z}/{x}/{y}.png");
    EXPECT_NE(layout.description.find("cyprus"), std::string::npos);
}

TEST(TileGrid, ParsesAGeodeticTileMapResource)
{
    ivevsg::TileSetLayout layout;
    std::string error;
    ASSERT_TRUE(ivevsg::parseTileMapResource(kGeodeticTileMap, "/data/geo", layout, error)) << error;
    EXPECT_EQ(layout.grid.crs, "EPSG:4326");
    EXPECT_EQ(layout.grid.tilesAcrossAtLevel0, 2);
    EXPECT_EQ(layout.grid.tilesDownAtLevel0, 1);
    EXPECT_DOUBLE_EQ(layout.grid.extent.min.x, -180.0);
    EXPECT_DOUBLE_EQ(layout.grid.extent.min.y, -90.0);
    EXPECT_DOUBLE_EQ(layout.grid.extent.max.x, 180.0);
    EXPECT_DOUBLE_EQ(layout.grid.extent.max.y, 90.0);
    EXPECT_EQ(layout.maxLevel, 1);
    EXPECT_EQ(layout.pathTemplate, "/data/geo/{z}/{x}/{y}.jpg");
    EXPECT_FALSE(ivevsg::parseTileMapResource("<html/>", "/x", layout, error));
}

TEST(TileGrid, DescribesAnXyzDirectoryFromItsFiles)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ivevsg_xyz_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "2" / "1");
    std::filesystem::create_directories(dir / "3" / "2");
    std::ofstream(dir / "2" / "1" / "0.png").put('x');
    std::ofstream(dir / "2" / "1" / "1.png").put('x');
    std::ofstream(dir / "3" / "2" / "1.png").put('x');

    ivevsg::TileSetLayout layout;
    std::string error;
    ASSERT_TRUE(ivevsg::looksLikeTileSet(dir.string()));
    ASSERT_TRUE(ivevsg::describeTileSet(dir.string(), layout, error)) << error;
    EXPECT_EQ(layout.minLevel, 2);
    EXPECT_EQ(layout.maxLevel, 3);
    EXPECT_FALSE(layout.rowsFromBottom);
    EXPECT_EQ(layout.grid.crs, "EPSG:3857");
    EXPECT_EQ(layout.tilePath({2, 1, 1}), (dir / "2/1/1.png").string());
    ASSERT_TRUE(layout.bounds.valid());
    const vsg::dbox expected(ivevsg::TileGrid::webMercatorQuad().tileExtent({2, 1, 1}).min, ivevsg::TileGrid::webMercatorQuad().tileExtent({2, 1, 0}).max);
    EXPECT_NEAR(layout.bounds.min.x, expected.min.x, 1e-6);
    EXPECT_NEAR(layout.bounds.min.y, expected.min.y, 1e-6);
    EXPECT_NEAR(layout.bounds.max.x, expected.max.x, 1e-6);
    EXPECT_NEAR(layout.bounds.max.y, expected.max.y, 1e-6);

    EXPECT_FALSE(ivevsg::describeTileSet((dir / "nothing").string(), layout, error));
    EXPECT_FALSE(ivevsg::looksLikeTileSet((dir / "2/1/0.png").string()));
    EXPECT_TRUE(ivevsg::looksLikeTileSet("/anywhere/{z}/{x}/{y}.png"));
    std::filesystem::remove_all(dir);
}
