#include <ivevsg/LayerOptions.h>
#include <ivevsg/LayerVisibility.h>

#include <gtest/gtest.h>

#include <cmath>

namespace
{
    ivevsg::LayerVisibilityInput member(double scale, const std::string& group, bool on = true)
    {
        ivevsg::LayerVisibilityInput in;
        in.scaleDenominator = scale;
        in.lodGroup = group;
        in.userVisible = on;
        return in;
    }
} // namespace

TEST(LayerVisibility, ScaleDenominatorsMapToZoomLevels)
{
    // 1:10 000 000 at the OGC 0.28 mm pixel is 2800 m/px, between zoom 5 and 6
    const double z10m = ivevsg::zoomLevelForScaleDenominator(1.0e7);
    EXPECT_NEAR(z10m, 5.805, 0.01);
    EXPECT_GT(ivevsg::zoomLevelForScaleDenominator(5.0e7), 3.0);
    EXPECT_LT(ivevsg::zoomLevelForScaleDenominator(5.0e7), z10m);
    EXPECT_NEAR(ivevsg::resolvedUntilZoom(1.0e7, 2.0), z10m + 2.0, 1e-9);
    EXPECT_TRUE(std::isinf(ivevsg::resolvedUntilZoom(0.0, 2.0))) << "no declared scale: resolves everywhere";
}

TEST(LayerVisibility, ZoomWindowsAndTheGlobalFloorGateEveryLayer)
{
    ivevsg::VisibilitySettings settings;
    settings.globalMinZoom = 3.0;
    std::vector<ivevsg::LayerVisibilityInput> layers(3);
    layers[0].minZoom = 0.0;
    layers[0].maxZoom = 30.0;
    layers[1].minZoom = 6.0;
    layers[1].maxZoom = 10.0;
    layers[2].userVisible = false;

    auto at = [&](double zoom) { return ivevsg::evaluateLayerVisibility(layers, zoom, settings); };
    EXPECT_FALSE(at(2.0)[0].shown) << "below the global floor nothing loads";
    EXPECT_TRUE(at(3.0)[0].shown);
    EXPECT_FALSE(at(5.0)[1].shown) << "below its own minimum";
    EXPECT_TRUE(at(6.0)[1].shown);
    EXPECT_TRUE(at(10.0)[1].shown);
    EXPECT_FALSE(at(10.5)[1].shown) << "above its maximum";
    EXPECT_FALSE(at(8.0)[2].shown) << "switched off by the user";
    EXPECT_FALSE(at(8.0)[2].inZoomWindow);
}

TEST(LayerVisibility, AGroupShowsTheCoarsestMemberThatStillResolves)
{
    ivevsg::VisibilitySettings settings;
    settings.resolutionTolerance = 0.0;
    const std::vector<ivevsg::LayerVisibilityInput> layers = {member(1.1e8, "admin0"), member(5.0e7, "admin0"), member(1.0e7, "admin0")};
    const double z110 = ivevsg::zoomLevelForScaleDenominator(1.1e8);
    const double z50 = ivevsg::zoomLevelForScaleDenominator(5.0e7);
    const double z10 = ivevsg::zoomLevelForScaleDenominator(1.0e7);

    auto shown = [&](double zoom) {
        const auto r = ivevsg::evaluateLayerVisibility(layers, zoom, settings);
        return std::vector<bool>{r[0].shown, r[1].shown, r[2].shown};
    };
    EXPECT_EQ(shown(1.0), (std::vector<bool>{true, false, false})) << "zoomed out: the coarsest resolves and is cheapest";
    EXPECT_EQ(shown(z110 + 0.1), (std::vector<bool>{false, true, false})) << "past 1:110m: 1:50m takes over";
    EXPECT_EQ(shown(z50 + 0.1), (std::vector<bool>{false, false, true})) << "past 1:50m: 1:10m takes over";
    EXPECT_EQ(shown(z10 + 5.0), (std::vector<bool>{false, false, true})) << "past every scale the finest stands in";
    const auto deep = ivevsg::evaluateLayerVisibility(layers, z10 + 5.0, settings);
    EXPECT_TRUE(deep[2].standingIn);
    EXPECT_FALSE(deep[2].resolved);
    const auto mid = ivevsg::evaluateLayerVisibility(layers, z110 + 0.1, settings);
    EXPECT_TRUE(mid[0].replacedInGroup);
    EXPECT_TRUE(mid[2].replacedInGroup);
    EXPECT_FALSE(mid[1].replacedInGroup);

    // the tolerance stretches each member's reach
    settings.resolutionTolerance = 2.0;
    EXPECT_EQ(shown(z110 + 1.0), (std::vector<bool>{true, false, false}));
}

TEST(LayerVisibility, GroupMembersThatAreOffOrOutOfTheirWindowDoNotTakePart)
{
    ivevsg::VisibilitySettings settings;
    settings.resolutionTolerance = 0.0;
    std::vector<ivevsg::LayerVisibilityInput> layers = {member(1.1e8, "g"), member(1.0e7, "g")};
    layers[1].userVisible = false;
    auto r = ivevsg::evaluateLayerVisibility(layers, 12.0, settings);
    EXPECT_TRUE(r[0].shown) << "the only member left stands in, however far past its scale";
    EXPECT_TRUE(r[0].standingIn);
    EXPECT_FALSE(r[1].shown);

    layers[1].userVisible = true;
    layers[1].maxZoom = 8.0;
    r = ivevsg::evaluateLayerVisibility(layers, 12.0, settings);
    EXPECT_TRUE(r[0].shown);
    EXPECT_FALSE(r[1].shown);

    // a member without a declared scale follows its own window and never displaces the others
    layers.push_back(member(0.0, "g"));
    r = ivevsg::evaluateLayerVisibility(layers, 1.0, settings);
    EXPECT_TRUE(r[0].shown);
    EXPECT_TRUE(r[2].shown);
    EXPECT_TRUE(r[2].resolved);
}

TEST(LayerVisibility, LayersInDifferentGroupsOrNoneAreIndependent)
{
    ivevsg::VisibilitySettings settings;
    const std::vector<ivevsg::LayerVisibilityInput> layers = {member(1.1e8, "a"), member(1.1e8, "b"), member(1.1e8, "")};
    const auto r = ivevsg::evaluateLayerVisibility(layers, 15.0, settings);
    for (const auto& result : r)
    {
        EXPECT_TRUE(result.shown);
        EXPECT_FALSE(result.resolved);
        EXPECT_FALSE(result.replacedInGroup);
    }
    EXPECT_TRUE(r[0].standingIn);
    EXPECT_FALSE(r[2].standingIn) << "no group, so nothing to stand in for";
}

TEST(LayerOptions, SpecsRoundTripAndScalesParse)
{
    std::string path, error;
    ivevsg::LayerOptions options;
    ASSERT_TRUE(ivevsg::parseLayerSpec("/data/ne_10m_admin_0_countries.shp?minzoom=3&maxzoom=12&scale=50m&group=admin0&colour=ff8000&width=2.5&kind=vector&cap=9", path, options, error)) << error;
    EXPECT_EQ(path, "/data/ne_10m_admin_0_countries.shp");
    EXPECT_EQ(options.minZoom, 3.0);
    EXPECT_EQ(options.maxZoom, 12.0);
    EXPECT_EQ(options.scaleDenominator, 5.0e7);
    EXPECT_EQ(options.lodGroup, "admin0");
    EXPECT_NEAR(options.vector.colour.r, 1.0f, 1e-6);
    EXPECT_NEAR(options.vector.colour.g, 128.0f / 255.0f, 1e-6);
    EXPECT_NEAR(options.vector.colour.b, 0.0f, 1e-6);
    EXPECT_EQ(options.vector.lineWidth, 2.5f);
    EXPECT_EQ(options.kind, ivevsg::LayerKind::Vector);
    EXPECT_EQ(options.detailLevelCap, 9);

    ivevsg::LayerOptions again;
    std::string path2;
    ASSERT_TRUE(ivevsg::parseLayerSpec(ivevsg::layerSpec(path, options), path2, again, error)) << error;
    EXPECT_EQ(path2, path);
    EXPECT_EQ(again.minZoom, options.minZoom);
    EXPECT_EQ(again.scaleDenominator, options.scaleDenominator);
    EXPECT_EQ(again.lodGroup, options.lodGroup);
    EXPECT_EQ(again.vector.lineWidth, options.vector.lineWidth);
    EXPECT_EQ(ivevsg::layerSpec("/plain.shp", ivevsg::LayerOptions{}), "/plain.shp");

    ivevsg::LayerOptions terrain;
    ASSERT_TRUE(ivevsg::parseLayerSpec("cell.dt2?kind=terrain&exaggeration=2&interp=nearest&oversample=6&cells=16&texture=64&hillshade=0", path, terrain, error)) << error;
    EXPECT_EQ(terrain.kind, ivevsg::LayerKind::Terrain);
    EXPECT_EQ(terrain.terrain.exaggeration, 2.0);
    EXPECT_EQ(terrain.terrain.interpolation, ivevsg::HeightInterpolation::Nearest);
    EXPECT_EQ(terrain.terrain.oversampleLevels, 6);
    EXPECT_EQ(terrain.terrain.cellsPerTile, 16);
    EXPECT_EQ(terrain.terrain.texturePixels, 64);
    EXPECT_FALSE(terrain.terrain.hillshade);

    EXPECT_FALSE(ivevsg::parseLayerSpec("a.shp?bogus=1", path, options, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(ivevsg::parseLayerSpec("a.shp?minzoom=abc", path, options, error));

    EXPECT_EQ(ivevsg::scaleDenominatorFromName("/x/ne_10m_admin_0_countries.shp").value_or(0.0), 1.0e7);
    EXPECT_EQ(ivevsg::scaleDenominatorFromName("ne_110m_admin_1_states_provinces.shp").value_or(0.0), 1.1e8);
    EXPECT_FALSE(ivevsg::scaleDenominatorFromName("counties.shp"));
    EXPECT_EQ(ivevsg::parseScaleDenominator("1:50000000").value_or(0.0), 5.0e7);
    EXPECT_EQ(ivevsg::parseScaleDenominator("50m").value_or(0.0), 5.0e7);
    EXPECT_EQ(ivevsg::parseScaleDenominator("250k").value_or(0.0), 2.5e5);
    EXPECT_EQ(ivevsg::parseScaleDenominator("1:1 000 000").value_or(0.0), 1.0e6);
    EXPECT_FALSE(ivevsg::parseScaleDenominator("big"));
    EXPECT_EQ(ivevsg::formatScaleDenominator(1.0e7), "1:10000000");
    EXPECT_EQ(ivevsg::lodGroupFromName("/x/ne_10m_admin_0_countries.shp"), "admin_0_countries");
    EXPECT_EQ(ivevsg::lodGroupFromName("/x/ne_50m_admin_0_countries.shp"), "admin_0_countries");
    EXPECT_EQ(ivevsg::lodGroupFromName("/x/counties.shp"), "counties");
}
