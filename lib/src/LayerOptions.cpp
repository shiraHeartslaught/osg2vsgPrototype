#include <ivevsg/LayerOptions.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <vector>

namespace ivevsg
{
    namespace
    {
        std::string lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        bool parseColour(const std::string& text, vsg::vec4& colour)
        {
            std::string t = text;
            if (!t.empty() && t[0] == '#') t.erase(0, 1);
            if (t.find(',') != std::string::npos)
            {
                std::istringstream in(t);
                float parts[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                int n = 0;
                std::string part;
                while (n < 4 && std::getline(in, part, ',')) parts[n++] = std::strtof(part.c_str(), nullptr);
                if (n < 3) return false;
                colour = vsg::vec4(parts[0], parts[1], parts[2], parts[3]);
                return true;
            }
            if (t.size() != 6 && t.size() != 8) return false;
            for (char c : t)
                if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
            auto channel = [&t](std::size_t at) { return static_cast<float>(std::strtol(t.substr(at, 2).c_str(), nullptr, 16)) / 255.0f; };
            colour = vsg::vec4(channel(0), channel(2), channel(4), t.size() == 8 ? channel(6) : 1.0f);
            return true;
        }

        std::string hexColour(const vsg::vec4& c)
        {
            char text[16];
            auto byte = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            std::snprintf(text, sizeof(text), "%02x%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
            return text;
        }

        std::string trimmed(double value)
        {
            std::ostringstream out;
            out << value;
            return out.str();
        }
    } // namespace

    const char* layerKindName(LayerKind kind)
    {
        switch (kind)
        {
        case LayerKind::Terrain: return "terrain";
        case LayerKind::Vector: return "vector";
        case LayerKind::Raster: return "raster";
        case LayerKind::Scene: return "scene";
        default: return "auto";
        }
    }

    LayerKind parseLayerKind(const std::string& text)
    {
        const std::string t = lower(text);
        if (t == "terrain" || t == "dted" || t == "elevation" || t == "dem") return LayerKind::Terrain;
        if (t == "vector" || t == "shapefile" || t == "shp" || t == "features") return LayerKind::Vector;
        if (t == "raster" || t == "imagery" || t == "image") return LayerKind::Raster;
        if (t == "scene" || t == "model") return LayerKind::Scene;
        return LayerKind::Auto;
    }

    const char* heightInterpolationName(HeightInterpolation mode)
    {
        switch (mode)
        {
        case HeightInterpolation::Nearest: return "nearest";
        case HeightInterpolation::Bilinear: return "bilinear";
        default: return "bicubic";
        }
    }

    HeightInterpolation parseHeightInterpolation(const std::string& text)
    {
        const std::string t = lower(text);
        if (t == "nearest" || t == "none" || t == "0") return HeightInterpolation::Nearest;
        if (t == "bilinear" || t == "linear" || t == "1") return HeightInterpolation::Bilinear;
        return HeightInterpolation::Bicubic;
    }

    std::optional<double> scaleDenominatorFromName(const std::string& path)
    {
        const std::string name = lower(std::filesystem::path(path).filename().string());
        // ne_<N>m_...: Natural Earth's 1:N million
        const std::size_t at = name.find("ne_");
        if (at == std::string::npos) return std::nullopt;
        std::size_t i = at + 3;
        double n = 0.0;
        bool digits = false;
        while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])))
        {
            n = n * 10.0 + (name[i] - '0');
            digits = true;
            ++i;
        }
        if (!digits || i + 1 >= name.size() || name[i] != 'm' || name[i + 1] != '_') return std::nullopt;
        return n * 1.0e6;
    }

    std::optional<double> parseScaleDenominator(const std::string& text)
    {
        std::string t = lower(text);
        t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c) { return std::isspace(c) || c == ',' || c == '_'; }), t.end());
        if (t.empty()) return std::nullopt;
        if (t.compare(0, 2, "1:") == 0) t.erase(0, 2);
        double multiplier = 1.0;
        if (!t.empty() && (t.back() == 'm')) multiplier = 1.0e6;
        if (!t.empty() && (t.back() == 'k')) multiplier = 1.0e3;
        if (multiplier != 1.0) t.pop_back();
        if (t.empty()) return std::nullopt;
        char* end = nullptr;
        const double value = std::strtod(t.c_str(), &end);
        if (!end || *end != '\0' || !(value > 0.0)) return std::nullopt;
        return value * multiplier;
    }

    std::string formatScaleDenominator(double scaleDenominator)
    {
        if (!(scaleDenominator > 0.0)) return {};
        char text[48];
        std::snprintf(text, sizeof(text), "1:%.0f", scaleDenominator);
        return text;
    }

    std::string lodGroupFromName(const std::string& path)
    {
        std::string stem = std::filesystem::path(path).stem().string();
        const std::string l = lower(stem);
        const std::size_t at = l.find("ne_");
        if (at != std::string::npos)
        {
            std::size_t i = at + 3;
            while (i < l.size() && std::isdigit(static_cast<unsigned char>(l[i]))) ++i;
            if (i + 1 < l.size() && l[i] == 'm' && l[i + 1] == '_' && i > at + 3) return stem.substr(i + 2);
        }
        return stem;
    }

    double zoomLevelForScaleDenominator(double scaleDenominator)
    {
        if (!(scaleDenominator > 0.0)) return 1.0e9;
        const double metersPerPixel = scaleDenominator * 0.00028;
        return std::log2(156543.03392804097 / metersPerPixel);
    }

    bool parseLayerSpec(const std::string& spec, std::string& path, LayerOptions& options, std::string& error)
    {
        error.clear();
        const std::size_t query = spec.find('?');
        path = spec.substr(0, query);
        if (query == std::string::npos) return true;
        std::istringstream in(spec.substr(query + 1));
        std::string pair;
        while (std::getline(in, pair, '&'))
        {
            if (pair.empty()) continue;
            const std::size_t eq = pair.find('=');
            const std::string key = lower(pair.substr(0, eq));
            const std::string value = eq == std::string::npos ? std::string("1") : pair.substr(eq + 1);
            auto number = [&](double& out) {
                char* end = nullptr;
                out = std::strtod(value.c_str(), &end);
                if (!end || *end != '\0')
                {
                    error = "layer option '" + key + "' needs a number, got '" + value + "'";
                    return false;
                }
                return true;
            };
            double v = 0.0;
            if (key == "kind")
                options.kind = parseLayerKind(value);
            else if (key == "minzoom" || key == "min")
            {
                if (!number(v)) return false;
                options.minZoom = v;
            }
            else if (key == "maxzoom" || key == "max")
            {
                if (!number(v)) return false;
                options.maxZoom = v;
            }
            else if (key == "scale")
            {
                std::optional<double> scale = parseScaleDenominator(value);
                if (!scale)
                {
                    error = "layer option 'scale' needs 1:N, N, Nm or Nk, got '" + value + "'";
                    return false;
                }
                options.scaleDenominator = *scale;
            }
            else if (key == "group")
                options.lodGroup = value;
            else if (key == "cap" || key == "maxlevel")
            {
                if (!number(v)) return false;
                options.detailLevelCap = static_cast<int>(v);
            }
            else if (key == "colour" || key == "color")
            {
                if (!parseColour(value, options.vector.colour))
                {
                    error = "layer option 'colour' needs rrggbb[aa] or r,g,b[,a], got '" + value + "'";
                    return false;
                }
            }
            else if (key == "width")
            {
                if (!number(v)) return false;
                options.vector.lineWidth = static_cast<float>(v);
            }
            else if (key == "pointsize")
            {
                if (!number(v)) return false;
                options.vector.pointSize = static_cast<float>(v);
            }
            else if (key == "exaggeration" || key == "exag")
            {
                if (!number(v)) return false;
                options.terrain.exaggeration = v;
            }
            else if (key == "interp" || key == "interpolation")
                options.terrain.interpolation = parseHeightInterpolation(value);
            else if (key == "oversample")
            {
                if (!number(v)) return false;
                options.terrain.oversampleLevels = static_cast<int>(v);
            }
            else if (key == "cells")
            {
                if (!number(v)) return false;
                options.terrain.cellsPerTile = static_cast<int>(v);
            }
            else if (key == "texture")
            {
                if (!number(v)) return false;
                options.terrain.texturePixels = static_cast<int>(v);
            }
            else if (key == "hillshade")
                options.terrain.hillshade = !(value == "0" || lower(value) == "false" || lower(value) == "off");
            else
            {
                error = "unknown layer option '" + key + "'";
                return false;
            }
        }
        return true;
    }

    std::string layerSpec(const std::string& path, const LayerOptions& o)
    {
        const LayerOptions d;
        std::vector<std::string> parts;
        if (o.kind != d.kind) parts.push_back(std::string("kind=") + layerKindName(o.kind));
        if (o.minZoom != d.minZoom) parts.push_back("minzoom=" + trimmed(o.minZoom));
        if (o.maxZoom != d.maxZoom) parts.push_back("maxzoom=" + trimmed(o.maxZoom));
        if (o.scaleDenominator != d.scaleDenominator) parts.push_back("scale=" + trimmed(o.scaleDenominator));
        if (o.lodGroup != d.lodGroup) parts.push_back("group=" + o.lodGroup);
        if (o.detailLevelCap != d.detailLevelCap) parts.push_back("cap=" + std::to_string(o.detailLevelCap));
        if (o.vector.colour != d.vector.colour) parts.push_back("colour=" + hexColour(o.vector.colour));
        if (o.vector.lineWidth != d.vector.lineWidth) parts.push_back("width=" + trimmed(o.vector.lineWidth));
        if (o.vector.pointSize != d.vector.pointSize) parts.push_back("pointsize=" + trimmed(o.vector.pointSize));
        if (o.terrain.exaggeration != d.terrain.exaggeration) parts.push_back("exaggeration=" + trimmed(o.terrain.exaggeration));
        if (o.terrain.interpolation != d.terrain.interpolation) parts.push_back(std::string("interp=") + heightInterpolationName(o.terrain.interpolation));
        if (o.terrain.oversampleLevels != d.terrain.oversampleLevels) parts.push_back("oversample=" + std::to_string(o.terrain.oversampleLevels));
        if (o.terrain.cellsPerTile != d.terrain.cellsPerTile) parts.push_back("cells=" + std::to_string(o.terrain.cellsPerTile));
        if (o.terrain.texturePixels != d.terrain.texturePixels) parts.push_back("texture=" + std::to_string(o.terrain.texturePixels));
        if (o.terrain.hillshade != d.terrain.hillshade) parts.push_back(std::string("hillshade=") + (o.terrain.hillshade ? "1" : "0"));
        if (parts.empty()) return path;
        std::string spec = path + "?";
        for (std::size_t i = 0; i < parts.size(); ++i) spec += (i ? "&" : "") + parts[i];
        return spec;
    }
} // namespace ivevsg
