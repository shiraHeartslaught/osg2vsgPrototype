#pragma once

#include <vsg/maths/vec4.h>

#include <optional>
#include <string>

namespace ivevsg
{
    /// How a file is to be read. Auto picks by extension: DTED / HGT become terrain, shapefiles and
    /// other OGR formats become vector layers, GeoTIFF and friends imagery, the rest scenes.
    enum class LayerKind
    {
        Auto,
        Terrain, // an elevation raster shown as a surface (any GDAL raster, forced)
        Vector,  // OGR features drawn as lines and markers
        Raster,  // imagery cut into tiles
        Scene    // .ive / .vsgb / glTF ...
    };

    /// How heights between the posts of an elevation raster are estimated once the mesh is finer
    /// than the data (zoomed in past its resolution).
    enum class HeightInterpolation
    {
        Nearest = 0,
        Bilinear = 1,
        Bicubic = 2 // Catmull-Rom: smooth slopes, no facets
    };

    struct TerrainStyle
    {
        double exaggeration = 1.0;                                            // vertical scale
        HeightInterpolation interpolation = HeightInterpolation::Bicubic;     // between posts
        int oversampleLevels = 4;                                             // tile levels past the data's own resolution that keep subdividing with interpolated heights
        int cellsPerTile = 32;                                                // mesh cells per tile side
        int texturePixels = 128;                                              // hillshade texture per tile side
        bool hillshade = true;                                                // shade by slope (else flat hypsometric tint)
    };

    struct VectorStyle
    {
        vsg::vec4 colour{1.0f, 0.85f, 0.2f, 1.0f};
        float lineWidth = 1.5f; // pixels (needs the wideLines device feature above 1)
        float pointSize = 8.0f; // marker size in pixels at a tile's hand-over size
    };

    /// Everything a layer can be told beyond its path and CRS. Kept in LayerSource so a layer is
    /// reloaded the same way when the display projection switches.
    struct LayerOptions
    {
        LayerKind kind = LayerKind::Auto;
        double minZoom = 0.0;        // the layer is drawn from this zoom level ...
        double maxZoom = 30.0;       // ... up to this one
        double scaleDenominator = 0; // declared resolution 1:S (0 = unknown, or parsed from the name: ne_10m_ = 1:10 000 000)
        std::string lodGroup;        // layers sharing a group are alternatives: the coarsest that still resolves at the current zoom is shown
        int detailLevelCap = -1;     // deepest tile level a paged layer may descend to (-1 = the layer's own limit)
        TerrainStyle terrain;
        VectorStyle vector;
    };

    const char* layerKindName(LayerKind kind);
    LayerKind parseLayerKind(const std::string& text); // "terrain", "vector", "raster", "scene", else Auto
    const char* heightInterpolationName(HeightInterpolation mode);
    HeightInterpolation parseHeightInterpolation(const std::string& text); // "nearest", "bilinear", else bicubic

    /// Natural Earth style names carry their scale: "ne_10m_admin_0_countries" -> 1:10 000 000,
    /// "ne_50m_..." -> 1:50 000 000, "ne_110m_..." -> 1:110 000 000. Nothing otherwise.
    std::optional<double> scaleDenominatorFromName(const std::string& path);
    /// "1:50000000", "50000000", "50m" / "50M" (millions), "250k" (thousands). Nothing for anything else.
    std::optional<double> parseScaleDenominator(const std::string& text);
    std::string formatScaleDenominator(double scaleDenominator); // "1:10000000", "" for 0
    /// The name with a Natural Earth scale prefix removed ("ne_10m_admin_0_countries" ->
    /// "admin_0_countries"), so the three scales of one data set fall into one LOD group by default.
    std::string lodGroupFromName(const std::string& path);

    /// Web Mercator zoom level at which one screen pixel (the OGC 0.28 mm pixel) covers the ground a
    /// map at 1:S resolves: data is "resolved" up to this zoom and magnified beyond it.
    double zoomLevelForScaleDenominator(double scaleDenominator);

    /// "PATH?key=value&key=value": a path with layer options attached, the form the command line,
    /// the panel's path field and scripts accept. Keys: kind, minzoom, maxzoom, scale, group, cap,
    /// colour|color (rrggbb[aa] hex, or r,g,b[,a] floats), width, pointsize, exaggeration|exag,
    /// interp, oversample, cells, texture, hillshade. Unknown keys are errors.
    bool parseLayerSpec(const std::string& spec, std::string& path, LayerOptions& options, std::string& error);
    /// The inverse: the path plus whatever differs from the defaults.
    std::string layerSpec(const std::string& path, const LayerOptions& options);
} // namespace ivevsg
