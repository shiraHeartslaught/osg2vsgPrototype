// ivetiler: cut an EPSG:3857 raster into an OGC WebMercatorQuad pyramid of OpenSceneGraph .ive tiles
// that the viewer pages in by level of detail.
//
//   ivetiler <source raster (EPSG:3857)> <output directory>
//            [--max-level N] [--tile-size 256] [--detail-bias 1.0] [--threads N] [--uncompressed]
//            [--detail-extent MINLON MINLAT MAXLON MAXLAT --detail-level N]
//
// Output: <out>/root.ive plus <out>/z/x/y.ive holding the four children of tile z/x/y (TileSetBuilder.h).
// Files are zlib-compressed unless --uncompressed is given. --detail-extent/--detail-level generate a
// region deeper than the rest of the world (a global level 14 would be 358 million tiles).

#include <ivevsg/TileSetBuilder.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    ivevsg::TileSetSettings settings;
    std::string positional[2];
    int positionals = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc)
            {
                std::cerr << name << " needs a value\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--max-level") settings.maxLevel = std::atoi(next("--max-level"));
        else if (arg == "--tile-size") settings.tileSizePixels = std::atoi(next("--tile-size"));
        else if (arg == "--detail-bias") settings.detailBias = std::atof(next("--detail-bias"));
        else if (arg == "--threads") settings.threads = static_cast<unsigned>(std::atoi(next("--threads")));
        else if (arg == "--uncompressed") settings.compressed = false;
        else if (arg == "--compressed") settings.compressed = true;
        else if (arg == "--detail-level") settings.detailMaxLevel = std::atoi(next("--detail-level"));
        else if (arg == "--detail-extent")
        {
            if (i + 4 >= argc)
            {
                std::cerr << "--detail-extent needs MINLON MINLAT MAXLON MAXLAT\n";
                return 1;
            }
            const double minLon = std::atof(argv[i + 1]), minLat = std::atof(argv[i + 2]), maxLon = std::atof(argv[i + 3]), maxLat = std::atof(argv[i + 4]);
            settings.setDetailExtentDegrees(minLon, minLat, maxLon, maxLat);
            i += 4;
        }
        else if (positionals < 2) positional[positionals++] = arg;
        else
        {
            std::cerr << "unexpected argument " << arg << "\n";
            return 1;
        }
    }
    if (positionals < 2)
    {
        std::cerr << "usage: ivetiler <source raster (EPSG:3857)> <output directory> [--max-level N] [--tile-size 256] [--detail-bias 1.0]\n"
                     "                [--threads N] [--uncompressed] [--detail-extent MINLON MINLAT MAXLON MAXLAT --detail-level N]\n";
        return 1;
    }
    settings.sourceRaster = positional[0];
    settings.outputDirectory = positional[1];

    const auto start = std::chrono::steady_clock::now();
    std::size_t lastPercent = 0;
    settings.progress = [&](std::size_t done, std::size_t total) {
        const std::size_t percent = total ? done * 100 / total : 100;
        if (percent != lastPercent)
        {
            lastPercent = percent;
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cout << percent << "% (" << done << "/" << total << " files, " << static_cast<int>(seconds) << " s)" << std::endl;
        }
        return true;
    };

    const ivevsg::TileSetReport report = ivevsg::buildTileSet(settings);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!report)
    {
        std::cerr << "ivetiler: " << report.error << "\n";
        return 2;
    }
    ivevsg::TileScheme scheme;
    scheme.tileSizePixels = settings.tileSizePixels;
    std::cout << "root:            " << report.rootFile << "\n"
              << "source:          " << report.sourceMetersPerPixel << " m/px\n"
              << "levels:          0.." << report.maxLevel << " everywhere (level " << report.maxLevel << " = " << scheme.metersPerPixel(report.maxLevel) << " m/px)";
    if (report.detailMaxLevel > report.maxLevel)
        std::cout << ", detail region to level " << report.detailMaxLevel << " (" << scheme.metersPerPixel(report.detailMaxLevel) << " m/px)";
    std::cout << "\n"
              << "tiles:           " << report.tilesWritten << " in " << report.filesWritten << " files (" << report.filesPlanned << " planned)\n"
              << "compression:     " << (report.compressed ? "zlib" : "none") << ", " << report.threads << " threads\n"
              << "hand-over:       sphere > " << ivevsg::subdivisionPixelThreshold(settings.tileSizePixels, settings.detailBias) << " px (1080 px screen)\n"
              << "time:            " << seconds << " s\n";
    return 0;
}
