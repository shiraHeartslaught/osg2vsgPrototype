# ivetovsgTest

[Warning]

This app if mostly coded by an AI (Claude) at this time for some prototyping work
.  It'll be expanded upon in the further with more human edits!

A VulkanSceneGraph web-map style viewer: a NASA Blue Marble base map, on the flat Web Mercator map
or on the WGS84 globe, with OpenSceneGraph `.ive` paged databases (or anything vsgXchange can read)
draped on top as layers, each reprojected from its own CRS into whichever frame is displayed.

```
ive_poc [scene files ...] [--layer FILE ...] [--layer-crs EPSG:xxxx] [--base-tiles <tiles/root.ive>]
        [--globe-tiles <tiles/root.ive>] [--base <image>] [--no-base]
        [--projection mercator|globe] [--source-crs EPSG:xxxx] [--trackball] [--debug|-d] [--frames N]
        [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z] [--elevation DEG]
        [--switch-at FRAME mercator|globe] [--log-loads] [--no-ui]
```

## Views: one module per world display

Each way of showing the world is a `WorldView` (`lib/include/ivevsg/WorldView.h`): the coordinate
maths (`Projection`), the camera model and input handling that suit it, and its scene assembly rules
live together in one module, and the viewer holds exactly one of them at a time:

- `MercatorView` — `MercatorProjection` + `MapNavigationHandler`; repeats the world sideways
  (`createWrappedScene`) and stops the camera at the Mercator latitude limit.
- `GlobeView` — `GeocentricProjection` (EPSG:4978) + `GlobeNavigationHandler`; bends single-quad
  tiles over the ellipsoid (8 x 8) when loading.
- `TrackballView` — any projection with VSG's free orbit (`--trackball`), for inspection.

The derivations behind every camera formula, with references, are in `docs/camera-maths.md`.

`--projection` picks the start view; the panel's "Display projection" chooser switches at runtime
(`ViewerApp::switchView`, or `--switch-at FRAME VIEW` for scripted runs). On a switch the viewer
reads the old view's `ViewState` (centre, altitude, heading, tilt), builds the new view's scene with
the base map registered for it, reloads every layer from its recorded `LayerSource` into the new
frame on worker threads, and hands the camera over at the same ground and altitude. Nothing outside
a `WorldView` knows which projection is current, so the camera transition never juggles two sets of
maths, and a new projection (e.g. polar stereographic) is a new `WorldView`, not a change to the
viewer. Base maps are registered per view (`ViewerApp::setBaseSource`): the Mercator pyramid for the
map, the geocentric pyramid for the globe.

Globe navigation works like a desk globe: dragging spins it about its polar axis (sideways motion
changes longitude, vertical motion latitude, north stays up) with the ground following the cursor,
and the view can be tilted and turned about the point under the screen centre.

## Reprojection

Every OSG file is reprojected on load into the displayed frame by `OsgReader`, before the osg2vsg
conversion. The file's native CRS is what it declares through its `CoordinateSystemNode` (VPB and
`ivetiler` write one at the root; a node carrying an `EllipsoidModel` means geocentric coordinates,
reported as EPSG:4978, the VPB whole-earth convention), else the CRS given for that layer (the
panel's "native CRS" chooser, `--layer-crs`), else `--source-crs` (default EPSG:3857). The resolved
CRS is written into the options the paged children are read with, so every tile of a database is
treated like its root even though only the root names the frame. The panel shows each layer's native
CRS and the display frame it was converted to ("shown as read" when they match), which is where to
look first when a conversion looks wrong.

Coordinates go through GDAL/OGR to geographic WGS84 and then into the target frame. Vertices are
re-localised around each tile's reprojected origin (float precision); PagedLOD centres move with their
content, distance ranges scale with the local change of scale, and the bounding sphere is widened to
enclose the reprojected children (a Mercator tile reaching towards a pole shrinks far less at its
near edge than at its centre); normals become the target's up vector. When source and target are the
same CRS nothing is touched. On a map that repeats sideways, longitudes are unwrapped around each
file's centre, so a geocentric tile straddling the antimeridian stays in one piece just past the map
edge, where the wrapped copy of the world shows it, instead of being torn across the whole width; the
poles of a geocentric database clamp to the Mercator edge rows.

PROJ and GDAL need their data folders (`share/proj/proj.db`, `share/gdal`): `PROJ_DATA` and
`GDAL_DATA` are exported globally on this machine as fish universal variables, and the library falls
back to the vcpkg paths recorded at configure time when they are absent (e.g. under CLion).

Every scene file is loaded as a layer above the base map; the first one frames the start view.
Layers can also be added while running, from the panel's "Browse..." button (the system file picker,
through the xdg-desktop-portal on Linux, so KDE shows its own dialog), the typed-path field with
"Add layer", or `--layer FILE`; the panel's native-CRS chooser (from file / ECEF / Web Mercator /
WGS84 degrees / custom) applies to files added from it. Neither interrupts rendering: the picker runs on a worker thread and
is polled by the panel, and each layer load runs read, OSG conversion and GPU compilation (through
VSG's thread-safe CompileManager) on its own worker thread; the main thread only registers the
result with the viewer and inserts it between frames. In-flight loads are listed in the panel. The
panel lists the layers with show/hide, Frame (re-centre on that layer) and Remove.

A Dear ImGui panel (rendered by vsgImGui as a node after the view, with its event handler ahead of
the navigation so clicks on the panel never move the view) shows the display projection chooser,
zoom, metres per pixel, OGC tile level, altitude, centre, heading / tilt and cursor coordinates,
go-to / home / north-up / max-zoom controls, the layers with their native and display CRS, the
native-CRS chooser for new layers, paging statistics and frame rate. `--no-ui` turns it off. Register more panels with
`ViewerApp::addUi()`; see `Hud.h`.

With no arguments the viewer shows the default tile pyramids (`IVEVSG_DEFAULT_TILES` for the map,
the level-11 Blue Marble set; `IVEVSG_DEFAULT_GLOBE_TILES` for the globe, the level-9 geocentric
set) and falls back to the single image on the map if that set is missing. `--zoom` takes any
level; past the deepest tiles the deepest tiles are simply magnified. `--max-zoom` (default 22) and
`--min-zoom` (default 0) bound interactive zooming.

Controls (Map mode): drag = pan, mouse wheel = zoom about the cursor, arrows = pan, `+`/`-` = zoom,
`Home`/`Space` = back to the start view, `Esc` = quit.

Controls (Globe mode): drag = spin the globe about its axis, mouse wheel = zoom about the cursor,
middle drag = tilt / turn the view about the screen centre, right drag = zoom, arrows = spin,
`+`/`-` = zoom, `Page Up`/`Page Down` = tilt, `N` = straight down with north up, `Home`/`Space` = back
to the start view. `--elevation` (default 90 on the globe) tilts the start view.

`--trackball` replaces either with VSG's free 3D orbit about the look-at point.

On the Mercator projection the map repeats sideways, so you can pan east or west forever and the
screen is always covered, and the camera stops the moment the map's top or bottom edge (latitude
±85.05°, the Mercator limit; the poles themselves are at infinity) would enter the view. Zooming out
is capped at the altitude where the whole map height just fits. Both rules come from
`Projection::viewLimits()` and `Projection::wrapWidth()`, so other projections are unaffected.

## Tiled base map (OGC WebMercatorQuad as .ive)

`ivetiler` cuts any EPSG:3857 raster into the OGC WebMercatorQuad pyramid (256 px tiles, level 0 =
156543 m/px, each level halves it, 2^z tiles across) as OpenSceneGraph `.ive` files that VSG pages in
by level of detail:

```
ivetiler <source.tif (EPSG:3857)> <out dir> [--max-level N] [--tile-size 256] [--detail-bias 1.0]
         [--threads N] [--uncompressed] [--detail-extent MINLON MINLAT MAXLON MAXLAT --detail-level N]
         [--geocentric [--globe-grid 8]]
ive_poc --base-tiles <out dir>/root.ive [scene.ive] [--log-loads]
ive_poc --projection globe --globe-tiles <ecef out dir>/root.ive
```

`--geocentric` keeps the same tiling, textures and paging but puts each tile on the WGS84 ellipsoid in
EPSG:4978 metres as an N x N grid (grid lines evenly spaced in Mercator x / y, so the texture maps
linearly), with the PagedLOD sphere sized from the grid. Such a set drapes the globe natively, and the
Mercator view reprojects it back onto the plane. `root.ive` carries a `CoordinateSystemNode` naming
the frame (with an `EllipsoidModel` for the geocentric set), which the reader uses as the native CRS.

Files are zlib-compressed (OSG's `compressed` ive option, zlib level 1: roughly 40% smaller on
imagery) and written by one worker per hardware thread; every file is independent. A global pyramid
stops at `--max-level` (default: the source's native level), and `--detail-extent`/`--detail-level`
take just the tiles touching a region deeper, e.g. to level 14 (9.5 m/px). That is the only sane way
to reach such levels: a global level 14 is 358 million tiles.

Layout: `root.ive` is the level-0 tile; `z/x/y.ive` holds the four level-(z+1) children of tile z/x/y,
each a textured quad in EPSG:3857 metres plus a PagedLOD that pages in its own children file once the
tile grows past 1:1 on screen (its bounding sphere spanning 256·√2 px on osg2vsg's 1080 px reference
screen; `--detail-bias` moves that point). The effect is that the tile level shown equals the view's
Web Mercator zoom level: zoom 3 shows level-3 tiles at ~19.6 km/px, zoom 5 level-5 tiles at ~4.9 km/px.
`--log-loads` prints every file the pager requests so you can watch it. `--max-level` defaults to the
level matching the source's own resolution; going deeper only upsamples. The sources and sets live on the data drive under
`/run/media/heartslaught/games/ive_test/`: `bluemarble_src/` holds NASA's eight 21600² Blue Marble
pieces (86400 x 43200 px, 464 m/px at the equator, native OGC level 8.4) and the derived
`bluemarble_3857_131072.tif` (EPSG:3857 world square at level-9 resolution, JPEG-in-TIFF with averaged
overviews; made by the script recorded in the session notes). `bluemarble_tiles_l11/` is the global
level 0..11 pyramid with a level-14 detail region over San Francisco Bay (5.6 million tiles in
1.4 million files, 97 GB, 2 h 20 min on 16 threads; levels 10 and 11 are upsampled from the 464 m
source, the drive's practical ceiling). `bluemarble_tiles_l9/` is the earlier level 0..9 set (11 GB).
`bluemarble_ecef_l9/` is the geocentric level 0..9 set for the globe (349,525 tiles in 87,382 files,
12 GB, 8 min on 16 threads), built with `ivetiler bluemarble_3857_131072.tif bluemarble_ecef_l9
--max-level 9 --geocentric`.

The base layer hangs under an "underlay" transform that the viewer keeps a small distance below the
data plane, scaled with the camera altitude (2% of it). A fixed offset cannot work across zoom levels:
400 km is invisible at zoom 2 but at zoom 14 the camera is only 18 km up, so the base tiles would be
drawn and LOD-selected as if viewed from zoom 9.5.

## VirtualPlanetBuilder geocentric databases

`osgdem --geocentric` (whole-earth databases, the form the production `.ive` files take) crashed in
the VPB build on this machine: `vpb::getCoordinateSystemType` imports the destination CRS as WKT and
dereferenced `GetRoot()` without a check, which is null under GDAL 3 while a `--geocentric` build's
destination is still unassigned. The one-line guard is applied in `~/VirtualPlanetBuilder` (an
uncommitted change in that clone) and kept here as `docs/vpb-gdal3-geocentric-null-root.patch`.
With it, geocentric builds of the project's two rasters live in `ive_test/ecef/`:

```
cd /run/media/heartslaught/games/ive_test/ecef
OSG_LIBRARY_PATH=<vcpkg>/plugins/osgPlugins-3.6.5 osgdem --geocentric --POLYGONAL --RGB-24 \
    -t ../source/gadas-world-3857.tif -o gadas-world-ecef.ive
ive_poc --projection globe --layer ecef/gadas-world-ecef.ive --layer ecef/cyprus/gadas-cyprus-ecef.ive
```

The root file's `CoordinateSystemNode` carries an `EllipsoidModel`, which the reader takes as EPSG:4978,
so these drape the globe as read and reproject onto the Mercator map.

## Layout

- `lib/` — the `ivevsg` library (`ivevsg::ivevsg` CMake target):
  - `WorldView` / `MercatorView` / `GlobeView` / `TrackballView` — one module per world display: projection + navigation + scene assembly
  - `Projection` / `MercatorProjection` / `GeocentricProjection` — geographic <-> world frames, camera framing, layer offsets
  - `CrsTransform` — any GDAL CRS -> geographic WGS84 (OGR)
  - `OsgReproject` — rewrites an OSG scene from its CRS into the current projection's frame
  - `SceneLoader` — reader chain (load monitor, `OsgReader`, vsgXchange), OSG plugin discovery
  - `OsgReader` — reads .ive/.osgb/.osgt/.osg with osgDB, normalises byte colour arrays, converts via osg2vsg
  - `SceneBounds`, `CameraFraming` — geometry bounds and camera placement math
  - `BaseMap` — Blue Marble quad under the data, with CPU mipmaps
  - `MapScene` — `LayerStack` (base + any number of switchable, removable layers), `createWrappedScene`
  - `Hud` — the ImGui map panel (`createMapHud`)
  - `FileDialog` — native file picker (nativefiledialog-extended, portal backend) + `AsyncFileDialog`
  - `AsyncLayerLoader` — worker-thread read/convert/compile of layers, merged by the viewer between frames
  - `MapNavigation` — Web Mercator zoom levels, pan/zoom arithmetic, `MapNavigationHandler`
  - `GlobeNavigation` — `GlobeCamera` (lon/lat/altitude/heading/tilt), ray-ellipsoid picking, spin/zoom arithmetic, `GlobeNavigationHandler`
  - `TileScheme` — OGC WebMercatorQuad tile matrix set maths (extents, resolutions, scale denominators)
  - `TileSetBuilder` — GDAL + OSG tiler behind `tools/ivetiler`
  - `ViewerApp` — window, camera, base map and layer sources, the current `WorldView` and runtime switching, frame loop
- `app/main.cpp` — thin command line viewer
- `tools/ivetiler/` — tile pyramid generator
- `tests/` — Google Test suite (`ctest` in the build directory, or CLion's test runner)
- `data/` — base map image and the script that regenerates it (see `data/README.md`)
- `docs/original_main.cpp.txt` — the monolithic proof of concept this grew from
- `docs/camera-maths.md` — the maths behind the Mercator, globe and trackball cameras, with references

## Building

The top-level CMakeLists.txt points at the vcpkg toolchain, triplet and installed tree; vsg must be
installed with the `windowing` feature and vsgxchange with the `osg` feature (overlay ports in the
vcpkg checkout provide the latter plus one osg2vsg fix for mipmapped RGB images; the byte-colour-array
fix lives in this library's `OsgReader` rather than as a patch). Reload the CMake project in CLion, or:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && (cd build && ctest)
```
