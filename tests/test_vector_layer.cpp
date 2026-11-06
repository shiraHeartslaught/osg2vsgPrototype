#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/LoaderKeys.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TileScheme.h>
#include <ivevsg/VectorLayerReader.h>
#include <ivevsg/VectorSource.h>

#include <gdal_priv.h>
#include <gtest/gtest.h>
#include <ogr_api.h>
#include <ogrsf_frmts.h>
#include <ogr_spatialref.h>
#include <vsg/all.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace
{
    using Line = std::vector<ivevsg::GeoPoint>;

    /// A shapefile with one layer of the given geometry type (shapefiles hold one type per file).
    /// Coordinates are given in the CRS of `epsg` (lon/lat for 4326, metres for 3857).
    std::filesystem::path writeShapefile(const std::filesystem::path& file, int epsg, OGRwkbGeometryType type, const std::vector<std::vector<Line>>& features)
    {
        GDALAllRegister();
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (!driver) return {};
        std::filesystem::create_directories(file.parent_path());
        GDALDataset* dataset = driver->Create(file.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (!dataset) return {};
        OGRSpatialReference srs;
        if (epsg > 0) srs.importFromEPSG(epsg);
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRLayer* layer = dataset->CreateLayer("features", epsg > 0 ? &srs : nullptr, type, nullptr);
        if (!layer)
        {
            GDALClose(dataset);
            return {};
        }
        for (const std::vector<Line>& parts : features)
        {
            OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
            OGRGeometry* geometry = nullptr;
            if (type == wkbPoint)
            {
                geometry = new OGRPoint(parts.front().front().longitude, parts.front().front().latitude);
            }
            else if (type == wkbLineString)
            {
                auto* line = new OGRLineString();
                for (const ivevsg::GeoPoint& p : parts.front()) line->addPoint(p.longitude, p.latitude);
                geometry = line;
            }
            else
            {
                auto* polygon = new OGRPolygon();
                for (const Line& ring : parts)
                {
                    auto* r = new OGRLinearRing();
                    for (const ivevsg::GeoPoint& p : ring) r->addPoint(p.longitude, p.latitude);
                    r->closeRings();
                    polygon->addRingDirectly(r);
                }
                geometry = polygon;
            }
            feature->SetGeometryDirectly(geometry);
            const OGRErr created = layer->CreateFeature(feature);
            OGRFeature::DestroyFeature(feature);
            if (created != OGRERR_NONE)
            {
                GDALClose(dataset);
                return {};
            }
        }
        GDALClose(dataset);
        return file;
    }

    struct WorldLines : public vsg::ConstVisitor
    {
        std::vector<vsg::dmat4> stack{vsg::dmat4()};
        std::vector<vsg::dvec3> positions;
        int draws = 0, pagedLODs = 0, cullNodes = 0;
        std::vector<const vsg::PagedLOD*> plods;
        void apply(const vsg::Node& node) override { node.traverse(*this); }
        void apply(const vsg::MatrixTransform& transform) override
        {
            stack.push_back(stack.back() * transform.matrix);
            transform.traverse(*this);
            stack.pop_back();
        }
        void apply(const vsg::PagedLOD& plod) override
        {
            ++pagedLODs;
            plods.push_back(&plod);
            plod.traverse(*this);
        }
        void apply(const vsg::CullNode& cull) override
        {
            ++cullNodes;
            cull.traverse(*this);
        }
        void apply(const vsg::VertexDraw& draw) override
        {
            ++draws;
            if (draw.arrays.empty() || !draw.arrays[0]->data) return;
            if (auto vertices = draw.arrays[0]->data.cast<vsg::vec3Array>())
                for (const vsg::vec3& v : *vertices) positions.push_back(stack.back() * vsg::dvec3(v));
        }
    };

    WorldLines linesOf(const vsg::Node& node)
    {
        WorldLines visitor;
        node.accept(visitor);
        return visitor;
    }

    std::string layerIdOf(const vsg::PagedLOD& plod)
    {
        const std::string name = plod.filename.string();
        const std::size_t start = name.find("://") + 3;
        return name.substr(start, name.find('/', start) - start);
    }

    double ramp(double lon, double lat) { return 100.0 + 1000.0 * (lon - 20.0) + 500.0 * (lat - 10.0); }

    std::filesystem::path writeDted(const std::filesystem::path& file, int lon0, int lat0)
    {
        GDALAllRegister();
        GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
        GDALDriver* dted = GetGDALDriverManager()->GetDriverByName("DTED");
        if (!mem || !dted) return {};
        const int n = 121; // level 0: 30 arc seconds, quick to write
        const double step = 1.0 / 120.0;
        GDALDataset* image = mem->Create("", n, n, 1, GDT_Int16, nullptr);
        double geo[6] = {lon0 - 0.5 * step, step, 0.0, lat0 + 1.0 + 0.5 * step, 0.0, -step};
        image->SetGeoTransform(geo);
        OGRSpatialReference srs;
        srs.importFromEPSG(4326);
        char* wkt = nullptr;
        srs.exportToWkt(&wkt);
        image->SetProjection(wkt);
        CPLFree(wkt);
        std::vector<int16_t> posts(static_cast<std::size_t>(n) * n);
        for (int row = 0; row < n; ++row)
            for (int col = 0; col < n; ++col) posts[static_cast<std::size_t>(row) * n + col] = static_cast<int16_t>(std::lround(ramp(lon0 + col * step, lat0 + 1.0 - row * step)));
        if (image->RasterIO(GF_Write, 0, 0, n, n, posts.data(), n, n, GDT_Int16, 1, nullptr, 0, 0, 0, nullptr) != CE_None)
        {
            GDALClose(image);
            return {};
        }
        std::filesystem::create_directories(file.parent_path());
        GDALDataset* out = dted->CreateCopy(file.string().c_str(), image, FALSE, nullptr, nullptr, nullptr);
        const bool ok = out != nullptr;
        if (out) GDALClose(out);
        GDALClose(image);
        return ok ? file : std::filesystem::path();
    }

    class VectorLayerTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            dir = std::filesystem::temp_directory_path() / ("ivevsg_vector_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
        }
        void TearDown() override { std::filesystem::remove_all(dir); }

        ivevsg::SceneLoader loaderFor(const std::string& targetCrs, std::function<void(ivevsg::LoaderSettings&)> tweak = {})
        {
            ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
            settings.targetCrs = targetCrs;
            if (tweak) tweak(settings);
            return ivevsg::SceneLoader(settings);
        }

        std::filesystem::path dir;
    };
} // namespace

TEST(VectorGeometry, DensifySimplifyAndClipBehave)
{
    const Line diagonal = {{0.0, 0.0, 0.0}, {10.0, 5.0, 0.0}};
    const Line dense = ivevsg::densifyLine(diagonal, 1.0);
    EXPECT_EQ(dense.size(), 11u) << "ten pieces of one degree along the longer axis";
    for (const ivevsg::GeoPoint& p : dense) EXPECT_NEAR(p.latitude, p.longitude * 0.5, 1e-9) << "inserted points lie on the segment";
    EXPECT_EQ(ivevsg::densifyLine(diagonal, 100.0).size(), 2u);

    // a wiggle of 0.01 degrees around a straight line, then a real corner
    Line wiggly;
    for (int i = 0; i <= 10; ++i) wiggly.push_back({static_cast<double>(i), (i % 2) * 0.01, 0.0});
    wiggly.push_back({10.0, 5.0, 0.0});
    const Line simple = ivevsg::simplifyLine(wiggly, 0.05);
    ASSERT_EQ(simple.size(), 3u) << "start, the corner, the end";
    EXPECT_EQ(simple[1].longitude, 10.0);
    EXPECT_EQ(simple[1].latitude, 0.0);
    EXPECT_EQ(ivevsg::simplifyLine(wiggly, 0.001).size(), wiggly.size()) << "a tolerance below the wiggle keeps everything";

    const ivevsg::GeoExtent box{0.0, 0.0, 10.0, 10.0};
    // in, out, back in: two runs, cut at the box edges
    const Line inOutIn = {{2.0, 5.0, 0.0}, {15.0, 5.0, 0.0}, {15.0, 7.0, 0.0}, {3.0, 7.0, 0.0}};
    const auto runs = ivevsg::clipLine(inOutIn, box);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].front().longitude, 2.0);
    EXPECT_NEAR(runs[0].back().longitude, 10.0, 1e-9);
    EXPECT_NEAR(runs[1].front().longitude, 10.0, 1e-9);
    EXPECT_EQ(runs[1].back().longitude, 3.0);
    // a polyline staying inside is one run with every vertex
    const Line inside = {{1.0, 1.0, 0.0}, {2.0, 3.0, 0.0}, {4.0, 4.0, 0.0}};
    const auto whole = ivevsg::clipLine(inside, box);
    ASSERT_EQ(whole.size(), 1u);
    EXPECT_EQ(whole[0].size(), 3u);
    // entirely outside: nothing; crossing a corner: one short run
    EXPECT_TRUE(ivevsg::clipLine({{20.0, 20.0, 0.0}, {30.0, 25.0, 0.0}}, box).empty());
    const auto corner = ivevsg::clipLine({{-5.0, 5.0, 0.0}, {5.0, 15.0, 0.0}}, box);
    ASSERT_EQ(corner.size(), 1u);
    EXPECT_NEAR(corner[0].front().longitude, 0.0, 1e-9);
    EXPECT_NEAR(corner[0].back().latitude, 10.0, 1e-9);

    EXPECT_TRUE(ivevsg::extentsOverlap(box, ivevsg::GeoExtent{9.0, 9.0, 12.0, 12.0}));
    EXPECT_FALSE(ivevsg::extentsOverlap(box, ivevsg::GeoExtent{11.0, 0.0, 12.0, 5.0}));
    EXPECT_TRUE(ivevsg::extentContains(box, 5.0, 5.0));
    EXPECT_FALSE(ivevsg::extentContains(box, 11.0, 5.0));
    EXPECT_TRUE(ivevsg::looksLikeVector("/a/b/ne_10m_admin_0_countries.shp"));
    EXPECT_TRUE(ivevsg::looksLikeVector("/a/roads.geojson"));
    EXPECT_FALSE(ivevsg::looksLikeVector("/a/image.tif"));
}

TEST_F(VectorLayerTest, ReadsShapefilesInDegreesAndReprojectsProjectedOnes)
{
    const Line line = {{20.0, 10.0, 0.0}, {20.5, 10.5, 0.0}, {21.0, 10.0, 0.0}};
    const std::filesystem::path degrees = writeShapefile(dir / "deg" / "lines.shp", 4326, wkbLineString, {{line}});
    ASSERT_FALSE(degrees.empty()) << "OGR could not write a shapefile";
    ivevsg::VectorData data;
    std::string error;
    ASSERT_TRUE(ivevsg::readVectorData(degrees.string(), "", data, error)) << error;
    ASSERT_EQ(data.features.size(), 1u);
    EXPECT_EQ(data.features[0].type, ivevsg::FeatureType::Line);
    ASSERT_EQ(data.features[0].parts.size(), 1u);
    ASSERT_EQ(data.features[0].parts[0].size(), 3u);
    EXPECT_NEAR(data.features[0].parts[0][1].longitude, 20.5, 1e-9);
    EXPECT_NEAR(data.features[0].parts[0][1].latitude, 10.5, 1e-9);
    EXPECT_NEAR(data.extent.minLongitude, 20.0, 1e-9);
    EXPECT_NEAR(data.extent.maxLatitude, 10.5, 1e-9);
    EXPECT_EQ(data.vertexCount, 3u);
    EXPECT_EQ(data.layerCount, 1u);
    EXPECT_NE(data.sourceCrs.find("4326"), std::string::npos);

    // the same line in Web Mercator metres comes back in degrees
    ivevsg::MercatorProjection mercator;
    Line metres;
    for (const ivevsg::GeoPoint& p : line)
    {
        const vsg::dvec3 w = mercator.geoToWorld(p);
        metres.push_back({w.x, w.y, 0.0});
    }
    const std::filesystem::path projected = writeShapefile(dir / "merc" / "lines.shp", 3857, wkbLineString, {{metres}});
    ASSERT_FALSE(projected.empty());
    ASSERT_TRUE(ivevsg::readVectorData(projected.string(), "", data, error)) << error;
    ASSERT_EQ(data.features[0].parts[0].size(), 3u);
    EXPECT_NEAR(data.features[0].parts[0][1].longitude, 20.5, 1e-6);
    EXPECT_NEAR(data.features[0].parts[0][1].latitude, 10.5, 1e-6);

    // a polygon with a hole: two rings; points: one part each
    const Line outer = {{0.0, 0.0, 0.0}, {4.0, 0.0, 0.0}, {4.0, 4.0, 0.0}, {0.0, 4.0, 0.0}};
    const Line hole = {{1.0, 1.0, 0.0}, {2.0, 1.0, 0.0}, {2.0, 2.0, 0.0}, {1.0, 2.0, 0.0}};
    const std::filesystem::path polygons = writeShapefile(dir / "poly" / "areas.shp", 4326, wkbPolygon, {{outer, hole}});
    ASSERT_TRUE(ivevsg::readVectorData(polygons.string(), "", data, error)) << error;
    ASSERT_EQ(data.features.size(), 1u);
    EXPECT_EQ(data.features[0].type, ivevsg::FeatureType::Polygon);
    EXPECT_EQ(data.features[0].parts.size(), 2u);
    EXPECT_EQ(data.features[0].parts[0].size(), 5u) << "rings come back closed";
    const std::filesystem::path points = writeShapefile(dir / "pts" / "cities.shp", 4326, wkbPoint, {{{{20.2, 10.2, 0.0}}}, {{{20.8, 10.8, 0.0}}}});
    ASSERT_TRUE(ivevsg::readVectorData(points.string(), "", data, error)) << error;
    EXPECT_EQ(data.features.size(), 2u);
    EXPECT_EQ(data.features[0].type, ivevsg::FeatureType::Point);
    // an override CRS wins over the file's own
    ASSERT_TRUE(ivevsg::readVectorData(projected.string(), "EPSG:3857", data, error)) << error;
    EXPECT_EQ(data.sourceCrs, "EPSG:3857");
    EXPECT_FALSE(ivevsg::readVectorData((dir / "missing.shp").string(), "", data, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(VectorLayerTest, BuildsPagedLineTilesInBothFrames)
{
    const Line line = {{20.0, 10.0, 0.0}, {20.5, 10.5, 0.0}, {21.0, 10.0, 0.0}};
    const std::filesystem::path shp = writeShapefile(dir / "ne_50m_test_lines.shp", 4326, wkbLineString, {{line}});
    ASSERT_FALSE(shp.empty());
    ASSERT_TRUE(ivevsg::VectorLayerReader::handles(vsg::Path(shp.string()), {}));

    ivevsg::SceneLoader map = loaderFor("EPSG:3857");
    ivevsg::LayerSource source;
    source.path = shp.string();
    source.options.vector.colour = vsg::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    source.options.vector.lineWidth = 3.0f;
    const ivevsg::LoadResult onMap = map.load(source);
    ASSERT_TRUE(onMap) << onMap.error;
    EXPECT_EQ(onMap.kind, "vector");
    EXPECT_EQ(onMap.targetCrs, "EPSG:3857");
    EXPECT_EQ(onMap.declaredScale, 5.0e7) << "the ne_50m_ prefix declares 1:50 000 000";
    EXPECT_EQ(onMap.maxLevel, 16);
    EXPECT_NE(onMap.info.find("1 features"), std::string::npos);

    const WorldLines lines = linesOf(*onMap.node);
    EXPECT_GE(lines.draws, 1);
    EXPECT_GE(lines.pagedLODs, 1) << "the root tile pages finer tiles";
    ASSERT_FALSE(lines.positions.empty());
    EXPECT_EQ(lines.positions.size() % 2, 0u) << "a line list";
    ivevsg::MercatorProjection mercator;
    const vsg::dvec3 sw = mercator.geoToWorld(ivevsg::GeoPoint{20.0, 10.0, 0.0}), ne = mercator.geoToWorld(ivevsg::GeoPoint{21.0, 10.5, 0.0});
    for (const vsg::dvec3& p : lines.positions)
    {
        EXPECT_NEAR(p.z, 0.0, 1e-3) << "on the map plane without terrain";
        EXPECT_GE(p.x, sw.x - 1.0);
        EXPECT_LE(p.x, ne.x + 1.0);
        EXPECT_GE(p.y, sw.y - 1.0);
        EXPECT_LE(p.y, ne.y + 1.0);
    }
    // the shared colour and width hang off the root for live edits; without wideLines the width clamps to 1
    auto colours = onMap.node->getRefObject<vsg::vec4Array>(ivevsg::loaderKeys::nodeVectorColour);
    ASSERT_TRUE(colours);
    EXPECT_EQ(colours->at(0), vsg::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_TRUE(colours->dynamic());
    auto width = onMap.node->getRefObject<vsg::SetLineWidth>(ivevsg::loaderKeys::nodeVectorLineWidth);
    ASSERT_TRUE(width);
    EXPECT_EQ(width->lineWidth, 1.0f);

    ivevsg::SceneLoader wide = loaderFor("EPSG:3857", [](ivevsg::LoaderSettings& s) { s.wideLines = true; });
    const ivevsg::LoadResult wideLoad = wide.load(source);
    ASSERT_TRUE(wideLoad) << wideLoad.error;
    EXPECT_EQ(wideLoad.node->getRefObject<vsg::SetLineWidth>(ivevsg::loaderKeys::nodeVectorLineWidth)->lineWidth, 3.0f);

    // on the globe the same vertices sit on the ellipsoid
    ivevsg::SceneLoader globe = loaderFor("EPSG:4978");
    const ivevsg::LoadResult onGlobe = globe.load(shp.string());
    ASSERT_TRUE(onGlobe) << onGlobe.error;
    EXPECT_EQ(onGlobe.targetCrs, "EPSG:4978");
    ivevsg::GeocentricProjection ecef;
    const WorldLines globeLines = linesOf(*onGlobe.node);
    ASSERT_FALSE(globeLines.positions.empty());
    EXPECT_GT(globeLines.positions.size(), 4u) << "long edges are densified so they follow the curve";
    for (const vsg::dvec3& p : globeLines.positions)
    {
        const ivevsg::GeoPoint g = ecef.worldToGeo(p);
        EXPECT_NEAR(g.altitude, 0.0, 0.5);
        EXPECT_GE(g.longitude, 19.99);
        EXPECT_LE(g.longitude, 21.01);
        EXPECT_GE(g.latitude, 9.99);
        EXPECT_LE(g.latitude, 10.51);
    }
}

TEST_F(VectorLayerTest, TilesHoldOnlyTheirOwnPartOfALineAndStopAtTheCap)
{
    // a line across 20 degrees of longitude, straddling several level-4 tiles
    const Line line = {{0.5, 10.0, 0.0}, {20.5, 10.0, 0.0}};
    const std::filesystem::path shp = writeShapefile(dir / "long.shp", 4326, wkbLineString, {{line}});
    ASSERT_FALSE(shp.empty());
    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");
    ivevsg::LayerSource source;
    source.path = shp.string();
    source.options.detailLevelCap = 5;
    const ivevsg::LoadResult loaded = loader.load(source);
    ASSERT_TRUE(loaded) << loaded.error;
    EXPECT_EQ(loaded.maxLevel, 5);
    const WorldLines root = linesOf(*loaded.node);
    ASSERT_FALSE(root.plods.empty());
    const std::string id = layerIdOf(*root.plods.front());
    const vsg::ref_ptr<vsg::Options> options = root.plods.front()->options;

    ivevsg::TileScheme scheme;
    ivevsg::MercatorProjection mercator;
    const vsg::dvec3 middle = mercator.geoToWorld(ivevsg::GeoPoint{10.0, 10.0, 0.0});
    const ivevsg::TileIndex parent = scheme.tileAt(middle.x, middle.y, 4);
    auto children = vsg::read_cast<vsg::Node>(vsg::Path("ivevsg-vector://" + id + "/4/" + std::to_string(parent.x) + "/" + std::to_string(parent.y)), options);
    ASSERT_TRUE(children);
    const WorldLines level5 = linesOf(*children);
    EXPECT_GT(level5.draws, 0);
    EXPECT_EQ(level5.pagedLODs, 0) << "level 5 is the cap: leaves";
    EXPECT_EQ(level5.cullNodes, level5.draws);
    const vsg::dbox parentBox = scheme.tileExtent(parent);
    const double slack = (parentBox.max.x - parentBox.min.x) * 0.02;
    for (const vsg::dvec3& p : level5.positions)
    {
        EXPECT_GE(p.x, parentBox.min.x - slack) << "clipped to the parent's footprint (plus a hair of overlap)";
        EXPECT_LE(p.x, parentBox.max.x + slack);
    }
    // a tile the line misses yields nothing
    const ivevsg::TileIndex far = scheme.tileAt(mercator.geoToWorld(ivevsg::GeoPoint{100.0, -40.0, 0.0}).x, mercator.geoToWorld(ivevsg::GeoPoint{100.0, -40.0, 0.0}).y, 4);
    auto empty = vsg::read_cast<vsg::Node>(vsg::Path("ivevsg-vector://" + id + "/4/" + std::to_string(far.x) + "/" + std::to_string(far.y)), options);
    ASSERT_TRUE(empty);
    EXPECT_EQ(linesOf(*empty).draws, 0);
    // once the layer's node is gone, child requests are refused rather than served from a dead layer
    EXPECT_EQ(loader.options()->getRefObject<vsg::Object>("none"), nullptr);
}

TEST_F(VectorLayerTest, DrapesOverTerrainFromTheDrapeLevelOn)
{
    const std::filesystem::path cell = writeDted(dir / "n10e020.dt0", 20, 10);
    ASSERT_FALSE(cell.empty());
    const Line line = {{20.2, 10.2, 0.0}, {20.8, 10.8, 0.0}};
    const std::filesystem::path shp = writeShapefile(dir / "border.shp", 4326, wkbLineString, {{line}});
    ASSERT_FALSE(shp.empty());

    // draping from level 0 on: every root vertex rides the ramp (plus a small clearance)
    ivevsg::SceneLoader draped = loaderFor("EPSG:4978", [](ivevsg::LoaderSettings& s) {
        s.drape = true;
        s.drapeMinZoom = 0.0;
    });
    const ivevsg::LoadResult terrain = draped.load(cell.string());
    ASSERT_TRUE(terrain) << terrain.error;
    ASSERT_TRUE(draped.elevation()->hasTerrain());
    const ivevsg::LoadResult lines = draped.load(shp.string());
    ASSERT_TRUE(lines) << lines.error;
    ivevsg::GeocentricProjection ecef;
    const WorldLines world = linesOf(*lines.node);
    ASSERT_FALSE(world.positions.empty());
    for (const vsg::dvec3& p : world.positions)
    {
        const ivevsg::GeoPoint g = ecef.worldToGeo(p);
        const double expected = ramp(g.longitude, g.latitude);
        EXPECT_GT(g.altitude, expected) << "lifted clear of the surface";
        EXPECT_LT(g.altitude, expected + 300.0) << "but close to it";
    }

    // below the drape level the same layer stays on the ellipsoid
    ivevsg::SceneLoader notYet = loaderFor("EPSG:4978", [](ivevsg::LoaderSettings& s) {
        s.drape = true;
        s.drapeMinZoom = 30.0;
    });
    const ivevsg::LoadResult terrain2 = notYet.load(cell.string());
    ASSERT_TRUE(terrain2) << terrain2.error;
    const ivevsg::LoadResult flat = notYet.load(shp.string());
    ASSERT_TRUE(flat) << flat.error;
    for (const vsg::dvec3& p : linesOf(*flat.node).positions) EXPECT_NEAR(ecef.worldToGeo(p).altitude, 0.0, 0.5);

    // draping switched off: on the ellipsoid as well
    ivevsg::SceneLoader off = loaderFor("EPSG:4978", [](ivevsg::LoaderSettings& s) {
        s.drape = false;
        s.drapeMinZoom = 0.0;
    });
    const ivevsg::LoadResult terrain3 = off.load(cell.string());
    ASSERT_TRUE(terrain3) << terrain3.error;
    const ivevsg::LoadResult undraped = off.load(shp.string());
    ASSERT_TRUE(undraped) << undraped.error;
    for (const vsg::dvec3& p : linesOf(*undraped.node).positions) EXPECT_NEAR(ecef.worldToGeo(p).altitude, 0.0, 0.5);

    // and without any terrain loaded there is nothing to drape on
    ivevsg::SceneLoader alone = loaderFor("EPSG:4978", [](ivevsg::LoaderSettings& s) { s.drapeMinZoom = 0.0; });
    const ivevsg::LoadResult bare = alone.load(shp.string());
    ASSERT_TRUE(bare) << bare.error;
    for (const vsg::dvec3& p : linesOf(*bare.node).positions) EXPECT_NEAR(ecef.worldToGeo(p).altitude, 0.0, 0.5);
}

TEST_F(VectorLayerTest, PointsBecomeMarkersAndForcedKindsAreHonoured)
{
    const std::filesystem::path points = writeShapefile(dir / "cities.shp", 4326, wkbPoint, {{{{20.2, 10.2, 0.0}}}, {{{20.8, 10.8, 0.0}}}});
    ASSERT_FALSE(points.empty());
    ivevsg::SceneLoader loader = loaderFor("EPSG:3857");
    const ivevsg::LoadResult loaded = loader.load(points.string());
    ASSERT_TRUE(loaded) << loaded.error;
    const WorldLines lines = linesOf(*loaded.node);
    EXPECT_EQ(lines.positions.size(), 8u) << "two crosses of two segments each";
    ivevsg::MercatorProjection mercator;
    const vsg::dvec3 city = mercator.geoToWorld(ivevsg::GeoPoint{20.2, 10.2, 0.0});
    bool nearCity = false;
    for (const vsg::dvec3& p : lines.positions)
        if (vsg::length(p - city) < 100000.0) nearCity = true;
    EXPECT_TRUE(nearCity);

    // asking for a raster reading of a shapefile is declined by the vector reader (whatever another
    // reader, e.g. OSG's shp plugin, then makes of it, it is not a vector layer)
    ivevsg::LayerSource asRaster;
    asRaster.path = points.string();
    asRaster.options.kind = ivevsg::LayerKind::Raster;
    const ivevsg::LoadResult declined = loader.load(asRaster);
    EXPECT_NE(declined.kind, "vector");
    EXPECT_FALSE(declined.node && declined.node->getRefObject<vsg::SetLineWidth>(ivevsg::loaderKeys::nodeVectorLineWidth));
    // and the vector reader itself says so
    auto options = vsg::Options::create();
    options->setValue(ivevsg::loaderKeys::layerKind, std::string("raster"));
    EXPECT_FALSE(ivevsg::VectorLayerReader::handles(vsg::Path(points.string()), options));
    options->setValue(ivevsg::loaderKeys::layerKind, std::string("vector"));
    EXPECT_TRUE(ivevsg::VectorLayerReader::handles(vsg::Path((dir / "anything.bin").string()), options));
}
