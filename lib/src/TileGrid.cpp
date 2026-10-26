#include <ivevsg/TileGrid.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace ivevsg
{
    TileGrid TileGrid::webMercatorQuad(int tileSizePixels)
    {
        TileGrid grid;
        grid.tileSizePixels = tileSizePixels;
        return grid;
    }

    TileGrid TileGrid::worldCrs84Quad(int tileSizePixels)
    {
        TileGrid grid;
        grid.crs = "EPSG:4326";
        grid.extent = vsg::dbox(vsg::dvec3(-180.0, -90.0, 0.0), vsg::dvec3(180.0, 90.0, 0.0));
        grid.tilesAcrossAtLevel0 = 2;
        grid.tilesDownAtLevel0 = 1;
        grid.tileSizePixels = tileSizePixels;
        return grid;
    }

    bool TileGrid::contains(const TileIndex& tile) const
    {
        return tile.level >= 0 && tile.x >= 0 && tile.y >= 0 && tile.x < tilesAcross(tile.level) && tile.y < tilesDown(tile.level);
    }

    vsg::dbox TileGrid::tileExtent(const TileIndex& tile) const
    {
        const double w = tileWidth(tile.level), h = tileHeight(tile.level);
        const double minX = extent.min.x + w * tile.x;
        const double maxY = extent.max.y - h * tile.y; // rows count down from the top edge
        return vsg::dbox(vsg::dvec3(minX, maxY - h, 0.0), vsg::dvec3(minX + w, maxY, 0.0));
    }

    TileIndex TileGrid::tileAt(double x, double y, int level) const
    {
        const double w = tileWidth(level), h = tileHeight(level);
        const int col = static_cast<int>(std::floor((x - extent.min.x) / w));
        const int row = static_cast<int>(std::floor((extent.max.y - y) / h));
        return {level, std::clamp(col, 0, tilesAcross(level) - 1), std::clamp(row, 0, tilesDown(level) - 1)};
    }

    int TileGrid::levelForUnitsPerPixel(double upp) const
    {
        if (upp <= 0.0) return 0;
        const double fractional = std::log2(unitsPerPixel(0) / upp);
        return std::max(0, static_cast<int>(std::ceil(fractional - 1e-9)));
    }

    std::vector<TileIndex> TileGrid::tilesOverlapping(const vsg::dbox& box, int level) const
    {
        std::vector<TileIndex> tiles;
        if (!box.valid()) return tiles;
        const double eps = 1e-9 * std::max(tileWidth(level), tileHeight(level));
        if (box.max.x <= extent.min.x + eps || box.min.x >= extent.max.x - eps || box.max.y <= extent.min.y + eps || box.min.y >= extent.max.y - eps) return tiles;
        const TileIndex a = tileAt(box.min.x + eps, box.max.y - eps, level); // north-west
        const TileIndex b = tileAt(box.max.x - eps, box.min.y + eps, level); // south-east
        for (int y = a.y; y <= b.y; ++y)
            for (int x = a.x; x <= b.x; ++x) tiles.push_back({level, x, y});
        return tiles;
    }

    std::vector<TileIndex> TileGrid::rootTilesFor(const vsg::dbox& box, int maxLevel) const
    {
        std::vector<TileIndex> tiles = tilesOverlapping(box, 0);
        while (tiles.size() == 1 && tiles.front().level < maxLevel)
        {
            const std::vector<TileIndex> finer = tilesOverlapping(box, tiles.front().level + 1);
            if (finer.size() != 1) break;
            tiles = finer;
        }
        return tiles;
    }

    // ---- layouts on disk -----------------------------------------------------------------------

    std::string TileSetLayout::tilePath(const TileIndex& tile) const
    {
        const int rowOnDisk = rowsFromBottom ? grid.tilesDown(tile.level) - 1 - tile.y : tile.y;
        std::string path = pathTemplate;
        auto replace = [&path](const std::string& token, int value) {
            for (std::size_t at = path.find(token); at != std::string::npos; at = path.find(token, at))
            {
                const std::string text = std::to_string(value);
                path.replace(at, token.size(), text);
                at += text.size();
            }
        };
        replace("{z}", tile.level);
        replace("{x}", tile.x);
        replace("{-y}", grid.tilesDown(tile.level) - 1 - tile.y);
        replace("{y}", rowOnDisk);
        return path;
    }

    namespace
    {
        /// The value of attribute `name` inside the first `<tag ...>` element, or "".
        std::string attribute(const std::string& xml, const std::string& tag, const std::string& name, std::size_t from = 0, std::size_t* elementEnd = nullptr)
        {
            const std::size_t open = xml.find("<" + tag, from);
            if (open == std::string::npos) return {};
            const std::size_t close = xml.find('>', open);
            if (close == std::string::npos) return {};
            if (elementEnd) *elementEnd = close;
            const std::string element = xml.substr(open, close - open);
            const std::size_t at = element.find(name + "=");
            if (at == std::string::npos) return {};
            const std::size_t quote = element.find_first_of("\"'", at);
            if (quote == std::string::npos) return {};
            const std::size_t end = element.find(element[quote], quote + 1);
            if (end == std::string::npos) return {};
            return element.substr(quote + 1, end - quote - 1);
        }

        /// Text between <tag> and </tag>, or "".
        std::string elementText(const std::string& xml, const std::string& tag)
        {
            const std::size_t open = xml.find("<" + tag);
            if (open == std::string::npos) return {};
            const std::size_t start = xml.find('>', open);
            const std::size_t end = xml.find("</" + tag, start);
            if (start == std::string::npos || end == std::string::npos) return {};
            std::string text = xml.substr(start + 1, end - start - 1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
            return text;
        }

        bool toDouble(const std::string& text, double& value)
        {
            if (text.empty()) return false;
            char* end = nullptr;
            value = std::strtod(text.c_str(), &end);
            return end != text.c_str();
        }

        bool isInteger(const std::string& text)
        {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
        }

        std::string lowerExtension(const std::filesystem::path& file)
        {
            std::string ext = file.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }
    } // namespace

    bool parseTileMapResource(const std::string& xml, const std::string& directory, TileSetLayout& layout, std::string& error)
    {
        if (xml.find("<TileMap") == std::string::npos)
        {
            error = "not a TileMap document";
            return false;
        }
        TileGrid grid;
        grid.crs = elementText(xml, "SRS");
        if (grid.crs.empty()) grid.crs = "EPSG:3857";

        double originX = 0.0, originY = 0.0;
        const bool haveOrigin = toDouble(attribute(xml, "Origin", "x"), originX) && toDouble(attribute(xml, "Origin", "y"), originY);
        double tileWidth = 256.0, tileHeight = 256.0;
        toDouble(attribute(xml, "TileFormat", "width"), tileWidth);
        toDouble(attribute(xml, "TileFormat", "height"), tileHeight);
        std::string extension = attribute(xml, "TileFormat", "extension");
        if (extension.empty()) extension = "png";
        grid.tileSizePixels = std::max(1, static_cast<int>(tileWidth));

        // the levels present, each with its resolution
        std::map<int, double> levels;
        for (std::size_t from = 0;;)
        {
            std::size_t end = 0;
            const std::string order = attribute(xml, "TileSet ", "order", from, &end);
            if (order.empty()) break;
            double upp = 0.0;
            if (toDouble(attribute(xml, "TileSet ", "units-per-pixel", from), upp) && upp > 0.0) levels[std::atoi(order.c_str())] = upp;
            from = end;
        }
        if (levels.empty())
        {
            error = "tilemapresource.xml lists no TileSet levels";
            return false;
        }

        // the full extent the CRS offers, which decides how many tiles sit at level 0
        const bool geographic = grid.crs.find("4326") != std::string::npos || grid.crs.find("4258") != std::string::npos || grid.crs.find("CRS84") != std::string::npos;
        double fullWidth = 2.0 * MercatorProjection::kMaxExtent, fullHeight = 2.0 * MercatorProjection::kMaxExtent;
        vsg::dvec3 defaultOrigin(-MercatorProjection::kMaxExtent, -MercatorProjection::kMaxExtent, 0.0);
        if (geographic)
        {
            fullWidth = 360.0;
            fullHeight = 180.0;
            defaultOrigin = vsg::dvec3(-180.0, -90.0, 0.0);
        }
        if (!haveOrigin)
        {
            originX = defaultOrigin.x;
            originY = defaultOrigin.y;
        }
        const auto [order0, upp0] = *levels.begin();
        const double levelWidth = upp0 * tileWidth; // one tile's width at the coarsest listed level
        const double levelHeight = upp0 * tileHeight;
        const int acrossAtListed = std::max(1, static_cast<int>(std::lround(fullWidth / levelWidth)));
        const int downAtListed = std::max(1, static_cast<int>(std::lround(fullHeight / levelHeight)));
        grid.tilesAcrossAtLevel0 = std::max(1, acrossAtListed >> order0);
        grid.tilesDownAtLevel0 = std::max(1, downAtListed >> order0);
        // the grid's extent is whatever the level-0 tiles cover from the origin (a geodetic set with
        // one 360 degree tile at level 0 reaches 270 degrees north; its top half is simply empty)
        double tile0Width = levelWidth * (1 << order0);
        double tile0Height = levelHeight * (1 << order0);
        // units-per-pixel is written with limited precision: when the level-0 tiles evidently cover
        // the whole CRS, use its exact extent (a 360 degree geodetic tile, twice the height, is kept)
        if (std::abs(tile0Width * grid.tilesAcrossAtLevel0 - fullWidth) < fullWidth * 0.01) tile0Width = fullWidth / grid.tilesAcrossAtLevel0;
        if (std::abs(tile0Height * grid.tilesDownAtLevel0 - fullHeight) < fullHeight * 0.01) tile0Height = fullHeight / grid.tilesDownAtLevel0;
        grid.extent = vsg::dbox(vsg::dvec3(originX, originY, 0.0),
                                vsg::dvec3(originX + tile0Width * grid.tilesAcrossAtLevel0, originY + tile0Height * grid.tilesDownAtLevel0, 0.0));

        layout.grid = grid;
        layout.rowsFromBottom = true; // the TMS specification counts rows from the origin, which is the south-west corner
        layout.minLevel = levels.begin()->first;
        layout.maxLevel = levels.rbegin()->first;
        layout.pathTemplate = (std::filesystem::path(directory) / ("{z}/{x}/{y}." + extension)).string();
        double minx = 0, miny = 0, maxx = 0, maxy = 0;
        if (toDouble(attribute(xml, "BoundingBox", "minx"), minx) && toDouble(attribute(xml, "BoundingBox", "miny"), miny) &&
            toDouble(attribute(xml, "BoundingBox", "maxx"), maxx) && toDouble(attribute(xml, "BoundingBox", "maxy"), maxy))
        {
            layout.bounds = vsg::dbox(vsg::dvec3(minx, miny, 0.0), vsg::dvec3(maxx, maxy, 0.0));
        }
        else
        {
            layout.bounds = vsg::dbox();
        }
        const std::string title = elementText(xml, "Title");
        layout.description = "TMS " + grid.crs + (title.empty() ? "" : " \"" + title + "\"") + ", levels " + std::to_string(layout.minLevel) + ".." + std::to_string(layout.maxLevel);
        return true;
    }

    bool looksLikeTileSet(const std::string& path)
    {
        if (path.find("{z}") != std::string::npos) return true;
        std::error_code ec;
        const std::filesystem::path p(path);
        if (std::filesystem::is_directory(p, ec)) return true;
        std::string name = p.filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return name == "tilemapresource.xml";
    }

    bool describeTileSet(const std::string& path, TileSetLayout& layout, std::string& error)
    {
        std::error_code ec;
        std::filesystem::path p(path);

        // a template: XYZ unless told otherwise through {-y}
        if (path.find("{z}") != std::string::npos)
        {
            layout.grid = TileGrid::webMercatorQuad();
            layout.pathTemplate = path;
            layout.rowsFromBottom = false;
            layout.minLevel = 0;
            layout.maxLevel = 22;
            layout.bounds = vsg::dbox();
            layout.description = "XYZ template " + path;
            // the levels actually present, when the template's directory can be listed
            const std::size_t zAt = path.find("{z}");
            const std::filesystem::path root = std::filesystem::path(path.substr(0, zAt)).parent_path();
            if (std::filesystem::is_directory(root, ec))
            {
                int minLevel = 1000, maxLevel = -1;
                for (const auto& entry : std::filesystem::directory_iterator(root, ec))
                {
                    const std::string name = entry.path().filename().string();
                    if (entry.is_directory(ec) && isInteger(name))
                    {
                        const int level = std::atoi(name.c_str());
                        minLevel = std::min(minLevel, level);
                        maxLevel = std::max(maxLevel, level);
                    }
                }
                if (maxLevel >= 0)
                {
                    layout.minLevel = minLevel;
                    layout.maxLevel = maxLevel;
                }
            }
            return true;
        }

        std::filesystem::path directory = p;
        std::filesystem::path resource;
        if (std::filesystem::is_directory(p, ec))
        {
            resource = p / "tilemapresource.xml";
        }
        else if (std::filesystem::is_regular_file(p, ec))
        {
            resource = p;
            directory = p.parent_path();
        }
        else
        {
            error = "no such file or directory: " + path;
            return false;
        }
        if (std::filesystem::is_regular_file(resource, ec))
        {
            std::ifstream in(resource);
            std::stringstream text;
            text << in.rdbuf();
            return parseTileMapResource(text.str(), directory.string(), layout, error);
        }
        if (!std::filesystem::is_directory(directory, ec))
        {
            error = "no such directory: " + directory.string();
            return false;
        }

        // XYZ tree: z/x/y.<ext>, rows from the top, Web Mercator
        int minLevel = 1000, maxLevel = -1;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        {
            const std::string name = entry.path().filename().string();
            if (!entry.is_directory(ec) || !isInteger(name)) continue;
            const int level = std::atoi(name.c_str());
            minLevel = std::min(minLevel, level);
            maxLevel = std::max(maxLevel, level);
        }
        if (maxLevel < 0)
        {
            error = directory.string() + " holds no tile levels (expected z/x/y.png directories or a tilemapresource.xml)";
            return false;
        }
        // the extension from the first tile found at the coarsest level, and that level's coverage
        std::string extension;
        TileGrid grid = TileGrid::webMercatorQuad();
        vsg::dbox bounds;
        for (const auto& column : std::filesystem::directory_iterator(directory / std::to_string(minLevel), ec))
        {
            if (!column.is_directory(ec) || !isInteger(column.path().filename().string())) continue;
            const int x = std::atoi(column.path().filename().string().c_str());
            for (const auto& file : std::filesystem::directory_iterator(column.path(), ec))
            {
                if (!file.is_regular_file(ec)) continue;
                const std::string stem = file.path().stem().string();
                if (!isInteger(stem)) continue;
                if (extension.empty()) extension = lowerExtension(file.path());
                const int y = std::atoi(stem.c_str());
                const TileIndex tile{minLevel, x, y};
                if (!grid.contains(tile)) continue;
                const vsg::dbox footprint = grid.tileExtent(tile);
                if (!bounds.valid())
                    bounds = footprint;
                else
                {
                    bounds.min = vsg::dvec3(std::min(bounds.min.x, footprint.min.x), std::min(bounds.min.y, footprint.min.y), 0.0);
                    bounds.max = vsg::dvec3(std::max(bounds.max.x, footprint.max.x), std::max(bounds.max.y, footprint.max.y), 0.0);
                }
            }
        }
        if (extension.empty())
        {
            error = directory.string() + "/" + std::to_string(minLevel) + " holds no tile images";
            return false;
        }
        layout.grid = grid;
        layout.pathTemplate = (directory / ("{z}/{x}/{y}" + extension)).string();
        layout.rowsFromBottom = false;
        layout.minLevel = minLevel;
        layout.maxLevel = maxLevel;
        layout.bounds = bounds;
        layout.description = "XYZ EPSG:3857, levels " + std::to_string(minLevel) + ".." + std::to_string(maxLevel) + ", " + extension;
        return true;
    }
} // namespace ivevsg
