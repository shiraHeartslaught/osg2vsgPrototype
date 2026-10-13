#!/usr/bin/env bash
# Georeference NASA's 8 Blue Marble pieces, mosaic them, warp to the EPSG:3857 world square at
# 131072 px (= OGC level 9, 512 tiles x 256 px) as a tiled JPEG-in-TIFF, and add averaged overviews
# so the tiler can read coarse levels cheaply.
set -euo pipefail
G=/home/heartslaught/vcpkg/vcpkg_installed/x64-linux-release-clang/tools/gdal
D=/run/media/heartslaught/games/ive_test/bluemarble_src
cd "$D"
E=20037508.342789244
declare -A ULX=([A]=-180 [B]=-90 [C]=0 [D]=90)
declare -A ULY=([1]=90 [2]=0)
mkdir -p pieces
echo "[1/4] pieces -> tiled GeoTIFFs"
for t in A1 B1 C1 D1 A2 B2 C2 D2; do
  col=${t:0:1}; row=${t:1:1}
  ulx=${ULX[$col]}; uly=${ULY[$row]}; lrx=$((ulx+90)); lry=$((uly-90))
  out=pieces/bm_$t.tif
  [ -s "$out" ] || "$G/gdal_translate" -q -of GTiff -a_srs EPSG:4326 -a_ullr $ulx $uly $lrx $lry \
      -co COMPRESS=JPEG -co JPEG_QUALITY=95 -co PHOTOMETRIC=YCBCR -co TILED=YES -co BLOCKXSIZE=512 -co BLOCKYSIZE=512 \
      "world.topo.bathy.200412.3x21600x21600.$t.jpg" "$out"
  echo "   $t done ($(date +%H:%M:%S))"
done
echo "[2/4] mosaic VRT"
"$G/gdalbuildvrt" -q -overwrite mosaic_4326.vrt pieces/bm_*.tif
echo "[3/4] warp to EPSG:3857 131072x131072 ($(date +%H:%M:%S))"
"$G/gdalwarp" -q -overwrite -t_srs EPSG:3857 -te -$E -$E $E $E -ts 131072 131072 -r cubic \
    -multi -wo NUM_THREADS=ALL_CPUS -wm 4096 --config GDAL_CACHEMAX 4096 \
    -co COMPRESS=JPEG -co JPEG_QUALITY=92 -co PHOTOMETRIC=YCBCR -co TILED=YES -co BLOCKXSIZE=512 -co BLOCKYSIZE=512 -co BIGTIFF=YES \
    mosaic_4326.vrt bluemarble_3857_131072.tif
echo "[4/4] overviews ($(date +%H:%M:%S))"
"$G/gdaladdo" -q -r average --config COMPRESS_OVERVIEW JPEG --config PHOTOMETRIC_OVERVIEW YCBCR --config JPEG_QUALITY_OVERVIEW 92 \
    --config GDAL_NUM_THREADS ALL_CPUS bluemarble_3857_131072.tif 2 4 8 16 32 64 128 256 512
echo "done ($(date +%H:%M:%S))"
ls -la bluemarble_3857_131072.tif
