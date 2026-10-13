#!/usr/bin/env bash
# Fetches NASA's Blue Marble Next Generation (Dec 2004, topography + bathymetry, 5400x2700, public domain)
# and reprojects it from equirectangular (EPSG:4326) to the Web Mercator square (EPSG:3857) used by the
# viewer's base map. Needs GDAL; the vcpkg-built tools are used when present.
set -euo pipefail
cd "$(dirname "$0")"

SRC_URL="https://eoimages.gsfc.nasa.gov/images/imagerecords/73000/73909/world.topo.bathy.200412.3x5400x2700.jpg"
SRC="world.topo.bathy.200412.3x5400x2700.jpg"
OUT="bluemarble_3857.jpg"
SIZE="${1:-4096}"

GDAL_BIN="${GDAL_BIN:-/home/heartslaught/vcpkg/vcpkg_installed/x64-linux-release-clang/tools/gdal}"
[ -x "$GDAL_BIN/gdalwarp" ] || GDAL_BIN="$(dirname "$(command -v gdalwarp)")"

[ -f "$SRC" ] || curl -L -o "$SRC" "$SRC_URL"

E=20037508.342789244
TMP="$(mktemp -d)"
"$GDAL_BIN/gdal_translate" -q -of GTiff -a_srs EPSG:4326 -a_ullr -180 90 180 -90 "$SRC" "$TMP/bm4326.tif"
"$GDAL_BIN/gdalwarp" -q -overwrite -t_srs EPSG:3857 -te -$E -$E $E $E -ts "$SIZE" "$SIZE" -r cubic "$TMP/bm4326.tif" "$TMP/bm3857.tif"
"$GDAL_BIN/gdal_translate" -q -of JPEG -co QUALITY=92 "$TMP/bm3857.tif" "$OUT"
rm -rf "$TMP" "$OUT.aux.xml"
echo "wrote $OUT (${SIZE}x${SIZE}, EPSG:3857)"
