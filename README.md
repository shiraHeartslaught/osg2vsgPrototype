# ivetovsgTest

[Warning]

This app if mostly coded by an AI (Claude) at this time for some prototyping work
.  It'll be expanded upon in the further with more human edits!

A VulkanSceneGraph web-map style viewer: a NASA Blue Marble base map on the Mercator plane with
OpenSceneGraph `.ive` paged terrain (or anything vsgXchange can read) draped on top.

```
ive_poc [scene file] [--base-tiles <tiles/root.ive>] [--base <image>] [--no-base] [--trackball]
        [--debug|-d] [--frames N] [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z] [--log-loads]
```

With no arguments the viewer shows the default tile pyramid (`IVEVSG_DEFAULT_TILES` in CMake, the
level-11 Blue Marble set) and falls back to the single image if that is missing. `--zoom` takes any
level; past the deepest tiles the deepest tiles are simply magnified. `--max-zoom` (default 22) and
`--min-zoom` (default 0) bound interactive zooming.

Controls (Map mode): drag = pan, mouse wheel = zoom about the cursor, arrows = pan, `+`/`-` = zoom,
`Home`/`Space` = back to the start view, `Esc` = quit. `--trackball` gives the free 3D orbit instead.

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
ive_poc --base-tiles <out dir>/root.ive [scene.ive] [--log-loads]
```

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

The base layer hangs under an "underlay" transform that the viewer keeps a small distance below the
data plane, scaled with the camera altitude (2% of it). A fixed offset cannot work across zoom levels:
400 km is invisible at zoom 2 but at zoom 14 the camera is only 18 km up, so the base tiles would be
drawn and LOD-selected as if viewed from zoom 9.5.

## Layout

- `lib/` — the `ivevsg` library (`ivevsg::ivevsg` CMake target):
  - `Projection` / `MercatorProjection` — geographic <-> world (EPSG:3857) and camera framing
  - `SceneLoader` — vsgXchange reader set, OSG plugin discovery, `.ive` support detection
  - `SceneBounds`, `CameraFraming` — geometry bounds and camera placement math
  - `BaseMap` — Blue Marble quad under the data, with CPU mipmaps
  - `MapScene` — `createLayeredScene` (base under data) and `createWrappedScene` (world repeated along x)
  - `MapNavigation` — Web Mercator zoom levels, pan/zoom arithmetic, `MapNavigationHandler`
  - `TileScheme` — OGC WebMercatorQuad tile matrix set maths (extents, resolutions, scale denominators)
  - `TileSetBuilder` — GDAL + OSG tiler behind `tools/ivetiler`
  - `ViewerApp` — window, camera, event handlers, frame loop (`Map` or `Trackball` navigation)
- `app/main.cpp` — thin command line viewer
- `tools/ivetiler/` — tile pyramid generator
- `tests/` — Google Test suite (`ctest` in the build directory, or CLion's test runner)
- `data/` — base map image and the script that regenerates it (see `data/README.md`)
- `docs/original_main.cpp.txt` — the monolithic proof of concept this grew from

## Building

The top-level CMakeLists.txt points at the vcpkg toolchain, triplet and installed tree; vsg must be
installed with the `windowing` feature and vsgxchange with the `osg` feature (overlay ports in the
vcpkg checkout provide the latter plus two osg2vsg fixes). Reload the CMake project in CLion, or:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && (cd build && ctest)
```
