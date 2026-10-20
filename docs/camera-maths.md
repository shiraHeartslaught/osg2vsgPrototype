# Camera maths behind the views

A study guide to how the Mercator, globe and trackball cameras in this project are built, with the
derivations behind each formula and pointers to the literature and source code they come from.
Every section names the function that implements it so you can read the two side by side.

Notation: `λ` longitude, `φ` latitude (degrees in the code, radians in the formulas), `h` altitude or
eye height in metres, `fov` the camera's *vertical* field of view, `W × H` the viewport in pixels.
`a = 6378137 m` and `b = 6356752.314245 m` are the WGS84 semi-axes.

## 0. Map of the code

| Concept | Code | Tests |
|---|---|---|
| Pixels ↔ metres ↔ zoom level | `lib/src/MapNavigation.cpp:17-34` | `tests/test_map_navigation.cpp` |
| Flat map camera, pan, zoom, limits, wrap | `lib/src/MapNavigation.cpp:36-186` | `tests/test_map_navigation.cpp` |
| Spherical Mercator forward / inverse | `lib/src/MercatorProjection.cpp:16-31` | `tests/test_mercator_projection.cpp` |
| WGS84 geodetic ↔ ECEF, surface normal | `lib/src/GeocentricProjection.cpp:22-70` | `tests/test_geocentric.cpp` |
| Globe camera: tangent frame, heading/tilt pose | `lib/src/GlobeNavigation.cpp:26-110` | `tests/test_globe_navigation.cpp` |
| Pixel ray and ray / ellipsoid intersection | `lib/src/GlobeNavigation.cpp:49-69` | `tests/test_globe_navigation.cpp` |
| Spin, zoom-at-cursor, clamping | `lib/src/GlobeNavigation.cpp:125-197` | `tests/test_globe_navigation.cpp` |
| Framing a bounding box | `lib/src/CameraFraming.cpp`, `lib/src/GeocentricProjection.cpp:72` | `tests/test_camera_framing.cpp` |
| Scene repeated sideways for wrapping | `lib/src/MapScene.cpp:110` | `tests/test_map_scene.cpp` |
| The three views and the hand-over | `lib/src/MercatorView.cpp`, `lib/src/GlobeView.cpp`, `lib/src/TrackballView.cpp`, `lib/src/ViewerApp.cpp:208` | `tests/test_world_view.cpp` |

The one design rule everything follows: each view keeps a tiny *navigation state* (`MapCamera` or
`GlobeCamera`) and recomputes the VSG camera from it after every input. The state is the truth, the
`LookAt` is derived. That is why hand-over between views only has to copy five numbers, and why every
pose is guaranteed to be well formed (the derivation never produces a degenerate up vector).

## 1. The camera VSG actually draws with

VSG describes a camera with two objects, and both views only ever write these:

- `vsg::LookAt { eye, center, up }` becomes the view matrix. `transform.h:179` in the VSG headers
  (`vcpkg_installed/x64-linux-release-clang/include/vsg/maths/transform.h`) builds an orthonormal basis
  by Gram–Schmidt:

  ```
  forward = normalize(center − eye)
  side    = normalize(forward × up)        // screen right
  u       = side × forward                 // screen up, now exactly perpendicular to forward
  view    = [ side ; u ; −forward ] · translate(−eye)
  ```

  `up` only has to be *not parallel* to `forward`; the cross products fix anything else. Looking
  straight down a flat map with `up = +z` would be parallel, which is why the flat framing uses
  north (`+y`) as up (`lib/src/CameraFraming.cpp:41`).

- `vsg::Perspective { fieldOfViewY, aspectRatio, nearDistance, farDistance }` becomes the projection
  matrix (`transform.h:140`). VSG uses *reverse depth* (1 at the near plane, 0 at the far plane) with a
  32-bit float depth buffer, which keeps precision roughly logarithmic in distance. That is what lets
  the globe camera use a near plane of 0.1 % of the eye height together with a far plane beyond the
  far side of the earth without z-fighting. Background: NVIDIA, *Depth Precision Visualized*, and the
  article linked from the VSG header.

The two `applyToCamera` functions (`lib/src/MapNavigation.cpp:177`, `lib/src/GlobeNavigation.cpp:274`)
are the only places the camera is written.

## 2. Pixels, metres and zoom levels (shared by both views)

**Metres per pixel.** A pinhole camera at height `h` looking straight down sees a strip of ground
`2·h·tan(fov/2)` tall (similar triangles: half the strip subtends half the field of view). Those
metres are spread over `H` pixels:

```
mpp = 2 · h · tan(fov/2) / H                       MapNavigation.cpp:17
```

Width never enters because the field of view is vertical and pixels are square.

**Web Mercator zoom level.** The slippy-map convention (OGC *WebMercatorQuad*, Bing/Google tiles) is
that at zoom `z` the whole world is `256 · 2^z` pixels wide and the world is the equator length
`Wm = 2πa = 40 075 016.69 m`. Equating pixel widths:

```
256 · 2^z = Wm / mpp     ⇒     z = log2( Wm / (256 · mpp) )     MapNavigation.cpp:23
```

and the inverse gives the altitude for a zoom level (`:29`). The globe has no Mercator plane but the
same number is still meaningful: it is the zoom whose tile resolution matches the ground under the
screen centre, which is exactly what you want for choosing a tile level or comparing with the map.

**Ground under a pixel, flat map.** Pixel `(px, py)` is `(px − W/2, py − H/2)` pixels from the
centre; multiply by `mpp` and flip y because pixel rows grow downward while map y grows north:

```
ground = centre + ( (px − W/2)·mpp , −(py − H/2)·mpp )       MapNavigation.cpp:36
```

References: OGC 17-083r4 *Two Dimensional Tile Matrix Set*, Annex D (WebMercatorQuad); J. Schwartz,
*Bing Maps Tile System* (the `groundResolution` formula is the same thing with `cos φ` folded in).

## 3. The flat Mercator view

### 3.1 Projection (`lib/src/MercatorProjection.cpp`)

EPSG:3857 is the *spherical* Mercator formula applied with the WGS84 semi-major axis as the sphere
radius (hence "pseudo"; it is not conformal on the ellipsoid, see EPSG Guidance Note 7-2, method
1024):

```
x = a · λ
y = a · ln tan(π/4 + φ/2)          equivalently  a · asinh(tan φ)
φ = 2 · atan(e^(y/a)) − π/2        inverse
```

Why the world is a square: `y` reaches `±π·a` (the same as half the equator) at
`φ = atan(sinh π) = 85.05112878°`, so clipping there gives a square `2πa` on a side. The poles are at
`y = ±∞` and cannot be shown, which drives the view limits below. Derivation and history: Snyder,
*Map Projections: A Working Manual*, chapter 7 (Mercator, formulas for the sphere).

### 3.2 State → pose

`MapCamera { centre (x, y), altitude }`. The camera always looks straight down with north up:

```
eye    = (cx, cy, ground + h)
center = (cx, cy, ground)
up     = (0, 1, 0)
near   = max(1, 0.001·h),   far = 10·h + padding          MapNavigation.cpp:177
```

### 3.3 Pan (`:43`)

The ground follows the cursor, so the camera goes the other way: `Δcentre = (−Δpx·mpp, +Δpy·mpp)`.

### 3.4 Zoom about the cursor (`:52`)

Let `k` be the pixel offset of the cursor from the screen centre (y flipped). Before zooming the
ground under the cursor is `A = c + k·mpp`. Scaling the altitude by `f` scales `mpp` by `f` too. We
want the same ground point under the same pixel afterwards:

```
A = c' + k·f·mpp  ⇒  c' = A − f·(A − c) = A + f·(c − A)
```

So the centre slides along the line towards the anchor by the zoom factor. This is a plain
similarity transform about `A`; the globe reuses it in lon/lat (§4.8).

### 3.5 Limits and wrapping (`:78`)

- *Never look past the top or bottom edge.* The visible half height is `h·tan(fov/2)`; keeping it
  inside the map's half height gives `h ≤ limitHalfHeight / tan(fov/2)`. A non-wrapping map is
  also capped by width, with `aspect` converting the vertical field of view to a horizontal one.
- The centre's y is then clamped to `[min + halfH, max − halfH]`; if the view is taller than the
  map that interval is inverted and the view is simply centred (`clampRange`).
- *Wrapping.* x is folded with `remainder(x, Wm)` into `[−Wm/2, Wm/2]`, and the scene is drawn
  `2n + 1` times translated by multiples of `Wm` (`lib/src/MapScene.cpp:110`) with
  `n = ceil(aspect / 2)` so any window up to `2n` times wider than tall is covered. The longitude
  shown for the pointer is likewise folded with `remainder(λ, 360)` (`lib/src/MercatorView.cpp:96`).

### 3.6 Framing a bounding box (`lib/src/CameraFraming.cpp`)

`summarize` takes the box centre and `radius = 0.6 · |diagonal|` (a little more than the true
half-diagonal); the eye sits `2.55 · radius` away. For reference, the exact distance that fits a
sphere of radius `r` into a vertical field of view is `d = r / sin(fov/2)` (3.86 r at 30°). The
smaller constant here was tuned on real data where the box is a flat slab, so its diagonal
overstates what has to fit.

## 4. The globe view

### 4.1 WGS84 and ECEF (`lib/src/GeocentricProjection.cpp`)

```
f  = (a − b)/a = 1/298.257223563       flattening
e² = 1 − b²/a² = 0.00669438            first eccentricity squared
N(φ) = a / sqrt(1 − e² sin²φ)          prime vertical radius of curvature (east–west)
M(φ) = a (1 − e²) / (1 − e² sin²φ)^1.5 meridional radius of curvature (north–south)
```

Geodetic to Earth-Centred Earth-Fixed (`geoToWorld`, `:22`):

```
X = (N + h) cos φ cos λ
Y = (N + h) cos φ sin λ
Z = (N (1 − e²) + h) sin φ
```

The `1 − e²` on Z is where the ellipsoid differs from a sphere: the normal at a point does *not*
pass through the centre, which is why geodetic latitude (the angle of the normal) differs from
geocentric latitude by up to 0.19°.

The inverse has no closed form for arbitrary `h`. `worldToGeo` (`:32`) starts with Bowring's
formula, which is exact on the surface, and then iterates `φ = atan2(Z, p (1 − e² N/(N + h)))` a few
times, where `p = sqrt(X² + Y²)`. References: Bowring (1976), *Survey Review* 23(181); EPSG Guidance
Note 7-2, "Geographic/geocentric conversions"; OSG does the same in
`include/osg/CoordinateSystemNode` (`convertXYZToLatLongHeight`) and VSG in
`src/vsg/app/EllipsoidModel.cpp`.

### 4.2 The local east-north-up frame (`lib/src/GlobeNavigation.cpp:26`)

Everything about heading and tilt is expressed in the tangent frame at the surface point under the
screen centre:

```
up    = (cos φ cos λ, cos φ sin λ, sin φ)       the ellipsoid normal (geodetic, not radial)
north = normalize( ẑ − (ẑ·up) up )             the earth's axis projected onto the tangent plane
east  = north × up
```

Sanity check at `λ = φ = 0`: `up = x̂`, `north = ẑ`, `east = ẑ × x̂ = ŷ`, i.e. east points along
+Y as it should. At a pole `ẑ` is parallel to `up`, the projection vanishes and north is undefined;
that is why `maxLatitude` is 89.99° and the code falls back to an arbitrary tangent there.
Background: "Local tangent plane coordinates" (ENU) on Wikipedia; OSG's
`computeLocalToWorldTransformFromLatLongHeight` and VSG's `computeLocalToWorldTransform` build the
same frame as a matrix.

### 4.3 GlobeCamera → pose (`frameOf`, `:34`)

`GlobeCamera { λ, φ, altitude, heading, tilt }`: heading is the compass direction the top of the
screen points along, tilt is how far the view is tipped from straight down. Same five numbers as a
KML `<LookAt>` (longitude, latitude, range, heading, tilt), Cesium's `HeadingPitchRange` (pitch =
tilt − 90°) and osgEarth's `Viewpoint`.

```
ahead    = north cos(heading) + east sin(heading)      ground direction towards the top of the screen
offset   = up cos(tilt) − ahead sin(tilt)              unit vector from the surface point to the eye
eye      = centre + altitude · offset
forward  = −offset
cameraUp = ahead cos(tilt) + up sin(tilt)
right    = forward × cameraUp
```

Why `cameraUp` has that form: it must lie in the plane of `up` and `ahead` and be perpendicular to
`forward`. Check: `forward · cameraUp = −(cos t sin t − sin t cos t) = 0` for every tilt. At zero
tilt it collapses to `cameraUp = ahead`, the same choice the flat map makes with north.

### 4.4 Pose → GlobeCamera (`globeCameraFromLookAt`, `:86`)

Needed so a framing pose or a hand-over `LookAt` can be read back as navigation state:

```
o       = (eye − surface) / |eye − surface|
tilt    = acos(o · up)
ahead   = −normalize( o − (o·up) up )       the horizontal part of the offset, reversed
heading = atan2(ahead · east, ahead · north)
```

When the tilt is zero the horizontal part vanishes and the given `up` vector is projected onto the
tangent plane instead. `GlobeView::frame` (`lib/src/GlobeView.cpp:45`) uses this to turn the
projection's framing pose into a `GlobeCamera`.

### 4.5 From a pixel to a ray (`pixelRay`, `:49`)

With `tanHalf = tan(fov/2)`, the ray through a pixel in camera space is

```
sx = (px − W/2) / (H/2) · tanHalf
sy = −(py − H/2) / (H/2) · tanHalf           pixel y runs down
dir = normalize( forward + right·sx + cameraUp·sy )
```

Dividing both axes by `H/2` handles the aspect ratio automatically because `fov` is vertical.
Reference: Scratchapixel, *Ray-Tracing: Generating Camera Rays*.

### 4.6 Ray / ellipsoid intersection (`intersectEllipsoid`, `:57`)

The affine map `S = diag(1/a, 1/a, 1/b)` turns the ellipsoid into the unit sphere and keeps lines
straight, so intersect `S·o + t·S·d` with `|p|² = 1`:

```
A = d'·d',  B = 2 o'·d',  C = o'·o' − 1          (primes: scaled vectors)
t = (−B − sqrt(B² − 4AC)) / (2A)                  the nearer root, i.e. the visible side
```

`t` is the same parameter in both spaces, so the hit is `o + t·d` with the *unscaled* vectors.
`B² − 4AC < 0` means the ray misses the globe (sky), `t < 0` means the globe is behind the eye.
Cesium's `IntersectionTests.rayEllipsoid` does exactly this; the generic derivation is the ray/sphere
test in *Real-Time Rendering*, 4th ed., chapter 22.

### 4.7 Spinning the globe (`spinGlobeByPixels`, `:125`)

Pick the ground under the drag start and end, form the ECEF motion vector, and decompose it in the
tangent frame of the start point:

```
east  = motion · e,   north = motion · n
Δλ = −east  / (N(φ) cos φ)        metres along a parallel per radian of longitude
Δφ = −north / M(φ)                metres along a meridian per radian of latitude
```

The minus signs make the ground follow the cursor. `N cos φ` and `M` are the standard
"length of a degree" factors (Torge & Müller, *Geodesy*, chapter 4; Wikipedia "Earth radius",
radii of curvature).

Why the tangent-plane decomposition instead of simply taking the latitude and longitude difference
of the two hit points: the ground along one screen row at latitude 40° is a *great circle*, which
dips towards the equator on both sides of the centre, so hit-point latitudes change on a purely
horizontal drag. Using them directly rolls the globe while you meant only to spin it. Reading the
motion as east and north components at the start point keeps a sideways drag a pure spin about the
polar axis. (A fun experiment: swap in the hit-point differences and watch the horizon wobble.)

When the cursor is off the globe the drag is read at the ground scale of the screen centre
(`mpp` from §2) and rotated by the heading so screen-right is still "east on screen".

### 4.8 Zoom about the cursor (`zoomGlobeAboutPixel`, `:172`)

The §3.4 similarity, applied to the anchor's longitude and latitude with the longitude difference
wrapped into ±180°. It is exact looking straight down (angles are proportional to screen distance
there) and only approximate when tilted; per wheel notch the error is invisible.

### 4.9 Near and far planes (`globeCameraPose`, `:72`)

```
near = max(1, 0.001 · eyeHeight)
far  = |eye| + 1.1 · a             distance to the origin plus a little more than one radius
```

The far plane reaches the far limb of the earth from any altitude; reverse-Z depth (§1) makes the
huge near/far ratio harmless.

### 4.10 Framing the globe (`GeocentricProjection::framingPose`, `:72`)

Find the surface point below the bounding box centre, put the eye along its normal at
`2.55 · radius`, tipped towards the south by the requested elevation, then read it back through
§4.4. The up vector is `north` when looking straight down and `up` otherwise, for the same
degeneracy reason as §1.

## 5. The trackball view (VSG's own manipulator)

`TrackballView` (`lib/src/TrackballView.cpp`) keeps `vsg::Trackball` as a reference implementation
you can compare the custom views against. With an `EllipsoidModel` it converts the look-at to
lat/lon/alt before and after each gesture so rotations stay anchored to the ellipsoid; the pan and
rotate code is in `src/vsg/app/Trackball.cpp` (local copy under
`/home/heartslaught/vcpkg/buildtrees/vsg/src/v1.1.15-b8a3d221d8.clean/`). The classic mapping of a
mouse drag onto a rotation of a virtual sphere is Shoemake's *Arcball* (Graphics Interface 1992);
OSG's `osgGA/TrackballManipulator` and `osgGA/TerrainManipulator` are the OpenSceneGraph versions.

## 6. Switching views: `ViewState` as the common currency

`ViewState { centre (λ, φ), altitude, zoomLevel, metersPerPixel, heading, tilt }` describes a view in
terms every projection shares. `ViewerApp::installView` (`lib/src/ViewerApp.cpp:208`) attaches the new
view, restores the zoom range, calls `lookAt(centre, altitude)` and, where the view has an
orientation, `setOrientation(heading, tilt)`. Because `mpp` and `zoom` are functions of altitude
alone (§2), carrying the altitude carries them too.

The only thing that cannot be carried is a latitude Mercator cannot show: if a globe camera is
zoomed out further than the map's height fits on screen, `clampCamera` lowers the altitude and
centres the view vertically (§3.5). `tests/test_world_view.cpp` drives the hand-over in both
directions and checks longitude and latitude to a nanodegree.

## 7. References

**Books and papers**

- J. P. Snyder, *Map Projections: A Working Manual*, USGS Professional Paper 1395 (1987). Free PDF:
  https://pubs.usgs.gov/pp/1395/report.pdf. Chapter 3 covers ellipsoid geometry and radii of
  curvature, chapter 7 the Mercator projection.
- B. R. Bowring, "Transformation from spatial to geographical coordinates", *Survey Review* 23(181),
  1976. The closed-form ECEF → geodetic start used in `worldToGeo`.
- W. Torge, J. Müller, *Geodesy*, 4th ed., de Gruyter 2012, chapter 4 (the geodetic earth model:
  ellipsoid, geodetic vs geocentric latitude, `M` and `N`).
- T. Akenine-Möller, E. Haines, N. Hoffman et al., *Real-Time Rendering*, 4th ed. Chapter 4
  (transforms, look-at and projection matrices), chapter 22 (ray/sphere and other intersections).
- K. Shoemake, "ARCBALL: A user interface for specifying three-dimensional orientation using a
  mouse", *Graphics Interface* 1992. The trackball idea behind `vsg::Trackball`.

**Standards**

- IOGP/EPSG *Geomatics Guidance Note 7, part 2: Coordinate Conversions and Transformations
  including Formulas* (https://epsg.org/guidance-notes.html). Popular Visualisation Pseudo-Mercator
  (method 1024) and Geographic/Geocentric conversions (method 9602).
- OGC 17-083r4, *Two Dimensional Tile Matrix Set and Tile Set Metadata*,
  https://docs.ogc.org/is/17-083r4/17-083r4.html. Annex D defines WebMercatorQuad, the 256·2^z pixel
  convention behind the zoom level.
- Google, *KML Reference*, `<LookAt>`: https://developers.google.com/kml/documentation/kmlreference.
  The same longitude/latitude/range/heading/tilt camera model as `GlobeCamera`.

**Articles**

- Microsoft, *Bing Maps Tile System*: https://learn.microsoft.com/en-us/bingmaps/articles/bing-maps-tile-system
- Scratchapixel, *Ray-Tracing: Generating Camera Rays* and *Ray-Sphere Intersection*:
  https://www.scratchapixel.com/
- NVIDIA, *Depth Precision Visualized*: https://developer.nvidia.com/content/depth-precision-visualized
- Wikipedia: "Web Mercator projection", "Geographic coordinate conversion", "Local tangent plane
  coordinates", "Earth radius" (radii of curvature), "Line–sphere intersection".

**Source code worth reading next to this project**

- VSG `include/vsg/maths/transform.h` (`lookAt`, `perspective`), `src/vsg/app/Trackball.cpp`,
  `src/vsg/app/EllipsoidModel.cpp`. Installed headers under
  `/home/heartslaught/vcpkg/vcpkg_installed/x64-linux-release-clang/include/vsg/`, full source under
  `/home/heartslaught/vcpkg/buildtrees/vsg/src/v1.1.15-b8a3d221d8.clean/`.
- OpenSceneGraph `include/osg/CoordinateSystemNode` (inline `EllipsoidModel` conversions and the
  local-to-world frame), `src/osgGA/TrackballManipulator.cpp`, `src/osgGA/TerrainManipulator.cpp`.
  Source under `/home/heartslaught/vcpkg/buildtrees/osg/src/raph-3.6.5-fa57c5b444.clean/`.
- VirtualPlanetBuilder `src/vpb/Destination.cpp` in `~/VirtualPlanetBuilder`: how VPB places
  geocentric tiles with the same ellipsoid maths.
- CesiumJS (`packages/engine/Source/`): `Core/Ellipsoid.js` (geodetic ↔ cartesian, surface normal),
  `Core/IntersectionTests.js` (`rayEllipsoid`), `Scene/Camera.js` (`lookAt` with
  `HeadingPitchRange`), `Scene/ScreenSpaceCameraController.js` (spinning the globe by picking
  under the cursor). https://github.com/CesiumGS/cesium
- osgEarth `src/osgEarth/EarthManipulator.cpp` and `Viewpoint`: an OSG globe manipulator with the
  heading/pitch/range model. https://github.com/gwaldron/osgearth

**Experiments**

- Change `fieldOfViewDegrees` in `GlobeNavigationSettings` and confirm the zoom level readout stays
  consistent with the map view at the same altitude (§2).
- Print `east`, `north`, `up` from `tangentFrame` at a few places (equator, 60° N, near the pole) and
  check them against the ENU formulas (§4.2).
- Replace the tangent-plane decomposition in `spinGlobeByPixels` with the hit points' lat/lon
  differences and watch a horizontal drag roll the globe (§4.7).
- Set `tilt` to 80° and `heading` to 90° in a test, then run `globeCameraFromLookAt` on the pose and
  confirm the round trip (§4.3, §4.4); `tests/test_globe_navigation.cpp` already does this for a
  few cases.
