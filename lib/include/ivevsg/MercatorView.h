#pragma once

#include <ivevsg/MapNavigation.h>
#include <ivevsg/MercatorProjection.h>
#include <ivevsg/WorldView.h>

namespace ivevsg
{
    /// The flat Web Mercator map: straight-down, north-up camera (MapNavigationHandler), the world
    /// repeated sideways so panning east never runs out of map, and the view stopped at the Mercator
    /// latitude limit instead of showing space beyond the poles.
    class MercatorView : public WorldView
    {
    public:
        explicit MercatorView(MapNavigationSettings navigation = {});

        std::string key() const override { return "mercator"; }
        std::string name() const override { return "Web Mercator map (EPSG:3857)"; }
        std::shared_ptr<const Projection> projection() const override { return _projection; }

        vsg::ref_ptr<vsg::Node> assembleScene(vsg::ref_ptr<vsg::Node> world, double viewportAspect) const override;

        void attach(vsg::ref_ptr<vsg::Camera> camera, const vsg::dbox& bounds, double elevationDegrees) override;
        void detach() override;
        vsg::ref_ptr<vsg::Visitor> eventHandler() const override;
        void frame(const vsg::dbox& bounds) override;
        void home() override;
        void lookAt(const GeoPoint& target, double altitudeMetres) override;
        ViewState state() const override;
        std::optional<GeoPoint> groundAtPixel(double px, double py) const override;
        std::optional<GeoPoint> groundAtPointer() const override;
        void setZoomLevelRange(double minZoomLevel, double maxZoomLevel) override;
        double minZoomLevel() const override;
        double maxZoomLevel() const override;
        std::vector<std::string> controls() const override;

        vsg::ref_ptr<MapNavigationHandler> navigation() const { return _navigation; }

    private:
        std::shared_ptr<const MercatorProjection> _projection;
        MapNavigationSettings _settings;
        vsg::ref_ptr<vsg::Camera> _camera;
        vsg::ref_ptr<MapNavigationHandler> _navigation;
    };
} // namespace ivevsg
