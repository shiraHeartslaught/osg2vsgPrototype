#pragma once

namespace ivevsg
{
    /// vsg::Options string / int values every reader in the loader chain reads, set by SceneLoader
    /// and copied into the options of paged children so a whole database is treated alike.
    namespace loaderKeys
    {
        constexpr const char* targetCrs = "ivevsg.target_crs";                // frame to show in ("EPSG:3857", "EPSG:4978"); "" = leave as read
        constexpr const char* sourceCrs = "ivevsg.source_crs";                // per-load override of a file's own CRS
        constexpr const char* defaultSourceCrs = "ivevsg.default_source_crs"; // CRS assumed when a file names none
        constexpr const char* quadTessellation = "ivevsg.quad_tessellation";  // int: grid single quads are split into
        constexpr const char* detailBias = "ivevsg.detail_bias";              // double: > 1 hands tiles over to children sooner

        /// Per-layer options (LayerOptions.h), set by SceneLoader::load() from the LayerSource.
        constexpr const char* layerKind = "ivevsg.layer_kind";                          // string: auto | terrain | vector | raster | scene
        constexpr const char* detailLevelCap = "ivevsg.detail_level_cap";               // int: deepest tile level a paged layer descends to (-1 = own limit)
        constexpr const char* terrainExaggeration = "ivevsg.terrain_exaggeration";      // double
        constexpr const char* terrainInterpolation = "ivevsg.terrain_interpolation";    // int: HeightInterpolation
        constexpr const char* terrainOversample = "ivevsg.terrain_oversample";          // int: levels past the data's resolution
        constexpr const char* terrainCells = "ivevsg.terrain_cells";                    // int: mesh cells per tile side
        constexpr const char* terrainTexture = "ivevsg.terrain_texture";                // int: hillshade texels per tile side
        constexpr const char* terrainHillshade = "ivevsg.terrain_hillshade";            // bool
        constexpr const char* vectorColour = "ivevsg.vector_colour";                    // vsg::vec4
        constexpr const char* vectorLineWidth = "ivevsg.vector_line_width";             // float, pixels
        constexpr const char* vectorPointSize = "ivevsg.vector_point_size";             // float, pixels
        constexpr const char* vectorMaxLevel = "ivevsg.vector_max_level";               // int: deepest tile level vector layers page to

        /// Viewer-wide settings the readers need (LoaderSettings).
        constexpr const char* drape = "ivevsg.drape";                       // bool: vector features follow the terrain ...
        constexpr const char* drapeMinZoom = "ivevsg.drape_min_zoom";       // double: ... from this tile level on
        constexpr const char* wideLines = "ivevsg.wide_lines";              // bool: the device can draw lines wider than one pixel
        constexpr const char* elevationRegistry = "ivevsg.elevation_registry"; // object: ElevationRegistry

        /// Set on the node a reader returns: the CRS the file was taken to be in, and the frame it was
        /// converted into ("" when shown as read), so a viewer can show both.
        constexpr const char* nodeSourceCrs = "ivevsg.node_source_crs";
        constexpr const char* nodeTargetCrs = "ivevsg.node_target_crs";
        constexpr const char* nodeLayerKind = "ivevsg.node_layer_kind";           // string: what the reader made of it
        constexpr const char* nodeInfo = "ivevsg.node_info";                      // string: one line for panels
        constexpr const char* nodeNativeLevel = "ivevsg.node_native_level";       // int: tile level matching the data's own resolution
        constexpr const char* nodeMaxLevel = "ivevsg.node_max_level";             // int: deepest tile level the layer pages to
        constexpr const char* nodeDeclaredScale = "ivevsg.node_declared_scale";   // double: 1:S parsed from the file name (vector layers)
        constexpr const char* nodeTerrainRegistryId = "ivevsg.node_terrain_registry_id"; // unsigned: the layer's ElevationRegistry entry
        constexpr const char* nodeVectorColour = "ivevsg.node_vector_colour";     // object: the layer's vsg::vec4Array colour (editable, dynamic)
        constexpr const char* nodeVectorLineWidth = "ivevsg.node_vector_line_width"; // object: the layer's vsg::SetLineWidth (editable)

        /// Stored inside VSG native files (.vsgb / .vsgt) as a value on the root node: the CRS of
        /// their coordinates, so they carry it the way an .ive carries a CoordinateSystemNode.
        constexpr const char* fileCrs = "ivevsg.crs";
    } // namespace loaderKeys
} // namespace ivevsg
