#include <ivevsg/BaseMap.h>

#include <vsg/all.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

#ifndef IVEVSG_DATA_DIR
#    define IVEVSG_DATA_DIR ""
#endif

namespace ivevsg
{
    std::string BaseMapSettings::defaultBaseMapImagePath()
    {
        const std::string dir = IVEVSG_DATA_DIR;
        return dir.empty() ? std::string() : dir + "/bluemarble_3857.jpg";
    }

    vsg::ref_ptr<vsg::ubvec4Array2D> generateMipmaps(const vsg::ubvec4Array2D& level0)
    {
        // Same level progression as vsg::Data::computeValueCountIncludingMipmaps():
        // halve each dimension (integer division) while it is above 1, stopping at 1x1.
        std::vector<std::pair<uint32_t, uint32_t>> levels;
        for (uint32_t w = level0.width(), h = level0.height();;)
        {
            levels.emplace_back(w, h);
            if (w <= 1 && h <= 1)
                break;
            if (w > 1)
                w /= 2;
            if (h > 1)
                h /= 2;
        }

        std::size_t totalTexels = 0;
        for (const auto& [w, h] : levels)
            totalTexels += static_cast<std::size_t>(w) * h;

        auto* buffer = static_cast<vsg::ubvec4*>(vsg::allocate(totalTexels * sizeof(vsg::ubvec4), vsg::ALLOCATOR_AFFINITY_DATA));
        std::copy_n(level0.data(), static_cast<std::size_t>(level0.width()) * level0.height(), buffer);

        std::size_t previousOffset = 0;
        std::size_t offset = static_cast<std::size_t>(level0.width()) * level0.height();
        for (std::size_t level = 1; level < levels.size(); ++level)
        {
            const auto [pw, ph] = levels[level - 1];
            const auto [cw, ch] = levels[level];
            const vsg::ubvec4* src = buffer + previousOffset;
            vsg::ubvec4* dst = buffer + offset;
            for (uint32_t y = 0; y < ch; ++y)
            {
                const uint32_t y0 = std::min(y * 2, ph - 1);
                const uint32_t y1 = std::min(y * 2 + 1, ph - 1);
                for (uint32_t x = 0; x < cw; ++x)
                {
                    const uint32_t x0 = std::min(x * 2, pw - 1);
                    const uint32_t x1 = std::min(x * 2 + 1, pw - 1);
                    const vsg::ubvec4& a = src[x0 + y0 * pw];
                    const vsg::ubvec4& b = src[x1 + y0 * pw];
                    const vsg::ubvec4& c = src[x0 + y1 * pw];
                    const vsg::ubvec4& d = src[x1 + y1 * pw];
                    vsg::ubvec4& out = dst[x + y * cw];
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        const unsigned sum = a[channel] + b[channel] + c[channel] + d[channel];
                        out[channel] = static_cast<uint8_t>((sum + 2) / 4);
                    }
                }
            }
            previousOffset = offset;
            offset += static_cast<std::size_t>(cw) * ch;
        }

        vsg::Data::Properties properties = level0.properties;
        properties.mipLevels = static_cast<uint8_t>(levels.size());
        return vsg::ubvec4Array2D::create(level0.width(), level0.height(), buffer, properties);
    }

    BaseMapResult createBaseMap(const BaseMapSettings& settings, vsg::ref_ptr<vsg::Options> options)
    {
        BaseMapResult result;

        vsg::ref_ptr<vsg::Data> image;
        if (settings.imagePath.empty())
        {
            result.message = "solid colour (no image configured)";
        }
        else if (!std::filesystem::exists(settings.imagePath))
        {
            result.message = "solid colour (image not found: " + settings.imagePath + ")";
        }
        else
        {
            image = vsg::read_cast<vsg::Data>(vsg::Path(settings.imagePath), options);
            if (!image)
            {
                result.message = "solid colour (could not read image: " + settings.imagePath + ")";
            }
            else
            {
                if (settings.generateMipmaps)
                {
                    if (auto rgba = image.cast<vsg::ubvec4Array2D>()) image = generateMipmaps(*rgba);
                }
                result.textured = true;
                result.message = settings.imagePath + " (" + std::to_string(image->width()) + "x" + std::to_string(image->height()) +
                                 ", " + std::to_string(std::max<int>(1, image->properties.mipLevels)) + " mip levels)";
            }
        }

        const float side = static_cast<float>(settings.halfExtent * 2.0);
        vsg::GeometryInfo geometry;
        geometry.position = vsg::vec3(0.0f, 0.0f, 0.0f);
        geometry.dx = vsg::vec3(side, 0.0f, 0.0f);
        geometry.dy = vsg::vec3(0.0f, side, 0.0f);
        geometry.dz = vsg::vec3(0.0f, 0.0f, 1.0f);
        geometry.color = settings.fallbackColor;

        vsg::StateInfo state;
        state.lighting = false;
        state.image = image;

        auto builder = vsg::Builder::create();
        builder->options = options;
        auto quad = builder->createQuad(geometry, state);

        auto transform = vsg::MatrixTransform::create(vsg::translate(0.0, 0.0, settings.zOffset));
        transform->addChild(quad);
        result.node = transform;
        return result;
    }
} // namespace ivevsg
