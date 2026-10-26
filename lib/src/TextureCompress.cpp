#include <ivevsg/BaseMap.h> // generateMipmaps
#include <ivevsg/TextureCompress.h>

#include <vsg/core/Allocator.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace ivevsg
{
    namespace
    {
        uint16_t to565(int r, int g, int b)
        {
            return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }

        vsg::ubvec4 from565(uint16_t v)
        {
            const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            return vsg::ubvec4(static_cast<uint8_t>((r << 3) | (r >> 2)), static_cast<uint8_t>((g << 2) | (g >> 4)), static_cast<uint8_t>((b << 3) | (b >> 2)), 255);
        }

        int distance2(const vsg::ubvec4& a, const vsg::ubvec4& b)
        {
            const int dr = int(a.r) - int(b.r), dg = int(a.g) - int(b.g), db = int(a.b) - int(b.b);
            return dr * dr + dg * dg + db * db;
        }

        vsg::ubvec4 mix(const vsg::ubvec4& a, int wa, const vsg::ubvec4& b, int wb)
        {
            const int w = wa + wb;
            return vsg::ubvec4(static_cast<uint8_t>((a.r * wa + b.r * wb + w / 2) / w), static_cast<uint8_t>((a.g * wa + b.g * wb + w / 2) / w),
                               static_cast<uint8_t>((a.b * wa + b.b * wb + w / 2) / w), 255);
        }

        void writeBlock(vsg::block64& out, uint16_t c0, uint16_t c1, const uint8_t indices[16])
        {
            out.value[0] = static_cast<uint8_t>(c0 & 0xFF);
            out.value[1] = static_cast<uint8_t>(c0 >> 8);
            out.value[2] = static_cast<uint8_t>(c1 & 0xFF);
            out.value[3] = static_cast<uint8_t>(c1 >> 8);
            for (int row = 0; row < 4; ++row)
                out.value[4 + row] = static_cast<uint8_t>(indices[row * 4] | (indices[row * 4 + 1] << 2) | (indices[row * 4 + 2] << 4) | (indices[row * 4 + 3] << 6));
        }

        void encodeBlock(const vsg::ubvec4 px[16], vsg::block64& out)
        {
            bool transparent[16];
            bool anyTransparent = false, anyOpaque = false;
            int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
            for (int i = 0; i < 16; ++i)
            {
                transparent[i] = px[i].a < 128;
                if (transparent[i])
                {
                    anyTransparent = true;
                    continue;
                }
                anyOpaque = true;
                const int c[3] = {px[i].r, px[i].g, px[i].b};
                for (int k = 0; k < 3; ++k)
                {
                    lo[k] = std::min(lo[k], c[k]);
                    hi[k] = std::max(hi[k], c[k]);
                }
            }
            uint8_t indices[16];
            if (!anyOpaque)
            {
                std::fill(indices, indices + 16, 3); // c0 == c1 selects the three-colour mode, index 3 is transparent
                writeBlock(out, 0, 0, indices);
                return;
            }
            // inset the bounding box a little: the extremes are rarely the best endpoints
            for (int k = 0; k < 3; ++k)
            {
                const int inset = (hi[k] - lo[k]) / 16;
                lo[k] += inset;
                hi[k] -= inset;
            }
            // the endpoints are two opposite corners of the box: the pair that follows the block's
            // colours, i.e. a channel that falls while red rises takes its low value where red is high
            double mean[3] = {0.0, 0.0, 0.0};
            int opaque = 0;
            for (int i = 0; i < 16; ++i)
            {
                if (transparent[i]) continue;
                mean[0] += px[i].r;
                mean[1] += px[i].g;
                mean[2] += px[i].b;
                ++opaque;
            }
            for (double& m : mean) m /= opaque;
            double covariance[3] = {0.0, 0.0, 0.0}; // of each channel with red
            for (int i = 0; i < 16; ++i)
            {
                if (transparent[i]) continue;
                const double dr = px[i].r - mean[0];
                covariance[1] += dr * (px[i].g - mean[1]);
                covariance[2] += dr * (px[i].b - mean[2]);
            }
            int endA[3] = {hi[0], hi[1], hi[2]}, endB[3] = {lo[0], lo[1], lo[2]};
            for (int k = 1; k < 3; ++k)
            {
                if (covariance[k] < 0.0) std::swap(endA[k], endB[k]);
            }
            uint16_t c0 = to565(endA[0], endA[1], endA[2]), c1 = to565(endB[0], endB[1], endB[2]);
            vsg::ubvec4 palette[4];
            int paletteSize = 4;
            if (anyTransparent)
            {
                if (c0 > c1) std::swap(c0, c1); // c0 <= c1: three colours plus transparent
                palette[0] = from565(c0);
                palette[1] = from565(c1);
                palette[2] = mix(palette[0], 1, palette[1], 1);
                paletteSize = 3;
            }
            else
            {
                if (c0 < c1) std::swap(c0, c1); // c0 > c1: four opaque colours
                palette[0] = from565(c0);
                palette[1] = from565(c1);
                if (c0 == c1)
                {
                    std::fill(indices, indices + 16, 0); // one flat colour
                    writeBlock(out, c0, c1, indices);
                    return;
                }
                palette[2] = mix(palette[0], 2, palette[1], 1);
                palette[3] = mix(palette[0], 1, palette[1], 2);
            }
            for (int i = 0; i < 16; ++i)
            {
                if (transparent[i])
                {
                    indices[i] = 3;
                    continue;
                }
                int best = 0, bestDistance = distance2(px[i], palette[0]);
                for (int p = 1; p < paletteSize; ++p)
                {
                    const int d = distance2(px[i], palette[p]);
                    if (d < bestDistance)
                    {
                        bestDistance = d;
                        best = p;
                    }
                }
                indices[i] = static_cast<uint8_t>(best);
            }
            writeBlock(out, c0, c1, indices);
        }

        /// Encodes one image level (w x h pixels at `pixels`) into blocks, clamping at the edges.
        void encodeLevel(const vsg::ubvec4* pixels, uint32_t w, uint32_t h, vsg::block64* out)
        {
            const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
            vsg::ubvec4 px[16];
            for (uint32_t by = 0; by < bh; ++by)
                for (uint32_t bx = 0; bx < bw; ++bx)
                {
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x)
                        {
                            const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                            px[y * 4 + x] = pixels[sy * w + sx];
                        }
                    encodeBlock(px, out[by * bw + bx]);
                }
        }
    } // namespace

    vsg::ref_ptr<vsg::block64Array2D> compressBc1(const vsg::ubvec4Array2D& rgba, bool withMipmaps)
    {
        const uint32_t width = rgba.width(), height = rgba.height();
        if (width == 0 || height == 0) return {};

        // the levels, halving like vsg::Data::computeValueCountIncludingMipmaps()
        std::vector<std::pair<uint32_t, uint32_t>> levels;
        for (uint32_t w = width, h = height;;)
        {
            levels.emplace_back(w, h);
            if (!withMipmaps || (w <= 1 && h <= 1)) break;
            if (w > 1) w /= 2;
            if (h > 1) h /= 2;
        }
        std::size_t totalBlocks = 0;
        for (const auto& [w, h] : levels) totalBlocks += static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4);

        vsg::ref_ptr<vsg::ubvec4Array2D> chain;
        const vsg::ubvec4* source = rgba.data();
        if (withMipmaps && levels.size() > 1)
        {
            chain = generateMipmaps(rgba); // level 0 followed by each smaller level
            source = chain->data();
        }

        auto* blocks = static_cast<vsg::block64*>(vsg::allocate(totalBlocks * sizeof(vsg::block64), vsg::ALLOCATOR_AFFINITY_DATA));
        std::size_t pixelOffset = 0, blockOffset = 0;
        for (const auto& [w, h] : levels)
        {
            encodeLevel(source + pixelOffset, w, h, blocks + blockOffset);
            pixelOffset += static_cast<std::size_t>(w) * h;
            blockOffset += static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4);
        }

        vsg::Data::Properties properties(VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
        properties.blockWidth = 4;
        properties.blockHeight = 4;
        properties.mipLevels = static_cast<uint8_t>(levels.size());
        properties.origin = rgba.properties.origin;
        return vsg::block64Array2D::create((width + 3) / 4, (height + 3) / 4, blocks, properties);
    }

    void decodeBc1Block(const vsg::block64& block, vsg::ubvec4 pixels[16])
    {
        const uint16_t c0 = static_cast<uint16_t>(block.value[0] | (block.value[1] << 8));
        const uint16_t c1 = static_cast<uint16_t>(block.value[2] | (block.value[3] << 8));
        vsg::ubvec4 palette[4];
        palette[0] = from565(c0);
        palette[1] = from565(c1);
        if (c0 > c1)
        {
            palette[2] = mix(palette[0], 2, palette[1], 1);
            palette[3] = mix(palette[0], 1, palette[1], 2);
        }
        else
        {
            palette[2] = mix(palette[0], 1, palette[1], 1);
            palette[3] = vsg::ubvec4(0, 0, 0, 0);
        }
        for (int i = 0; i < 16; ++i)
        {
            const int index = (block.value[4 + i / 4] >> (2 * (i % 4))) & 3;
            pixels[i] = palette[index];
        }
    }
} // namespace ivevsg
