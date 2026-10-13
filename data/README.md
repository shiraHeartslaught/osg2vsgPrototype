# data/

- `bluemarble_3857.jpg` — base map used behind the terrain: NASA Blue Marble Next Generation
  (December 2004, topography + bathymetry), public domain, reprojected to the EPSG:3857 world square
  (±20037508.34 m) at 4096×4096 with GDAL. Regenerate with `./fetch_bluemarble.sh [size]`.
- `world.topo.bathy.200412.3x5400x2700.jpg` — the untouched NASA source image (equirectangular).

Source: https://visibleearth.nasa.gov/images/73909/december-blue-marble-next-generation-w-topography-and-bathymetry
