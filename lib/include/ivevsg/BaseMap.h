#pragma once

#include <ivevsg/MercatorProjection.h>

#include <vsg/core/Array2D.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/io/Options.h>
#include <vsg/nodes/Node.h>

#include <string>

namespace ivevsg
{
    struct BaseMapSettings
    {
        /// Image already in EPSG:3857 covering the whole world square (see data/README.md). "" = solid colour.
        std::string imagePath = defaultBaseMapImagePath();
        /// Used when there is no image or it cannot be read: deep ocean blue.
        vsg::vec4 fallbackColor{0.07f, 0.17f, 0.35f, 1.0f};
        /// Half the side length of the square the map covers, in metres.
        double halfExtent = MercatorProjection::kMaxExtent;
        /// Height of the map plane. Leave at 0 and let ViewerApp::setUnderlay() keep the base map
        /// below the data: a fixed offset is either too small for depth precision when zoomed out or
        /// so large when zoomed in that it changes which tile level the base map selects.
        double zOffset = 0.0;
        /// Build a CPU mip chain for the image so it stays smooth when zoomed out.
        bool generateMipmaps = true;

        static std::string defaultBaseMapImagePath();
    };

    struct BaseMapResult
    {
        vsg::ref_ptr<vsg::Node> node;
        bool textured = false;
        std::string message;
    };

    /// Full mip chain (box filtered) for an RGBA8 image, laid out the way VSG expects for a single
    /// vsg::Data: level 0 followed by each half-size level down to 1x1.
    vsg::ref_ptr<vsg::ubvec4Array2D> generateMipmaps(const vsg::ubvec4Array2D& level0);

    /// A flat textured quad covering the Mercator world square, or a solid-colour quad if the image
    /// is missing or unreadable (the message says which).
    BaseMapResult createBaseMap(const BaseMapSettings& settings, vsg::ref_ptr<vsg::Options> options);
} // namespace ivevsg
