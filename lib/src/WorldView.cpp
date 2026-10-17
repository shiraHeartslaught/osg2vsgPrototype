#include <ivevsg/GlobeView.h>
#include <ivevsg/MercatorView.h>
#include <ivevsg/TrackballView.h>
#include <ivevsg/WorldView.h>

#include <algorithm>
#include <cctype>

namespace ivevsg
{
    vsg::ref_ptr<vsg::Node> WorldView::assembleScene(vsg::ref_ptr<vsg::Node> world, double /*viewportAspect*/) const
    {
        return world;
    }

    namespace
    {
        std::string lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }
    } // namespace

    std::unique_ptr<WorldView> createWorldView(const std::string& key, bool freeOrbit)
    {
        return createWorldView(key, WorldViewSettings{freeOrbit});
    }

    std::unique_ptr<WorldView> createWorldView(const std::string& key, const WorldViewSettings& settings)
    {
        const std::string k = lower(key);
        if (k == "mercator" || k == "epsg:3857" || k == "map" || k == "webmercator")
        {
            if (settings.freeOrbit) return std::make_unique<TrackballView>(std::make_shared<MercatorProjection>(), "mercator");
            return std::make_unique<MercatorView>(settings.map);
        }
        if (k == "globe" || k == "epsg:4978" || k == "ecef" || k == "geocentric" || k == "epsg:4326")
        {
            if (settings.freeOrbit) return std::make_unique<TrackballView>(std::make_shared<GeocentricProjection>(), "globe");
            return std::make_unique<GlobeView>(settings.globe);
        }
        return nullptr;
    }

    std::vector<std::string> worldViewKeys()
    {
        return {"mercator", "globe"};
    }
} // namespace ivevsg
