#pragma once

#include <ivevsg/GeocentricProjection.h>
#include <ivevsg/GlobeNavigation.h>
#include <ivevsg/WorldView.h>

namespace ivevsg
{
    /// The WGS84 globe (EPSG:4978 geocentric frame) handled like a desk globe: dragging spins it about
    /// its axis with north up, the view can be tilted and turned about the screen centre, and
    /// single-quad tiles are bent over the surface when loaded.
    class GlobeView : public WorldView
    {
    public:
        explicit GlobeView(GlobeNavigationSettings navigation = {});

        std::string key() const override { return "globe"; }
        std::string name() const override { return "WGS84 globe (EPSG:4978 geocentric)"; }
        std::shared_ptr<const Projection> projection() const override { return _projection; }

        int quadTessellation() const override { return 8; }
        bool supportsFlatBaseImage() const override { return false; }

        void attach(vsg::ref_ptr<vsg::Camera> camera, const vsg::dbox& bounds, double elevationDegrees) override;
        void detach() override;
        vsg::ref_ptr<vsg::Visitor> eventHandler() const override;
        void frame(const vsg::dbox& bounds) override;
        void home() override;
        void lookAt(const GeoPoint& target, double altitudeMetres) override;
        ViewState state() const override;
        bool hasOrientation() const override { return true; }
        void setOrientation(double headingDegrees, double tiltDegrees) override;
        std::optional<GeoPoint> groundAtPixel(double px, double py) const override;
        std::optional<GeoPoint> groundAtPointer() const override;
        void setZoomLevelRange(double minZoomLevel, double maxZoomLevel) override;
        double minZoomLevel() const override;
        double maxZoomLevel() const override;
        std::vector<std::string> controls() const override;

        vsg::ref_ptr<GlobeNavigationHandler> navigation() const { return _navigation; }

    private:
        std::shared_ptr<const GeocentricProjection> _projection;
        GlobeNavigationSettings _settings;
        double _elevationDegrees = 90.0;
        vsg::ref_ptr<vsg::Camera> _camera;
        vsg::ref_ptr<GlobeNavigationHandler> _navigation;
    };
} // namespace ivevsg
