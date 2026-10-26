// ivevsgconv: convert a scene file the viewer can read (.ive, .osgb, .vsgb, a GeoTIFF, ...) into a
// VSG native file, recording the CRS of its coordinates inside it so the viewer places it correctly
// without being told.
//
//   ivevsgconv <input> <output.vsgb|.vsgt> [--crs EPSG:xxxx] [--source-crs EPSG:xxxx] [--target EPSG:3857|EPSG:4978]
//
// --source-crs  the CRS the input is in when it does not say (default: EPSG:3857, like the viewer)
// --crs         the CRS written into the output (default: what the input was taken to be in)
// --target      reproject into this display frame before writing (default: write as read); the
//               output is then tagged with the target CRS
//
// For a paged database only the file named is converted; its children stay where they are, so write
// the output next to the input for their relative paths to keep resolving.

#include <ivevsg/CrsTransform.h>
#include <ivevsg/SceneLoader.h>
#include <ivevsg/VsgReproject.h>

#include <vsg/all.h>

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    vsg::CommandLine arguments(&argc, argv);
    const std::string crs = arguments.value(std::string(), "--crs");
    const std::string sourceCrs = arguments.value(std::string(), "--source-crs");
    const std::string targetCrs = arguments.value(std::string(), "--target");
    if (argc != 3)
    {
        std::cerr << "usage: ivevsgconv <input> <output.vsgb|.vsgt> [--crs EPSG:xxxx] [--source-crs EPSG:xxxx] [--target EPSG:xxxx]\n";
        return 1;
    }
    const std::string input = argv[1], output = argv[2];

    ivevsg::LoaderSettings settings = ivevsg::SceneLoader::defaultSettings();
    settings.targetCrs = targetCrs; // "" = read as is
    if (!sourceCrs.empty()) settings.defaultSourceCrs = sourceCrs;
    ivevsg::SceneLoader loader(settings);
    const ivevsg::LoadResult loaded = loader.load(input, sourceCrs);
    if (!loaded)
    {
        std::cerr << "ivevsgconv: " << loaded.error << "\n";
        return 2;
    }
    const std::string recorded = !crs.empty() ? crs : !loaded.targetCrs.empty() ? loaded.targetCrs : loaded.sourceCrs;
    if (recorded.empty())
    {
        std::cerr << "ivevsgconv: the input names no CRS; give one with --crs\n";
        return 2;
    }
    ivevsg::tagCrs(*loaded.node, recorded);
    if (!vsg::write(loaded.node, vsg::Path(output)))
    {
        std::cerr << "ivevsgconv: could not write " << output << " (use a .vsgb or .vsgt name)\n";
        return 3;
    }
    std::cout << "wrote " << output << "  (coordinates in " << ivevsg::CrsTransform::displayName(recorded)
              << (loaded.targetCrs.empty() ? ", as read" : ", reprojected from " + ivevsg::CrsTransform::displayName(loaded.sourceCrs)) << ")\n";
    return 0;
}
