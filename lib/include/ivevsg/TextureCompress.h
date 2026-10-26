#pragma once

#include <vsg/core/Array2D.h>
#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>

namespace ivevsg
{
    /// Encodes an RGBA8 image as BC1 (DXT1: 8 bytes per 4 x 4 block, an eighth of RGBA) with a full
    /// mip chain, in the layout VSG uploads as is (VK_FORMAT_BC1_RGBA_UNORM_BLOCK, blocks of 4 x 4,
    /// level after level down to 1 x 1). Blocks holding a pixel with alpha below 128 use the format's
    /// 1-bit alpha mode, so transparency outside a raster's footprint survives. Imagery quality: the
    /// endpoints are the block's colour bounding box, inset a little, as fast encoders do.
    vsg::ref_ptr<vsg::block64Array2D> compressBc1(const vsg::ubvec4Array2D& rgba, bool withMipmaps = true);

    /// Decodes one BC1 block into 16 RGBA pixels, row by row (for tests and tools).
    void decodeBc1Block(const vsg::block64& block, vsg::ubvec4 pixels[16]);
} // namespace ivevsg
