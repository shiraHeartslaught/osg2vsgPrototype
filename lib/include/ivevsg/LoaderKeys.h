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

        /// Set on the node a reader returns: the CRS the file was taken to be in, and the frame it was
        /// converted into ("" when shown as read), so a viewer can show both.
        constexpr const char* nodeSourceCrs = "ivevsg.node_source_crs";
        constexpr const char* nodeTargetCrs = "ivevsg.node_target_crs";

        /// Stored inside VSG native files (.vsgb / .vsgt) as a value on the root node: the CRS of
        /// their coordinates, so they carry it the way an .ive carries a CoordinateSystemNode.
        constexpr const char* fileCrs = "ivevsg.crs";
    } // namespace loaderKeys
} // namespace ivevsg
