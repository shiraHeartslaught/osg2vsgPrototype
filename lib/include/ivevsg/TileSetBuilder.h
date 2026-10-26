#pragma once

#include <ivevsg/TileScheme.h>

#include <vsg/maths/box.h>

#include <cstddef>
#include <functional>
#include <string>

namespace ivevsg
{
    /// How a tile set is written: OpenSceneGraph .ive files paged by VSG; VSG native .vsgb files
    /// (geometry ready to draw, textures BC1 compressed with mip chains, the CRS recorded in root.vsgb);
    /// or plain tile images in a TMS (rows from the south, with tilemapresource.xml) or XYZ (rows from
    /// the north) directory tree that the viewer reads through TileLayerReader.
    enum class TileSetFormat
    {
        Ive,
        Vsgb,
        Tms,
        Xyz
    };

    struct TileSetSettings
    {
        TileSetFormat format = TileSetFormat::Ive;
        /// Image type for Tms / Xyz output: "png" (with alpha outside the raster) or "jpg".
        std::string imageExtension = "png";
        /// Vsgb output: BC1-compress the textures (an eighth of RGBA, with mip chains; slight blocking
        /// in smooth gradients) rather than store them as RGBA8 with a mip chain made on the GPU.
        bool compressTextures = true;
        /// Any GDAL-readable raster georeferenced in EPSG:3857 metres (e.g. the output of
        /// `gdalwarp -t_srs EPSG:3857`). It may cover part of the world; tiles outside it are skipped.
        std::string sourceRaster;
        /// Receives root.ive and the z/x/y.ive files.
        std::string outputDirectory;
        /// Deepest level generated everywhere; -1 picks the level matching the source's own resolution.
        int maxLevel = -1;
        /// Optional region (EPSG:3857 metres, z ignored) that is generated deeper, down to detailMaxLevel.
        /// Lets a pyramid go to e.g. level 14 where it is needed without the billions of tiles a global
        /// level 14 would take. An invalid box (the default) means no detail region.
        vsg::dbox detailExtent;
        int detailMaxLevel = -1;
        int tileSizePixels = 256;
        /// > 1 hands over to finer tiles sooner (sharper, more tiles resident); < 1 later.
        double detailBias = 1.0;
        /// zlib-compress the .ive streams (OSG's "compressed" option): smaller files, a little CPU to load.
        bool compressed = true;
        /// Worker threads; 0 = one per hardware thread. Every file is independent, so this scales well.
        unsigned threads = 0;
        /// Tile geometry on the WGS84 ellipsoid in geocentric metres (EPSG:4978, "ECEF") instead of the
        /// flat EPSG:3857 plane. The tiling, texturing and paging stay WebMercatorQuad; only where the
        /// corners sit changes, so the set drapes a globe natively and a Mercator viewer reprojects it.
        bool geocentric = false;
        /// Cells per tile side on the globe (each tile becomes an N x N grid so it follows the curvature).
        int geocentricGrid = 8;
        /// Called after each file (from worker threads, serialised); return false to abort.
        std::function<bool(std::size_t filesDone, std::size_t filesTotal)> progress;

        /// Convenience: set detailExtent from a longitude/latitude box in degrees.
        void setDetailExtentDegrees(double minLongitude, double minLatitude, double maxLongitude, double maxLatitude);
    };

    struct TileSetReport
    {
        std::string rootFile; // root.ive, root.vsgb, tilemapresource.xml, or the XYZ directory
        std::string layout;   // "ive", "vsgb", "tms" or "xyz"
        std::string crs; // "EPSG:3857" or "EPSG:4978", also recorded in root.ive's CoordinateSystemNode
        int maxLevel = 0;
        int detailMaxLevel = 0;
        double sourceMetersPerPixel = 0.0;
        std::size_t filesPlanned = 0;
        std::size_t filesWritten = 0; // .ive files, or tile images
        std::size_t tilesWritten = 0; // textured tiles across all levels (= filesWritten for image output)
        unsigned threads = 1;
        bool compressed = false;
        std::string error;

        explicit operator bool() const { return error.empty(); }
    };

    /// On-screen size (bounding-sphere diameter in pixels, on the 1080 px tall reference screen osg2vsg
    /// assumes) above which a tile hands over to its four children. With bias 1 that is the size at
    /// which the tile's texture is shown 1:1, i.e. the view's metres per pixel reach the tile's own.
    double subdivisionPixelThreshold(int tileSizePixels, double detailBias);

    /// Deepest level a tile may be subdivided to under these settings (maxLevel, or detailMaxLevel when
    /// the tile touches the detail region). A tile gets a children file iff its level is below this.
    int subdivisionLimit(const TileIndex& tile, const TileSetSettings& settings, const TileScheme& scheme, int resolvedMaxLevel);

    /// Writes an OGC WebMercatorQuad pyramid as OpenSceneGraph .ive files that VSG pages in by level
    /// of detail (through vsgXchange/osg2vsg), or with settings.format as VSG native .vsgb files of
    /// the same layout (root.vsgb, z/x/y.vsgb), or as tile images:
    ///
    ///   root.ive   the level-0 tile; a PagedLOD that loads 0/0/0.ive when it grows too large on screen
    ///   z/x/y.ive  the four level-(z+1) children of tile z/x/y, each a textured quad in EPSG:3857 metres
    ///              (or an N x N grid on the ellipsoid in EPSG:4978 with settings.geocentric) and, where
    ///              deeper tiles exist, a PagedLOD pointing at its own z+1/x'/y'.ive
    ///
    /// root.ive carries an osg::CoordinateSystemNode naming the frame (with an EllipsoidModel for a
    /// geocentric set, the VirtualPlanetBuilder convention), so a viewer can tell the two apart.
    /// Textures are embedded in the files, so a tile set is self-contained.
    TileSetReport buildTileSet(const TileSetSettings& settings);
} // namespace ivevsg
