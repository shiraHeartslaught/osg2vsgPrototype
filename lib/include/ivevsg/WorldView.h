#pragma once

#include <ivevsg/GlobeNavigation.h>
#include <ivevsg/MapNavigation.h>
#include <ivevsg/Projection.h>

#include <vsg/app/Camera.h>
#include <vsg/core/Visitor.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/maths/box.h>
#include <vsg/nodes/Node.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ivevsg
{
    /// What a view is looking at, in terms every projection shares. Used by panels and logs, and to
    /// hand the camera over when the viewer switches from one WorldView to another.
    struct ViewState
    {
        GeoPoint centre;             // ground under the middle of the screen
        double altitude = 0.0;       // metres from that point to the eye
        double zoomLevel = 0.0;      // Web Mercator zoom equivalent of the resolution at the centre
        double metersPerPixel = 0.0; // ground resolution at the centre
        double heading = 0.0;        // degrees clockwise from north the top of the screen points to (0 on flat maps)
        double tilt = 0.0;           // degrees from straight down (0 on flat maps)
    };

    /// One way of showing the world: the coordinate maths (a Projection) bundled with the camera
    /// model and input handling that suit it, and with the scene assembly rules it needs (a map that
    /// repeats sideways, a globe that does not). The viewer holds exactly one WorldView at a time and
    /// can swap it at runtime; nothing outside a WorldView needs to know which projection is current,
    /// and adding a projection means adding a WorldView, not touching the viewer.
    class WorldView
    {
    public:
        virtual ~WorldView() = default;

        virtual std::string key() const = 0;  // short id used on the command line and in panels: "mercator", "globe"
        virtual std::string name() const = 0; // human readable
        virtual std::shared_ptr<const Projection> projection() const = 0;

        // ---- scene assembly ---------------------------------------------------------------------

        /// The node to render for a scene in this projection's world frame: the scene itself on a
        /// globe, repeated sideways on a map that wraps. The viewport aspect decides how many copies.
        virtual vsg::ref_ptr<vsg::Node> assembleScene(vsg::ref_ptr<vsg::Node> world, double viewportAspect) const;
        /// Loader setting: grid that single-quad tiles are split into so they follow the surface.
        virtual int quadTessellation() const { return 1; }
        /// Whether one flat image (BaseMap) can serve as the base; false on a globe.
        virtual bool supportsFlatBaseImage() const { return true; }

        // ---- camera -----------------------------------------------------------------------------

        /// Takes over a camera: writes the start pose framing `bounds` into its LookAt / Perspective and
        /// creates the navigation handler. elevationDegrees is the start angle above the ground (90 =
        /// straight down); views without tilt ignore it.
        virtual void attach(vsg::ref_ptr<vsg::Camera> camera, const vsg::dbox& bounds, double elevationDegrees) = 0;
        virtual void detach() = 0;
        /// The event handler to register with the viewer; null before attach() and after detach().
        virtual vsg::ref_ptr<vsg::Visitor> eventHandler() const = 0;

        virtual void frame(const vsg::dbox& bounds) = 0; // re-frame these bounds (and make that the home view)
        virtual void home() = 0;
        /// Straight down over a position, north up where the view has an orientation.
        virtual void lookAt(const GeoPoint& target, double altitudeMetres) = 0;
        virtual ViewState state() const = 0;

        virtual bool hasOrientation() const { return false; } // heading / tilt are meaningful
        virtual void setOrientation(double /*headingDegrees*/, double /*tiltDegrees*/) {}

        /// Ground under a viewport pixel, or nothing (off the globe, no viewport).
        virtual std::optional<GeoPoint> groundAtPixel(double px, double py) const = 0;
        /// Ground under the last known pointer position.
        virtual std::optional<GeoPoint> groundAtPointer() const = 0;

        virtual void setZoomLevelRange(double minZoomLevel, double maxZoomLevel) = 0;
        virtual double minZoomLevel() const = 0;
        virtual double maxZoomLevel() const = 0;

        /// One line per control, for a panel or the console.
        virtual std::vector<std::string> controls() const = 0;
    };

    /// Settings every WorldView the factory can make may need.
    struct WorldViewSettings
    {
        bool freeOrbit = false; // VSG's trackball instead of the view's own navigation
        MapNavigationSettings map;
        GlobeNavigationSettings globe;
    };

    /// Views by key ("mercator" / "epsg:3857", "globe" / "epsg:4978" / "ecef"), or null for an unknown
    /// key. freeOrbit wraps the projection in the trackball view instead of its own navigation.
    std::unique_ptr<WorldView> createWorldView(const std::string& key, bool freeOrbit = false);
    std::unique_ptr<WorldView> createWorldView(const std::string& key, const WorldViewSettings& settings);

    /// The keys createWorldView() accepts as primary names, for a chooser.
    std::vector<std::string> worldViewKeys();
} // namespace ivevsg
