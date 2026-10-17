#include <ivevsg/SceneBounds.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/TileSetBuilder.h>

#include <gdal_priv.h>
#include <gtest/gtest.h>
#include <vsg/all.h>

#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace
{
    constexpr double kE = 20037508.342789244;

    /// 512x512 RGB GeoTIFF covering the whole EPSG:3857 world square: red grows eastwards, green southwards.
    std::filesystem::path writeSyntheticWorld(const std::filesystem::path& dir)
    {
        GDALAllRegister();
        const std::filesystem::path file = dir / "synthetic_world_3857.tif";
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        if (!driver) return {};
        GDALDataset* dataset = driver->Create(file.string().c_str(), 512, 512, 3, GDT_Byte, nullptr);
        if (!dataset) return {};
        double geo[6] = {-kE, 2.0 * kE / 512.0, 0.0, kE, 0.0, -2.0 * kE / 512.0};
        dataset->SetGeoTransform(geo);
        std::vector<unsigned char> rgb(512 * 512 * 3);
        for (int y = 0; y < 512; ++y)
            for (int x = 0; x < 512; ++x)
            {
                unsigned char* px = rgb.data() + (static_cast<std::size_t>(y) * 512 + x) * 3;
                px[0] = static_cast<unsigned char>(x / 2);
                px[1] = static_cast<unsigned char>(y / 2);
                px[2] = 128;
            }
        const CPLErr err = dataset->RasterIO(GF_Write, 0, 0, 512, 512, rgb.data(), 512, 512, GDT_Byte, 3, nullptr, 3, 512 * 3, 1, nullptr);
        GDALClose(dataset);
        return err == CE_None ? file : std::filesystem::path();
    }

    struct Census : public vsg::ConstVisitor
    {
        int pagedLODs = 0;
        int draws = 0;
        void apply(const vsg::Node& node) override { node.traverse(*this); }
        void apply(const vsg::PagedLOD& plod) override
        {
            ++pagedLODs;
            plod.traverse(*this);
        }
        void apply(const vsg::VertexIndexDraw&) override { ++draws; }
    };

    Census census(vsg::Node& node)
    {
        Census c;
        node.accept(c);
        return c;
    }

    class TileSetBuilderTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            dir = std::filesystem::temp_directory_path() / "ivevsg_tileset_test";
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
            source = writeSyntheticWorld(dir);
            ASSERT_FALSE(source.empty()) << "GDAL could not write the synthetic GeoTIFF";
        }
        void TearDown() override { std::filesystem::remove_all(dir); }

        std::filesystem::path dir;
        std::filesystem::path source;
    };
} // namespace

TEST(TileSetBuilder, SubdivisionThresholdIsTheTileDiagonalInPixels)
{
    EXPECT_NEAR(ivevsg::subdivisionPixelThreshold(256, 1.0), 256.0 * std::sqrt(2.0), 1e-9);
    EXPECT_NEAR(ivevsg::subdivisionPixelThreshold(256, 2.0), 128.0 * std::sqrt(2.0), 1e-9); // bias 2: hand over at half the size
    EXPECT_NEAR(ivevsg::subdivisionPixelThreshold(64, 1.0), 64.0 * std::sqrt(2.0), 1e-9);
}

TEST(TileSetBuilder, RejectsAnUnreadableSource)
{
    ivevsg::TileSetSettings settings;
    settings.sourceRaster = "/nonexistent/raster.tif";
    settings.outputDirectory = (std::filesystem::temp_directory_path() / "ivevsg_tileset_none").string();
    const ivevsg::TileSetReport report = ivevsg::buildTileSet(settings);
    EXPECT_FALSE(report);
    EXPECT_FALSE(report.error.empty());
}

TEST_F(TileSetBuilderTest, WritesTheExpectedPyramidOfFiles)
{
    ivevsg::TileSetSettings settings;
    settings.sourceRaster = source.string();
    settings.outputDirectory = (dir / "tiles").string();
    settings.maxLevel = 2;
    settings.tileSizePixels = 64;
    settings.threads = 1;
    settings.compressed = false;
    std::size_t progressCalls = 0;
    settings.progress = [&](std::size_t, std::size_t) { ++progressCalls; return true; };

    const ivevsg::TileSetReport report = ivevsg::buildTileSet(settings);
    ASSERT_TRUE(report) << report.error;
    EXPECT_EQ(report.maxLevel, 2);
    EXPECT_EQ(report.filesWritten, 1u + 1u + 4u);  // root, 0/0/0, 1/x/y
    EXPECT_EQ(report.tilesWritten, 1u + 4u + 16u); // levels 0, 1, 2
    EXPECT_EQ(progressCalls, report.filesWritten);
    EXPECT_NEAR(report.sourceMetersPerPixel, 2.0 * kE / 512.0, 1e-6);

    const std::filesystem::path tiles = dir / "tiles";
    EXPECT_TRUE(std::filesystem::exists(tiles / "root.ive"));
    EXPECT_TRUE(std::filesystem::exists(tiles / "0/0/0.ive"));
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            EXPECT_TRUE(std::filesystem::exists(tiles / "1" / std::to_string(x) / (std::to_string(y) + ".ive")));
    EXPECT_FALSE(std::filesystem::exists(tiles / "2")); // leaves live inside their parents' files
}

TEST_F(TileSetBuilderTest, NativeLevelMatchesTheSourceResolution)
{
    ivevsg::TileSetSettings settings;
    settings.sourceRaster = source.string();
    settings.outputDirectory = (dir / "native").string();
    settings.tileSizePixels = 256;
    const ivevsg::TileSetReport report = ivevsg::buildTileSet(settings);
    ASSERT_TRUE(report) << report.error;
    EXPECT_EQ(report.maxLevel, 1); // 2 x 256 px tiles across = the 512 px source
    EXPECT_EQ(report.filesWritten, 2u);
}

TEST_F(TileSetBuilderTest, TilesLoadThroughVsgWithPagedLODsAndGeometry)
{
    ivevsg::TileSetSettings settings;
    settings.sourceRaster = source.string();
    settings.outputDirectory = (dir / "tiles").string();
    settings.maxLevel = 2;
    settings.tileSizePixels = 64;
    ASSERT_TRUE(ivevsg::buildTileSet(settings));

    ivevsg::SceneLoader loader;

    const ivevsg::LoadResult root = loader.load((dir / "tiles" / "root.ive").string());
    ASSERT_TRUE(root) << root.error;
    const Census rootCensus = census(*root.node);
    EXPECT_EQ(rootCensus.pagedLODs, 1); // the level-0 tile pages in 0/0/0.ive
    EXPECT_EQ(rootCensus.draws, 1);
    const vsg::dbox rootBounds = ivevsg::computeGeometryBounds(*root.node);
    ASSERT_TRUE(rootBounds.valid());
    EXPECT_NEAR(rootBounds.min.x, -kE, kE * 1e-6);
    EXPECT_NEAR(rootBounds.max.y, kE, kE * 1e-6);
    EXPECT_NEAR(rootBounds.max.z, 0.0, 1.0);

    const ivevsg::LoadResult level1 = loader.load((dir / "tiles" / "0/0/0.ive").string());
    ASSERT_TRUE(level1) << level1.error;
    const Census level1Census = census(*level1.node);
    EXPECT_EQ(level1Census.pagedLODs, 4); // four level-1 tiles, each paging in its own children
    EXPECT_EQ(level1Census.draws, 4);

    const ivevsg::LoadResult leaves = loader.load((dir / "tiles" / "1/1/1.ive").string());
    ASSERT_TRUE(leaves) << leaves.error;
    const Census leafCensus = census(*leaves.node);
    EXPECT_EQ(leafCensus.pagedLODs, 0); // level 2 is the deepest level: plain quads
    EXPECT_EQ(leafCensus.draws, 4);
    const vsg::dbox leafBounds = ivevsg::computeGeometryBounds(*leaves.node);
    EXPECT_NEAR(leafBounds.min.x, 0.0, 1.0);   // the south-east quarter of the world ...
    EXPECT_NEAR(leafBounds.max.y, 0.0, 1.0);
    EXPECT_NEAR(leafBounds.max.x, kE, kE * 1e-6);
    EXPECT_NEAR(leafBounds.min.y, -kE, kE * 1e-6);
}

namespace
{
    struct FirstPagedLOD : public vsg::Visitor
    {
        vsg::ref_ptr<vsg::PagedLOD> found;
        void apply(vsg::Node& node) override { if (!found) node.traverse(*this); }
        void apply(vsg::PagedLOD& plod) override { if (!found) found = vsg::ref_ptr<vsg::PagedLOD>(&plod); }
    };
} // namespace

TEST_F(TileSetBuilderTest, PagedChildrenResolveThroughTheInheritedOptions)
{
    ivevsg::TileSetSettings settings;
    settings.sourceRaster = source.string();
    settings.outputDirectory = (dir / "tiles").string();
    settings.maxLevel = 2;
    settings.tileSizePixels = 64;
    ASSERT_TRUE(ivevsg::buildTileSet(settings));

    ivevsg::SceneLoader loader;
    const ivevsg::LoadResult root = loader.load((dir / "tiles" / "root.ive").string());
    ASSERT_TRUE(root) << root.error;

    FirstPagedLOD finder;
    root.node->accept(finder);
    ASSERT_TRUE(finder.found);
    EXPECT_EQ(finder.found->filename, vsg::Path("0/0/0.ive"));
    ASSERT_TRUE(finder.found->options) << "PagedLOD must carry options for the pager";
    EXPECT_FALSE(finder.found->options->readerWriters.empty()) << "options must still include the vsgXchange readers";

    // exactly what the database pager does for a child tile
    auto child = vsg::read_cast<vsg::Node>(finder.found->filename, finder.found->options);
    ASSERT_TRUE(child) << "relative child filename did not resolve: paths="
                       << [&] { std::string s; for (auto& p : finder.found->options->paths) s += p.string() + " "; return s; }();
    const Census childCensus = census(*child);
    EXPECT_EQ(childCensus.pagedLODs, 4);
    EXPECT_EQ(childCensus.draws, 4);
}

namespace
{
    std::set<std::string> iveFilesUnder(const std::filesystem::path& dir)
    {
        std::set<std::string> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
            if (entry.is_regular_file() && entry.path().extension() == ".ive") files.insert(std::filesystem::relative(entry.path(), dir).string());
        return files;
    }

    std::uintmax_t bytesUnder(const std::filesystem::path& dir)
    {
        std::uintmax_t total = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
            if (entry.is_regular_file()) total += entry.file_size();
        return total;
    }
} // namespace

TEST(TileSetBuilder, SubdivisionLimitIsDeeperOnlyInsideTheDetailRegion)
{
    ivevsg::TileScheme scheme;
    ivevsg::TileSetSettings settings;
    settings.detailMaxLevel = 14;
    settings.setDetailExtentDegrees(-123.0, 37.0, -121.5, 38.5); // San Francisco Bay
    const vsg::dvec3 bay = ivevsg::MercatorProjection().geoToWorld(ivevsg::GeoPoint{-122.4, 37.8, 0.0});
    const vsg::dvec3 paris = ivevsg::MercatorProjection().geoToWorld(ivevsg::GeoPoint{2.35, 48.85, 0.0});

    EXPECT_EQ(ivevsg::subdivisionLimit(scheme.tileAt(bay.x, bay.y, 9), settings, scheme, 9), 14);
    EXPECT_EQ(ivevsg::subdivisionLimit(scheme.tileAt(paris.x, paris.y, 9), settings, scheme, 9), 9);
    EXPECT_EQ(ivevsg::subdivisionLimit({0, 0, 0}, settings, scheme, 9), 14); // the world tile touches the region

    ivevsg::TileSetSettings plain;
    EXPECT_EQ(ivevsg::subdivisionLimit(scheme.tileAt(bay.x, bay.y, 9), plain, scheme, 9), 9);
}

TEST_F(TileSetBuilderTest, CompressedFilesAreSmallerAndStillLoad)
{
    ivevsg::TileSetSettings settings;
    settings.sourceRaster = source.string();
    settings.maxLevel = 1;
    settings.tileSizePixels = 64;
    settings.threads = 1;

    settings.compressed = false;
    settings.outputDirectory = (dir / "plain").string();
    ASSERT_TRUE(ivevsg::buildTileSet(settings));
    settings.compressed = true;
    settings.outputDirectory = (dir / "zlib").string();
    const ivevsg::TileSetReport report = ivevsg::buildTileSet(settings);
    ASSERT_TRUE(report) << report.error;
    EXPECT_TRUE(report.compressed);

    // OSG's "compressed" option is zlib level 1 (fastest): expect a clear but not dramatic saving
    EXPECT_LT(bytesUnder(dir / "zlib"), bytesUnder(dir / "plain") * 8 / 10);
    EXPECT_EQ(iveFilesUnder(dir / "zlib"), iveFilesUnder(dir / "plain"));

    ivevsg::SceneLoader loader;
    const ivevsg::LoadResult root = loader.load((dir / "zlib" / "root.ive").string());
    ASSERT_TRUE(root) << root.error;
    EXPECT_EQ(census(*root.node).draws, 1);
    const ivevsg::LoadResult children = loader.load((dir / "zlib" / "0/0/0.ive").string());
    ASSERT_TRUE(children) << children.error;
    EXPECT_EQ(census(*children.node).draws, 4);
}

TEST_F(TileSetBuilderTest, WorkerThreadsProduceTheSameFileSet)
{
    ivevsg::TileSetSettings settings;
    settings.sourceRaster = source.string();
    settings.maxLevel = 3;
    settings.tileSizePixels = 32;

    settings.threads = 1;
    settings.outputDirectory = (dir / "single").string();
    const ivevsg::TileSetReport single = ivevsg::buildTileSet(settings);
    ASSERT_TRUE(single) << single.error;

    settings.threads = 4;
    settings.outputDirectory = (dir / "multi").string();
    const ivevsg::TileSetReport multi = ivevsg::buildTileSet(settings);
    ASSERT_TRUE(multi) << multi.error;

    EXPECT_EQ(multi.threads, 4u);
    EXPECT_EQ(multi.filesWritten, single.filesWritten);
    EXPECT_EQ(multi.tilesWritten, single.tilesWritten);
    EXPECT_EQ(single.filesWritten, 1u + 1u + 4u + 16u);
    EXPECT_EQ(iveFilesUnder(dir / "multi"), iveFilesUnder(dir / "single"));
}

TEST_F(TileSetBuilderTest, DetailRegionGoesDeeperOnlyWhereAsked)
{
    ivevsg::TileSetSettings settings;
    settings.sourceRaster = source.string();
    settings.outputDirectory = (dir / "detail").string();
    settings.maxLevel = 1;
    settings.detailMaxLevel = 3;
    settings.tileSizePixels = 32;
    settings.threads = 2;
    // a small box in the north-east quarter: only tiles touching it get levels 2 and 3
    settings.detailExtent = vsg::dbox(vsg::dvec3(kE * 0.3, kE * 0.3, 0.0), vsg::dvec3(kE * 0.4, kE * 0.4, 0.0));

    const ivevsg::TileSetReport report = ivevsg::buildTileSet(settings);
    ASSERT_TRUE(report) << report.error;
    EXPECT_EQ(report.maxLevel, 1);
    EXPECT_EQ(report.detailMaxLevel, 3);

    const std::set<std::string> files = iveFilesUnder(dir / "detail");
    EXPECT_TRUE(files.count("root.ive"));
    EXPECT_TRUE(files.count("0/0/0.ive"));
    EXPECT_TRUE(files.count("1/1/0.ive"));  // north-east level-1 tile touches the box -> has level-2 children
    EXPECT_FALSE(files.count("1/0/0.ive")); // the others stop at level 1
    EXPECT_FALSE(files.count("1/0/1.ive"));
    EXPECT_FALSE(files.count("1/1/1.ive"));
    // exactly one level-2 tile (2/2/1 covers x in [0, E/2], y in [E/2, E]... the box lies in 2/2/1) goes on to level 3
    int level2Files = 0;
    for (const auto& f : files) if (f.rfind("2/", 0) == 0) ++level2Files;
    EXPECT_EQ(level2Files, 1);
    EXPECT_TRUE(files.count("2/2/1.ive"));

    // the deeper tiles really are PagedLODs down to level 3 and plain quads there
    ivevsg::SceneLoader loader;
    const ivevsg::LoadResult level2 = loader.load((dir / "detail" / "1/1/0.ive").string());
    ASSERT_TRUE(level2) << level2.error;
    EXPECT_EQ(census(*level2.node).draws, 4);
    EXPECT_EQ(census(*level2.node).pagedLODs, 1); // only the child touching the box pages further
}

// ---- geocentric output ------------------------------------------------------------------------

TEST(TileSetBuilderGeocentric, TilesSitOnTheEllipsoidAndDeclareTheirFrame)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ivevsg_tiles_ecef";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path source = writeSyntheticWorld(dir);
    ASSERT_FALSE(source.empty());

    ivevsg::TileSetSettings settings;
    settings.sourceRaster = source.string();
    settings.outputDirectory = (dir / "tiles").string();
    settings.maxLevel = 1;
    settings.geocentric = true;
    settings.geocentricGrid = 4;
    settings.threads = 1;
    const ivevsg::TileSetReport report = ivevsg::buildTileSet(settings);
    ASSERT_TRUE(report) << report.error;
    EXPECT_EQ(report.crs, "EPSG:4978");

    // read as the globe viewer would: the file declares EPSG:4978, so nothing is reprojected
    ivevsg::LoaderSettings loaderSettings = ivevsg::SceneLoader::defaultSettings();
    loaderSettings.targetCrs = "EPSG:4978";
    loaderSettings.defaultSourceCrs = "EPSG:3857"; // must not be used: the root names its own frame
    ivevsg::SceneLoader loader(loaderSettings);
    const ivevsg::LoadResult root = loader.load(report.rootFile);
    ASSERT_TRUE(root) << root.error;
    EXPECT_EQ(root.sourceCrs, "EPSG:4978");
    EXPECT_TRUE(root.targetCrs.empty()); // same frame: left as read

    // the level-0 tile covers the world square: its corners reach +-a in x and y and +-b sin(85) in z
    const vsg::dbox bounds = ivevsg::computeGeometryBounds(*root.node);
    ASSERT_TRUE(bounds.valid());
    const double a = 6378137.0, zMax = 6356752.314245179 * std::sin(85.05112878 * M_PI / 180.0);
    EXPECT_NEAR(bounds.max.x, a, a * 0.01);
    EXPECT_NEAR(bounds.min.x, -a, a * 0.01);
    EXPECT_NEAR(bounds.max.y, a, a * 0.01);
    EXPECT_NEAR(bounds.max.z, zMax, zMax * 0.01);
    EXPECT_NEAR(bounds.min.z, -zMax, zMax * 0.01);

    // the paged children (level 1) resolve through the inherited options and are geocentric too
    ivevsg::LoaderSettings childSettings = loaderSettings;
    childSettings.targetCrs = "EPSG:3857"; // a Mercator viewer reprojects them back onto the plane
    ivevsg::SceneLoader mercatorLoader(childSettings);
    const ivevsg::LoadResult onMap = mercatorLoader.load(report.rootFile);
    ASSERT_TRUE(onMap) << onMap.error;
    EXPECT_EQ(onMap.sourceCrs, "EPSG:4978");
    EXPECT_EQ(onMap.targetCrs, "EPSG:3857");
    const vsg::dbox flat = ivevsg::computeGeometryBounds(*onMap.node);
    ASSERT_TRUE(flat.valid());
    EXPECT_NEAR(flat.max.x, kE, kE * 0.001);
    EXPECT_NEAR(flat.min.y, -kE, kE * 0.001);
    EXPECT_NEAR(flat.max.z - flat.min.z, 0.0, 10.0); // back on the plane
}
