#pragma once

#include <ivevsg/WorldView.h>

#include <vsg/app/Trackball.h>

namespace ivevsg
{
    /// Any projection with VSG's free trackball orbit (about the look-at point): a debugging view with
    /// no zoom levels, no scene wrapping and no cursor pick, kept for inspecting geometry from any angle.
    class TrackballView : public WorldView
    {
    public:
        /// key: the WorldView key of the projection's own view ("mercator", "globe"), so base maps
        /// registered per view are found.
        TrackballView(std::shared_ptr<const Projection> projection, std::string key);

        std::string key() const override { return _key; }
        std::string name() const override { return _projection->name() + " (free orbit)"; }
        std::shared_ptr<const Projection> projection() const override { return _projection; }

        int quadTessellation() const override { return _projection->geocentric() ? 8 : 1; }
        bool supportsFlatBaseImage() const override { return !_projection->geocentric(); }

        void attach(vsg::ref_ptr<vsg::Camera> camera, const vsg::dbox& bounds, double elevationDegrees) override;
        void detach() override;
        vsg::ref_ptr<vsg::Visitor> eventHandler() const override;
        void frame(const vsg::dbox& bounds) override;
        void home() override;
        void lookAt(const GeoPoint& target, double altitudeMetres) override;
        ViewState state() const override;
        std::optional<GeoPoint> groundAtPixel(double px, double py) const override;
        std::optional<GeoPoint> groundAtPointer() const override { return std::nullopt; }
        void setZoomLevelRange(double, double) override {}
        double minZoomLevel() const override { return 0.0; }
        double maxZoomLevel() const override { return 28.0; }
        std::vector<std::string> controls() const override;

    private:
        void applyPose(const CameraPose& pose);

        std::shared_ptr<const Projection> _projection;
        std::string _key;
        double _elevationDegrees = 45.0;
        vsg::ref_ptr<vsg::Camera> _camera;
        vsg::ref_ptr<vsg::Trackball> _trackball;
        CameraPose _home;
        vsg::dbox _bounds;
    };
} // namespace ivevsg
