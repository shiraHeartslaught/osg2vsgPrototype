# ivetovsgTest

[Warning]

This app if mostly coded by an AI (Claude) at this time for some prototyping work
.  It'll be expanded upon in the further with more human edits!

A VulkanSceneGraph web-map style viewer: a NASA Blue Marble base map, on the flat Web Mercator map
or on the WGS84 globe, with OpenSceneGraph `.ive` paged databases (or anything vsgXchange can read)
draped on top as layers, each reprojected from its own CRS into whichever frame is displayed. DTED
and other elevation rasters become terrain surfaces that stay smooth past their own resolution;
shapefiles and other OGR vector files become line layers that gain detail per zoom level, switch
between scales, and drape over the terrain.

```
ive_poc [scene files ...] [--layer FILE[?opts] ...] [--terrain FILE|DIR ...] [--vector FILE ...] [--layer-crs EPSG:xxxx]
        [--base-tiles <tiles/root.ive>] [--globe-tiles <tiles/root.ive>] [--base <image>] [--no-base]
        [--projection mercator|globe] [--source-crs EPSG:xxxx] [--trackball] [--debug|-d] [--frames N]
        [--zoom Z] [--center LON LAT] [--max-zoom Z] [--min-zoom Z] [--globe-max-zoom Z] [--map-max-zoom Z]
        [--global-min-zoom Z] [--no-drape] [--drape-min-zoom Z] [--resolution-tolerance N]
        [--exaggeration F] [--terrain-interp nearest|bilinear|bicubic] [--terrain-oversample N] [--vector-max-level N]
        [--elevation DEG] [--switch-at FRAME mercator|globe] [--script FILE] [--log-loads] [--no-ui]
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

## Other data: GeoTIFF, TMS / XYZ tile sets, VSG native files

Every file the viewer shows goes through one `vsg::ReaderWriter` chain (`SceneLoader`), and each
reader places what it reads in the current display frame, so all of these work on the Mercator map
and on the globe alike, as layers (`--layer`, the panel's Browse / Tile folder buttons, or a typed
path) or as a base map (`--base-tiles` / `--globe-tiles`):

| Data | Reader | How it is placed |
|---|---|---|
| `.ive`, `.osgb`, `.osgt`, `.osg` | `OsgReader` | reprojected from the CRS the file declares (or the override / default) |
| `.vsgb`, `.vsgt` (single files, or `ivetiler --format vsgb` pyramids) | `VsgSceneReader` | reprojected by `VsgReproject` from the CRS tagged in the file (or the override / default); paged children inherit it |
| GeoTIFF, JPEG 2000, VRT, ERDAS `.img`, ... (anything GDAL reads, by extension) | `TileLayerReader` + `RasterTileSource` | warped by GDAL into WebMercatorQuad tiles on demand, from its own CRS or the override |
| TMS directory (`tilemapresource.xml`), XYZ directory, `{z}/{x}/{y}.png` template | `TileLayerReader` + `DirectoryTileSource` | tile images read as they are; EPSG:3857 or EPSG:4326 grids |
| DTED (`.dt0`, `.dt1`, `.dt2`), SRTM `.hgt`, a directory of cells, any GDAL DEM with `--terrain` / `?kind=terrain` | `TerrainLayerReader` + `ElevationSource` | a paged terrain surface in the display frame (see "Terrain" below) |
| Shapefile, GeoJSON, GeoPackage, KML, GML, FlatGeobuf, ... (anything OGR reads) | `VectorLayerReader` + `VectorSource` | paged line layers in the display frame, draped over the terrain (see "Vector layers" below) |

**Rasters and tile sets** become a quadtree of `vsg::PagedLOD` nodes whose children have virtual
names (`ivevsg-tile://<layer>/z/x/y`) that the database pager hands back to `TileLayerReader`, so a
100 000 pixel GeoTIFF pages exactly like a pre-built `.ive` pyramid and nothing is cut in advance.
Each tile is a textured grid: one quad on the Mercator plane when the tiles are Mercator, an 8 x 8 grid
following the surface otherwise (a geodetic TMS on the map, anything on the globe). Textures are RGBA:
the raster's alpha band, its nodata value and everything outside its footprint are transparent, so a
regional GeoTIFF sits cleanly on the base map. The tile hand-over threshold is the same as the
`.ive` pyramids use (`subdivisionPixelThreshold`, `LoaderSettings::detailBias`).

Notes and limits:

- A raster must be georeferenced and name a CRS; one that names none is loaded with the native CRS
  chosen in the panel (or `--layer-crs`). Colour-table images are expanded to RGBA; 16-bit data is
  truncated to 8 bits.
- Large rasters should carry overviews (`gdaladdo -r average file.tif 2 4 8 16 ...`); without them
  the coarse tiles read the whole file. The panel's tooltip and the log say "(no overviews)".
- Tiling is always WebMercatorQuad, so data beyond +-85.05 degrees is not shown, on the globe either
  (the same holds for the `.ive` pyramids).
- `tilemapresource.xml` is read for the SRS, origin, tile size, bounding box and levels; a directory
  without one is taken as XYZ (rows from the north, EPSG:3857) and its levels are scanned. A template
  may use `{-y}` for TMS row numbering.
- VSG native files carry their CRS as a value on the root node (`ivevsg.crs`, see `tagCrs()`);
  `ivevsgconv` writes it when converting:

```
ivevsgconv <input.ive|.osgb|.tif|...> <output.vsgb|.vsgt> [--crs EPSG:xxxx] [--source-crs EPSG:xxxx] [--target EPSG:xxxx]
```

  `--target` reprojects into a display frame before writing (the output is then tagged with it),
  which is also a handy way to inspect what the loader does to a file: write a `.vsgt` and read it.

  Only the file named is converted; a paged database's children stay as they are and are found
  through the absolute paths VSG records, so keep the output next to the input. Files from other
  tools (`vsgconv`) have no tag and are taken as the loader default (`--source-crs`) unless a
  native CRS is chosen when adding them.

## Terrain (DTED and other elevation rasters)

`--terrain FILE`, a `.dt0` / `.dt1` / `.dt2` / `.hgt` file given as a layer, or a directory of such
cells (mosaicked through a GDAL VRT at the finest resolution) becomes a terrain surface: a
quadtree of `vsg::PagedLOD` tiles on the WebMercatorQuad grid, each a 32 x 32 mesh whose vertices
carry the sampled heights (`ElevationSource`, GDAL underneath; projected DEMs are warped to
geographic coordinates on the fly) placed in the display frame, above the ellipsoid on the globe and
as z on the flat map, with a hillshaded hypsometric texture (128 px per tile) computed from the same
heights. Any GDAL raster is read as terrain when asked (`--terrain`, `?kind=terrain`, or the panel's
"Load as" chooser); by extension only DTED and HGT are.

Zooming in past the data's resolution keeps the surface smooth: tiles hand over to children at the
same screen size as imagery tiles, and keep subdividing `oversampleLevels` (default 4, i.e. 16 x
finer than the posts) past the *native level*, the tile level at which a mesh cell equals a post
(level 14 for DTED 1 with 32 cells, 16 for DTED 2; `TerrainLayerReader::nativeLevelFor`). Below the
native level a tile reads the posts it covers averaged down to about two per texel; at or past it the
tile reads the native posts with a margin and interpolates between them, bicubic (Catmull-Rom) by
default, bilinear or nearest on request. The result is that the DTED is shown at full resolution and
then magnified with smooth slopes within the viewport rather than steps, until the oversample limit,
where the deepest tiles simply stay on screen. Per-layer settings (`?key=value` on the path, the
panel's layer settings, the `--exaggeration` / `--terrain-interp` / `--terrain-oversample` defaults):

| option | meaning | default |
|---|---|---|
| `exaggeration=F` | vertical scale (the registry reports exaggerated heights, so draping and the camera match the mesh) | 1 |
| `interp=nearest|bilinear|bicubic` | between posts, past the native level | bicubic |
| `oversample=N` | tile levels past the native level that keep subdividing | 4 |
| `cells=N`, `texture=N` | mesh cells and hillshade texels per tile side | 32, 128 |
| `hillshade=0|1` | shade by slope, or a flat hypsometric tint | 1 |
| `cap=N` | deepest tile level, whatever the data says | own limit |

While a terrain layer is shown its source is registered in the loader's `ElevationRegistry`
(shared across view switches), which answers "height under this point" for the camera and for
vector draping; hiding or removing the layer takes it out again. The camera rides the terrain: every
frame the height under the screen centre becomes the view's *surface height*
(`WorldView::setSurfaceHeight`), from which the altitude and zoom level count, so zooming into a
mountain never sinks the eye into it; on the globe the pick under the cursor meets the ellipsoid
lifted by that height. The panel shows the terrain height under the cursor and under the centre.

`data/fetch_dted_sample.sh` makes genuine DTED cells to try this with: two Copernicus GLO-30 tiles
over San Francisco Bay converted with GDAL to DTED level 2 (`n37_w123.dt2`, `n37_w122.dt2`) and one
level 1 cell, in `ive_test/dted/`. The whole directory loads as one terrain:

```
ive_poc --projection globe --terrain /run/media/heartslaught/games/ive_test/dted --center -122.45 37.75 --zoom 12
ive_poc --terrain "/run/media/heartslaught/games/ive_test/dted/n37_w123.dt2?exaggeration=2&interp=bilinear"
```

## Vector layers (shapefiles and other OGR files)

`--vector FILE`, a `.shp` / `.geojson` / `.gpkg` / `.kml` / ... given as a layer, or anything with
`?kind=vector`, is read through OGR (`VectorSource`: every layer of the data source, transformed to
WGS84 degrees, curves linearised; a shapefile without `.prj` is taken as EPSG:4326, or the native
CRS chosen when adding it) and drawn as lines (polygon outlines, line strings, and crosses for
points) in the display frame. The features are kept in memory once and served as a quadtree of
`vsg::PagedLOD` tiles on the WebMercatorQuad grid: each tile holds the features clipped to its
footprint, simplified (Douglas-Peucker) to half a pixel at the tile's hand-over size and densified so
that no edge spans more than a 32nd of the tile, so a long border follows the globe and the terrain.
Tiles hand over at the same screen size as imagery and page down to level 16
(`--vector-max-level`, `?cap=N`), so a boundary file gains detail as the view zooms in and costs
little when zoomed out. One pipeline per layer draws them with a dynamic line width
(`vsg::SetLineWidth`, needs the device's `wideLines` feature, which the viewer enables when present;
otherwise widths clamp to a pixel) and one shared colour, both editable live from the panel
(`?colour=rrggbb[aa]`, `?width=PX`, `?pointsize=PX` set the start values).

**Draping.** With "Drape vector layers over terrain" on (default; `--no-drape` turns it off) a
vector tile at or past the drape level (default 8, `--drape-min-zoom`, the panel's "from zoom")
puts every vertex on the terrain from the `ElevationRegistry`, a little above it, when any terrain
covers it. Coarser tiles stay on the ellipsoid (lifted two ground pixels per layer index, the same
lift that keeps overlapping layers from z-fighting). Loading or removing a terrain layer, or changing
the draping settings, reloads the vector layers so their tiles pick the change up.

**Scales and LOD groups.** A vector layer may declare the resolution it was drawn for, as a scale
denominator: Natural Earth names carry it (`ne_10m_` = 1:10 000 000, `ne_50m_`, `ne_110m_`), and
`?scale=1:50000000` / `?scale=50m` / the panel's "scale" field set it for anything else. A layer at
1:S *resolves* up to the zoom level at which one OGC 0.28 mm pixel covers the ground S maps to
(`zoomLevelForScaleDenominator`: 5.8 for 1:10M, 3.5 for 1:50M, 2.3 for 1:110M), plus the
*resolution tolerance* (default 2 levels, `--resolution-tolerance`). Layers that share an *LOD group*
(`?group=NAME`; by default the name without its Natural Earth prefix, so the three scales of
`admin_0_countries` group themselves) are alternatives: the coarsest member that still resolves at
the current zoom is the one drawn, and past the finest member's reach the finest stands in,
magnified. So with all three Natural Earth country sets loaded, zooming in moves from the 1:110M
to the 1:50M to the 1:10M boundaries by itself; the panel says which one is showing and why the
others are hidden. `data/fetch_natural_earth.sh` downloads the country, state / province and US
county sets at all available scales:

```
ive_poc --projection globe --terrain ive_test/dted \
        --layer ive_test/naturalearth/ne_110m_admin_0_countries.shp \
        --layer ive_test/naturalearth/ne_50m_admin_0_countries.shp \
        --layer "ive_test/naturalearth/ne_10m_admin_0_countries.shp?colour=ff4040&width=2" \
        --layer "ive_test/naturalearth/ne_10m_admin_1_states_provinces.shp?colour=40ff80&minzoom=4" \
        --layer "ive_test/naturalearth/ne_10m_admin_2_counties.shp?colour=ffff40&minzoom=7"
```

## Zoom rules: what shows when

Every layer has a zoom window and every frame the viewer decides what is drawn
(`evaluateLayerVisibility`, `LayerVisibility.h`), hidden layers being skipped by the traversal so
nothing below them pages in:

- **Layers load from zoom** (`--global-min-zoom`, the panel slider): below this zoom no layer is
  drawn or paged at all, whatever its own window says.
- **Per layer min / max zoom** (`?minzoom=`, `?maxzoom=`, the layer's settings in the panel): the
  layer appears only between them.
- **LOD groups** as above pick one member per group.
- **Globe max zoom / Map max zoom** (`--globe-max-zoom`, `--map-max-zoom`, the panel sliders) bound
  how far each view can be zoomed in, on top of the shared `--min-zoom` / `--max-zoom`; the ceiling
  is remembered per view across switches.
- **Draping** as above, from the drape level on.

The panel's layer list shows each layer's kind, why it is hidden or magnified ("shows from zoom 7",
"another scale of 'admin_0_countries' shows at this zoom", "beyond 1:10000000, magnified"), and a
Settings node with the zoom window, declared scale and group (applied at once), the detail cap, and
the kind-specific style (colour and width live for vectors; exaggeration, interpolation,
oversampling, mesh and texture density for terrain, applied by "Reload layer").

### Changing data at runtime

Layers of any format can be added, removed and reloaded into the other projection while the
viewer renders and pages. Two rules keep that crash-free:

- A node taken out of the scene (a removed layer, the whole scene on a view switch) is kept for a
  few frames (`ViewerApp::kRetireFrames`) before it is released, because a frame already
  submitted to the GPU may still bind its pipelines and textures. Without this the Vulkan
  validation layer reports `vkDestroyPipeline` / `vkDestroyImageView` on objects in use.
- A tile layer (raster or tile directory) lives exactly as long as the node it was read into; the
  reader only keeps a weak reference, so removing the layer closes its raster, and a child request
  that the pager issues afterwards is answered with a read error rather than served from freed
  memory. Layer ids are unique across readers and views.

`ive_poc --script FILE` runs a scenario unattended for checking this (one `FRAME ACTION ARGS` per
line: `add PATH[?opts] [CRS]`, `terrain PATH`, `vector PATH`, `remove INDEX|all`, `reload INDEX`,
`switch VIEW`, `goto LON LAT ZOOM`, `orient HEADING TILT`, `home`, `set KEY VALUE` with the keys `drape on|off`,
`drapeminzoom`, `globalminzoom`, `globemaxzoom`, `mapmaxzoom`, `tolerance`, `width INDEX PX`,
`colour INDEX rrggbb`, `quit`).
Run it with `--debug` for the validation layer, or with an AddressSanitizer build
(`cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-fsanitize=address
-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address`, then `ASAN_OPTIONS=detect_leaks=0`). A scenario that
adds every format, switches views six times and removes layers while their tiles are still paging
passes both.

## Tiled base map (OGC WebMercatorQuad as .ive)

`ivetiler` cuts any EPSG:3857 raster into the OGC WebMercatorQuad pyramid (256 px tiles, level 0 =
156543 m/px, each level halves it, 2^z tiles across) as OpenSceneGraph `.ive` files that VSG pages in
by level of detail:

```
ivetiler <source.tif (EPSG:3857)> <out dir> [--max-level N] [--tile-size 256] [--detail-bias 1.0]
         [--threads N] [--uncompressed] [--detail-extent MINLON MINLAT MAXLON MAXLAT --detail-level N]
         [--geocentric [--globe-grid 8]] [--format ive|vsgb|tms|xyz [--image-ext png|jpg] [--texture bc1|rgba]]
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

`--format vsgb` writes the same pyramid as VSG native files (`root.vsgb`, `z/x/y.vsgb`): the tiles
are ready to draw (flat quads, or grids on the ellipsoid with `--geocentric`), their textures are BC1
compressed with full mip chains (`TextureCompress.h`, an eighth of RGBA, so a global level 9 is about
the size of the `.ive` set; `--texture rgba` keeps them uncompressed for small sets), and `root.vsgb`
records the CRS so the viewer places the set without being told. The viewer shows it as read in its own frame and reprojects it, children included, in the
other one. `--format tms` or `--format xyz` writes plain tile images instead (PNG with alpha outside
the raster, or JPEG) in a `z/x/y` tree: TMS counts rows from the south and adds
`tilemapresource.xml`, XYZ counts from the north. The viewer reads either tree directly (see "Other
data" above), on the map or the globe, so `--geocentric` does not apply to them.

The demo data sets built from the level-9 Blue Marble (`bluemarble_src/bluemarble_3857_131072.tif`,
JPEG-in-TIFF with overviews, itself loadable directly as a GeoTIFF):

```
ivetiler bluemarble_3857_131072.tif bluemarble_ecef_l9      --geocentric --max-level 9        # .ive, 12 GB
ivetiler bluemarble_3857_131072.tif bluemarble_vsgb_ecef_l9 --geocentric --max-level 9 --format vsgb
ive_poc --projection globe --globe-tiles bluemarble_vsgb_ecef_l9/root.vsgb
ive_poc --base-tiles bluemarble_src/bluemarble_3857_131072.tif          # the GeoTIFF as the map's base
```

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
  - `ElevationSource` — DTED / DEM heights through GDAL (windows at any resolution, mosaics, bilinear / bicubic sampling) and the `ElevationRegistry` the terrain layers publish to
  - `TerrainLayerReader` — elevation as paged, hillshaded terrain tiles that keep subdividing past the data's resolution
  - `VectorSource`, `VectorLayerReader` — OGR features in degrees (densify / simplify / clip helpers) and the paged, drapable line layers made from them
  - `LayerOptions`, `LayerVisibility` — per-layer options (`path?key=value` specs, declared scales, LOD groups, styles) and the zoom rules that decide what is drawn
  - `TileSetBuilder` — GDAL + OSG tiler behind `tools/ivetiler`
  - `ViewerApp` — window, camera, base map and layer sources, the current `WorldView` and runtime switching, frame loop
- `app/main.cpp` — thin command line viewer
- `tools/ivetiler/` — tile pyramid generator
- `tests/` — Google Test suite (`ctest` in the build directory, or CLion's test runner); the fixtures use per-test temporary directories, so `ctest -j` is safe
- `data/` — base map image and the script that regenerates it (see `data/README.md`), plus `fetch_natural_earth.sh` (boundary shapefiles at three scales) and `fetch_dted_sample.sh` (genuine DTED cells made from Copernicus DEM tiles)
- `lib/.../TileGrid`, `TileSource`, `TileLayerReader` — any-CRS tile matrix sets, GDAL raster and tile directory sources, and the reader that pages them as textured tiles in the display frame
- `lib/.../VsgReproject` — reprojection of VSG scene graphs and the `.vsgb` / `.vsgt` reader that applies it
- `lib/.../TileGeometry`, `TextureCompress` — textured tile meshes and their shared pipeline (used by the on-the-fly reader and the `vsgb` tiler), BC1 texture compression with mip chains
- `lib/.../LoaderKeys.h` — the option keys the readers share (target / source CRS, tessellation, detail bias)
- `tools/ivevsgconv/` — converts any readable scene to `.vsgb` / `.vsgt` with its CRS recorded
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
